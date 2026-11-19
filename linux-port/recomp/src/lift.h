// x86-64 -> C++ lifting of one discovered function (integer instructions; SIMD is a later task).
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "cfg.h"

namespace dcp::recomp {

// Byte patches: image offset of an instruction -> the instruction(s) that replace it while the patch flag is set. The lifter
// emits both versions behind `dcp_patch_<module>_<hex>` (declared by lift_declarations, defined and registered with
// register_patch_slot by lift_function_table), so the host switches a patch on or off at run time.
using PatchMap = std::map<uint64_t, std::vector<Insn>>;

struct LiftResult {
    std::string code;                          // the C++ definition of f_<hex start>(Ctx*)
    std::map<std::string, size_t> unsupported; // mnemonic (or "mnemonic <form>") -> occurrences left as traps
    size_t insns = 0;
};

// `functions` holds the start addresses of every recompiled function: direct calls to them become direct C++ calls.
// `module` names the module the function belongs to ("" = the only one): its symbols are f_<module>_<hex> and it addresses its
// image through dcp_img_<module>, so several modules can share one program.
// `comments` keeps the "// 0x...: mov eax, 5" line before every instruction (readable, about 40% larger).
// `hooks` lists functions that call a host EntryHook slot (dcp_hook_<module>_<hex>) before their body, see lift_declarations.
LiftResult lift_function(const Function& fn, const std::set<uint64_t>& functions, const std::string& module = "", bool comments = true,
                         const std::set<uint64_t>& hooks = {}, const PatchMap& patches = {});

// Includes and helper macros every generated translation unit starts with.
std::string lift_prelude();

// One `extern "C" void f_<hex>(Ctx*);` line per function, for the top of every generated translation unit.
// A hooked function also gets `extern dcp::rt::EntryHook dcp_hook_<hex>;`.
std::string lift_declarations(const std::set<uint64_t>& functions, const std::string& module = "", const std::set<uint64_t>& hooks = {},
                              const PatchMap& patches = {});

// The sorted function table and `dcp_register(image_base, image_size)`, which installs it in the dispatch runtime. Goes after
// lift_prelude() and lift_declarations() in one translation unit.
// It also defines the slot of every hooked function and registers it with register_hook_slot (module name, image offset).
std::string lift_function_table(const std::set<uint64_t>& functions, const std::string& module = "", const std::set<uint64_t>& hooks = {},
                                const PatchMap& patches = {});

std::string function_name(uint64_t start, const std::string& module = "");  // "f_<hex>" or "f_<module>_<hex>"

}  // namespace dcp::recomp
