#include "lift.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
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
        if (mn == "vmovlps" || mn == "vmovhps") {
            const int idx = mn == "vmovhps" ? 1 : 0;
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
        emit(fmt("switch (%s - dcp_img) {", target.c_str()));
        std::set<uint64_t> seen;
        for (uint64_t t : jt.targets) {
            if (!seen.insert(t).second) continue;
            if (fn.blocks.count(t)) emit(fmt("case 0x%llx: goto %s;", (unsigned long long)t, label(t).c_str()));
            else if (functions.count(t)) emit(fmt("case 0x%llx: { DCP_SAVE(); %s(c); return; }", (unsigned long long)t, function_name(t).c_str()));
            else emit(fmt("case 0x%llx: { DCP_SAVE(); jmp_ind(c, dcp_img + 0x%llxull); return; }", (unsigned long long)t, (unsigned long long)t));
        }
        emit(fmt("default: { DCP_SAVE(); jmp_ind(c, %s); return; }", target.c_str()));
        emit("}");
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
            if (!in.direct && fn.jump_tables.count(in.addr)) return jump_table(in);
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

        for (const auto& [ja, jt] : fn.jump_tables)
            for (uint64_t t : jt.targets)
                if (fn.blocks.count(t)) targets.insert(t);
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

std::string lift_declarations(const std::set<uint64_t>& functions) {
    std::string out;
    for (uint64_t f : functions) out += "extern \"C\" void " + function_name(f) + "(Ctx*);\n";
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

LiftResult lift_function(const Function& fn, const std::set<uint64_t>& functions) {
    Lifter l(fn, functions);
    return l.run();
}

}  // namespace dcp::recomp
