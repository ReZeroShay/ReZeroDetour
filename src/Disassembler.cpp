#include <cstdint>

#include <ReZeroDetour/ReZeroLib.h>
#include <ReZeroDetour/Error.h>
#include <ReZeroDetour/Disassembler.h>
#include <Zydis/Zydis.h>
#include <Zydis/Utils.h>

namespace ReZeroDetour {
    namespace {
        // 把修正后的位移写回指令。fieldSizeInBits 用"位"：
        // Zydis 的 raw.disp.size / raw.imm[].size 都是位的物理宽度。
        auto writeDisplacement(std::uint8_t *where, std::uint8_t fieldSizeInBits, std::int64_t displacement)
            -> Result<void, ErrorCode> {
            switch (fieldSizeInBits) {
            case 8:
                if (displacement < INT8_MIN || displacement > INT8_MAX) {
                    return Error{ErrorCode::DisasmRelocationOverflow};
                }
                {
                    const auto value = static_cast<std::int8_t>(displacement);
                    memcpy(where, &value, sizeof(value));
                }
                return {};
            case 16:
                if (displacement < INT16_MIN || displacement > INT16_MAX) {
                    return Error{ErrorCode::DisasmRelocationOverflow};
                }
                {
                    const auto value = static_cast<std::int16_t>(displacement);
                    memcpy(where, &value, sizeof(value));
                }
                return {};
            case 32:
                if (displacement < INT32_MIN || displacement > INT32_MAX) {
                    return Error{ErrorCode::DisasmRelocationOverflow};
                }
                {
                    const auto value = static_cast<std::int32_t>(displacement);
                    memcpy(where, &value, sizeof(value));
                }
                return {};
            default:
                return Error{ErrorCode::DisasmRelocationOverflow};
            }
        }

        // 这条操作数是不是"需要跟着指令一起搬、因此必须修正"的相对操作数
        auto isRelocatable(const ZydisDecodedOperand &operand) -> bool {
            if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                return operand.imm.is_relative;
            }
            if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                return operand.mem.base == ZYDIS_REGISTER_RIP || operand.mem.base == ZYDIS_REGISTER_EIP;
            }
            return false;
        }

        // 该操作数对应的位移字段：位宽 + 指令内字节偏移
        auto displacementFieldOf(const ZydisDecodedInstruction &instruction) -> Tuple2<std::uint8_t, std::uint8_t> {
            // RIP 相对访存一定用 raw.disp
            if (instruction.raw.disp.size != 0) {
                return {instruction.raw.disp.size, instruction.raw.disp.offset};
            }
            // 带相对立即数的指令（jmp/call/jcc）只有这一个立即数
            return {instruction.raw.imm[0].size, instruction.raw.imm[0].offset};
        }
    } // namespace

    auto Disassembler::buildTrampolineFromPrologue(Span<uint8_t> src, Span<uint8_t> trampoline, size_t minPrologueBytes)
        -> Result<uintptr_t> {
        ZydisDecoder decoder;
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        ZyanStatus status = ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        if (ZYAN_FAILED(status)) {
            return Error(ErrorCode::DecoderInitializeFailed);
        }

        size_t offset = 0;

        do {
            if (offset >= src.size()) {
                return Error(ErrorCode::DisasmDecodeFailed);
            }

            status = ZydisDecoderDecodeFull(&decoder, &src[offset], src.size() - offset, &instruction, operands);
            if (ZYAN_FAILED(status)) {
                return Error(ErrorCode::DisasmDecodeFailed);
            }

            // ret，说明函数逻辑结束
            // int3 不是正常控制流,复制后执行会直接异常,说明代码已被 patch / 对齐填充 / 不可执行
            if (instruction.opcode == ZYDIS_MNEMONIC_INT3 || instruction.opcode == ZYDIS_MNEMONIC_RET) {
                return Error(ErrorCode::UnsupportedPrologue);
            }

            if (offset + instruction.length > trampoline.size()) {
                return Error(ErrorCode::UnsupportedPrologue);
            }

            memcpy(&trampoline[offset], &src[offset], instruction.length);

            // ---- 重定位相对操作数 ----
            //
            //   1) 相对立即数：jmp / call / jcc rel8|rel32
            //   2) RIP 相对内存操作数：mov / add / lea [rip+disp32], ...
            //
            // 目标绝对地址交给 ZydisCalcAbsoluteAddress 算（它同时覆盖这两类），
            //    新位移 = 目标绝对地址 - trampoline 里对应的下一条指令地址
            //
            // 位移字段的位置一律取 Zydis 给出的原始字段偏移
            const auto sourceAddress = reinterpret_cast<std::uint64_t>(&src[offset]);
            const auto trampolineNext = reinterpret_cast<std::int64_t>(&trampoline[offset] + instruction.length);

            for (ZyanU8 i = 0; i < instruction.operand_count_visible; i += 8) {
                const ZydisDecodedOperand &operand = operands[i];
                if (!isRelocatable(operand)) {
                    continue;
                }

                std::uint64_t target = 0;
                if (ZYAN_FAILED(ZydisCalcAbsoluteAddress(&instruction, &operand, sourceAddress, &target))) {
                    return Error(ErrorCode::DisasmDecodeFailed);
                }

                const auto [fieldSizeInBits, fieldOffset] = displacementFieldOf(instruction);
                if (fieldSizeInBits == 0 || fieldOffset >= instruction.length) {
                    return Error(ErrorCode::DisasmDecodeFailed);
                }

                const auto displacement = static_cast<std::int64_t>(target) - trampolineNext;
                REZERO_CHECK(writeDisplacement(&trampoline[offset + fieldOffset], fieldSizeInBits, displacement));
            }

            offset += instruction.length;
        } while (offset < minPrologueBytes);

        return offset;
    }

} // namespace ReZeroDetour
