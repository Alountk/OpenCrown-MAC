#include "lift.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
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

static std::string patch_name(uint64_t site, const std::string& module) {
    return module.empty() ? fmt("dcp_patch_%llx", (unsigned long long)site) : fmt("dcp_patch_%s_%llx", module.c_str(), (unsigned long long)site);
}

static std::string hook_name(uint64_t start, const std::string& module) {
    return module.empty() ? fmt("dcp_hook_%llx", (unsigned long long)start) : fmt("dcp_hook_%s_%llx", module.c_str(), (unsigned long long)start);
}

static std::string site_name(uint64_t addr, const std::string& module) {
    return module.empty() ? fmt("dcp_site_%llx", (unsigned long long)addr) : fmt("dcp_site_%s_%llx", module.c_str(), (unsigned long long)addr);
}

struct Lifter {
    const Function& fn;
    const std::set<uint64_t>& functions;
    std::string module;
    bool comments = true;
    bool hooked = false;  // the function calls its EntryHook slot on entry
    const PatchMap* patches = nullptr;  // instructions with a runtime-switchable replacement
    const SiteMap* sites = nullptr;     // instructions preceded by a host hook
    std::string img;  // the C++ name of this module's image base
    LiftResult res;
    std::string body;
    bool unsupported_here = false;

    Lifter(const Function& f, const std::set<uint64_t>& fs, const std::string& m, bool c)
        : fn(f), functions(fs), module(m), comments(c), img(m.empty() ? "dcp_img" : "dcp_img_" + m) {}
    std::string fname(uint64_t a) const { return function_name(a, module); }

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
        if (m.base == kRip) return fmt("(%s + 0x%llxull)", img.c_str(), (unsigned long long)m.rip_target);
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
        const bool indirect = !in.direct;
        if (indirect) emit(fmt("{ u64 t_ = %s;", read(in.ops[0], 64).c_str()));  // the operand is read before the push: call [rsp+8]
        emit(fmt("r[RSP] -= 8; st<u64>(r[RSP], %s + 0x%llxull);", img.c_str(), (unsigned long long)next));
        emit("DCP_SAVE();");
        if (in.direct && functions.count(in.target)) emit(fname(in.target) + "(c);");
        else if (in.direct) emit(fmt("call_ind(c, %s + 0x%llxull);", img.c_str(), (unsigned long long)in.target));
        else emit("call_ind(c, t_);");
        emit("DCP_LOAD();");
        if (indirect) emit("}");
    }

    void tail_to(const Insn& in) {
        emit("DCP_SAVE();");
        if (in.direct && functions.count(in.target)) emit(fname(in.target) + "(c);");
        else if (in.direct) emit(fmt("jmp_ind(c, %s + 0x%llxull);", img.c_str(), (unsigned long long)in.target));
        else emit(fmt("jmp_ind(c, %s);", read(in.ops[0], 64).c_str()));
        emit("return;");
    }

    // ---- vector instructions (VEX only: legacy SSE leaves the upper bits alone, which this port never needs) -----

    static bool is_vec(const Operand& o) { return o.kind == OpKind::Reg && o.vec != kNoReg; }
    std::string vread(const Operand& o) const {
        return is_vec(o) ? fmt("x[%d]", o.vec) : fmt("vld<%d>(%s)", o.bits, addr(o).c_str());
    }
    void vset(const Operand& d, const std::string& v) { emit(fmt("x[%d] = vcut<%d>(%s);", d.vec, d.bits, v.c_str())); }

    // x87, modelled on the Fpu in the Ctx (doubles, not 80-bit extended). Every x87 mnemonic starts with 'f'.
    void x87_insn(const Insn& in) {
        const std::string& mn = in.mnemonic;
        const Operand& a = in.ops[0];
        const Operand& b = in.ops[1];
        const std::string f = "c->fpu";
        auto st = [&](int i) { return fmt("x87::reg(%s, %d)", f.c_str(), i); };
        auto ldm = [&](const Operand& m) -> std::string {  // a floating-point memory operand as a double
            if (m.bits == 32) return fmt("x87::ld32(%s)", addr(m).c_str());
            if (m.bits == 64) return fmt("x87::ld64(%s)", addr(m).c_str());
            return fmt("x87::from80(%s)", addr(m).c_str());
        };
        auto stm = [&](const Operand& m, const std::string& v) {
            if (m.bits == 32) emit(fmt("x87::st32(%s, %s);", addr(m).c_str(), v.c_str()));
            else if (m.bits == 64) emit(fmt("x87::st64(%s, %s);", addr(m).c_str(), v.c_str()));
            else emit(fmt("x87::to80(%s, %s);", addr(m).c_str(), v.c_str()));
        };
        auto is_st = [](const Operand& o) { return o.kind == OpKind::Reg && o.st != kNoReg; };
        auto pop = [&] { emit(fmt("(void)x87::pop(%s);", f.c_str())); };
        auto push = [&](const std::string& v) { emit(fmt("x87::push(%s, %s);", f.c_str(), v.c_str())); };

        if (mn == "fwait" || mn == "fnop" || mn == "fnclex" || mn == "wait") return;
        if (mn == "fninit") return emit(fmt("%s = Fpu();", f.c_str()));
        // constants
        static const struct { const char* mn; const char* v; } consts[] = {
            {"fld1", "1.0"}, {"fldz", "0.0"}, {"fldpi", "3.14159265358979323846"}, {"fldl2e", "1.44269504088896340736"},
            {"fldl2t", "3.32192809488736234787"}, {"fldlg2", "0.30102999566398119521"}, {"fldln2", "0.69314718055994530942"}};
        for (const auto& k : consts)
            if (mn == k.mn) return push(k.v);
        if (mn == "fld" && in.nops == 1) {
            if (is_st(a)) return emit(fmt("{ double v = %s; x87::push(%s, v); }", st(a.st).c_str(), f.c_str()));
            return push(ldm(a));
        }
        if (mn == "fild") return emit(fmt("x87::fild<i%d>(%s, %s);", a.bits, f.c_str(), addr(a).c_str()));
        if (mn == "fist" || mn == "fistp" || mn == "fisttp") {
            emit(fmt("x87::fist<i%d>(%s, %s, %s);", a.bits, f.c_str(), addr(a).c_str(), mn == "fisttp" ? "true" : "false"));
            if (mn != "fist") pop();
            return;
        }
        if (mn == "fst" || mn == "fstp") {
            if (is_st(a)) emit(fmt("%s = %s;", st(a.st).c_str(), st(0).c_str()));
            else stm(a, st(0));
            if (mn == "fstp") pop();
            return;
        }
        // arithmetic: st(dst) = st(dst) OP src, subr / divr with the operands swapped; the p forms pop afterwards
        static const struct { const char* n; const char* op; bool rev; } arith[] = {
            {"add", "+", false}, {"sub", "-", false}, {"subr", "-", true}, {"mul", "*", false}, {"div", "/", false}, {"divr", "/", true}};
        for (const auto& k : arith) {
            const std::string base = std::string("f") + k.n;
            const bool popf = mn == base + "p";
            if (mn != base && !popf) continue;
            std::string dst, src;
            if (in.nops == 1) dst = st(0), src = ldm(a);
            else dst = st(a.st), src = st(b.st);
            const std::string l = k.rev ? src : dst, r = k.rev ? dst : src;
            emit(fmt("{ double l_ = %s, r_ = %s; %s = l_ %s r_; }", l.c_str(), r.c_str(), dst.c_str(), k.op));
            if (popf) pop();
            return;
        }
        if (mn == "fchs") return emit(fmt("%s = -%s;", st(0).c_str(), st(0).c_str()));
        if (mn == "fabs") return emit(fmt("%s = std::fabs(%s);", st(0).c_str(), st(0).c_str()));
        if (mn == "fsqrt") return emit(fmt("%s = std::sqrt(%s);", st(0).c_str(), st(0).c_str()));
        if (mn == "frndint") return emit(fmt("%s = x87::round_cw(%s, %s);", st(0).c_str(), f.c_str(), st(0).c_str()));
        if (mn == "fprem1") return emit(fmt("%s = std::remainder(%s, %s); %s.c &= 0xfbff;", st(0).c_str(), st(0).c_str(), st(1).c_str(), f.c_str()));
        if (mn == "fprem") return emit(fmt("%s = std::fmod(%s, %s); %s.c &= 0xfbff;", st(0).c_str(), st(0).c_str(), st(1).c_str(), f.c_str()));
        if (mn == "fscale") return emit(fmt("x87::fscale(%s);", f.c_str()));
        if (mn == "fxch") return emit(fmt("{ double t_ = %s; %s = %s; %s = t_; }", st(0).c_str(), st(0).c_str(), st(in.nops ? a.st : 1).c_str(), st(in.nops ? a.st : 1).c_str()));
        if (mn == "fucomi" || mn == "fcomi" || mn == "fucomip" || mn == "fcomip") {
            emit(fmt("x87::fcomi(%s, %s, fl);", st(0).c_str(), st(b.st).c_str()));
            if (mn.back() == 'p') pop();
            return;
        }
        if (mn == "fucom" || mn == "fcom" || mn == "fucomp" || mn == "fcomp" || mn == "fucompp" || mn == "fcompp") {
            // fucom st(i) may be decoded with the implicit st(0) as a visible first operand
            const std::string rhs = in.nops == 0 ? st(1) : in.nops == 2 && is_st(b) ? st(b.st) : is_st(a) ? st(a.st) : ldm(a);
            emit(fmt("x87::compare(%s, %s, %s);", f.c_str(), st(0).c_str(), rhs.c_str()));
            if (mn.back() == 'p') pop();
            if (mn == "fucompp" || mn == "fcompp") pop();
            return;
        }
        if (mn == "ftst") return emit(fmt("x87::compare(%s, %s, 0.0);", f.c_str(), st(0).c_str()));
        if (mn.compare(0, 5, "fcmov") == 0) {
            const std::string cc = mn.substr(5);
            static const struct { const char* n; const char* e; } m[] = {{"b", "cc_b"}, {"e", "cc_z"}, {"be", "cc_be"}, {"u", "cc_p"},
                                                                         {"nb", "cc_nb"}, {"ne", "cc_nz"}, {"nbe", "cc_nbe"}, {"nu", "cc_np"}};
            for (const auto& k : m)
                if (cc == k.n) return emit(fmt("if (%s(fl)) %s = %s;", k.e, st(0).c_str(), st(b.st).c_str()));
        }
        if (mn == "fnstsw") {
            if (is_st(a) || a.kind == OpKind::Mem) {
                if (a.kind == OpKind::Mem) return emit(fmt("st<u16>(%s, x87::status(%s));", addr(a).c_str(), f.c_str()));
            }
            return emit(write(a, fmt("x87::status(%s)", f.c_str())));  // ax
        }
        if (mn == "fnstcw") return emit(fmt("st<u16>(%s, %s.cw);", addr(a).c_str(), f.c_str()));
        if (mn == "fldcw") return emit(fmt("%s.cw = ld<u16>(%s);", f.c_str(), addr(a).c_str()));
        if (mn == "fnstenv") return emit(fmt("x87::fnstenv(%s, %s);", f.c_str(), addr(a).c_str()));
        if (mn == "fldenv") return emit(fmt("x87::fldenv(%s, %s);", f.c_str(), addr(a).c_str()));
        unsupported(in);
    }

    // String instructions with and without rep. The direction flag is treated as always clear (cld is a no-op, std unsupported).
    bool string_insn(const Insn& in) {
        const std::string& mn = in.mnemonic;
        static const char* names[] = {"movs", "stos", "lods", "cmps", "scas"};
        int kind = -1;
        for (int k = 0; k < 5; ++k)
            if (mn.size() == 5 && mn.compare(0, 4, names[k]) == 0) kind = k;
        if (kind < 0) return false;
        const char sz = mn[4];
        if (sz != 'b' && sz != 'w' && sz != 'd' && sz != 'q') return false;
        if (sz == 'd' && (kind == 0 || kind == 3) && in.nops >= 2 && (in.ops[0].kind == OpKind::Reg || in.ops[1].kind == OpKind::Reg))
            return false;  // SSE movsd / cmpsd
        const int n = sz == 'b' ? 1 : sz == 'w' ? 2 : sz == 'd' ? 4 : 8;
        const char* T = ut(n * 8);
        std::string step;
        switch (kind) {
        case 0: step = fmt("st<%s>(r[RDI], ld<%s>(r[RSI])); r[RSI] += %d; r[RDI] += %d;", T, T, n, n); break;
        case 1: step = fmt("st<%s>(r[RDI], rd<%s>(r[RAX])); r[RDI] += %d;", T, T, n); break;
        case 2: step = fmt("wr<%s>(r[RAX], ld<%s>(r[RSI])); r[RSI] += %d;", T, T, n); break;
        case 3: step = fmt("(void)sub<%s>(ld<%s>(r[RSI]), ld<%s>(r[RDI]), fl); r[RSI] += %d; r[RDI] += %d;", T, T, T, n, n); break;
        default: step = fmt("(void)sub<%s>(rd<%s>(r[RAX]), ld<%s>(r[RDI]), fl); r[RDI] += %d;", T, T, T, n); break;
        }
        if (!in.rep_kind) return emit("{ " + step + " }"), true;
        // cmps / scas stop on the zero flag: repe while ZF = 1, repne while ZF = 0; the others just count rcx down
        std::string stop;
        if (kind >= 3) stop = in.rep_kind == 2 ? " if (fl.zf) break;" : " if (!fl.zf) break;";
        emit("while (r[RCX] != 0) { " + step + " r[RCX] -= 1;" + stop + " }");
        return true;
    }

    // Atomic and exchange-like instructions: lock-prefixed read-modify-write on memory, xchg with memory (implicitly locked),
    // xadd, cmpxchg, cmpxchg16b, and bts / btr / btc. Returns false when `in` is none of them (or has nothing atomic about it).
    bool atomic_insn(const Insn& in) {
        const std::string& mn = in.mnemonic;
        const Operand& d = in.ops[0];
        const Operand& s = in.ops[1];
        const int N = d.bits;
        const char* T = ut(N);
        const bool mem = d.kind == OpKind::Mem;

        if (mn == "cmpxchg16b" && mem) {
            emit(fmt("{ u64 e[2] = {r[RAX], r[RDX]}, w[2] = {r[RBX], r[RCX]}; fl.zf = atomic_cas128(%s, e, w);"
                     " if (!fl.zf) { r[RAX] = e[0]; r[RDX] = e[1]; } }", addr(d).c_str()));
            return true;
        }
        if (mn == "xchg" && (mem || s.kind == OpKind::Mem)) {  // either operand in memory: always locked
            const Operand& m = mem ? d : s;
            const Operand& r = mem ? s : d;
            emit(fmt("{ %s v = %s; %s o = atomic_rmw<%s>(%s, [&](%s) -> %s { return v; }); %s }", T, read(r, N).c_str(), T, T,
                     addr(m).c_str(), T, T, write(r, "o").c_str()));
            return true;
        }
        if (mn == "xadd") {
            if (mem) {
                emit(fmt("{ %s v = %s; %s o = atomic_rmw<%s>(%s, [&](%s x) -> %s { return add<%s>(x, v, fl); }); %s }", T,
                         read(s, N).c_str(), T, T, addr(d).c_str(), T, T, T, write(s, "o").c_str()));
            } else {
                emit(fmt("{ %s a = %s, b = %s; %s t = add<%s>(a, b, fl); %s %s }", T, read(d).c_str(), read(s, N).c_str(), T, T,
                         write(s, "a").c_str(), write(d, "t").c_str()));
            }
            return true;
        }
        if (mn == "cmpxchg") {
            if (mem) {
                emit(fmt("{ %s e = rd<%s>(r[RAX]), v = %s, e0 = e; bool ok = atomic_cas<%s>(%s, e, v); (void)sub<%s>(e0, e, fl);"
                         " if (!ok) wr<%s>(r[RAX], e); }", T, T, read(s, N).c_str(), T, addr(d).c_str(), T, T));
            } else {
                emit(fmt("{ %s acc = rd<%s>(r[RAX]), dv = %s; (void)sub<%s>(acc, dv, fl); if (fl.zf) { %s } else wr<%s>(r[RAX], dv); }", T, T,
                         read(d).c_str(), T, write(d, read(s, N)).c_str(), T));
            }
            return true;
        }
        if (mn == "bts" || mn == "btr" || mn == "btc") {
            if (mem && s.kind != OpKind::Imm) return false;  // bit-string addressing through a register: unsupported
            const std::string f = mn == "bts" ? "bit_set" : mn == "btr" ? "bit_reset" : "bit_flip";
            if (mem && in.lock) {
                emit(fmt("atomic_rmw<%s>(%s, [&](%s o) -> %s { return %s<%s>(o, %s, fl); });", T, addr(d).c_str(), T, T, f.c_str(), T,
                         read(s, 8).c_str()));
            } else {
                emit(write(d, fmt("%s<%s>(%s, %s, fl)", f.c_str(), T, read(d).c_str(), read(s, 8).c_str())));
            }
            return true;
        }
        if (!(in.lock && mem)) return false;
        // lock-prefixed ALU on memory
        std::string body;
        const std::string S = in.nops > 1 ? read(s, N, true) : "";
        if (mn == "add" || mn == "adc" || mn == "sub" || mn == "sbb") body = fmt("%s<%s>(o, v, fl)", mn.c_str(), T);
        else if (mn == "and") body = fmt("logic<%s>((%s)(o & v), fl)", T, T);
        else if (mn == "or") body = fmt("logic<%s>((%s)(o | v), fl)", T, T);
        else if (mn == "xor") body = fmt("logic<%s>((%s)(o ^ v), fl)", T, T);
        else if (mn == "inc" || mn == "dec" || mn == "neg") body = fmt("%s<%s>(o, fl)", mn.c_str(), T);
        else if (mn == "not") body = fmt("(%s)~o", T);
        else return false;
        emit(fmt("{ %s v = %s; (void)v; atomic_rmw<%s>(%s, [&](%s o) -> %s { return %s; }); }", T, S.empty() ? "0" : S.c_str(), T,
                 addr(d).c_str(), T, T, body.c_str()));
        return true;
    }

    // 128-bit integer SIMD families. Returns false when `in` is none of them.
    bool int_simd_insn(const Insn& in) {
        const std::string& mn = in.mnemonic;
        const Operand& d = in.ops[0];
        const Operand& s = in.ops[1];
        const Operand& t = in.ops[2];
        auto V = [&](const std::string& e) { vset(d, e); return true; };
        auto lane_t = [](char c) -> const char* {
            switch (c) { case 'b': return "u8"; case 'w': return "u16"; case 'd': return "u32"; case 'q': return "u64"; default: return nullptr; }
        };
        auto imm = [&](const Operand& o) { return unsigned(o.imm) & 0xffu; };
        auto ends = [&](const char* p, std::string& rest) {
            const size_t n = std::strlen(p);
            if (mn.size() > n && mn.compare(0, n, p) == 0) return rest = mn.substr(n), true;
            return false;
        };
        std::string r;
        if (in.nops == 3 && is_vec(d) && is_vec(s)) {
            static const char* bin[] = {"vpadd", "vpsub", "vpcmpeq", "vpcmpgt"};
            for (const char* b : bin)
                if (ends(b, r) && r.size() == 1 && lane_t(r[0]))
                    return V(fmt("%s<%s>(x[%d], %s)", b, lane_t(r[0]), s.vec, vread(t).c_str()));
            if (ends("vpunpckl", r) || ends("vpunpckh", r)) {
                const bool hi = mn[7] == 'h';
                static const char* k[][2] = {{"bw", "u8"}, {"wd", "u16"}, {"dq", "u32"}, {"qdq", "u64"}};
                for (auto& e : k)
                    if (r == e[0]) return V(fmt("vunpck<%s, %s, 128>(x[%d], %s)", e[1], hi ? "true" : "false", s.vec, vread(t).c_str()));
            }
            if (mn == "vpshufb") return V(fmt("vpshufb(x[%d], %s)", s.vec, vread(t).c_str()));
            if (mn == "vpmulld") return V(fmt("vpmul<u32>(x[%d], %s)", s.vec, vread(t).c_str()));
            if (ends("vphadd", r) || ends("vphsub", r)) {
                if (r.size() == 1 && (r[0] == 'd' || r[0] == 'w'))
                    return V(fmt("vphaddi<%s, %s>(x[%d], %s)", lane_t(r[0]), mn[3] == 's' ? "true" : "false", s.vec, vread(t).c_str()));
            }
            if (ends("vpsll", r) || ends("vpsrl", r) || ends("vpsra", r)) {
                if (r.size() == 1 && r[0] != 'b' && lane_t(r[0]) && !(mn[4] == 'a' && r[0] == 'q')) {
                    const std::string cnt = t.kind == OpKind::Imm ? fmt("%uull", imm(t)) : fmt("lane<u64>(%s, 0)", vread(t).c_str());
                    return V(fmt("%s<%s>(x[%d], %s)", mn.substr(0, 5).c_str(), lane_t(r[0]), s.vec, cnt.c_str()));
                }
            }
            if (ends("vpack", r) && r.size() == 4 && (r.substr(0, 2) == "us" || r.substr(0, 2) == "ss")) {
                const bool u = r[0] == 'u';
                const std::string w = r.substr(2);
                if (w == "wb") return V(fmt("vpack<i16, %s>(x[%d], %s)", u ? "u8" : "i8", s.vec, vread(t).c_str()));
                if (w == "dw") return V(fmt("vpack<i32, %s>(x[%d], %s)", u ? "u16" : "i16", s.vec, vread(t).c_str()));
            }
        }
        if (in.nops == 4 && mn == "vpblendw") return V(fmt("vblendi<u16, 128>(x[%d], %s, %uu)", s.vec, vread(t).c_str(), imm(in.ops[3])));
        if (in.nops == 4 && mn == "vpblendvb") return V(fmt("vblendv<u8, 128>(x[%d], %s, x[%d])", s.vec, vread(t).c_str(), in.ops[3].vec));
        if (in.nops == 3 && t.kind == OpKind::Imm && is_vec(d)) {
            if (mn == "vpshufd") return V(fmt("vpermilps_i<128>(%s, %uu)", vread(s).c_str(), imm(t)));
            if (mn == "vpshuflw" || mn == "vpshufhw") return V(fmt("%s(%s, %uu)", mn.c_str(), vread(s).c_str(), imm(t)));
            if (mn == "vpslldq" || mn == "vpsrldq") return V(fmt("%s(x[%d], %uu)", mn.c_str(), s.vec, imm(t)));
        }
        if (in.nops == 2 && is_vec(d) && (ends("vpmovzx", r) || ends("vpmovsx", r)) && r.size() == 2) {
            const bool sx = mn[5] == 's';
            auto st = [&](char c) -> std::string { return std::string(sx ? "i" : "u") + (c == 'b' ? "8" : c == 'w' ? "16" : "32"); };
            const char* dt = r[1] == 'w' ? "u16" : r[1] == 'd' ? "u32" : "u64";
            return V(fmt("vpmovx<%s, %s>(%s)", st(r[0]).c_str(), dt, vread(s).c_str()));
        }
        if (in.nops == 4 && is_vec(d) && ends("vpinsr", r) && r.size() == 1 && lane_t(r[0]))
            return V(fmt("vpins<%s>(x[%d], %s, %uu)", lane_t(r[0]), s.vec, read(t, r[0] == 'b' ? 8 : r[0] == 'w' ? 16 : r[0] == 'd' ? 32 : 64).c_str(), imm(in.ops[3])));
        if (in.nops == 3 && is_vec(s) && ends("vpextr", r) && r.size() == 1 && lane_t(r[0])) {
            const char* T = lane_t(r[0]);
            const int n = r[0] == 'b' ? 16 : r[0] == 'w' ? 8 : r[0] == 'd' ? 4 : 2;
            const std::string e = fmt("lane<%s>(x[%d], %u)", T, s.vec, imm(t) % unsigned(n));
            if (d.kind == OpKind::Mem) emit(fmt("st<%s>(%s, %s);", T, addr(d).c_str(), e.c_str()));
            else emit(write(d, e));
            return true;
        }
        return false;
    }

    // Blends, shuffles, lane inserts / extracts and broadcasts. Returns false when `in` is none of them.
    bool shuffle_insn(const Insn& in) {
        const std::string& mn = in.mnemonic;
        const Operand& d = in.ops[0];
        const Operand& s = in.ops[1];
        const Operand& t = in.ops[2];
        const int B = d.bits;
        const bool ps = mn.size() > 2 && mn.compare(mn.size() - 2, 2, "ps") == 0;
        const char* U = ps ? "u32" : "u64";
        auto imm = [&](const Operand& o) { return unsigned(o.imm) & 0xffu; };
        auto V = [&](const std::string& e) { vset(d, e); return true; };

        if ((mn == "vblendvps" || mn == "vblendvpd") && in.nops == 4)
            return V(fmt("vblendv<%s, %d>(%s, %s, x[%d])", U, B, vread(s).c_str(), vread(t).c_str(), in.ops[3].vec));
        if ((mn == "vblendps" || mn == "vblendpd") && in.nops == 4)
            return V(fmt("vblendi<%s, %d>(%s, %s, %uu)", U, B, vread(s).c_str(), vread(t).c_str(), imm(in.ops[3])));
        if (mn == "vpermilps" || mn == "vpermilpd") {
            const bool immf = t.kind == OpKind::Imm;
            const std::string name = mn == "vpermilps" ? "vpermilps" : "vpermilpd";
            if (immf) return V(fmt("%s_i<%d>(%s, %uu)", name.c_str(), B, vread(s).c_str(), imm(t)));
            return V(fmt("%s_v<%d>(%s, %s)", name.c_str(), B, vread(s).c_str(), vread(t).c_str()));
        }
        if (mn == "vinsertps" && in.nops == 4)
            return V(fmt("vinsertps(x[%d], %s, %uu, %s)", s.vec, vread(t).c_str(), imm(in.ops[3]), is_vec(t) ? "false" : "true"));
        if (mn == "vinsertf128" && in.nops == 4)
            return V(fmt("vinsert128(x[%d], %s, %uu)", s.vec, vread(t).c_str(), imm(in.ops[3])));
        if (mn == "vextractf128" && in.nops == 3) {
            if (is_vec(d)) return V(fmt("vextract128(x[%d], %uu)", s.vec, imm(t)));
            emit(fmt("vst<128>(%s, vextract128(x[%d], %uu));", addr(d).c_str(), s.vec, imm(t)));
            return true;
        }
        if (mn == "vperm2f128" && in.nops == 4)
            return V(fmt("vperm2f128(x[%d], %s, %uu)", s.vec, vread(t).c_str(), imm(in.ops[3])));
        if (mn == "vbroadcastss" || mn == "vbroadcastsd") {
            const char* T = mn == "vbroadcastss" ? "u32" : "u64";
            return V(fmt("vbcast<%s, %d>(%s)", T, B, is_vec(s) ? fmt("vlo<%s>(x[%d])", T, s.vec).c_str()
                                                                  : fmt("ld<%s>(%s)", T, addr(s).c_str()).c_str()));
        }
        if (mn == "vbroadcastf128") return V(fmt("vbcast128(%s)", vread(s).c_str()));
        if (mn == "vmovshdup" || mn == "vmovsldup" || mn == "vmovddup") return V(fmt("%s<%d>(%s)", mn.c_str(), B, vread(s).c_str()));
        static const char* unp[] = {"vunpcklps", "vunpckhps", "vunpcklpd", "vunpckhpd"};
        for (int k = 0; k < 4; ++k)
            if (mn == unp[k] && in.nops == 3)
                return V(fmt("vunpck<%s, %s, %d>(%s, %s)", k < 2 ? "u32" : "u64", k % 2 ? "true" : "false", B, vread(s).c_str(),
                             vread(t).c_str()));
        if ((mn == "vshufps" || mn == "vshufpd") && in.nops == 4)
            return V(fmt("%s<%d>(%s, %s, %uu)", mn.c_str(), B, vread(s).c_str(), vread(t).c_str(), imm(in.ops[3])));
        if (mn == "vextractps" && in.nops == 3) {
            const std::string e = fmt("lane<u32>(x[%d], %u)", s.vec, imm(t) & 3u);
            if (d.kind == OpKind::Mem) emit(fmt("st<u32>(%s, %s);", addr(d).c_str(), e.c_str()));
            else emit(write(d, e));
            return true;
        }
        if (mn == "vmovlps" || mn == "vmovhps" || mn == "vmovlpd" || mn == "vmovhpd") {
            const int idx = mn == "vmovhps" || mn == "vmovhpd" ? 1 : 0;
            if (in.nops == 3) return V(fmt("vmk(%s, %s)", idx ? fmt("x[%d].q[0]", s.vec).c_str() : fmt("ld<u64>(%s)", addr(t).c_str()).c_str(),
                                            idx ? fmt("ld<u64>(%s)", addr(t).c_str()).c_str() : fmt("x[%d].q[1]", s.vec).c_str()));
            emit(fmt("st<u64>(%s, x[%d].q[%d]);", addr(d).c_str(), s.vec, idx));
            return true;
        }
        if (mn == "vmovlhps" && in.nops == 3) return V(fmt("vmk(x[%d].q[0], x[%d].q[0])", s.vec, t.vec));
        if (mn == "vmovhlps" && in.nops == 3) return V(fmt("vmk(x[%d].q[1], x[%d].q[1])", t.vec, s.vec));
        if (mn == "vmovmskps" || mn == "vmovmskpd") {
            emit(write(d, fmt("vmovmsk<%s, %d>(x[%d])", mn == "vmovmskps" ? "u32" : "u64", s.bits, s.vec)));
            return true;
        }
        return false;
    }

    // Legacy (non-VEX) SSE: the same operation as the VEX form with the destination also as the first source, except that bits
    // 255:128 of the destination register are left alone. Translated to the VEX instruction and the upper half restored.
    void legacy_sse(const Insn& in) {
        const std::string& mn = in.mnemonic;
        const Operand& d = in.ops[0];
        // VEX forms that take (dest, src1, src2): the legacy form has dest == src1
        static const char* dup[] = {"add", "sub", "mul", "div", "min", "max", "sqrt", "and", "andn", "or", "xor", "cmp", "unpckl",
                                    "unpckh", "shuf", "movlp", "movhp", "cvtsi2", "cvtss2sd", "cvtsd2ss", "padd", "psub", "pcmpeq", "pcmpgt", "punpckl",
                                    "punpckh", "pxor", "por", "pand", "pandn", "pmull", "pack"};
        static const char* same[] = {"movaps", "movups", "movapd", "movupd", "movdqa", "movdqu", "ucomiss", "ucomisd", "comiss",
                                     "comisd", "movntdq", "movntps", "movntpd", "movntdqa", "movd", "movq", "cvttss2si", "cvttsd2si", "cvtss2si", "cvtsd2si", "pshufd", "pshuflw",
                                     "pshufhw", "movddup", "movshdup", "movsldup", "pcmpistri"};
        if (mn == "pcmpistrm" && in.nops == 3) {  // writes xmm0; bits 255:128 are preserved
            emit(fmt("{ const Vec t = pcmpistrm(x[%d], %s, %d, fl); x[0].q[0] = t.q[0]; x[0].q[1] = t.q[1]; }", d.vec,
                     vread(in.ops[1]).c_str(), int(in.ops[2].imm)));
            return;
        }
        bool shift = false, known = false;
        for (const char* k : same) known |= mn == k;
        for (const char* k : dup) {
            const std::string p = k;
            if (mn.compare(0, p.size(), p) == 0) {
                known = shift = true;
                // an arithmetic name must end in ss/sd/ps/pd (or be a packed-integer / conversion name)
                const std::string rest = mn.substr(p.size());
                if ((p == "add" || p == "sub" || p == "mul" || p == "div" || p == "min" || p == "max" || p == "sqrt" || p == "and" ||
                     p == "andn" || p == "or" || p == "xor") &&
                    rest != "ss" && rest != "sd" && rest != "ps" && rest != "pd")
                    known = shift = false;
            }
        }
        // cmpeqss, cmpltsd, ... are cmpss / cmpsd with the predicate in the name
        static const char* preds[] = {"eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"};
        Insn v = in;
        if (mn.size() == 8 && mn.compare(0, 3, "cmp") == 0 && (mn.compare(6, 2, "ss") == 0 || mn.compare(6, 2, "sd") == 0 ||
                                                                 mn.compare(6, 2, "ps") == 0 || mn.compare(6, 2, "pd") == 0)) {
            // cmpeqps: 3 letters of predicate would not fit; handled by the length check below
        }
        for (int k = 0; k < 8; ++k) {
            const std::string pr = std::string("cmp") + preds[k];
            if (mn.size() == pr.size() + 2 && mn.compare(0, pr.size(), pr) == 0 && in.nops == 2) {
                v.mnemonic = "cmp" + mn.substr(pr.size());
                v.ops[2] = Operand{};
                v.ops[2].kind = OpKind::Imm, v.ops[2].imm = k, v.ops[2].bits = 8;
                v.nops = 3;
                known = shift = true;
                break;
            }
        }
        if ((mn == "movss" || mn == "movsd") && in.nops == 2 && is_vec(in.ops[0]) && is_vec(in.ops[1])) known = shift = true;
        else if (mn == "movss" || mn == "movsd") known = true;  // load / store forms
        if (!known) return unsupported(in, "legacy SSE");
        if (v.mnemonic == in.mnemonic) v.mnemonic = "v" + mn;
        if (shift) {  // (dest, src, [imm]) -> (dest, dest, src, [imm])
            for (int i = v.nops; i > 1; --i) v.ops[i] = v.ops[i - 1];
            v.ops[1] = v.ops[0];
            ++v.nops;
        }
        if (!is_vec(d)) return vector_insn(v);  // GPR or memory destination: no vector register to preserve
        emit(fmt("{ const u64 hi0 = x[%d].q[2], hi1 = x[%d].q[3];", d.vec, d.vec));
        vector_insn(v);
        emit(fmt("x[%d].q[2] = hi0; x[%d].q[3] = hi1; }", d.vec, d.vec));
    }

    void vector_insn(const Insn& in) {
        const std::string& mn = in.mnemonic;
        const Operand& d = in.ops[0];
        const Operand& s = in.ops[1];
        if (!starts_with(mn, "v")) return legacy_sse(in);

        if (mn == "vmovups" || mn == "vmovaps" || mn == "vmovupd" || mn == "vmovapd" || mn == "vmovdqa" || mn == "vmovdqu" ||
            mn == "vlddqu" || mn == "vmovntdq" || mn == "vmovntps" || mn == "vmovntpd" || mn == "vmovntdqa") {
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

        if ((mn == "vpcmpistri" || mn == "vpcmpistrm") && in.nops == 3) {
            const std::string args = fmt("x[%d], %s, %d, fl", d.vec, vread(s).c_str(), int(in.ops[2].imm));
            if (mn == "vpcmpistri") return emit(fmt("r[RCX] = pcmpistri(%s);", args.c_str())), void();
            return emit(fmt("x[0] = vcut<128>(pcmpistrm(%s));", args.c_str())), void();
        }
        if (mn == "vptest" && in.nops == 2) return emit(fmt("vptest<%d>(x[%d], %s, fl);", d.bits, d.vec, vread(s).c_str())), void();
        if (mn == "vcvtdq2pd" && in.nops == 2) return vset(d, fmt("vpcvtdq2pd<%d>(%s)", d.bits, vread(s).c_str()));
        if ((mn == "vmaskmovps" || mn == "vmaskmovpd") && in.nops == 3) {
            const char* U = mn == "vmaskmovps" ? "u32" : "u64";
            if (is_vec(d)) return vset(d, fmt("vmaskload<%s, %d>(x[%d], %s)", U, d.bits, s.vec, addr(in.ops[2]).c_str()));
            return emit(fmt("vmaskstore<%s, %d>(%s, x[%d], x[%d]);", U, in.ops[2].bits, addr(d).c_str(), s.vec, in.ops[2].vec)), void();
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
            if ((base == "rcp" || base == "rsqrt") && in.nops == 3 && !dbl)
                return vset(d, fmt("vins<u32>(x[%d], f%s(%s))", s.vec, base.c_str(), elem(in.ops[2], "u32").c_str()));
            if (base == "round" && in.nops == 4)
                return vset(d, fmt("vins<%s>(x[%d], fround<%s>(%s, %uu))", U, s.vec, F, elem(in.ops[2], U).c_str(),
                                   unsigned(in.ops[3].imm) & 7u));
            if (base == "cvtsi2" && in.nops == 3) {
                const char* I = in.ops[2].bits == 64 ? "i64" : "i32";
                return vset(d, fmt("vins<%s>(x[%d], fcvti<%s, %s>((%s)%s))", U, s.vec, F, I, I, read(in.ops[2]).c_str()));
            }
        }
        if ((mn == "vcvtss2si" || mn == "vcvtsd2si") && in.nops == 2) {
            const bool dbl = mn == "vcvtsd2si";
            const char* I = d.bits == 64 ? "i64" : "i32";
            return emit(write(d, fmt("fcvtn<%s, %s>(%s)", dbl ? "double" : "float", I,
                                     is_vec(s) ? fmt("vlo<%s>(x[%d])", dbl ? "u64" : "u32", s.vec).c_str()
                                               : fmt("ld<%s>(%s)", dbl ? "u64" : "u32", addr(s).c_str()).c_str())));
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


        // packed float: ps = float, pd = double; the register width comes from the destination
        if (mn.size() > 3 && is_vec(d) && (mn.compare(mn.size() - 2, 2, "ps") == 0 || mn.compare(mn.size() - 2, 2, "pd") == 0)) {
            const char* F = mn.back() == 'd' ? "double" : "float";
            const int B = d.bits;
            const std::string base = mn.substr(1, mn.size() - 3);
            static const char* arith[] = {"add", "sub", "mul", "div", "min", "max"};
            for (const char* a : arith)
                if (base == a && in.nops == 3)
                    return vset(d, fmt("vpf<%s, %d, f%s<%s>>(%s, %s)", F, B, a, F, vread(s).c_str(), vread(in.ops[2]).c_str()));
            if ((base == "rcp" || base == "rsqrt") && in.nops == 2 && mn.back() == 's')
                return vset(d, fmt("vp%s<%d>(%s)", base.c_str(), B, vread(s).c_str()));
            if (base == "sqrt" && in.nops == 2) return vset(d, fmt("vpsqrt<%s, %d>(%s)", F, B, vread(s).c_str()));
            if (base == "cmp" && in.nops == 4)
                return vset(d, fmt("vpcmp<%s, %d>(%s, %s, %uu)", F, B, vread(s).c_str(), vread(in.ops[2]).c_str(),
                                   unsigned(in.ops[3].imm) & 15u));
            if ((base == "hadd" || base == "hsub") && in.nops == 3)
                return vset(d, fmt("vphop<%s, %d, %s>(%s, %s)", F, B, base == "hsub" ? "true" : "false", vread(s).c_str(),
                                   vread(in.ops[2]).c_str()));
            if (base == "addsub" && in.nops == 3)
                return vset(d, fmt("vpaddsub<%s, %d>(%s, %s)", F, B, vread(s).c_str(), vread(in.ops[2]).c_str()));
            if (base == "round" && in.nops == 3)
                return vset(d, fmt("vpround<%s, %d>(%s, %uu)", F, B, vread(s).c_str(), unsigned(in.ops[2].imm) & 7u));
        }
        if (mn == "vcvtdq2ps" && in.nops == 2) return vset(d, fmt("vpcvtdq2ps<%d>(%s)", d.bits, vread(s).c_str()));
        if (mn == "vcvttps2dq" && in.nops == 2) return vset(d, fmt("vpcvttps2dq<%d>(%s)", d.bits, vread(s).c_str()));

        if (int_simd_insn(in)) return;
        if (shuffle_insn(in)) return;
        unsupported(in);
    }

    // A recognised jump table: dispatch on the computed target address (the instructions before the jump already
    // computed it exactly as the original did), so no index tracking is needed. Cases outside this function are tail calls.
    void jump_table(const Insn& in) {
        const JumpTable& jt = fn.jump_tables.at(in.addr);
        const std::string target = read(in.ops[0], 64);
        emit(fmt("switch (%s - %s) {", target.c_str(), img.c_str()));
        std::set<uint64_t> seen;
        for (uint64_t t : jt.targets) {
            if (!seen.insert(t).second) continue;
            if (fn.blocks.count(t)) emit(fmt("case 0x%llx: goto %s;", (unsigned long long)t, label(t).c_str()));
            else if (functions.count(t)) emit(fmt("case 0x%llx: { DCP_SAVE(); %s(c); return; }", (unsigned long long)t, fname(t).c_str()));
            else emit(fmt("case 0x%llx: { DCP_SAVE(); jmp_ind(c, %s + 0x%llxull); return; }", (unsigned long long)t, img.c_str(), (unsigned long long)t));
        }
        emit(fmt("default: { DCP_SAVE(); jmp_ind(c, %s); return; }", target.c_str()));
        emit("}");
    }

    // An indirect jump that could not be resolved, in a function that has other blocks: the target may be one of them (a computed
    // goto, memcpy's size dispatch) or another function (a tail call through a pointer). Dispatch on the target address.
    void computed_jump(const Insn& in) {
        const std::string target = read(in.ops[0], 64);
        emit(fmt("{ u64 t_ = %s; switch (t_ - %s) {", target.c_str(), img.c_str()));
        for (const auto& [a, b] : fn.blocks)
            if (a != fn.start) emit(fmt("case 0x%llx: goto %s;", (unsigned long long)a, label(a).c_str()));
        emit("default: { DCP_SAVE(); jmp_ind(c, t_); return; }");
        emit("} }");
    }

    // ---- one instruction ---------------------------------------------------------------------------------------

    void insn(const Insn& in) {
        const std::string& mn = in.mnemonic;
        ++res.insns;
        if (comments) body += fmt("  // 0x%llx: %s\n", (unsigned long long)in.addr, in.text.c_str());

        if (in.flow == Flow::Ret) {
            long long extra = in.nops && in.ops[0].kind == OpKind::Imm ? (long long)in.ops[0].imm : 0;
            emit(fmt("r[RSP] += %lld; DCP_SAVE(); return;", 8 + extra));
            return;
        }
        if (in.flow == Flow::Trap) return emit("__builtin_trap();");
        if (in.flow == Flow::Call) return call_to(in);
        if (in.flow == Flow::Jump) {
            if (!in.direct && fn.jump_tables.count(in.addr)) return jump_table(in);
            if (!in.direct && fn.has_indirect_jump && fn.blocks.size() > 1) return computed_jump(in);
            if (in.direct && fn.blocks.count(in.target)) return emit("goto " + label(in.target) + ";");
            return tail_to(in);
        }
        if (in.flow == Flow::CondJump && (mn == "loop" || mn == "loope" || mn == "loopne" || mn == "jrcxz")) {
            if (!in.direct) return unsupported(in, "indirect");
            std::string cond;
            if (mn == "jrcxz") cond = "r[RCX] == 0";
            else {
                emit("r[RCX] -= 1;");
                cond = mn == "loop" ? "r[RCX] != 0" : mn == "loope" ? "r[RCX] != 0 && fl.zf" : "r[RCX] != 0 && !fl.zf";
            }
            if (fn.blocks.count(in.target)) return emit(fmt("if (%s) goto %s;", cond.c_str(), label(in.target).c_str()));
            emit(fmt("if (%s) {", cond.c_str()));
            body += "  ";
            tail_to(in);
            return emit("}");
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
        if (mn.size() > 1 && mn[0] == 'f' && mn != "fs") return x87_insn(in);
        if (!supported_operands(in)) return;
        for (int i = 0; i < in.nops; ++i)
            if (is_vec(in.ops[i])) return vector_insn(in);
        if (mn == "vcvttss2si" || mn == "vcvttsd2si" || mn == "vcvtss2si" || mn == "vcvtsd2si" ||
            mn == "cvttss2si" || mn == "cvttsd2si" || mn == "cvtss2si" || mn == "cvtsd2si")
            return vector_insn(in);  // GPR destination, memory source
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
        if (string_insn(in)) return;
        if (mn == "stmxcsr") return emit(fmt("st<u32>(%s, 0x1f80);  // the default: all exceptions masked, round to nearest", addr(in.ops[0]).c_str()));
        if (mn == "pushfq") return emit("{ r[RSP] -= 8; st<u64>(r[RSP], pack_flags(fl) | 0x202); }");
        if (mn == "popfq") return emit("{ u64 v = ld<u64>(r[RSP]); r[RSP] += 8; fl = unpack_flags(v); }");
        if (mn == "movnti") return emit(fmt("st<%s>(%s, %s);", ut(in.ops[1].bits), addr(in.ops[0]).c_str(), read(in.ops[1]).c_str()));
        if ((mn == "shld" || mn == "shrd") && in.nops == 3)
            return emit(write(in.ops[0], fmt("%s<%s>(%s, %s, %s, fl)", mn.c_str(), ut(in.ops[0].bits), read(in.ops[0]).c_str(),
                                             read(in.ops[1], in.ops[0].bits).c_str(), read(in.ops[2], 8).c_str())));
        if (mn == "clc") return emit("fl.cf = false;");
        if (mn == "stc") return emit("fl.cf = true;");
        if (mn == "cmc") return emit("fl.cf = !fl.cf;");
        if (mn == "cld" || mn == "pause" || mn == "ldmxcsr" || mn == "prefetcht0" || mn == "prefetcht1" || mn == "prefetcht2" ||
            mn == "prefetchnta" || mn == "prefetchw" || mn == "prefetch")
            return;  // no effect on guest-visible state (the rounding mode in mxcsr is not modelled)
        if (mn == "mfence" || mn == "lfence" || mn == "sfence") return emit("__atomic_thread_fence(__ATOMIC_SEQ_CST);");
        if (mn == "in" || mn == "out" || mn == "insb" || mn == "insw" || mn == "insd" || mn == "outsb" || mn == "outsw" || mn == "outsd")
            return emit("__builtin_trap();  // privileged: never executed by a user program");
        if (atomic_insn(in)) return;
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
        if (mn == "blsi" || mn == "blsr" || mn == "blsmsk") return emit(write(d, fmt("%s<%s>(%s, fl)", mn.c_str(), ut(N), read(s, N).c_str())));
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

    // A host hook before the instruction (see SiteMap): it may change registers and memory; a non-zero result leaves for the target.
    void site_hook(const Insn& in) {
        auto s = sites->find(in.addr);
        if (s == sites->end()) return;
        res.sites_hit.insert(in.addr);
        const std::string n = site_name(in.addr, module);
        emit("if (" + n + ") {");
        emit("DCP_SAVE();");
        emit("u64 go_ = " + n + "(c);");
        emit("DCP_LOAD();");
        if (s->second == kSiteRet) emit("if (go_) { r[RSP] += 8; DCP_SAVE(); return; }");
        else if (s->second == kSiteSkip) emit(fmt("if (go_) goto S_%llx;", (unsigned long long)in.addr));
        else if (s->second != kSiteNoTarget) emit(fmt("if (go_) goto %s;", label(s->second).c_str()));
        emit("}");
    }

    void patched_insn(const Insn& in) {
        if (sites) site_hook(in);
        if (patches) {
            if (auto p = patches->find(in.addr); p != patches->end()) {
                emit("if (" + patch_name(in.addr, module) + ") {");
                for (const Insn& a : p->second) insn(a);
                emit("} else {");
                insn(in);
                emit("}");
                skip_label(in);
                return;
            }
        }
        insn(in);
        skip_label(in);
    }

    void skip_label(const Insn& in) {  // where a kSiteSkip hook jumps to
        if (!sites) return;
        if (auto s = sites->find(in.addr); s != sites->end() && s->second == kSiteSkip) emit(fmt("S_%llx:;", (unsigned long long)in.addr));
    }

    LiftResult run() {
        // Labels only for blocks that are the target of a direct jump inside this function.
        std::set<uint64_t> targets;
        for (const auto& [a, b] : fn.blocks)
            for (const Insn& in : b.insns)
                if ((in.flow == Flow::Jump || in.flow == Flow::CondJump) && in.direct && fn.blocks.count(in.target))
                    targets.insert(in.target);

        if (sites)  // a site hook's target is a goto label
            for (const auto& [a, b] : fn.blocks)
                for (const Insn& in : b.insns)
                    if (auto s = sites->find(in.addr); s != sites->end() && s->second != kSiteNoTarget && s->second != kSiteRet && s->second != kSiteSkip) {
                        if (!fn.blocks.count(s->second))
                            throw std::runtime_error(fmt("site hook at 0x%llx: target 0x%llx does not start a basic block of the function",
                                                         (unsigned long long)in.addr, (unsigned long long)s->second));
                        targets.insert(s->second);
                    }

        for (const auto& [ja, jt] : fn.jump_tables)
            for (uint64_t t : jt.targets)
                if (fn.blocks.count(t)) targets.insert(t);
        if (fn.has_indirect_jump)  // a computed jump may land on any block
            for (const auto& [a, b] : fn.blocks) targets.insert(a);
        for (const auto& [a, b] : fn.blocks) {
            if (targets.count(a)) body += label(a) + ":;\n";
            for (const Insn& in : b.insns) patched_insn(in);
        }
        if (fn.fell_off_end) body += "  __builtin_trap();  // control reaches the end of the function: a noreturn call\n";

        // The optimizer is super-linear in function size: past this many lines compiling would take hours, so such a function
        // is built unoptimized (it is cold initialisation code in practice).
        constexpr size_t kHugeLines = 20000;
        const bool huge = size_t(std::count(body.begin(), body.end(), '\n')) > kHugeLines;
        res.code = fmt("extern \"C\" %svoid %s(Ctx* c) {\n", huge ? "__attribute__((optnone)) " : "", fname(fn.start).c_str());
        if (hooked) {
            const std::string h = hook_name(fn.start, module);
            res.code += fmt("  if (%s) %s(c);\n", h.c_str(), h.c_str());
        }
        res.code += "  u64 r[16];\n  Vec x[16];\n  Flags fl;\n  DCP_LOAD();\n";
        res.code += body + "}\n";
        return std::move(res);
    }
};

}  // namespace

std::string function_name(uint64_t start, const std::string& module) {
    return module.empty() ? fmt("f_%llx", (unsigned long long)start) : fmt("f_%s_%llx", module.c_str(), (unsigned long long)start);
}

std::string lift_declarations(const std::set<uint64_t>& functions, const std::string& module, const std::set<uint64_t>& hooks,
                             const PatchMap& patches, const SiteMap& sites) {
    std::string out;
    for (const auto& [site, target] : sites) out += "extern dcp::rt::SiteHook " + site_name(site, module) + ";\n";
    for (const auto& [site, alt] : patches) out += "extern uint8_t " + patch_name(site, module) + ";\n";
    for (uint64_t h : hooks)
        if (functions.count(h)) out += "extern dcp::rt::EntryHook " + hook_name(h, module) + ";\n";
    if (!module.empty()) out += "extern dcp::rt::u64 dcp_img_" + module + ";\n";
    for (uint64_t f : functions) out += "extern \"C\" void " + function_name(f, module) + "(Ctx*);\n";
    return out;
}

std::string lift_function_table(const std::set<uint64_t>& functions, const std::string& module, const std::set<uint64_t>& hooks,
                                const PatchMap& patches, const SiteMap& sites) {
    const std::string sfx = module.empty() ? "" : "_" + module;
    std::string slots, regs;
    for (const auto& [site, target] : sites) {
        slots += "dcp::rt::SiteHook " + site_name(site, module) + " = nullptr;\n";
        regs += fmt("  dcp::rt::register_site_slot(\"%s\", 0x%llxu, &%s);\n", module.c_str(), (unsigned long long)site, site_name(site, module).c_str());
    }
    for (uint64_t h : hooks) {
        if (!functions.count(h)) continue;
        slots += "dcp::rt::EntryHook " + hook_name(h, module) + " = nullptr;\n";
        regs += fmt("  dcp::rt::register_hook_slot(\"%s\", 0x%llxu, &%s);\n", module.c_str(), (unsigned long long)h, hook_name(h, module).c_str());
    }
    for (const auto& [site, alt] : patches) {
        slots += "uint8_t " + patch_name(site, module) + " = 0;\n";
        regs += fmt("  dcp::rt::register_patch_slot(\"%s\", 0x%llxu, &%s);\n", module.c_str(), (unsigned long long)site, patch_name(site, module).c_str());
    }
    std::string out = slots + "#include \"dcp_dispatch.h\"\nnamespace {\nconst dcp::rt::FnEntry kFunctions" + sfx + "[] = {\n";
    for (uint64_t f : functions) out += fmt("  {0x%llxu, %s},\n", (unsigned long long)f, function_name(f, module).c_str());
    if (functions.empty()) out += "  {0, nullptr},\n";
    out += "};\n}  // namespace\n";
    // not through fmt(): its buffer is small and the registration of many hooks is long
    if (module.empty())
        out += "extern \"C\" void dcp_register(unsigned long long img_base, unsigned long long img_size) {\n"
               "  dcp::rt::set_image(img_base, img_size);\n  dcp::rt::set_function_table(kFunctions, " + std::to_string(functions.size()) + ");\n" +
               regs + "}\n";
    else
        out += "dcp::rt::u64 dcp_img_" + module + ";\n" + "extern \"C\" void dcp_register_" + module +
               "(unsigned long long img_base, unsigned long long img_size) {\n  dcp_img_" + module +
               " = img_base;\n  dcp::rt::add_module(img_base, img_size, kFunctions" + sfx + ", " + std::to_string(functions.size()) + ");\n" + regs + "}\n";
    return out;
}

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

LiftResult lift_function(const Function& fn, const std::set<uint64_t>& functions, const std::string& module, bool comments,
                         const std::set<uint64_t>& hooks, const PatchMap& patches, const SiteMap& sites) {
    Lifter l(fn, functions, module, comments);
    l.hooked = hooks.count(fn.start) != 0;
    l.patches = &patches;
    l.sites = &sites;
    return l.run();
}

}  // namespace dcp::recomp
