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
#include "module.h"

using namespace dcp;


int main(int argc, char** argv) {
    if (argc < 2) return std::fprintf(stderr, "usage: dcp_scan <module> [--hist | --indirect [--all [--context | --targets]] | --show-jmp N | --show-fn ADDR | --lift out.cpp]\n"), 2;
    recomp::LoadedModule mod;
    try { mod = recomp::load_module(argv[1]); }
    catch (const std::exception& e) { return std::fprintf(stderr, "scan: %s\n", e.what()), 1; }
    const std::vector<uint8_t>& img = mod.image;
    const uint64_t eh = mod.eh_frame;
    auto& ranges = mod.functions;
    loader::ElfInfo info;
    info.image_size = img.size();

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
        out << recomp::lift_function_table(fns);
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
        size_t table_blocks = 0, table_fns = 0, resolved = 0, entries = 0, unresolved_idiom = 0, tables_checked = 0, tables_fresh = 0, tables_outside = 0, tables_nobound = 0;
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
                            if (unresolved_idiom <= 3 || (argc > 3 && !std::strcmp(argv[3], "--all"))) std::printf("unresolved idiom at 0x%llx (fn 0x%llx)\n", (unsigned long long)last.addr, (unsigned long long)r.start);
                            if (argc > 4 && !std::strcmp(argv[3], "--all") && !std::strcmp(argv[4], "--targets")) {
                                // What the unresolved table would hold if the bounds check in the block before is taken at its word: the
                                // entries, and whether each is already a block of this function (reached some other way) or not.
                                uint64_t table = 0, count = 0;
                                for (auto& in : b.insns) if (in.mnemonic == "lea" && in.ops[1].base == recomp::kRip) table = in.ops[1].rip_target;
                                for (auto& [pa, pb] : fn.blocks) {
                                    bool leads = false;
                                    for (uint64_t sc : pb.succ) leads |= sc == a;
                                    const size_t n = pb.insns.size();
                                    if (!leads || n < 2) continue;
                                    const recomp::Insn& j = pb.insns[n - 1];
                                    const bool above = j.mnemonic == "ja" || j.mnemonic == "jnbe" || j.mnemonic == "jg" || j.mnemonic == "jnle";
                                    size_t m = n - 1;  // the cmp may be followed by moves, which leave the flags alone
                                    while (m > 0 && (pb.insns[m - 1].mnemonic.compare(0, 3, "mov") == 0 || pb.insns[m - 1].mnemonic == "lea")) --m;
                                    if (m == 0) continue;
                                    const recomp::Insn& c = pb.insns[m - 1];
                                    if (above && c.mnemonic == "cmp" && c.nops == 2 && c.ops[1].kind == recomp::OpKind::Imm) count = uint64_t(c.ops[1].imm) + 1;
                                }
                                size_t known = 0, fresh = 0, outside = 0;
                                std::string fresh_list;
                                for (uint64_t k = 0; k < count && table; ++k) {
                                    int32_t rel;
                                    std::memcpy(&rel, img.data() + table + 4 * k, 4);
                                    const uint64_t t = table + uint64_t(int64_t(rel));
                                    if (t < r.start || t >= r.start + r.size) ++outside;
                                    else if (fn.blocks.count(t)) ++known;
                                    else ++fresh, fresh_list += " 0x" + [&] { char h[24]; std::snprintf(h, sizeof h, "%llx", (unsigned long long)t); return std::string(h); }();
                                }
                                ++tables_checked, tables_fresh += fresh != 0, tables_outside += outside != 0, tables_nobound += count == 0;
                                std::printf("  table 0x%llx  entries %llu: already blocks %zu, NOT blocks %zu, outside the function %zu%s\n",
                                            (unsigned long long)table, (unsigned long long)count, known, fresh, outside, fresh_list.c_str());
                            }
                            if (argc > 4 && !std::strcmp(argv[3], "--all") && !std::strcmp(argv[4], "--context")) {
                                // The idiom block and the blocks that reach it (two levels), to see why the table was not recovered.
                                // Text derived from the game: read it, do not paste it into the repo.
                                std::set<uint64_t> seen{a};
                                std::vector<uint64_t> level{a};
                                for (int depth = 0; depth < 3 && !level.empty(); ++depth) {
                                    std::vector<uint64_t> next;
                                    for (uint64_t ba : level) {
                                        std::printf("  %s block 0x%llx\n", depth == 0 ? "idiom" : "pred", (unsigned long long)ba);
                                        for (auto& in : fn.blocks.at(ba).insns) std::printf("     0x%llx: %s\n", (unsigned long long)in.addr, in.text.c_str());
                                        for (auto& [pa, pb] : fn.blocks)
                                            for (uint64_t sc : pb.succ)
                                                if (sc == ba && seen.insert(pa).second) next.push_back(pa);
                                    }
                                    level = next;
                                }
                            }
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
        if (tables_checked)
            std::printf("unresolved tables read at the bound of their check: %zu; with an entry that is not a block of the function: %zu; with an entry outside it: %zu; no bound found: %zu\n",
                        tables_checked, tables_fresh, tables_outside, tables_nobound);
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
    if (argc > 3 && !std::strcmp(argv[2], "--show-fn")) {
        // Every instruction of the function starting at the given address, by block. Text derived from the game: read it, do not commit it.
        const uint64_t at = std::strtoull(argv[3], nullptr, 0);
        for (auto& r : ranges) {
            if (r.start != at) continue;
            recomp::Function fn = recomp::discover_function(img.data(), img.size(), 0, r.start, r.size);
            for (auto& [a, b] : fn.blocks) {
                std::printf("-- block 0x%llx\n", (unsigned long long)a);
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
