#pragma once

#include <ReZeroDetour/ReZeroLib.h>
#include <ReZeroDetour/Error.h>

namespace ReZeroDetour {
    class Disassembler {
        template <typename T> using Result = Result<T, ErrorCode>;

      public:
        // 把 src 开头至少 minPrologueBytes 字节的指令搬到 trampoline，并修正其中的
        // 相对操作数（相对立即数 + RIP 相对访存）。返回实际搬走的字节数。
        auto buildTrampolineFromPrologue(Span<uint8_t> src, Span<uint8_t> trampoline, size_t minPrologueBytes)
            -> Result<uintptr_t>;
    };

} // namespace ReZeroDetour
