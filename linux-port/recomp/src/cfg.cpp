#include "cfg.h"

#include <algorithm>

namespace dcp::recomp {

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

    while (!work.empty()) {
        uint64_t a = work.back();
        work.pop_back();
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
            const uint64_t next = a + in.len;
            if (in.flow == Flow::Ret || in.flow == Flow::Trap) break;
            if (in.flow == Flow::Call) {
                if (in.direct) fn.calls.insert(in.target);
            } else if (in.flow == Flow::Jump) {
                if (in.direct) push_target(in.target);
                else fn.has_indirect_jump = true;
                break;
            } else if (in.flow == Flow::CondJump) {
                if (in.direct) push_target(in.target);
                else fn.has_indirect_jump = true;
                leaders.insert(next);
            }
            a = next;
        }
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
