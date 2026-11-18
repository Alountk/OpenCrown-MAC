#include "emit.h"

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

EmitStats emit_module(const LoadedModule& m, const EmitOptions& o) {
    EmitStats st;
    const std::vector<FdeRange> funcs = complete_functions(m);  // .eh_frame ranges plus entry points, PLT stubs and tail-jump targets
    std::set<uint64_t> starts;
    for (const FdeRange& r : funcs) starts.insert(r.start);
    const int shards = std::max(1, o.shards);
    const std::string head = lift_prelude() + lift_declarations(starts, o.module);

    // Shards get consecutive functions (neighbours call each other), cut when a shard holds its share of the instructions.
    std::vector<std::string> bodies(size_t(shards), head);
    size_t total_insns = 0;
    std::vector<LiftResult> lifted;
    lifted.reserve(funcs.size());
    for (const FdeRange& r : funcs) {
        Function fn = discover_function(m.image.data(), m.image.size(), 0, r.start, r.size);
        lifted.push_back(lift_function(fn, starts, o.module, o.comments));
        total_insns += lifted.back().insns;
        st.insns += lifted.back().insns;
        for (const auto& [k, n] : lifted.back().unsupported) st.unsupported += n, st.unsupported_by_mnemonic[k] += n;
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
    write(o.module + "_table.cpp", lift_prelude() + lift_declarations(starts, o.module) + lift_function_table(starts, o.module));
    return st;
}

}  // namespace dcp::recomp
