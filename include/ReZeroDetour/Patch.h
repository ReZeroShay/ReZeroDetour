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

namespace ReZeroDetour {
    // push imm32
    // mov dword ptr [rsp+4], imm32
    // ret
    inline void makeAbsoluteJumpAt(Span<uint8_t> payload, uintptr_t dest) {
        REZERO_ASSERT(payload.size() == 14);
        uint8_t buffer[14] = {};

        auto low = static_cast<uint32_t>(dest & 0xFFFFFFFF);
        auto high = static_cast<uint32_t>(dest >> 32);

        buffer[0] = 0x68;
        *reinterpret_cast<uint32_t *>(&buffer[1]) = low;
        buffer[5] = 0xC7;
        buffer[6] = 0x44;
        buffer[7] = 0x24;
        buffer[8] = 0x04;
        *reinterpret_cast<uint32_t *>(&buffer[9]) = high;
        buffer[13] = 0xC3;

        memcpy(payload.data(), buffer, sizeof(buffer));
    }

    // 假设当前指令的 RIP = 0x00007FF612341000
    // 目标地址           = 0x00007FF612341500
    // 则 disp = 目标 - (当前RIP + 指令长度) = 0x500 - 5 = 0x4FB
    //
    // 位移必须装得进 int32：装不下就明确报错，不能静默截断成野地址。
    inline auto makeRel32JumpAt(Span<uint8_t> payload, intptr_t src, intptr_t dest) -> Result<void, ErrorCode> {
        REZERO_ASSERT(payload.size() == 5);

        const intptr_t displacement = dest - (src + 5);
        if (displacement < INT32_MIN || displacement > INT32_MAX) {
            return Error{ErrorCode::DisasmRelocationOverflow};
        }

        uint8_t buffer[5] = {};
        buffer[0] = 0xE9;
        auto disp32 = static_cast<int32_t>(displacement);
        memcpy(&buffer[1], &disp32, sizeof(disp32));
        memcpy(payload.data(), buffer, sizeof(buffer));

        return {};
    }

    inline auto writeProtectedMemory(void *address, const void *data, size_t size) -> Result<void, ErrorCode> {
        if (size == 0) {
            return {};
        }

        DWORD previous = 0;

        if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &previous)) {
            return Error{ErrorCode::VirtualProtectFailed};
        }

        std::memcpy(address, data, size);

        DWORD ignored = 0;

        if (!VirtualProtect(address, size, previous, &ignored)) {
            return Error{ErrorCode::VirtualProtectFailed};
        }
        return {};
    }

    class InlinePatch {
        template <typename T> using Result = Result<T, ErrorCode>;

      public:
        static constexpr std::size_t kPatchSize = 5;

        InlinePatch() = default;
        InlinePatch(const InlinePatch &) = delete;
        InlinePatch &operator=(const InlinePatch &) = delete;

        auto install(void *src, void *dest) -> Result<void> {
            REZERO_ASSERT(src != nullptr);
            REZERO_ASSERT(dest != nullptr);

            uint8_t buffer[kPatchSize] = {};
            REZERO_CHECK(makeRel32JumpAt({buffer, kPatchSize}, reinterpret_cast<intptr_t>(src),
                                         reinterpret_cast<intptr_t>(dest)));
            memcpy(OriginalBytes, src, kPatchSize);
            REZERO_CHECK(writeProtectedMemory(src, buffer, kPatchSize));
            Src = src;
            Installed = true;
            return {};
        }

        auto uninstall() -> Result<void> {
            if (!Installed) {
                return {};
            }
            REZERO_CHECK(writeProtectedMemory(Src, OriginalBytes, kPatchSize));

            Installed = false;
            return {};
        }

      private:
        void *Src = nullptr;
        uint8_t OriginalBytes[kPatchSize];
        bool Installed = false;
    };
} // namespace ReZeroDetour
