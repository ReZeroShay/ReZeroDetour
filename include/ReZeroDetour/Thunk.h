#pragma once
#if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#    define NOMINMAX
#endif
#include <windows.h>
#include <ReZeroDetour/ReZeroLib.h>
#include <ReZeroDetour/Error.h>
#include <ReZeroDetour/TlsContext.h>
namespace ReZeroDetour {

    class Thunk {
        template <typename T> using Result = Result<T, ErrorCode>;

      public:
        // 生成的 stub 需要的空间上限（实际约 102 字节）
        static constexpr std::size_t kCapacity = 256;

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
        void build(void *instance, void *entry, void *activeCalls) {
            REZERO_ASSERT(instance != nullptr);
            REZERO_ASSERT(entry != nullptr);
            REZERO_ASSERT(activeCalls != nullptr);

            constexpr std::uint8_t kFrameSize = 0x60;   // 影子空间 + XMM 保存槽
            constexpr std::uint8_t kXmmSlotBase = 0x20; // XMM 槽起点（跳过影子空间）
            constexpr std::uint8_t kXmmSlotSize = 16;

            const void *setter = reinterpret_cast<const void *>(&detail::bindCurrentHook);

            uint8_t code[kCapacity] = {};
            std::size_t n = 0;

            const auto byte = [&](std::uint8_t value) { code[n++] = value; };
            const auto imm32 = [&](std::int32_t value) {
                memcpy(&code[n], &value, 4);
                n += 4;
            };
            const auto imm64 = [&](const void *value) {
                memcpy(&code[n], &value, 8);
                n += 8;
            };

            // mov r10, imm64(instance)
            byte(0x49);
            byte(0xBA);
            imm64(instance);

            // mov r11, imm64(activeCalls) ; lock inc dword ptr [r11]
            //   在飞调用计数 +1。放在这里是为了让"未被计数的窗口"尽量短；
            //   lock inc 在 x86 上自带全屏障，所以与 dispatch 里的原子自减配对没有问题。
            byte(0x49);
            byte(0xBB);
            imm64(activeCalls);
            byte(0xF0); // lock
            byte(0x41);
            byte(0xFF);
            byte(0x03);

            // push rcx / rdx / r8 / r9 / rax
            byte(0x51);
            byte(0x52);
            byte(0x41);
            byte(0x50);
            byte(0x41);
            byte(0x51);
            byte(0x50);

            // sub rsp, kFrameSize
            byte(0x48);
            byte(0x83);
            byte(0xEC);
            byte(kFrameSize);

            // movups [rsp + slot], xmm0..xmm3
            for (std::uint8_t i = 0; i < 4; ++i) {
                byte(0x0F);
                byte(0x11);
                byte(static_cast<std::uint8_t>(0x44 | (i << 3)));
                byte(0x24);
                byte(static_cast<std::uint8_t>(kXmmSlotBase + i * kXmmSlotSize));
            }

            // mov rcx, r10 ; mov r11, imm64(setter) ; call r11
            byte(0x4C);
            byte(0x89);
            byte(0xD1);
            byte(0x49);
            byte(0xBB);
            imm64(setter);
            byte(0x41);
            byte(0xFF);
            byte(0xD3);

            // movups xmm0..xmm3, [rsp + slot]
            for (std::uint8_t i = 0; i < 4; ++i) {
                byte(0x0F);
                byte(0x10);
                byte(static_cast<std::uint8_t>(0x44 | (i << 3)));
                byte(0x24);
                byte(static_cast<std::uint8_t>(kXmmSlotBase + i * kXmmSlotSize));
            }

            // add rsp, kFrameSize
            byte(0x48);
            byte(0x83);
            byte(0xC4);
            byte(kFrameSize);

            // pop rax / r9 / r8 / rdx / rcx
            byte(0x58);
            byte(0x41);
            byte(0x59);
            byte(0x41);
            byte(0x58);
            byte(0x5A);
            byte(0x59);

            // jmp qword ptr [rip+0] ; <quad entry>
            // 用内存间接跳转而不是 mov rax/jmp rax：不破坏刚恢复的 RAX（变参函数要用 AL）
            byte(0xFF);
            byte(0x25);
            imm32(0);
            imm64(entry);

            REZERO_ASSERT(n <= kCapacity);
            memcpy(Ptr, code, n);
        }

        void destroy() noexcept {
            if (Ptr) {
                memset(Ptr, 0, kCapacity);
            }
        }

        auto &&at(this auto &&self, void *ptr) {
            self.Ptr = ptr;
            return std::forward<decltype(self)>(self);
        }
        auto pointer() -> void * { return Ptr; }

      private:
        void *Ptr = nullptr;
    };
} // namespace ReZeroDetour
