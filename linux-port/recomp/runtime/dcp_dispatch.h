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

// Entry hooks. The generated code of a hooked function (dcp_recompile --hooks) owns one EntryHook slot and registers it by
// module name and image offset; the host installs a hook function there (nullptr removes it). False when no such slot exists.
void register_hook_slot(const char* module, u64 off, EntryHook* slot);
bool set_entry_hook(const char* module, u64 off, EntryHook fn);

// Byte patches (dcp_recompile --patches). The generated code owns one flag per patched instruction, registered by module name and
// image offset: while it is set the replacement instruction runs instead of the original. False when no such patch exists.
void register_patch_slot(const char* module, u64 off, unsigned char* slot);
bool set_patch(const char* module, u64 off, bool on);

// Site hooks (`site` lines of the patch list): one SiteHook slot per hooked instruction, registered like the others. The host function
// runs just before the instruction, may change registers and memory, and returns non-zero to leave for the line's target.
void register_site_slot(const char* module, u64 off, SiteHook* slot);
u64 call_guest(Ctx* c, u64 fn, u64 a0 = 0, u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0, u64 a5 = 0);  // for a hook: calls a guest function
bool set_site_hook(const char* module, u64 off, SiteHook fn);  // fn = nullptr switches it off; false when no such site exists

// A host function the guest can reach by pointer (an HLE import: the loader stores its address in the guest's GOT).
// `thunk` adapts the guest registers to it (see dcp_abi.h) and pops the return address. Register before running guest code.
void register_host_function(const void* host_fn, Fn thunk);

// The recompiled function whose entry is exactly at host pointer `target`, or nullptr.
Fn find_function(u64 target);

}  // namespace dcp::rt
