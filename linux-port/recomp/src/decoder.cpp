#include "decoder.h"

#include <Zydis/Zydis.h>

namespace dcp::recomp {
namespace {

const ZydisDecoder& decoder() {
    static const ZydisDecoder d = [] {
        ZydisDecoder x;
        ZydisDecoderInit(&x, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        return x;
    }();
    return d;
}

const ZydisFormatter& formatter() {
    static const ZydisFormatter f = [] {
        ZydisFormatter x;
        ZydisFormatterInit(&x, ZYDIS_FORMATTER_STYLE_INTEL);
        return x;
    }();
    return f;
}

Flow classify(const ZydisDecodedInstruction& i) {
    switch (i.meta.category) {
    case ZYDIS_CATEGORY_CALL: return Flow::Call;
    case ZYDIS_CATEGORY_UNCOND_BR: return Flow::Jump;
    case ZYDIS_CATEGORY_COND_BR: return Flow::CondJump;
    case ZYDIS_CATEGORY_RET: return Flow::Ret;
    default: break;
    }
    // ud2 is how the compiler marks unreachable code after a noreturn call; int3 is a breakpoint. Neither falls through.
    if (i.mnemonic == ZYDIS_MNEMONIC_UD2 || i.mnemonic == ZYDIS_MNEMONIC_INT3) return Flow::Trap;
    return Flow::Normal;
}

int8_t gpr_index(ZydisRegister r, bool& high8) {
    high8 = r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_CH || r == ZYDIS_REGISTER_DH || r == ZYDIS_REGISTER_BH;
    const ZydisRegisterClass c = ZydisRegisterGetClass(r);
    if (c != ZYDIS_REGCLASS_GPR8 && c != ZYDIS_REGCLASS_GPR16 && c != ZYDIS_REGCLASS_GPR32 && c != ZYDIS_REGCLASS_GPR64)
        return kNoReg;
    if (high8) return int8_t(r - ZYDIS_REGISTER_AH + 0);  // AH=0 (rax), CH=1 (rcx), DH=2 (rdx), BH=3 (rbx) in this order
    return int8_t(ZydisRegisterGetId(r));
}

}  // namespace

bool decode(const uint8_t* code, size_t avail, uint64_t addr, Insn& out) {
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder(), code, avail, &ins, ops))) return false;
    out = Insn{};
    out.addr = addr;
    out.len = ins.length;
    out.flow = classify(ins);
    out.mnemonic = ZydisMnemonicGetString(ins.mnemonic);
    if (out.flow == Flow::Call || out.flow == Flow::Jump || out.flow == Flow::CondJump) {
        const ZydisDecodedOperand& o = ops[0];
        if (o.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && o.imm.is_relative) {
            ZyanU64 t = 0;
            if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, &o, addr, &t))) out.direct = true, out.target = t;
        }
    }
    for (int i = 0; i < ins.operand_count_visible && i < 4; ++i) {
        const ZydisDecodedOperand& o = ops[i];
        Operand& d = out.ops[out.nops++];
        d.bits = o.size;
        if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
            d.kind = OpKind::Reg;
            d.reg = gpr_index(o.reg.value, d.high8);
        } else if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
            d.kind = OpKind::Mem;
            bool h = false;
            if (o.mem.base == ZYDIS_REGISTER_RIP) {
                d.base = kRip;
                ZyanU64 t = 0;
                if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, &o, addr, &t))) d.rip_target = t;
            } else if (o.mem.base != ZYDIS_REGISTER_NONE) {
                d.base = gpr_index(o.mem.base, h);
            }
            if (o.mem.index != ZYDIS_REGISTER_NONE) d.index = gpr_index(o.mem.index, h);
            d.scale = o.mem.scale ? o.mem.scale : 1;
            if (o.mem.disp.has_displacement) d.disp = o.mem.disp.value;
        } else if (o.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            d.kind = OpKind::Imm;
            d.imm = o.imm.is_signed ? int64_t(o.imm.value.s) : int64_t(o.imm.value.u);
        }
    }
    out.lock = ins.attributes & ZYDIS_ATTRIB_HAS_LOCK;
    out.rep = ins.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE);
    char buf[128];
    if (ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter(), &ins, ops, ins.operand_count_visible, buf,
                                                     sizeof buf, addr, nullptr)))
        out.text = buf;
    return true;
}

}  // namespace dcp::recomp
