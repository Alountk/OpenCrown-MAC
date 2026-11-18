#include "lift.h"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace dcp::recomp {
namespace {

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

const char* ut(int bits) {
    switch (bits) {
    case 8: return "u8";
    case 16: return "u16";
    case 32: return "u32";
    default: return "u64";
    }
}
const char* st_(int bits) {
    switch (bits) {
    case 8: return "i8";
    case 16: return "i16";
    case 32: return "i32";
    default: return "i64";
    }
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

struct Lifter {
    const Function& fn;
    const std::set<uint64_t>& functions;
    LiftResult res;
    std::string body;
    bool unsupported_here = false;

    Lifter(const Function& f, const std::set<uint64_t>& fs) : fn(f), functions(fs) {}

    void emit(const std::string& s) { body += "  " + s + "\n"; }
    void unsupported(const Insn& in, const char* why = nullptr) {
        std::string k = in.mnemonic;
        if (why) k += std::string(" <") + why + ">";
        ++res.unsupported[k];
        emit(fmt("DCP_UNSUPPORTED(0x%llx);  // %s", (unsigned long long)in.addr, in.text.c_str()));
        unsupported_here = true;
    }

    // ---- operands ----------------------------------------------------------------------------------------------

    static bool gpr(const Operand& o) { return o.kind == OpKind::Reg && o.reg != kNoReg; }

    std::string addr(const Operand& m) const {
        if (m.base == kRip) return fmt("(dcp_img + 0x%llxull)", (unsigned long long)m.rip_target);
        std::string e;
        auto add = [&](const std::string& t) { e += e.empty() ? t : " + " + t; };
        if (m.base != kNoReg) add(fmt("r[%d]", m.base));
        if (m.index != kNoReg) add(m.scale == 1 ? fmt("r[%d]", m.index) : fmt("r[%d] * %d", m.index, m.scale));
        if (m.disp || e.empty()) add(fmt("(u64)%lldll", (long long)m.disp));
        return "(u64)(" + e + ")";
    }

    // Value of operand `o` as an unsigned expression of `bits` width (zero- or sign-extended from its own width).
    std::string read(const Operand& o, int bits, bool sext = false) const {
        std::string v;
        int own = o.bits;
        switch (o.kind) {
        case OpKind::Reg:
            v = o.high8 ? fmt("rdh(r[%d])", o.reg)
                        : own == 64 ? fmt("r[%d]", o.reg) : fmt("rd<%s>(r[%d])", ut(own), o.reg);
            break;
        case OpKind::Mem: v = fmt("ld<%s>(%s)", ut(own), addr(o).c_str()); break;
        case OpKind::Imm: {
            // Immediates are sign-extended to 64 bits by the decoder; truncate to the width the instruction uses.
            return fmt("(%s)0x%llxull", ut(bits), (unsigned long long)o.imm);
        }
        default: return "0";
        }
        if (own == bits) return v;
        if (sext) return fmt("(%s)(%s)(%s)", ut(bits), st_(own), v.c_str());
        return fmt("(%s)(%s)", ut(bits), v.c_str());
    }
    std::string read(const Operand& o) const { return read(o, o.bits); }

    std::string write(const Operand& o, const std::string& v) const {
        const int n = o.bits;
        if (o.kind == OpKind::Reg) {
            if (o.high8) return fmt("wrh(r[%d], (u8)(%s));", o.reg, v.c_str());
            return fmt("wr<%s>(r[%d], (%s)(%s));", ut(n), o.reg, ut(n), v.c_str());
        }
        return fmt("st<%s>(%s, (%s)(%s));", ut(n), addr(o).c_str(), ut(n), v.c_str());
    }

    bool supported_operands(const Insn& in) {
        for (int i = 0; i < in.nops; ++i) {
            const Operand& o = in.ops[i];
            if (o.kind == OpKind::Reg && o.reg == kNoReg && o.vec == kNoReg) return unsupported(in, "non-GPR operand"), false;
            if (o.kind == OpKind::None) return unsupported(in, "operand"), false;
        }
        return true;
    }

    static std::string cc_of(const std::string& mn, size_t prefix) { return "cc_" + mn.substr(prefix); }
    static bool is_cc(const std::string& s) {
        static const char* k[] = {"z", "nz", "b", "nb", "be", "nbe", "l", "nl", "le", "nle", "s", "ns", "p", "np", "o", "no"};
        for (const char* c : k) if (s == c) return true;
        return false;
    }

    std::string label(uint64_t a) const { return fmt("L_%llx", (unsigned long long)a); }

    // ---- control flow ------------------------------------------------------------------------------------------

    void call_to(const Insn& in) {
        const uint64_t next = in.addr + in.len;
        emit(fmt("r[RSP] -= 8; st<u64>(r[RSP], dcp_img + 0x%llxull);", (unsigned long long)next));
        emit("DCP_SAVE();");
        if (in.direct && functions.count(in.target)) emit(function_name(in.target) + "(c);");
        else if (in.direct) emit(fmt("call_va(c, 0x%llxull);", (unsigned long long)in.target));
        else emit(fmt("call_ind(c, %s);", read(in.ops[0], 64).c_str()));
        emit("DCP_LOAD();");
    }

    void tail_to(const Insn& in) {
        emit("DCP_SAVE();");
        if (in.direct && functions.count(in.target)) emit(function_name(in.target) + "(c);");
        else if (in.direct) emit(fmt("jmp_ind(c, dcp_img + 0x%llxull);", (unsigned long long)in.target));
        else emit(fmt("jmp_ind(c, %s);", read(in.ops[0], 64).c_str()));
        emit("return;");
    }

    // ---- vector instructions (VEX only: legacy SSE leaves the upper bits alone, which this port never needs) -----

    static bool is_vec(const Operand& o) { return o.kind == OpKind::Reg && o.vec != kNoReg; }
    std::string vread(const Operand& o) const {
        return is_vec(o) ? fmt("x[%d]", o.vec) : fmt("vld<%d>(%s)", o.bits, addr(o).c_str());
    }
    void vset(const Operand& d, const std::string& v) { emit(fmt("x[%d] = vcut<%d>(%s);", d.vec, d.bits, v.c_str())); }

    void vector_insn(const Insn& in) {
        const std::string& mn = in.mnemonic;
        const Operand& d = in.ops[0];
        const Operand& s = in.ops[1];
        if (!starts_with(mn, "v")) return unsupported(in, "legacy SSE");

        if (mn == "vmovups" || mn == "vmovaps" || mn == "vmovupd" || mn == "vmovapd" || mn == "vmovdqa" || mn == "vmovdqu" ||
            mn == "vlddqu") {
            if (is_vec(d)) return vset(d, vread(s));
            return emit(fmt("vst<%d>(%s, x[%d]);", d.bits, addr(d).c_str(), s.vec));
        }
        if (mn == "vmovss" || mn == "vmovsd") {
            const char* T = mn == "vmovss" ? "u32" : "u64";
            if (in.nops == 3) return vset(d, fmt("vins<%s>(x[%d], vlo<%s>(x[%d]))", T, s.vec, T, in.ops[2].vec));
            if (is_vec(d)) return vset(d, fmt("vscalar<%s>(ld<%s>(%s))", T, T, addr(s).c_str()));
            return emit(fmt("st<%s>(%s, vlo<%s>(x[%d]));", T, addr(d).c_str(), T, s.vec));
        }
        if (mn == "vmovd" || mn == "vmovq") {
            const char* T = mn == "vmovd" ? "u32" : "u64";
            if (is_vec(d)) {
                if (is_vec(s)) return vset(d, fmt("vscalar<u64>(vlo<u64>(x[%d]))", s.vec));
                return vset(d, fmt("vscalar<%s>(%s)", T, s.kind == OpKind::Mem ? fmt("ld<%s>(%s)", T, addr(s).c_str()).c_str()
                                                                                 : read(s, mn == "vmovd" ? 32 : 64).c_str()));
            }
            if (d.kind == OpKind::Mem) return emit(fmt("st<%s>(%s, vlo<%s>(x[%d]));", T, addr(d).c_str(), T, s.vec));
            return emit(write(d, fmt("vlo<%s>(x[%d])", T, s.vec)));
        }

        struct Bit { const char* mn; const char* fn; };
        static const Bit bits[] = {{"vxorps", "vxor"}, {"vxorpd", "vxor"}, {"vpxor", "vxor"},  {"vandps", "vand"},
                                   {"vandpd", "vand"}, {"vpand", "vand"},  {"vandnps", "vandn"}, {"vandnpd", "vandn"},
                                   {"vpandn", "vandn"}, {"vorps", "vor"},   {"vorpd", "vor"},     {"vpor", "vor"}};
        for (const Bit& b : bits)
            if (mn == b.mn && in.nops == 3)
                return vset(d, fmt("%s(%s, %s)", b.fn, vread(s).c_str(), vread(in.ops[2]).c_str()));


        // scalar float: ss = float / u32, sd = double / u64
        if (mn.size() > 3 && (mn.compare(mn.size() - 2, 2, "ss") == 0 || mn.compare(mn.size() - 2, 2, "sd") == 0)) {
            const bool dbl = mn.back() == 'd';
            const char* F = dbl ? "double" : "float";
            const char* U = dbl ? "u64" : "u32";
            const std::string base = mn.substr(1, mn.size() - 3);  // "vaddss" -> "add"
            auto elem = [&](const Operand& o, const char* u) {
                return is_vec(o) ? fmt("vlo<%s>(x[%d])", u, o.vec) : fmt("ld<%s>(%s)", u, addr(o).c_str());
            };
            static const char* arith[] = {"add", "sub", "mul", "div", "min", "max"};
            for (const char* a : arith)
                if (base == a && in.nops == 3 && is_vec(s))
                    return vset(d, fmt("vins<%s>(x[%d], f%s<%s>(vlo<%s>(x[%d]), %s))", U, s.vec, a, F, U, s.vec,
                                       elem(in.ops[2], U).c_str()));
            if (base == "sqrt" && in.nops == 3)
                return vset(d, fmt("vins<%s>(x[%d], fsqrt<%s>(%s))", U, s.vec, F, elem(in.ops[2], U).c_str()));
            if ((base == "ucomi" || base == "comi") && in.nops == 2)
                return emit(fmt("fucomi<%s>(vlo<%s>(x[%d]), %s, fl);", F, U, d.vec, elem(s, U).c_str()));
            if (base == "cmp" && in.nops == 4)
                return vset(d, fmt("vins<%s>(x[%d], fcmp<%s>(vlo<%s>(x[%d]), %s, %uu))", U, s.vec, F, U, s.vec,
                                   elem(in.ops[2], U).c_str(), unsigned(in.ops[3].imm) & 15u));
            if (base == "cvtsi2" && in.nops == 3) {
                const char* I = in.ops[2].bits == 64 ? "i64" : "i32";
                return vset(d, fmt("vins<%s>(x[%d], fcvti<%s, %s>((%s)%s))", U, s.vec, F, I, I, read(in.ops[2]).c_str()));
            }
        }
        if ((mn == "vcvttss2si" || mn == "vcvttsd2si") && in.nops == 2) {
            const bool dbl = mn == "vcvttsd2si";
            const char* I = d.bits == 64 ? "i64" : "i32";
            return emit(write(d, fmt("fcvtt<%s, %s>(%s)", dbl ? "double" : "float", I,
                                     is_vec(s) ? fmt("vlo<%s>(x[%d])", dbl ? "u64" : "u32", s.vec).c_str()
                                               : fmt("ld<%s>(%s)", dbl ? "u64" : "u32", addr(s).c_str()).c_str())));
        }
        if (mn == "vcvtss2sd" && in.nops == 3)
            return vset(d, fmt("vins<u64>(x[%d], fcvt_f2d(%s))", s.vec,
                               is_vec(in.ops[2]) ? fmt("vlo<u32>(x[%d])", in.ops[2].vec).c_str()
                                                 : fmt("ld<u32>(%s)", addr(in.ops[2]).c_str()).c_str()));
        if (mn == "vcvtsd2ss" && in.nops == 3)
            return vset(d, fmt("vins<u32>(x[%d], fcvt_d2f(%s))", s.vec,
                               is_vec(in.ops[2]) ? fmt("vlo<u64>(x[%d])", in.ops[2].vec).c_str()
                                                 : fmt("ld<u64>(%s)", addr(in.ops[2]).c_str()).c_str()));

        unsupported(in);
    }

    // ---- one instruction ---------------------------------------------------------------------------------------

    void insn(const Insn& in) {
        const std::string& mn = in.mnemonic;
        ++res.insns;
        body += fmt("  // 0x%llx: %s\n", (unsigned long long)in.addr, in.text.c_str());

        if (in.flow == Flow::Ret) {
            long long extra = in.nops && in.ops[0].kind == OpKind::Imm ? (long long)in.ops[0].imm : 0;
            emit(fmt("r[RSP] += %lld; DCP_SAVE(); return;", 8 + extra));
            return;
        }
        if (in.flow == Flow::Trap) return emit("__builtin_trap();");
        if (in.flow == Flow::Call) return call_to(in);
        if (in.flow == Flow::Jump) {
            if (in.direct && fn.blocks.count(in.target)) return emit("goto " + label(in.target) + ";");
            return tail_to(in);
        }
        if (in.flow == Flow::CondJump) {
            if (!in.direct) return unsupported(in, "indirect");
            const std::string c = cc_of(mn, 1);
            if (!is_cc(c.substr(3))) return unsupported(in);
            if (fn.blocks.count(in.target)) return emit(fmt("if (%s(fl)) goto %s;", c.c_str(), label(in.target).c_str()));
            emit(fmt("if (%s(fl)) {", c.c_str()));
            body += "  ";
            tail_to(in);
            return emit("}");
        }

        if (mn == "nop") return;
        if (!supported_operands(in)) return;
        for (int i = 0; i < in.nops; ++i)
            if (is_vec(in.ops[i])) return vector_insn(in);
        if (mn == "vcvttss2si" || mn == "vcvttsd2si") return vector_insn(in);  // GPR destination, memory source
        const Operand& d = in.ops[0];
        const Operand& s = in.ops[1];
        const int N = d.bits;

        if (mn == "mov") return emit(write(d, read(s, N)));
        if (mn == "lea") return emit(write(d, addr(s)));
        if (mn == "movzx") return emit(write(d, read(s, N)));
        if (mn == "movsx" || mn == "movsxd") return emit(write(d, read(s, N, true)));
        if (mn == "movbe") {
            const char* t = ut(N);
            return emit(write(d, fmt("bswap<%s>(%s)", t, read(s, N).c_str())));
        }
        if (mn == "push") {
            emit(fmt("{ u64 v = %s; r[RSP] -= 8; st<u64>(r[RSP], v); }", read(d, 64, true).c_str()));
            return;
        }
        if (mn == "pop") {
            emit("{ u64 v = ld<u64>(r[RSP]); r[RSP] += 8; " + write(d, "v") + " }");
            return;
        }
        if (mn == "xchg") {
            emit(fmt("{ %s a = %s, b = %s; %s %s }", ut(N), read(d).c_str(), read(s, N).c_str(), write(d, "b").c_str(),
                     write(s, "a").c_str()));
            return;
        }
        if (mn == "bswap") return emit(write(d, fmt("bswap<%s>(%s)", ut(N), read(d).c_str())));

        // two-operand ALU: dst = op(dst, src)
        struct Alu { const char* mn; const char* fn; bool writes; };
        static const Alu alu[] = {{"add", "add", true}, {"adc", "adc", true}, {"sub", "sub", true}, {"sbb", "sbb", true},
                                  {"cmp", "sub", false}};
        for (const Alu& a : alu) {
            if (mn != a.mn) continue;
            std::string e = fmt("%s<%s>(%s, %s, fl)", a.fn, ut(N), read(d).c_str(), read(s, N, true).c_str());
            return emit(a.writes ? write(d, e) : "(void)" + e + ";");
        }
        if (mn == "and" || mn == "or" || mn == "xor" || mn == "test") {
            const char* op = mn == "and" || mn == "test" ? "&" : mn == "or" ? "|" : "^";
            std::string e = fmt("logic<%s>((%s)(%s %s %s), fl)", ut(N), ut(N), read(d).c_str(), op, read(s, N, true).c_str());
            return emit(mn == "test" ? "(void)" + e + ";" : write(d, e));
        }
        if (mn == "inc" || mn == "dec" || mn == "neg")
            return emit(write(d, fmt("%s<%s>(%s, fl)", mn.c_str(), ut(N), read(d).c_str())));
        if (mn == "not") return emit(write(d, fmt("(%s)~%s", ut(N), read(d).c_str())));

        if (mn == "shl" || mn == "shr" || mn == "sar" || mn == "rol" || mn == "ror") {
            const std::string cnt = in.nops > 1 ? read(s, 8) : "(u8)1";
            return emit(write(d, fmt("%s<%s>(%s, %s, fl)", mn.c_str(), ut(N), read(d).c_str(), cnt.c_str())));
        }

        if (mn == "imul") {
            if (in.nops == 2) return emit(write(d, fmt("imul<%s>(%s, %s, fl)", ut(N), read(d).c_str(), read(s, N, true).c_str())));
            if (in.nops == 3)
                return emit(write(d, fmt("imul<%s>(%s, %s, fl)", ut(N), read(s, N).c_str(), read(in.ops[2], N, true).c_str())));
        }
        if ((mn == "mul" || mn == "imul") && in.nops == 1) {  // one-operand: rdx:rax = rax * src
            const std::string f = mn + "_wide";
            if (N == 8)
                return emit(fmt("{ u8 lo; u8 hi = %s<u8>(rd<u8>(r[RAX]), %s, &lo, fl); wr<u16>(r[RAX], (u16)(hi << 8 | lo)); }",
                                f.c_str(), read(d).c_str()));
            return emit(fmt("{ %s lo; %s hi = %s<%s>(rd<%s>(r[RAX]), %s, &lo, fl); wr<%s>(r[RAX], lo); wr<%s>(r[RDX], hi); }",
                            ut(N), ut(N), f.c_str(), ut(N), ut(N), read(d).c_str(), ut(N), ut(N)));
        }
        if (mn == "div" || mn == "idiv") {
            const std::string f = mn + "_wide";
            if (N == 8)
                return emit(fmt("{ u8 q, m; %s<u8>(rdh(r[RAX]), rd<u8>(r[RAX]), %s, &q, &m); wr<u8>(r[RAX], q); wrh(r[RAX], m); }",
                                f.c_str(), read(d).c_str()));
            return emit(fmt("{ %s q, m; %s<%s>(rd<%s>(r[RDX]), rd<%s>(r[RAX]), %s, &q, &m); wr<%s>(r[RAX], q); wr<%s>(r[RDX], m); }",
                            ut(N), f.c_str(), ut(N), ut(N), ut(N), read(d).c_str(), ut(N), ut(N)));
        }
        if (mn == "cdqe") return emit("r[RAX] = (u64)(i64)(i32)r[RAX];");
        if (mn == "cwde") return emit("wr<u32>(r[RAX], (u32)(i32)(i16)r[RAX]);");
        if (mn == "cdq") return emit("wr<u32>(r[RDX], (i32)r[RAX] < 0 ? 0xffffffffu : 0u);");
        if (mn == "cqo") return emit("r[RDX] = (i64)r[RAX] < 0 ? ~0ull : 0ull;");

        if (mn == "bt") {
            if (d.kind == OpKind::Mem && s.kind == OpKind::Reg) return unsupported(in, "mem, reg");
            return emit(fmt("bt<%s>(%s, %s, fl);", ut(N), read(d).c_str(), read(s, 8).c_str()));
        }
        if (mn == "bsf" || mn == "bsr")
            return emit(write(d, fmt("%s<%s>(%s, %s, fl)", mn.c_str(), ut(N), read(s, N).c_str(), read(d).c_str())));
        if (mn == "lzcnt" || mn == "tzcnt" || mn == "popcnt")
            return emit(write(d, fmt("%s<%s>(%s, fl)", mn.c_str(), ut(N), read(s, N).c_str())));
        if (mn == "andn") return emit(write(d, fmt("andn<%s>(%s, %s, fl)", ut(N), read(s, N).c_str(), read(in.ops[2], N).c_str())));
        if (mn == "bextr") return emit(write(d, fmt("bextr<%s>(%s, %s, fl)", ut(N), read(s, N).c_str(), read(in.ops[2], N).c_str())));

        if (mn.size() > 3 && starts_with(mn, "set") && is_cc(mn.substr(3)))
            return emit(write(d, fmt("%s(fl) ? 1 : 0", cc_of(mn, 3).c_str())));
        if (mn.size() > 4 && starts_with(mn, "cmov") && is_cc(mn.substr(4)))
            return emit(write(d, fmt("%s(fl) ? %s : %s", cc_of(mn, 4).c_str(), read(s, N).c_str(), read(d).c_str())));
        if (mn == "sahf") return emit("{ u8 a = rdh(r[RAX]); fl.cf = a & 1; fl.pf = a >> 2 & 1; fl.af = a >> 4 & 1; fl.zf = a >> 6 & 1; fl.sf = a >> 7; }");
        if (mn == "lahf") return emit("wrh(r[RAX], (u8)pack_flags(fl));");

        unsupported(in);
    }

    LiftResult run() {
        // Labels only for blocks that are the target of a direct jump inside this function.
        std::set<uint64_t> targets;
        for (const auto& [a, b] : fn.blocks)
            for (const Insn& in : b.insns)
                if ((in.flow == Flow::Jump || in.flow == Flow::CondJump) && in.direct && fn.blocks.count(in.target))
                    targets.insert(in.target);

        for (const auto& [a, b] : fn.blocks) {
            if (targets.count(a)) body += label(a) + ":;\n";
            for (const Insn& in : b.insns) insn(in);
        }
        if (fn.fell_off_end) body += "  __builtin_trap();  // control reaches the end of the function: a noreturn call\n";

        res.code = fmt("extern \"C\" void %s(Ctx* c) {\n", function_name(fn.start).c_str());
        res.code += "  u64 r[16];\n  Vec x[16];\n  Flags fl;\n  DCP_LOAD();\n";
        res.code += body + "}\n";
        return std::move(res);
    }
};

}  // namespace

std::string function_name(uint64_t start) { return fmt("f_%llx", (unsigned long long)start); }

std::string lift_prelude() {
    return R"(// Generated by dcp_recomp. Do not edit. Derived from the user's own game files: never commit or distribute.
#include "dcp_rt.h"
using namespace dcp::rt;
#ifdef DCP_TEST_FLAGS
#define DCP_LOAD() do { std::memcpy(r, c->r, sizeof r); std::memcpy(x, c->x, sizeof x); fl = unpack_flags(c->rflags); } while (0)
#define DCP_SAVE() do { std::memcpy(c->r, r, sizeof r); std::memcpy(c->x, x, sizeof x); c->rflags = pack_flags(fl); } while (0)
#else
#define DCP_LOAD() do { std::memcpy(r, c->r, sizeof r); std::memcpy(x, c->x, sizeof x); } while (0)
#define DCP_SAVE() do { std::memcpy(c->r, r, sizeof r); std::memcpy(c->x, x, sizeof x); } while (0)
#endif
#define DCP_UNSUPPORTED(va) __builtin_trap()
)";
}

LiftResult lift_function(const Function& fn, const std::set<uint64_t>& functions) {
    Lifter l(fn, functions);
    return l.run();
}

}  // namespace dcp::recomp
