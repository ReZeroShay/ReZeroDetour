#pragma once
#include <ReZeroDetour/ReZeroLib.h>
#include <ReZeroDetour/CodeGen.h>

namespace ReZeroDetour {

    // 把 src 开头至少 minPrologueBytes 字节的指令搬到 trampoline，并修正其中的
    // 相对操作数（相对立即数 + RIP 相对访存）。返回实际搬走的字节数。
    // 实现在 src/Disassembler.cpp（Zydis 解码器只在那里出现）。
    class Disassembler {
        template <typename T> using Result = Result<T, ErrorCode>;

      public:
        auto buildTrampolineFromPrologue(Span<uint8_t> src, Span<uint8_t> trampoline, size_t minPrologueBytes)
            -> Result<uintptr_t>;
    };

    class Trampoline {
        template <typename T> using Result = Result<T, ErrorCode>;
        using Pointer = void *;

      public:
        // 搬过来的序言 + 尾部绝对跳转所需的空间
        static constexpr std::size_t kCapacity = 100;
        static constexpr std::size_t kReturnJumpSize = kAbsoluteJumpSize;

        Trampoline() = default;

        Trampoline(const Trampoline &) = delete;
        Trampoline &operator=(const Trampoline &) = delete;

        auto build(void *src) -> Result<void> {
            REZERO_ASSERT(Ptr != nullptr);
            REZERO_ASSERT(src != nullptr);

            // Near jmp + 4 byte relative offset
            constexpr size_t minPrologueBytes = 5;

            REZERO_TRY(offset, disasm.buildTrampolineFromPrologue({reinterpret_cast<uint8_t *>(src), kCapacity},
                                                                  {reinterpret_cast<uint8_t *>(Ptr), kCapacity},
                                                                  minPrologueBytes));

            if (offset + kReturnJumpSize > kCapacity) {
                return Error(ErrorCode::UnsupportedPrologue);
            }

            // 在 trampoline 尾部添加跳转回原函数剩余部分的跳转指令
            // 1. 计算原函数剩余指令的绝对内存地址
            uintptr_t returnAddress = reinterpret_cast<uintptr_t>(src) + offset;

            // 2. 生成跳转到该绝对地址的代码片段
            REZERO_CHECK(makeAbsoluteJumpAt(Span<uint8_t>(reinterpret_cast<uint8_t *>(Ptr) + offset, kReturnJumpSize),
                                            returnAddress));

            return {};
        }
        auto &&at(this auto &&self, void *ptr) {
            self.Ptr = ptr;
            return std::forward<decltype(self)>(self);
        }

        // 与 Thunk::destroy 保持一致：Ptr 为空时直接返回
        auto destroy() -> void {
            if (Ptr != nullptr) {
                memset(Ptr, 0, kCapacity);
            }
        }

        [[nodiscard]] auto pointer() -> Pointer { return Ptr; }

      public:
      private:
        Pointer Ptr = nullptr;
        Disassembler disasm;
    };
} // namespace ReZeroDetour
