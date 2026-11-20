#include "emit.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "lift.h"

namespace dcp::recomp {

bool inside_git_work_tree(const std::string& dir) {
    char real[4096];
    if (!realpath(dir.c_str(), real)) return false;
    std::string p = real;
    for (;;) {
        struct stat sb;
        if (stat((p + "/.git").c_str(), &sb) == 0) return true;
        const size_t slash = p.rfind('/');
        if (slash == std::string::npos || p == "/" || slash == 0) return false;
        p.erase(slash);
    }
}

std::set<uint64_t> parse_hooks(const std::string& text) {
    std::set<uint64_t> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        line = line.substr(0, line.find('#'));
        const size_t a = line.find_first_not_of(" \t\r"), b = line.find_last_not_of(" \t\r");
        if (a == std::string::npos) continue;
        line = line.substr(a, b - a + 1);
        char* end = nullptr;
        const uint64_t v = std::strtoull(line.c_str(), &end, 16);
        if (end == line.c_str() || *end) throw std::runtime_error("hook list: not a hexadecimal offset: " + line);
        out.insert(v);
    }
    return out;
}

static std::vector<uint8_t> parse_hex_bytes(const std::string& hex, const std::string& line) {
    if (hex.empty() || hex.size() % 2) throw std::runtime_error("patch list: bad byte string in: " + line);
    std::vector<uint8_t> out;
    for (size_t i = 0; i < hex.size(); i += 2) {
        char* end = nullptr;
        const std::string pair = hex.substr(i, 2);
        const unsigned long v = std::strtoul(pair.c_str(), &end, 16);
        if (*end || !std::isxdigit((unsigned char)pair[0]) || !std::isxdigit((unsigned char)pair[1]))
            throw std::runtime_error("patch list: not hexadecimal in: " + line);
        out.push_back(uint8_t(v));
    }
    return out;
}

std::map<uint64_t, BytePatch> parse_patches(const std::string& text) {
    std::map<uint64_t, BytePatch> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        line = line.substr(0, line.find('#'));
        std::istringstream in(line);
        std::string off, expect, repl, extra;
        if (!(in >> off)) continue;
        if (off == "site") continue;  // parse_sites
        if (!(in >> expect >> repl) || (in >> extra)) throw std::runtime_error("patch list: expected three fields in: " + line);
        char* end = nullptr;
        const uint64_t v = std::strtoull(off.c_str(), &end, 16);
        if (end == off.c_str() || *end) throw std::runtime_error("patch list: not a hexadecimal offset: " + line);
        BytePatch p{parse_hex_bytes(expect, line), parse_hex_bytes(repl, line)};
        if (p.expect.size() != p.repl.size()) throw std::runtime_error("patch list: lengths differ in: " + line);
        out[v] = std::move(p);
    }
    return out;
}

std::map<uint64_t, SiteHook> parse_sites(const std::string& text) {
    std::map<uint64_t, SiteHook> out;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        line = line.substr(0, line.find('#'));
        std::istringstream in(line);
        std::string kw, off, expect, target, extra;
        if (!(in >> kw) || kw != "site") continue;
        if (!(in >> off >> expect >> target) || (in >> extra)) throw std::runtime_error("site list: expected four fields in: " + line);
        char* end = nullptr;
        const uint64_t v = std::strtoull(off.c_str(), &end, 16);
        if (end == off.c_str() || *end) throw std::runtime_error("site list: not a hexadecimal offset: " + line);
        SiteHook h;
        h.expect = parse_hex_bytes(expect, line);
        if (target == "ret") h.target = kSiteRet;
        else if (target != "-") {
            h.target = std::strtoull(target.c_str(), &end, 16);
            if (end == target.c_str() || *end || h.target == kSiteNoTarget) throw std::runtime_error("site list: bad target in: " + line);
        }
        out[v] = std::move(h);
    }
    return out;
}

// Decodes the replacement of every patch against the module, refusing a patch that does not belong to this build of the game.
static PatchMap build_patch_map(const LoadedModule& m, const std::map<uint64_t, BytePatch>& patches) {
    PatchMap out;
    for (const auto& [off, p] : patches) {
        char at[32];
        std::snprintf(at, sizeof at, "patch at 0x%llx: ", (unsigned long long)off);
        const std::string where = at;
        if (off + p.expect.size() > m.image.size() || std::memcmp(m.image.data() + off, p.expect.data(), p.expect.size()) != 0)
            throw std::runtime_error(where + "the module does not hold the expected bytes");
        Insn orig;
        if (!decode(m.image.data() + off, m.image.size() - off, off, orig) || orig.len != p.expect.size())
            throw std::runtime_error(where + "the expected bytes are not exactly one instruction");
        std::vector<Insn> alt;
        for (size_t at_byte = 0; at_byte < p.repl.size();) {
            Insn a;
            if (!decode(p.repl.data() + at_byte, p.repl.size() - at_byte, off + at_byte, a))
                throw std::runtime_error(where + "the replacement does not decode");
            at_byte += a.len;
            alt.push_back(std::move(a));
        }
        size_t total = 0;
        for (const Insn& a : alt) total += a.len;
        if (total != p.repl.size()) throw std::runtime_error(where + "the replacement does not end on an instruction boundary");
        out[off] = std::move(alt);
    }
    return out;
}

// Checks every site hook against the module (the bytes it expects, exactly one instruction) and returns the lifter's SiteMap.
static SiteMap build_site_map(const LoadedModule& m, const std::map<uint64_t, SiteHook>& sites) {
    SiteMap out;
    for (const auto& [off, h] : sites) {
        char at[32];
        std::snprintf(at, sizeof at, "site at 0x%llx: ", (unsigned long long)off);
        const std::string where = at;
        if (off + h.expect.size() > m.image.size() || std::memcmp(m.image.data() + off, h.expect.data(), h.expect.size()) != 0)
            throw std::runtime_error(where + "the module does not hold the expected bytes");
        Insn orig;
        if (!decode(m.image.data() + off, m.image.size() - off, off, orig) || orig.len != h.expect.size())
            throw std::runtime_error(where + "the expected bytes are not exactly one instruction");
        out[off] = h.target;
    }
    return out;
}

EmitStats emit_module(const LoadedModule& m, const EmitOptions& o) {
    EmitStats st;
    const std::vector<FdeRange> funcs = complete_functions(m);  // .eh_frame ranges plus entry points, PLT stubs and tail-jump targets
    std::set<uint64_t> starts;
    for (const FdeRange& r : funcs) starts.insert(r.start);
    const int shards = std::max(1, o.shards);
    const PatchMap patches = build_patch_map(m, o.patches);
    const SiteMap sites = build_site_map(m, o.sites);
    std::set<uint64_t> sites_hit;
    const std::string head = lift_prelude() + lift_declarations(starts, o.module, o.hooks, patches, sites);

    // Shards get consecutive functions (neighbours call each other), cut when a shard holds its share of the instructions.
    std::vector<std::string> bodies(size_t(shards), head);
    size_t total_insns = 0;
    std::vector<LiftResult> lifted;
    lifted.reserve(funcs.size());
    for (const FdeRange& r : funcs) {
        Function fn = discover_function(m.image.data(), m.image.size(), 0, r.start, r.size);
        lifted.push_back(lift_function(fn, starts, o.module, o.comments, o.hooks, patches, sites));
        sites_hit.insert(lifted.back().sites_hit.begin(), lifted.back().sites_hit.end());
        total_insns += lifted.back().insns;
        st.insns += lifted.back().insns;
        for (const auto& [k, n] : lifted.back().unsupported) st.unsupported += n, st.unsupported_by_mnemonic[k] += n;
    }
    for (const auto& [off, target] : sites)
        if (!sites_hit.count(off)) {
            char msg[80];
            std::snprintf(msg, sizeof msg, "site at 0x%llx: no function holds that instruction", (unsigned long long)off);
            throw std::runtime_error(msg);
        }
    size_t acc = 0, shard = 0;
    const size_t share = total_insns / size_t(shards) + 1;
    for (const LiftResult& lr : lifted) {
        if (acc >= share * (shard + 1) && shard + 1 < size_t(shards)) ++shard;
        bodies[shard] += lr.code + "\n";
        acc += lr.insns;
    }
    st.functions = funcs.size();

    auto write = [&](const std::string& name, const std::string& text) {
        const std::string path = o.out_dir + "/" + name;
        std::ofstream f(path, std::ios::binary);
        if (!f || !(f << text)) throw std::runtime_error("cannot write " + path);
        ++st.files;
        st.bytes += text.size();
    };
    for (int i = 0; i < shards; ++i) {
        char name[256];
        std::snprintf(name, sizeof name, "%s_%03d.cpp", o.module.c_str(), i);
        if (bodies[size_t(i)].size() > head.size() || i == 0) write(name, bodies[size_t(i)]);
    }
    write(o.module + "_table.cpp", lift_prelude() + lift_declarations(starts, o.module, o.hooks, patches, sites) + lift_function_table(starts, o.module, o.hooks, patches, sites));
    return st;
}

}  // namespace dcp::recomp
