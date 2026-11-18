// Runtime dispatch for recompiled code: turns a guest address (or the host pointer that equals it) into a call of the
// recompiled function that starts there, and hands everything else to a "foreign" handler (HLE import stubs, other modules).
#pragma once
#include <cstddef>

#include "dcp_rt.h"

namespace dcp::rt {

using Fn = void (*)(Ctx*);

// One recompiled function: `off` is its guest virtual address (offset from the image base).
struct FnEntry {
    u32 off;
    Fn fn;
};

// Every module (the eboot and each PRX library) has its own image and function table. set_image / set_function_table describe
// the first one; add_module registers further ones. `sorted` must outlive all calls.
void add_module(u64 base, u64 size, const FnEntry* sorted, size_t count);

// Where the image is mapped (guest pointer == host pointer, so a guest address is base + virtual address).
void set_image(u64 base, u64 size);
// Table of every recompiled function, sorted by `off` with no duplicates. Must outlive all calls.
void set_function_table(const FnEntry* sorted, size_t count);
// Called for a target that is not a recompiled function entry: the host pointer is passed through unchanged. The default
// handler reports the address and aborts.
using ForeignHandler = void (*)(Ctx*, u64 target);
void set_foreign_handler(ForeignHandler handler);

// A host function the guest can reach by pointer (an HLE import: the loader stores its address in the guest's GOT).
// `thunk` adapts the guest registers to it (see dcp_abi.h) and pops the return address. Register before running guest code.
void register_host_function(const void* host_fn, Fn thunk);

// The recompiled function whose entry is exactly at host pointer `target`, or nullptr.
Fn find_function(u64 target);

}  // namespace dcp::rt
