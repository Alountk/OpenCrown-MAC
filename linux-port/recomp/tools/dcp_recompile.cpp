// dcp_recompile <module> <out_dir> [--name N] [--shards N] [--comments] [--hooks FILE] [--patches FILE] [--allow-repo]
// Turns a decrypted PS4 module (eboot.bin, a .prx) into C++ sources that run natively. The output is derived from your own
// game files: keep it out of any repository and do not share it.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <cstring>
#include <string>

#include "emit.h"

using namespace dcp::recomp;

static std::string default_name(const std::string& path) {
    std::string base = path.substr(path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/') + 1);
    base = base.substr(0, base.find('.'));
    std::string n;
    for (char c : base) n += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    return n.empty() ? "module" : n;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: dcp_recompile <module> <out_dir> [--name N] [--shards N] [--comments] [--hooks FILE] [--patches FILE] [--allow-repo]\n");
        return 2;
    }
    const std::string path = argv[1];
    EmitOptions o;
    o.out_dir = argv[2];
    o.module = default_name(path);
    bool allow_repo = false;
    std::string patches_file;  // switchable byte patches (see BytePatch)
    std::string hooks_file;  // image offsets of functions the host observes or overrides (see EntryHook)
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--name") && i + 1 < argc) o.module = argv[++i];
        else if (!std::strcmp(argv[i], "--shards") && i + 1 < argc) o.shards = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--comments")) o.comments = true;
        else if (!std::strcmp(argv[i], "--hooks") && i + 1 < argc) hooks_file = argv[++i];
        else if (!std::strcmp(argv[i], "--patches") && i + 1 < argc) patches_file = argv[++i];
        else if (!std::strcmp(argv[i], "--allow-repo")) allow_repo = true;
        else return std::fprintf(stderr, "dcp_recompile: unknown option %s\n", argv[i]), 2;
    }
    if (!allow_repo && inside_git_work_tree(o.out_dir)) {
        std::fprintf(stderr,
                     "dcp_recompile: %s is inside a git work tree. The output is derived from your game files and must not be\n"
                     "committed or shared: pick a directory outside any repository (or pass --allow-repo if you know it is ignored).\n",
                     o.out_dir.c_str());
        return 1;
    }
    try {
        if (!hooks_file.empty()) {
            std::ifstream f(hooks_file);
            if (!f) return std::fprintf(stderr, "dcp_recompile: cannot read %s\n", hooks_file.c_str()), 1;
            o.hooks = parse_hooks(std::string((std::istreambuf_iterator<char>(f)), {}));
        }
        if (!patches_file.empty()) {
            std::ifstream f(patches_file);
            if (!f) return std::fprintf(stderr, "dcp_recompile: cannot read %s\n", patches_file.c_str()), 1;
            o.patches = parse_patches(std::string((std::istreambuf_iterator<char>(f)), {}));
        }
        const LoadedModule m = load_module(path);
        const EmitStats st = emit_module(m, o);
        std::printf("%s: %zu functions, %zu instructions -> %zu files, %.1f MB in %s\n", o.module.c_str(), st.functions, st.insns, st.files,
                    double(st.bytes) / 1e6, o.out_dir.c_str());
        if (st.unsupported) {
            std::printf("  %zu instructions are not supported yet and will trap if executed:\n", st.unsupported);
            for (const auto& [k, n] : st.unsupported_by_mnemonic) std::printf("    %6zu %s\n", n, k.c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "dcp_recompile: %s\n", e.what());
        return 1;
    }
    return 0;
}
