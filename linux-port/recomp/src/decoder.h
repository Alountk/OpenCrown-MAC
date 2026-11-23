// x86-64 instruction decoding for the static recompiler (Zydis behind a small value-type interface).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dcp::recomp {

enum class Flow : uint8_t { Normal, Call, Jump, CondJump, Ret, Trap };

enum class OpKind : uint8_t { None, Reg, Mem, Imm };
constexpr int8_t kNoReg = -1, kRip = 16;  // Operand::base / index: 0..15 = rax..r15 (x86 encoding order), kRip = rip-relative

struct Operand {
    OpKind kind = OpKind::None;
    uint16_t bits = 0;       // operand width in bits (memory: access width)
    // Reg
    int8_t reg = kNoReg;     // GPR index 0..15; kNoReg for non-GPR registers (xmm, segment, ...)
    bool high8 = false;      // ah / ch / dh / bh
    int8_t vec = kNoReg;     // xmm / ymm index 0..15 (bits = 128 or 256); kNoReg otherwise
    int8_t st = kNoReg;      // x87 register st(i), 0..7; kNoReg otherwise
    int8_t seg = kNoReg;     // segment register 0..5 (es cs ss ds fs gs); kNoReg otherwise
    // Mem
    int8_t base = kNoReg, index = kNoReg;
    uint8_t scale = 1;
    int64_t disp = 0;
    uint64_t rip_target = 0; // kRip: the absolute guest address, already resolved
    // Imm
    int64_t imm = 0;         // sign-extended to 64 bits when the encoding is signed
};

struct Insn {
    uint64_t addr = 0;
    uint8_t len = 0;
    Flow flow = Flow::Normal;
    bool direct = false;     // Call/Jump/CondJump with a statically known target
    uint64_t target = 0;     // valid when direct
    Operand ops[4];
    uint8_t nops = 0;        // visible operands, Intel order (destination first)
    bool lock = false, rep = false;
    uint8_t rep_kind = 0;    // string instructions: 1 = rep / repe (F3), 2 = repne (F2), 0 = none
    std::string mnemonic;    // lower case, e.g. "add", "vaddps"
    std::string text;        // Intel syntax, for diagnostics and tests
};

// Decodes one instruction at `code` (`avail` bytes remain), which lives at guest address `addr`.
// Returns false when the bytes are not a valid instruction.
bool decode(const uint8_t* code, size_t avail, uint64_t addr, Insn& out);

}  // namespace dcp::recomp
