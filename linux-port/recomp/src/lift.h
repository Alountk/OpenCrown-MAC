// x86-64 -> C++ lifting of one discovered function (integer instructions; SIMD is a later task).
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>

#include "cfg.h"

namespace dcp::recomp {

struct LiftResult {
    std::string code;                          // the C++ definition of f_<hex start>(Ctx*)
    std::map<std::string, size_t> unsupported; // mnemonic (or "mnemonic <form>") -> occurrences left as traps
    size_t insns = 0;
};

// `functions` holds the start addresses of every recompiled function: direct calls to them become direct C++ calls.
LiftResult lift_function(const Function& fn, const std::set<uint64_t>& functions);

// Includes and helper macros every generated translation unit starts with.
std::string lift_prelude();

// One `extern "C" void f_<hex>(Ctx*);` line per function, for the top of every generated translation unit.
std::string lift_declarations(const std::set<uint64_t>& functions);

std::string function_name(uint64_t start);  // "f_<hex>"

}  // namespace dcp::recomp
