#pragma once
#include <cstdint>
#include <ReZeroDetour/ReZeroLib.h>
namespace ReZeroDetour {

    enum class ErrorCode : uint8_t {
        VirtualProtectFailed,
        DisasmDecodeFailed,
        UnsupportedPrologue,
        DisasmRelocationOverflow,
        AllocationFailed,
        DecoderInitializeFailed,
        UninstallTimedOut,
    };

} // namespace ReZeroDetour
