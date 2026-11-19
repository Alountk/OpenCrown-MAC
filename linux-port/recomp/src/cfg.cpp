#include "cfg.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace dcp::recomp {

namespace {

// Recognises the jump-table idiom ending in `run.back()` (a `jmp R`). `run` holds the instructions decoded along the
// straight-line path that led here, in order. Returns false when any part of the pattern is missing or the table does
// not stay inside the function: the jump then stays an ordinary indirect jump.
// `table_of_reg`, when given, supplies the table address of a register that the run itself never loads (hoisted base).
bool recover_jump_table(const std::vector<Insn>& run, const uint8_t* code, size_t code_size, uint64_t code_base,
                        JumpTable& out, const std::function<bool(int, uint64_t&)>* table_of_reg = nullptr) {
    auto gpr = [](const Operand& o) { return o.kind == OpKind::Reg && o.reg != kNoReg; };
    const Insn& j = run.back();
    if (!j.nops || !gpr(j.ops[0])) return false;
    const int R = j.ops[0].reg;
    int T = kNoReg, I = kNoReg, S = kNoReg, i = int(run.size()) - 2;
    for (; i >= 0; --i) {  // add R, S
        const Insn& x = run[size_t(i)];
        if (x.mnemonic == "add" && x.nops == 2 && gpr(x.ops[0]) && x.ops[0].reg == R && x.ops[0].bits == 64 && gpr(x.ops[1])) {
            S = x.ops[1].reg;
            break;
        }
    }
    if (S == kNoReg) return false;
    int movsxd_at = -1;
    for (--i; i >= 0; --i) {  // movsxd R, [S + I*4] (table base in S) or movsxd S, [R + I*4] (table base in R, the jump register)
        const Insn& x = run[size_t(i)];
        if (x.mnemonic != "movsxd" || x.nops != 2 || !gpr(x.ops[0]) || x.ops[1].kind != OpKind::Mem || x.ops[1].index == kNoReg ||
            x.ops[1].scale != 4 || x.ops[1].disp != 0)
            continue;
        const bool base_in_s = x.ops[0].reg == R && x.ops[1].base == S, base_in_r = x.ops[0].reg == S && x.ops[1].base == R;
        if (!base_in_s && !base_in_r) continue;
        T = base_in_s ? S : R;
        I = x.ops[1].index;
        movsxd_at = i;
        break;
    }
    if (I == kNoReg) return false;
    uint64_t table = 0;
    bool have_lea = false;
    int lea_at = -1;
    for (--i; i >= 0; --i) {  // lea T, [rip + table], possibly copied through `mov T, T'` first
        const Insn& x = run[size_t(i)];
        if (x.mnemonic == "lea" && x.nops == 2 && gpr(x.ops[0]) && x.ops[0].reg == T && x.ops[1].kind == OpKind::Mem &&
            x.ops[1].base == kRip) {
            table = x.ops[1].rip_target, have_lea = true, lea_at = i;
            break;
        }
        if (x.mnemonic == "mov" && x.nops == 2 && gpr(x.ops[0]) && x.ops[0].reg == T && gpr(x.ops[1]) && x.ops[1].bits == 64) {
            T = x.ops[1].reg;
            continue;
        }
        if (x.nops && gpr(x.ops[0]) && x.ops[0].reg == T && x.mnemonic != "cmp" && x.mnemonic != "test") return false;
    }
    if (!have_lea && table_of_reg && (*table_of_reg)(T, table)) have_lea = true;
    if (!have_lea) return false;
    // Bounds check: `cmp I, N` then `ja / jnbe default` before the table is touched. The compiler may schedule other
    // instructions between the two as long as none of them writes the flags.
    auto writes_flags = [](const std::string& m) {
        if (m == "lea" || m == "push" || m == "pop" || m == "nop" || m == "not" || m == "xchg" || m == "bswap" || m == "cdq" ||
            m == "cdqe" || m == "cqo" || m == "cwde" || m == "movbe")
            return false;
        if (m.compare(0, 3, "mov") == 0 || m.compare(0, 4, "cmov") == 0 || m.compare(0, 3, "set") == 0) return false;
        if (m[0] == 'v') return m.find("comis") != std::string::npos || m.find("test") != std::string::npos;
        return true;
    };
    int cmp_at = -1;
    bool inclusive = true;  // `ja N` admits indices 0..N; `jae N` (jnb) admits 0..N-1
    (void)lea_at;
    for (int k = movsxd_at - 1; k >= 1 && cmp_at < 0; --k) {
        const Insn& x = run[size_t(k)];
        const bool above = x.mnemonic == "ja" || x.mnemonic == "jnbe", at_least = x.mnemonic == "jae" || x.mnemonic == "jnb";
        if (!above && !at_least) continue;
        inclusive = above;
        for (int m = k - 1; m >= 0; --m) {
            const Insn& c = run[size_t(m)];
            if (!writes_flags(c.mnemonic)) continue;
            if (c.mnemonic == "cmp" && c.nops == 2 && gpr(c.ops[0]) && c.ops[1].kind == OpKind::Imm) cmp_at = m;
            break;
        }
        break;
    }
    if (cmp_at < 0) return false;
    // The index register used by movsxd must hold the value the bounds check tested: either it is that register, or a
    // register-to-register mov copied it (before or after the cmp), and neither was changed afterwards.
    {
        const int C = run[size_t(cmp_at)].ops[0].reg;
        auto writes = [&](const Insn& x, int r) {
            return x.nops && gpr(x.ops[0]) && x.ops[0].reg == r && x.mnemonic != "cmp" && x.mnemonic != "test";
        };
        int cur = I;
        bool passed = false, ok = false;
        for (int k = movsxd_at - 1; k >= 0 && !ok; --k) {
            const Insn& x = run[size_t(k)];
            if (k == cmp_at) {
                passed = true;
                ok = cur == C;
                continue;
            }
            if (x.mnemonic == "mov" && x.nops == 2 && gpr(x.ops[0]) && x.ops[0].reg == cur && gpr(x.ops[1]) && x.ops[1].bits >= 32) {
                cur = x.ops[1].reg;
                ok = passed && cur == C;
                continue;
            }
            if (writes(x, cur) || (passed && writes(x, C))) return false;
        }
        if (!ok) return false;
    }
    const Insn& c = run[size_t(cmp_at)];
    const uint64_t mask = c.ops[0].bits >= 64 ? ~0ull : (1ull << c.ops[0].bits) - 1;
    const uint64_t count = (uint64_t(c.ops[1].imm) & mask) + (inclusive ? 1 : 0);
    if (count == 0 || count > 4096) return false;
    if (table < code_base || table + 4 * count > code_base + code_size) return false;
    out = JumpTable{};
    out.table = table;
    for (uint64_t k = 0; k < count; ++k) {
        int32_t rel;
        std::memcpy(&rel, code + (table - code_base) + 4 * k, 4);
        const uint64_t t = table + uint64_t(int64_t(rel));
        if (t < code_base || t - code_base >= code_size) return false;
        out.targets.push_back(t);
    }
    return true;
}

}  // namespace

Function discover_function(const uint8_t* code, size_t code_size, uint64_t code_base, uint64_t start, uint64_t size) {
    Function fn;
    fn.start = start, fn.size = size;
    const uint64_t end = start + size;
    auto inside = [&](uint64_t a) { return a >= start && a < end; };

    std::map<uint64_t, Insn> insns;
    std::set<uint64_t> leaders{start};
    std::vector<uint64_t> work{start};
    auto push_target = [&](uint64_t t) {
        if (inside(t)) leaders.insert(t), work.push_back(t);
        else fn.tail_jumps.insert(t);
    };

    struct Pending { uint64_t addr; std::vector<Insn> run; };
    std::vector<Pending> pending;  // indirect jumps not recognised from their own path alone

    auto drain = [&] {
      while (!work.empty()) {
        uint64_t a = work.back();
        work.pop_back();
        std::vector<Insn> run;  // the straight-line path decoded from this entry, for jump-table recognition
        for (; !insns.count(a);) {
            if (a == end) {  // straight-line code reached the end exactly: a call to a noreturn function (no ud2 after it)
                fn.fell_off_end = true;
                break;
            }
            if (!inside(a) || a < code_base || a - code_base >= code_size) {
                fn.has_bad_bytes = true;  // a jump landed outside the function, or outside the image
                if (!fn.bad_at) fn.bad_at = a;
                break;
            }
            Insn in;
            if (!decode(code + (a - code_base), std::min<uint64_t>(code_size - (a - code_base), end - a), a, in)) {
                fn.has_bad_bytes = fn.decode_failed = true;
                if (!fn.bad_at) fn.bad_at = a;
                break;
            }
            insns[a] = in;
            run.push_back(in);
            const uint64_t next = a + in.len;
            if (in.flow == Flow::Ret || in.flow == Flow::Trap) break;
            if (in.flow == Flow::Call) {
                if (in.direct) fn.calls.insert(in.target);
            } else if (in.flow == Flow::Jump) {
                JumpTable jt;
                if (in.direct) push_target(in.target);
                else if (recover_jump_table(run, code, code_size, code_base, jt)) {
                    for (uint64_t t : jt.targets) push_target(t);
                    fn.jump_tables.emplace(in.addr, std::move(jt));
                } else pending.push_back({in.addr, run});
                break;
            } else if (in.flow == Flow::CondJump) {
                if (in.direct) push_target(in.target);
                else fn.has_indirect_jump = true;
                leaders.insert(next);
            }
            a = next;
        }
      }
    };
    drain();

    // A jump table whose base register is loaded outside the jump's own path (hoisted out of a loop, set in the prologue):
    // accept the base when the function writes that callee-saved register exactly once, with `lea reg,[rip+table]`.
    const std::function<bool(int, uint64_t&)> hoisted_base = [&](int reg, uint64_t& table) {
        if (reg != 3 && reg != 5 && reg < 12) return false;  // rbx, rbp, r12-r15 survive calls; the others may not
        int writers = 0;
        bool is_lea = false;
        for (const auto& [a, in] : insns) {
            if (!in.nops || in.ops[0].kind != OpKind::Reg || in.ops[0].reg != reg || in.mnemonic == "cmp" || in.mnemonic == "test") continue;
            ++writers;
            is_lea = in.mnemonic == "lea" && in.ops[1].kind == OpKind::Mem && in.ops[1].base == kRip;
            if (is_lea) table = in.ops[1].rip_target;
        }
        return writers == 1 && is_lea;
    };
    for (bool progress = true; progress && !pending.empty();) {
        progress = false;
        std::vector<Pending> still;
        for (Pending& p : pending) {
            JumpTable jt;
            if (recover_jump_table(p.run, code, code_size, code_base, jt, &hoisted_base)) {
                for (uint64_t t : jt.targets) push_target(t);
                fn.jump_tables.emplace(p.addr, std::move(jt));
                progress = true;
            } else {
                still.push_back(std::move(p));
            }
        }
        pending = std::move(still);
        if (progress) drain();  // the newly found cases may hold more jumps
    }
    if (!pending.empty()) {
        fn.has_indirect_jump = true;
        // An indirect jump that could not be resolved (memcpy-style dispatch through a table of absolute addresses, a computed
        // goto) lands on code that nothing else reaches. Decode what the walk skipped: every byte not yet covered starts a
        // walk of its own. Data inside the function decodes into nonsense that is never jumped to; failures there are not errors.
        const bool bad = fn.has_bad_bytes, failed = fn.decode_failed, off_end = fn.fell_off_end;
        const uint64_t bad_at = fn.bad_at;
        for (uint64_t a = start; a < end;) {
            auto it = insns.find(a);
            if (it != insns.end()) {
                a += it->second.len;
                continue;
            }
            leaders.insert(a);
            work.push_back(a);
            drain();
            if (!insns.count(a)) ++a;  // not decodable: skip one byte
        }
        fn.has_bad_bytes = bad, fn.decode_failed = failed, fn.fell_off_end = off_end, fn.bad_at = bad_at;
    }

    for (uint64_t l : leaders) {
        if (!insns.count(l)) continue;
        Block b;
        b.start = l;
        for (uint64_t a = l;;) {
            auto it = insns.find(a);
            if (it == insns.end()) break;  // decoding stopped here (bad bytes)
            const Insn& in = it->second;
            b.insns.push_back(in);
            a += in.len;
            b.end = a;
            if (in.flow == Flow::Ret || in.flow == Flow::Trap) break;
            if (in.flow == Flow::Jump) {
                if (in.direct && inside(in.target)) b.succ.push_back(in.target);
                auto jt = fn.jump_tables.find(in.addr);
                if (jt != fn.jump_tables.end())
                    for (uint64_t t : jt->second.targets)
                        if (std::find(b.succ.begin(), b.succ.end(), t) == b.succ.end()) b.succ.push_back(t);
                break;
            }
            if (in.flow == Flow::CondJump) {
                if (in.direct && inside(in.target)) b.succ.push_back(in.target);
                b.succ.push_back(a);
                break;
            }
            if (leaders.count(a)) {
                b.succ.push_back(a);
                break;
            }
        }
        fn.blocks.emplace(l, std::move(b));
    }
    return fn;
}

namespace {

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    bool need(size_t n) { return ok = ok && size_t(end - p) >= n; }
    uint64_t u(int bytes) {
        if (!need(size_t(bytes))) return 0;
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= uint64_t(p[i]) << (8 * i);
        p += bytes;
        return v;
    }
    uint64_t uleb() {
        uint64_t v = 0;
        for (int shift = 0; need(1) && shift < 64; shift += 7) {
            uint8_t b = *p++;
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) break;
        }
        return v;
    }
    void sleb() { uleb(); }
};

// DW_EH_PE value size for the low nibble of an encoding; 0 = unsupported.
int enc_size(uint8_t enc) {
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: return 8;
    case 0x02: case 0x0a: return 2;
    case 0x03: case 0x0b: return 4;
    default: return 0;
    }
}

int64_t sign_extend(uint64_t v, int bytes) {
    const int sh = 64 - 8 * bytes;
    return int64_t(v << sh) >> sh;
}

// Reads one encoded pointer. `base` is the guest address of the byte being read, for pcrel.
bool read_enc(Reader& r, uint8_t enc, uint64_t base_of_field, uint64_t& out) {
    const int n = enc_size(enc);
    if (!n || (enc & 0x60)) return false;  // only absolute and pcrel are used by .eh_frame function ranges
    uint64_t v = r.u(n);
    if (!r.ok) return false;
    if ((enc & 0x08) || (enc & 0x0f) == 0x0a || (enc & 0x0f) == 0x0b || (enc & 0x0f) == 0x0c)
        v = uint64_t(sign_extend(v, n));
    if ((enc & 0x70) == 0x10) v += base_of_field;
    out = v;
    return true;
}

}  // namespace

std::vector<FdeRange> parse_eh_frame(const uint8_t* data, size_t size, uint64_t vaddr) {
    struct Cie { uint8_t fde_enc = 0; bool z = false; };
    std::map<size_t, Cie> cies;
    std::vector<FdeRange> out;
    size_t pos = 0;
    while (pos + 4 <= size) {
        Reader r{data + pos, data + size};
        uint64_t len = r.u(4);
        if (len == 0) break;  // terminator
        if (len == 0xffffffffu) len = r.u(8);
        if (!r.ok || len > size_t(r.end - r.p)) return {};
        const uint8_t* body = r.p;
        const size_t body_off = size_t(body - data);
        Reader e{body, body + len};
        const uint64_t id = e.u(4);
        if (id == 0) {  // CIE
            Cie c;
            uint8_t version = uint8_t(e.u(1));
            std::string aug;
            while (e.need(1) && *e.p) aug += char(*e.p++);
            e.u(1);  // NUL
            e.uleb(), e.sleb();             // code / data alignment
            version == 1 ? (void)e.u(1) : (void)e.uleb();  // return address register
            if (!aug.empty() && aug[0] == 'z') {
                c.z = true;
                const uint64_t alen = e.uleb();
                Reader a{e.p, e.p + std::min<uint64_t>(alen, size_t(e.end - e.p))};
                for (size_t i = 1; i < aug.size() && a.ok; ++i) {
                    if (aug[i] == 'R') c.fde_enc = uint8_t(a.u(1));
                    else if (aug[i] == 'L') a.u(1);
                    else if (aug[i] == 'P') {
                        uint8_t penc = uint8_t(a.u(1)), n = uint8_t(enc_size(penc));
                        if (!n) return {};
                        a.u(n);
                    }
                }
                if (!a.ok) return {};
            }
            cies[pos] = c;
        } else {  // FDE: id is the distance back from this field to its CIE
            const size_t id_field = body_off;
            if (id > id_field) return {};
            auto it = cies.find(id_field - size_t(id));
            if (it == cies.end()) return {};
            const uint8_t enc = it->second.fde_enc;
            uint64_t begin = 0, range = 0;
            const uint64_t field_va = vaddr + (e.p - data);
            if (!read_enc(e, enc, field_va, begin)) return {};
            if (!read_enc(e, enc & 0x0f, 0, range)) return {};  // the range is never pc-relative
            out.push_back({begin, range});
        }
        pos = body_off + size_t(len);
    }
    return out;
}

}  // namespace dcp::recomp
