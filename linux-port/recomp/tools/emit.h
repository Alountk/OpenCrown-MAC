// Writing a recompiled module as C++ sources: a few shards of function bodies (so they build in parallel) and one table file.
// The output is derived from the user's own game files: it goes to a directory the user chooses, never into this repository.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lift.h"
#include "module.h"

namespace dcp::recomp {

// A byte patch: the instruction at an image offset (`expect` are its bytes in the module) can be replaced at run time by `repl`,
// which must decode to instructions of the same total length.
struct BytePatch {
    std::vector<uint8_t> expect, repl;
};

// A site hook (see lift.h SiteMap): `expect` are the bytes of the one instruction it sits before, `target` as in SiteMap.
struct SiteHook {
    std::vector<uint8_t> expect;
    uint64_t target = kSiteNoTarget;
};

struct EmitOptions {
    std::string module;    // name used in symbols and file names ("eboot", "libc", ...)
    std::string out_dir;   // must exist
    int shards = 8;        // function bodies are split over this many files, balanced by instruction count
    bool comments = false; // keep the "// 0x...: instruction" lines (about 40% more text)
    std::set<uint64_t> hooks;  // image offsets of functions that call an EntryHook slot on entry (offsets that are no function are ignored)
    std::map<uint64_t, SiteHook> sites;     // image offset -> host hook slot called before that instruction (dcp_site_<module>_<hex>)
    std::map<uint64_t, BytePatch> patches;  // image offset -> switchable replacement (dcp_patch_<module>_<hex>), see lift.h PatchMap
};

// A hook list: one hexadecimal image offset per line, `#` starts a comment. Throws std::runtime_error on anything else.
std::set<uint64_t> parse_hooks(const std::string& text);

// A patch list: one patch per line, `<hex offset> <hex expected bytes> <hex replacement bytes>` (both byte strings the same length),
// `#` starts a comment. Throws std::runtime_error on anything else.
std::map<uint64_t, BytePatch> parse_patches(const std::string& text);

// The `site <hex offset> <hex expected bytes> <target>` lines of the same file (target: `-` none, `ret`, or a hex offset; parse_patches
// skips them). Throws std::runtime_error on a malformed line.
std::map<uint64_t, SiteHook> parse_sites(const std::string& text);

struct EmitStats {
    size_t functions = 0, insns = 0, files = 0, unsupported = 0;
    size_t bytes = 0;
    std::map<std::string, size_t> unsupported_by_mnemonic;
};

// True when `dir` is inside a git work tree (an ancestor holds a .git): the output must not go there.
bool inside_git_work_tree(const std::string& dir);

// Writes <module>_000.cpp ... and <module>_table.cpp into out_dir. Throws std::runtime_error when a file cannot be written.
EmitStats emit_module(const LoadedModule& m, const EmitOptions& o);

}  // namespace dcp::recomp
