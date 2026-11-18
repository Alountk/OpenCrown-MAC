// dcp_scan <eboot.bin|prx>: function and block statistics for a decrypted PS4 module. Reads the file, prints counts only.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <vector>

#include "cfg.h"
#include "lift.h"
#include "loader/elf.h"

using namespace dcp;

static uint64_t rd(const uint8_t* p, int n) { uint64_t v = 0; std::memcpy(&v, p, size_t(n)); return v; }

int main(int argc, char** argv) {
    if (argc < 2) return std::fprintf(stderr, "usage: dcp_scan <module>\n"), 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
    std::vector<uint8_t> elf;
    loader::ElfInfo info;
    try { elf = loader::unwrap_self(file); info = loader::parse_elf(elf); }
    catch (const std::exception& e) { return std::fprintf(stderr, "scan: %s\n", e.what()), 1; }

    std::vector<uint8_t> img(info.image_size, 0);  // image laid out at base 0, like map_image
    for (const auto& s : info.load)
        if (s.offset + s.filesz <= elf.size() && s.vaddr + s.filesz <= img.size())
            std::memcpy(img.data() + s.vaddr, elf.data() + s.offset, s.filesz);

    // PT_GNU_EH_FRAME (0x6474e550) -> .eh_frame_hdr -> .eh_frame
    const uint64_t phoff = rd(elf.data() + 0x20, 8);
    const unsigned phentsize = unsigned(rd(elf.data() + 0x36, 2)), phnum = unsigned(rd(elf.data() + 0x38, 2));
    uint64_t hdr = 0;
    for (unsigned i = 0; i < phnum; ++i) {
        const uint8_t* ph = elf.data() + phoff + uint64_t(i) * phentsize;
        if (rd(ph, 4) == 0x6474e550) hdr = rd(ph + 16, 8);
    }
    if (!hdr || hdr + 8 > img.size()) return std::fprintf(stderr, "scan: no PT_GNU_EH_FRAME\n"), 1;
    const int32_t rel = int32_t(rd(img.data() + hdr + 4, 4));  // eh_frame_ptr, pcrel sdata4 (checked below)
    if (img[hdr + 1] != 0x1b) return std::fprintf(stderr, "scan: unexpected eh_frame_ptr encoding 0x%x\n", img[hdr + 1]), 1;
    const uint64_t eh = hdr + 4 + uint64_t(int64_t(rel));
    auto ranges = recomp::parse_eh_frame(img.data() + eh, img.size() - eh, eh);

    size_t blocks = 0, insns = 0, bad = 0, indirect = 0, tails = 0;
    std::set<uint64_t> starts, call_targets;
    for (auto& r : ranges) starts.insert(r.start);
    size_t calls_outside = 0, dec_fail = 0, off_end = 0, shown = 0;
    std::map<std::string, size_t> hist;
    for (auto& r : ranges) {
        recomp::Function fn = recomp::discover_function(img.data(), img.size(), 0, r.start, r.size);
        blocks += fn.blocks.size();
        for (auto& [a, b] : fn.blocks) {
            insns += b.insns.size();
            for (auto& in : b.insns) ++hist[in.mnemonic];
        }
        dec_fail += fn.decode_failed, off_end += fn.fell_off_end;
        if (fn.has_bad_bytes && shown < 8) {
            ++shown;
            recomp::Insn in;
            const bool ok = recomp::decode(img.data() + fn.bad_at, 15, fn.bad_at, in);
            std::printf("  bad fn 0x%llx+0x%llx at +0x%llx %s: %02x %02x %02x %02x %s\n", (unsigned long long)r.start,
                        (unsigned long long)r.size, (unsigned long long)(fn.bad_at - r.start),
                        fn.decode_failed ? "undecodable" : "fell off end", img[fn.bad_at], img[fn.bad_at + 1],
                        img[fn.bad_at + 2], img[fn.bad_at + 3], ok ? in.text.c_str() : "");
        }
        bad += fn.has_bad_bytes, indirect += fn.has_indirect_jump, tails += fn.tail_jumps.size();
        for (uint64_t t : fn.calls) if (!starts.count(t)) ++calls_outside, call_targets.insert(t);
    }
    std::printf("image 0x%llx  eh_frame 0x%llx\n", (unsigned long long)info.image_size, (unsigned long long)eh);
    std::printf("functions %zu  blocks %zu  insns %zu\n", ranges.size(), blocks, insns);
    std::printf("functions with bad bytes %zu  with indirect jumps %zu  tail jumps %zu\n", bad, indirect, tails);
    std::printf("undecodable %zu  fell off end %zu\n", dec_fail, off_end);
    std::printf("direct calls to addresses that are not an FDE start: %zu (%zu distinct)\n", calls_outside, call_targets.size());
    if (argc > 3 && !std::strcmp(argv[2], "--lift")) {
        // Lift every function into one file (put it OUTSIDE the repo: it is derived from the game) and report coverage.
        std::set<uint64_t> fns;
        for (auto& r : ranges) fns.insert(r.start);
        std::ofstream out(argv[3]);
        out << recomp::lift_prelude() << recomp::lift_declarations(fns);
        std::map<std::string, size_t> unsup;
        size_t lifted = 0, with_unsup = 0, total = 0, bytes = 0;
        for (auto& r : ranges) {
            recomp::Function fn = recomp::discover_function(img.data(), img.size(), 0, r.start, r.size);
            recomp::LiftResult lr = recomp::lift_function(fn, fns);
            out << lr.code << "\n";
            bytes += lr.code.size(), total += lr.insns, ++lifted;
            if (!lr.unsupported.empty()) ++with_unsup;
            for (auto& [k, n] : lr.unsupported) unsup[k] += n;
        }
        size_t nu = 0;
        for (auto& [k, n] : unsup) nu += n;
        std::printf("lifted %zu functions, %zu insns, %zu bytes of C++; unsupported insns %zu (%.3f%%) in %zu functions\n", lifted,
                    total, bytes, nu, total ? 100.0 * double(nu) / double(total) : 0.0, with_unsup);
        std::vector<std::pair<size_t, std::string>> v;
        for (auto& [k, n] : unsup) v.push_back({n, k});
        std::sort(v.rbegin(), v.rend());
        for (auto& [n, k] : v) std::printf("  %8zu %s\n", n, k.c_str());
    }
    if (argc > 2 && !std::strcmp(argv[2], "--indirect")) {
        // How indirect control transfers look, to decide how to recompile them. Counts only.
        auto kind = [](const recomp::Operand& o) -> std::string {
            if (o.kind == recomp::OpKind::Reg) return "reg";
            if (o.kind != recomp::OpKind::Mem) return "other";
            if (o.base == recomp::kRip) return "[rip+disp]";
            return o.index != recomp::kNoReg ? "[base+index*s+disp]" : "[base+disp]";
        };
        std::map<std::string, size_t> cls;
        size_t table_blocks = 0, table_fns = 0, resolved = 0, entries = 0, unresolved_idiom = 0;
        for (auto& r : ranges) {
            recomp::Function fn = recomp::discover_function(img.data(), img.size(), 0, r.start, r.size);
            bool fn_table = false;
            resolved += fn.jump_tables.size();
            for (auto& [ja, jt] : fn.jump_tables) entries += jt.targets.size();
            for (auto& [a, b] : fn.blocks) {
                for (auto& in : b.insns) {
                    if ((in.flow == recomp::Flow::Call || in.flow == recomp::Flow::Jump) && !in.direct && in.nops)
                        ++cls[in.mnemonic + " " + kind(in.ops[0])];
                }
                const recomp::Insn& last = b.insns.back();
                if (last.flow == recomp::Flow::Jump && !last.direct && last.ops[0].kind == recomp::OpKind::Reg) {
                    bool lea = false, movsxd = false, add = false;
                    for (auto& in : b.insns) {
                        lea |= in.mnemonic == "lea" && in.ops[1].base == recomp::kRip;
                        movsxd |= in.mnemonic == "movsxd";
                        add |= in.mnemonic == "add";
                    }
                    if (lea && movsxd && add) {
                        ++table_blocks, fn_table = true;
                        if (!fn.jump_tables.count(last.addr)) {
                            ++unresolved_idiom;
                            if (unresolved_idiom <= 3) std::printf("unresolved idiom at 0x%llx (fn 0x%llx)\n", (unsigned long long)last.addr, (unsigned long long)r.start);
                        }
                    }
                }
            }
            table_fns += fn_table;
        }
        {
            // Direct jumps that leave their function: to another function start, or into the middle of one (split cold code).
            size_t to_start = 0, to_other = 0, inside_other = 0;
            std::set<uint64_t> odd;
            for (auto& r : ranges) {
                recomp::Function fn = recomp::discover_function(img.data(), img.size(), 0, r.start, r.size);
                for (uint64_t t : fn.tail_jumps) {
                    if (starts.count(t)) { ++to_start; continue; }
                    ++to_other, odd.insert(t);
                    auto it = std::upper_bound(ranges.begin(), ranges.end(), t, [](uint64_t v, const recomp::FdeRange& x) { return v < x.start; });
                    if (it != ranges.begin() && t < (it - 1)->start + (it - 1)->size) ++inside_other;
                }
            }
            std::printf("tail jumps: %zu to a function start, %zu elsewhere (%zu distinct; %zu inside another function's range)\n",
                        to_start, to_other, odd.size(), inside_other);
        }
        for (auto& [k, n] : cls) std::printf("%8zu %s\n", n, k.c_str());
        std::printf("blocks ending in the lea/movsxd/add/jmp reg jump-table idiom: %zu (in %zu functions)\n", table_blocks, table_fns);
        std::printf("jump tables resolved: %zu (%zu entries); idiom blocks left unresolved: %zu\n", resolved, entries, unresolved_idiom);
    }
    if (argc > 3 && !std::strcmp(argv[2], "--show-jmp")) {
        // Prints the instructions leading to the first N `jmp reg` blocks (and the blocks that branch into them), to study
        // the jump-table idiom. The text is derived from the game: look at it, do not paste it into the repo.
        int left = std::atoi(argv[3]);
        for (auto& r : ranges) {
            recomp::Function fn = recomp::discover_function(img.data(), img.size(), 0, r.start, r.size);
            for (auto& [a, b] : fn.blocks) {
                const recomp::Insn& last = b.insns.back();
                if (left <= 0 || !(last.flow == recomp::Flow::Jump && !last.direct && last.ops[0].kind == recomp::OpKind::Reg)) continue;
                --left;
                std::printf("-- fn 0x%llx block 0x%llx\n", (unsigned long long)r.start, (unsigned long long)a);
                for (auto& [pa, pb] : fn.blocks)
                    for (uint64_t sc : pb.succ)
                        if (sc == a) for (auto& in : pb.insns) std::printf("   pred 0x%llx: %s\n", (unsigned long long)in.addr, in.text.c_str());
                for (auto& in : b.insns) std::printf("   0x%llx: %s\n", (unsigned long long)in.addr, in.text.c_str());
            }
        }
    }
    if (argc > 2 && !std::strcmp(argv[2], "--hist")) {
        std::vector<std::pair<size_t, std::string>> v;
        for (auto& [m, n] : hist) v.push_back({n, m});
        std::sort(v.rbegin(), v.rend());
        for (auto& [n, m] : v) std::printf("%8zu %s\n", n, m.c_str());
    }
    return 0;
}
