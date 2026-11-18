// Writing a recompiled module as C++ sources: a few shards of function bodies (so they build in parallel) and one table file.
// The output is derived from the user's own game files: it goes to a directory the user chooses, never into this repository.
#pragma once
#include <map>
#include <string>

#include "module.h"

namespace dcp::recomp {

struct EmitOptions {
    std::string module;    // name used in symbols and file names ("eboot", "libc", ...)
    std::string out_dir;   // must exist
    int shards = 8;        // function bodies are split over this many files, balanced by instruction count
    bool comments = false; // keep the "// 0x...: instruction" lines (about 40% more text)
};

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
