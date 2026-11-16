#pragma once

#include <cstddef>
#include <cstdint>
#include <Zydis/Encoder.h>
#include <ReZeroDetour/ReZeroLib.h>

// 指令编码器：替掉手写机器码。
//
//   CodeGen gen({buffer, capacity});
//   REZERO_CHECK(gen.push(ZYDIS_REGISTER_RCX));
//   REZERO_CHECK(gen.movRegImm64(ZYDIS_REGISTER_R11, target, /*expectedLength=*/10));
//
// 每一处 emit 都返回实际写入的字节数，gen.size() 是累计长度。指令长度是这套
// 机制里最容易出错的东西（跳转位移、stub 内偏移、容量上限都依赖它），所以：
//
//   - 长度永远从编码器拿回来，不在调用点假设；
//   - 布局依赖长度时传 expectedLength，长度不符当场报错，而不是让偏移悄悄漂移；
//   - 写之前检查剩余容量，越界返回 CodeGenBufferFull，不是踩内存。
//
// 注意 Zydis 会挑最短编码：`mov r10, 0x1234` 是 7 字节（C7 形式），只有立即数
// 装不进 int32 才是 10 字节的 imm64 形式。需要固定布局就别依赖这个自动选择。

namespace ReZeroDetour {

    class CodeGen {
        template <typename T> using Result = Result<T, ErrorCode>;

      public:
        // x86 单条指令最长 15 字节
        static constexpr std::size_t kMaxInstructionLength = 15;

        // 几个长度被布局依赖的指令的固定长度
        static constexpr std::size_t kJmpRel32Length = 5;
        static constexpr std::size_t kPushImm32Length = 5;
        static constexpr std::size_t kMovMemImm32Length = 8;
        static constexpr std::size_t kRetLength = 1;

        constexpr explicit CodeGen(Span<std::uint8_t> out) noexcept : Out(out) {}

        // 编码一条指令，返回写入的字节数。
        auto emit(const ZydisEncoderRequest &request) -> Result<std::size_t> {
            std::uint8_t buffer[kMaxInstructionLength] = {};
            ZyanUSize length = kMaxInstructionLength;

            if (ZYAN_FAILED(ZydisEncoderEncodeInstruction(&request, buffer, &length))) {
                return Error{ErrorCode::EncodeFailed};
            }

            return append(buffer, length);
        }

        // 编码一条指令并校验长度。expectedLength 为 0 表示不校验。
        auto emit(const ZydisEncoderRequest &request, std::size_t expectedLength) -> Result<std::size_t> {
            auto encoded = emit(request);
            if (!encoded) {
                return encoded;
            }
            if (expectedLength != 0 && *encoded != expectedLength) {
                return Error{ErrorCode::EncodedLengthMismatch};
            }
            return encoded;
        }

        // 原样追加数据（例如 `jmp [rip+0]` 后面那个 8 字节指针）。
        auto emitData(const void *data, std::size_t count) -> Result<void> {
            if (count > remaining()) {
                return Error{ErrorCode::CodeGenBufferFull};
            }
            memcpy(Out.data() + Offset, data, count);
            Offset += count;
            return {};
        }

        // ---------------- 常用指令 ----------------

        auto movRegImm64(ZydisRegister destination, std::uint64_t value, std::size_t expectedLength = 0)
            -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_MOV);
            addRegister(request, destination);
            addImmediateUnsigned(request, value);
            return emit(request, expectedLength);
        }

        auto movRegReg(ZydisRegister destination, ZydisRegister source, std::size_t expectedLength = 0)
            -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_MOV);
            addRegister(request, destination);
            addRegister(request, source);
            return emit(request, expectedLength);
        }

        // lock inc dword ptr [base]
        auto lockIncDword(ZydisRegister base, std::size_t expectedLength = 0) -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_INC);
            request.prefixes |= ZYDIS_ATTRIB_HAS_LOCK;
            addMemory(request, base, 0, 4);
            return emit(request, expectedLength);
        }

        auto push(ZydisRegister reg, std::size_t expectedLength = 0) -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_PUSH);
            addRegister(request, reg);
            return emit(request, expectedLength);
        }

        auto pop(ZydisRegister reg, std::size_t expectedLength = 0) -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_POP);
            addRegister(request, reg);
            return emit(request, expectedLength);
        }

        auto subRegImm(ZydisRegister reg, std::int32_t value, std::size_t expectedLength = 0) -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_SUB);
            addRegister(request, reg);
            addImmediateSigned(request, value);
            return emit(request, expectedLength);
        }

        auto addRegImm(ZydisRegister reg, std::int32_t value, std::size_t expectedLength = 0) -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_ADD);
            addRegister(request, reg);
            addImmediateSigned(request, value);
            return emit(request, expectedLength);
        }

        // movups xmmword ptr [base + displacement], xmm
        auto movupsStore(ZydisRegister xmm, ZydisRegister base, std::int32_t displacement, std::size_t expectedLength = 0)
            -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_MOVUPS);
            addMemory(request, base, displacement, 16);
            addRegister(request, xmm);
            return emit(request, expectedLength);
        }

        // movups xmm, xmmword ptr [base + displacement]
        auto movupsLoad(ZydisRegister xmm, ZydisRegister base, std::int32_t displacement, std::size_t expectedLength = 0)
            -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_MOVUPS);
            addRegister(request, xmm);
            addMemory(request, base, displacement, 16);
            return emit(request, expectedLength);
        }

        auto callReg(ZydisRegister reg, std::size_t expectedLength = 0) -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_CALL);
            addRegister(request, reg);
            return emit(request, expectedLength);
        }

        // jmp qword ptr [rip+0]，紧接着写入指针本体。
        // disp 是相对"下一条指令"的，所以 jmp 自己多长都不影响这个写法。
        auto jmpRipPtr(const void *destination, std::size_t expectedLength = 0) -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_JMP);
            addMemory(request, ZYDIS_REGISTER_RIP, 0, 8);

            auto jump = emit(request, expectedLength);
            if (!jump) {
                return jump;
            }

            auto pointer = emitData(&destination, sizeof(destination));
            if (!pointer) {
                return Error{pointer.error()};
            }

            return *jump + sizeof(destination);
        }

        // jmp rel32。强制 32 位位移：不强制的话编码器会对装得进 int8 的位移
        // 选 EB（2 字节），而补丁那套东西是按"jmp 恒为 5 字节"算位移的。
        auto jmpRel32(std::int32_t displacement, std::size_t expectedLength = kJmpRel32Length) -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_JMP);
            request.branch_type = ZYDIS_BRANCH_TYPE_NEAR;
            request.branch_width = ZYDIS_BRANCH_WIDTH_32;
            addImmediateSigned(request, displacement);
            return emit(request, expectedLength);
        }

        // push imm32。
        //
        // 编码器对装得进 int8 的立即数会选 6A（2 字节）。6A 与 68 都是压入 8 字节，
        // 对这里的用途（压地址的低半部分）语义等价，所以用 NOP 把长度补齐到 5 字节，
        // 让"固定 14 字节绝对跳转"这种依赖长度的布局仍然成立。
        auto pushImm32(std::int32_t value, std::size_t expectedLength = kPushImm32Length) -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_PUSH);
            addImmediateSigned(request, value);

            auto encoded = emit(request);
            if (!encoded) {
                return encoded;
            }
            if (expectedLength == 0 || *encoded == expectedLength) {
                return encoded;
            }
            if (*encoded > expectedLength) {
                return Error{ErrorCode::EncodedLengthMismatch};
            }

            REZERO_CHECK(padWithNops(expectedLength - *encoded));
            return expectedLength;
        }

        // mov dword ptr [base + displacement], imm32
        auto movMemImm(ZydisRegister base, std::int32_t displacement, std::uint16_t size, std::uint32_t value,
                       std::size_t expectedLength = kMovMemImm32Length) -> Result<std::size_t> {
            auto request = makeRequest(ZYDIS_MNEMONIC_MOV);
            addMemory(request, base, displacement, size);
            addImmediateUnsigned(request, value);
            return emit(request, expectedLength);
        }

        auto ret(std::size_t expectedLength = kRetLength) -> Result<std::size_t> {
            return emit(makeRequest(ZYDIS_MNEMONIC_RET), expectedLength);
        }

        // ---------------- 状态 ----------------

        [[nodiscard]] auto size() const noexcept -> std::size_t { return Offset; }
        [[nodiscard]] auto capacity() const noexcept -> std::size_t { return Out.size(); }
        [[nodiscard]] auto remaining() const noexcept -> std::size_t { return Out.size() - Offset; }
        [[nodiscard]] auto data() const noexcept -> std::uint8_t * { return Out.data(); }

      private:
        auto append(const void *bytes, std::size_t count) -> Result<std::size_t> {
            if (count > remaining()) {
                return Error{ErrorCode::CodeGenBufferFull};
            }
            memcpy(Out.data() + Offset, bytes, count);
            Offset += count;
            return count;
        }

        auto padWithNops(std::size_t count) -> Result<void> {
            for (std::size_t i = 0; i < count; ++i) {
                auto emitted = emit(makeRequest(ZYDIS_MNEMONIC_NOP), 1);
                if (!emitted) {
                    return Error{emitted.error()};
                }
            }
            return {};
        }

        static auto makeRequest(ZydisMnemonic mnemonic) noexcept -> ZydisEncoderRequest {
            ZydisEncoderRequest request{};
            request.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
            request.mnemonic = mnemonic;
            return request;
        }

        static auto addRegister(ZydisEncoderRequest &request, ZydisRegister reg) noexcept -> void {
            auto &operand = request.operands[request.operand_count++];
            operand.type = ZYDIS_OPERAND_TYPE_REGISTER;
            operand.reg.value = reg;
        }

        static auto addMemory(ZydisEncoderRequest &request, ZydisRegister base, std::int32_t displacement,
                              std::uint16_t size) noexcept -> void {
            auto &operand = request.operands[request.operand_count++];
            operand.type = ZYDIS_OPERAND_TYPE_MEMORY;
            operand.mem.base = base;
            operand.mem.displacement = displacement;
            operand.mem.size = size;
        }

        static auto addImmediateUnsigned(ZydisEncoderRequest &request, std::uint64_t value) noexcept -> void {
            auto &operand = request.operands[request.operand_count++];
            operand.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            operand.imm.u = value;
        }

        static auto addImmediateSigned(ZydisEncoderRequest &request, std::int64_t value) noexcept -> void {
            auto &operand = request.operands[request.operand_count++];
            operand.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            operand.imm.s = value;
        }

        Span<std::uint8_t> Out;
        std::size_t Offset = 0;
    };

    // 两种跳转的长度是补丁机制的硬约定，所以在这里定死。
    // 绝对跳转 = push imm32 + mov [rsp+4], imm32 + ret，不碰任何寄存器。
    inline constexpr std::size_t kAbsoluteJumpSize =
        CodeGen::kPushImm32Length + CodeGen::kMovMemImm32Length + CodeGen::kRetLength;
    inline constexpr std::size_t kRel32JumpSize = CodeGen::kJmpRel32Length;

    // push imm32(低 32 位)
    // mov  dword ptr [rsp+4], imm32(高 32 位)
    // ret
    inline auto makeAbsoluteJumpAt(Span<std::uint8_t> payload, std::uintptr_t dest) -> Result<void, ErrorCode> {
        if (payload.size() != kAbsoluteJumpSize) {
            return Error{ErrorCode::UnsupportedPatchSize};
        }

        CodeGen gen(payload);
        REZERO_CHECK(gen.pushImm32(static_cast<std::int32_t>(dest & 0xFFFFFFFF)));
        REZERO_CHECK(gen.movMemImm(ZYDIS_REGISTER_RSP, 4, 4, static_cast<std::uint32_t>(dest >> 32)));
        REZERO_CHECK(gen.ret());

        return {};
    }

    // 假设当前指令的 RIP = 0x00007FF612341000
    // 目标地址           = 0x00007FF612341500
    // 则 disp = 目标 - (当前RIP + 指令长度) = 0x500 - 5 = 0x4FB
    //
    // 位移必须装得进 int32：装不下就明确报错，不能静默截断成野地址。
    inline auto makeRel32JumpAt(Span<std::uint8_t> payload, std::intptr_t src, std::intptr_t dest)
        -> Result<void, ErrorCode> {
        if (payload.size() != kRel32JumpSize) {
            return Error{ErrorCode::UnsupportedPatchSize};
        }

        const std::intptr_t displacement = dest - (src + static_cast<std::intptr_t>(kRel32JumpSize));
        if (displacement < INT32_MIN || displacement > INT32_MAX) {
            return Error{ErrorCode::DisasmRelocationOverflow};
        }

        CodeGen gen(payload);
        REZERO_CHECK(gen.jmpRel32(static_cast<std::int32_t>(displacement)));

        return {};
    }

} // namespace ReZeroDetour
