#pragma once
#if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#    define NOMINMAX
#endif
#include <windows.h>
#include <ReZeroDetour/ReZeroLib.h>
#include <ReZeroDetour/CodeGen.h>
namespace ReZeroDetour {

    // ============================================================
    // TLS 交接槽：实例指针怎么从入口 stub 交到 dispatch
    //
    // 这里刻意不自己推导 TLS 数据块的索引与偏移：那是编译器的知识。所以让
    // stub 回调一个普通 C++ 函数，由编译器生成那句 `tlsCurrentHookInstance = instance;`。
    // stub 只承担"保存/恢复易失实参寄存器"这件 ABI 上非常稳定的事。
    // 写入与读出之间不存在任何用户代码，所以单槽就够；嵌套与重入的恢复由
    // ReZeroDetour.h 里的每线程活动栈负责。
    // ============================================================
    namespace detail {
        inline thread_local void *tlsCurrentHookInstance = nullptr;

        // 入口 stub 调用的 setter。
        REZERO_NOINLINE inline void bindCurrentHook(void *instance) noexcept { tlsCurrentHookInstance = instance; }

        // 读走并清空，所以不会留下悬垂的实例指针。
        [[nodiscard]] inline auto takeCurrentHook() noexcept -> void * {
            void *current = tlsCurrentHookInstance;
            tlsCurrentHookInstance = nullptr;
            return current;
        }
    } // namespace detail

    class Thunk {
        template <typename T> using Result = Result<T, ErrorCode>;

      public:
        // 生成的 stub 需要的空间上限（实际约 116 字节）
        static constexpr std::size_t kCapacity = 256;

        // 指令由 CodeGen 编码，长度从编码器返回，不再手写操作码。
        // 每个 emit 的长度都会被检查是否超出 kCapacity。
        Thunk() = default;
        ~Thunk() { destroy(); }

        Thunk(const Thunk &) = delete;
        Thunk &operator=(const Thunk &) = delete;

        // 生成入口 stub：
        //
        //   进入 Stub
        //     ↓
        //   本实例的在飞调用计数 +1（最先执行，见下）
        //     ↓
        //   保存原始参数寄存器 (RCX/RDX/R8/R9/RAX) 与浮点参数寄存器 (XMM0-3)
        //     ↓
        //   调用 bindCurrentHook(instance)
        //     ↓
        //   恢复原始参数寄存器
        //     ↓
        //   跳到该签名专属的 C++ entry (dispatch)
        //
        // 栈状态（进入 stub 时 RSP = X，X % 16 == 8，这是函数入口的 ABI 状态）：
        //
        //   push rcx/rdx/r8/r9/rax   ; 40 字节，RSP % 16 == 0
        //   sub  rsp, 0x60           ; 0x20 影子空间 + 0x40 保存 XMM0-3
        //   call r11                 ; 被调方入口 RSP % 16 == 8 ✓
        //   add  rsp, 0x60 / pop ×5  ; RSP 精确回到 X
        //   jmp  [rip]               ; 参数寄存器已还原，栈布局与进入时一模一样
        //
        // 影子空间在 [rsp, rsp+0x20)，保存槽在 [rsp+0x20, rsp+0x60)，两者不重叠，
        // 所以 bindCurrentHook 不会踩到我们保存的值。结束时 RSP 精确还原，
        // 因此被 hook 函数的栈参数位置不变，栈回溯也保持有效。
        //
        // 计数为什么放在最前面：uninstall() 靠这个计数判断"还有没有调用在飞"，
        // 计数越早自增，"未被计数的窗口"就越短（这个窗口无法减到 0，见 uninstall() 的说明）。
        // 计数器本身活在 InlineHook 对象里、不在这份可执行页里 —— 这样即使页将来被释放，
        // 计数也不会跟着消失。r10/r11 是易失且不参与传参的寄存器，用它们不会碰到实参。
        auto build(void *instance, void *entry, void *activeCalls) -> Result<void> {
            REZERO_ASSERT(Ptr != nullptr);
            REZERO_ASSERT(instance != nullptr);
            REZERO_ASSERT(entry != nullptr);
            REZERO_ASSERT(activeCalls != nullptr);

            if (Ptr == nullptr || instance == nullptr || entry == nullptr || activeCalls == nullptr) {
                return Error{ErrorCode::CodeGenBufferFull};
            }

            constexpr std::int32_t kFrameSize = 0x60;    // 影子空间 + XMM 保存槽
            constexpr std::int32_t kXmmSlotBase = 0x20;  // XMM 槽起点（跳过影子空间）
            constexpr std::int32_t kXmmSlotSize = 16;

            CodeGen gen({static_cast<std::uint8_t *>(Ptr), kCapacity});

            // mov r10, imm64(instance)
            REZERO_CHECK(gen.movRegImm64(ZYDIS_REGISTER_R10, reinterpret_cast<std::uint64_t>(instance)));

            // mov r11, imm64(activeCalls) ; lock inc dword ptr [r11]
            //   在飞调用计数 +1。放在这里是为了让"未被计数的窗口"尽量短；
            //   lock inc 在 x86 上自带全屏障，所以与 dispatch 里的原子自减配对没有问题。
            REZERO_CHECK(gen.movRegImm64(ZYDIS_REGISTER_R11, reinterpret_cast<std::uint64_t>(activeCalls)));
            REZERO_CHECK(gen.lockIncDword(ZYDIS_REGISTER_R11));

            // push rcx / rdx / r8 / r9 / rax
            REZERO_CHECK(gen.push(ZYDIS_REGISTER_RCX));
            REZERO_CHECK(gen.push(ZYDIS_REGISTER_RDX));
            REZERO_CHECK(gen.push(ZYDIS_REGISTER_R8));
            REZERO_CHECK(gen.push(ZYDIS_REGISTER_R9));
            REZERO_CHECK(gen.push(ZYDIS_REGISTER_RAX));

            // sub rsp, kFrameSize
            REZERO_CHECK(gen.subRegImm(ZYDIS_REGISTER_RSP, kFrameSize));

            // movups [rsp + slot], xmm0..xmm3
            for (std::int32_t i = 0; i < 4; ++i) {
                const auto xmm = static_cast<ZydisRegister>(ZYDIS_REGISTER_XMM0 + i);
                REZERO_CHECK(gen.movupsStore(xmm, ZYDIS_REGISTER_RSP, kXmmSlotBase + i * kXmmSlotSize));
            }

            // mov rcx, r10 ; mov r11, imm64(setter) ; call r11
            REZERO_CHECK(gen.movRegReg(ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_R10));
            REZERO_CHECK(
                gen.movRegImm64(ZYDIS_REGISTER_R11, reinterpret_cast<std::uint64_t>(&detail::bindCurrentHook)));
            REZERO_CHECK(gen.callReg(ZYDIS_REGISTER_R11));

            // movups xmm0..xmm3, [rsp + slot]
            for (std::int32_t i = 0; i < 4; ++i) {
                const auto xmm = static_cast<ZydisRegister>(ZYDIS_REGISTER_XMM0 + i);
                REZERO_CHECK(gen.movupsLoad(xmm, ZYDIS_REGISTER_RSP, kXmmSlotBase + i * kXmmSlotSize));
            }

            // add rsp, kFrameSize
            REZERO_CHECK(gen.addRegImm(ZYDIS_REGISTER_RSP, kFrameSize));

            // pop rax / r9 / r8 / rdx / rcx
            REZERO_CHECK(gen.pop(ZYDIS_REGISTER_RAX));
            REZERO_CHECK(gen.pop(ZYDIS_REGISTER_R9));
            REZERO_CHECK(gen.pop(ZYDIS_REGISTER_R8));
            REZERO_CHECK(gen.pop(ZYDIS_REGISTER_RDX));
            REZERO_CHECK(gen.pop(ZYDIS_REGISTER_RCX));

            // jmp qword ptr [rip+0] ; <quad entry>
            // 用内存间接跳转而不是 mov rax/jmp rax：不破坏刚恢复的 RAX（变参函数要用 AL）。
            // 指针就写在 jmp 后面，disp=0 指的是"下一条指令"，所以 jmp 自身长度不影响它。
            REZERO_CHECK(gen.jmpRipPtr(entry, 6));

            Size = gen.size();
            return {};
        }

        // 上一次 build 生成的字节数。
        [[nodiscard]] auto size() const noexcept -> std::size_t { return Size; }

        void destroy() noexcept {
            if (Ptr) {
                memset(Ptr, 0, kCapacity);
            }
            Size = 0;
        }

        auto &&at(this auto &&self, void *ptr) {
            self.Ptr = ptr;
            return std::forward<decltype(self)>(self);
        }
        auto pointer() -> void * { return Ptr; }

      private:
        void *Ptr = nullptr;
        std::size_t Size = 0;
    };
} // namespace ReZeroDetour
