// Host code calling guest code: callbacks an HLE function invokes, and the entry point of a guest thread.
#pragma once
#include <cstddef>

#include "dcp_rt.h"

namespace dcp::rt {

// From inside a host function that the guest called (dcp_abi.h): calls the guest function at host pointer `fn` with up to six
// integer arguments and returns rax. The guest's registers and stack pointer are restored afterwards, so the host function
// returns to the guest exactly as if it had not called back. A `fn` that is not recompiled code (an HLE import) works too.
u64 call_guest(u64 fn, u64 a0 = 0, u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0, u64 a5 = 0);

// Runs guest code from the host on the calling thread (a guest thread's entry point, module start, main): a fresh register
// file and a private guest stack of `stack_size` bytes, `fn(arg)`, returns rax.
u64 run_guest(u64 fn, u64 arg, size_t stack_size);

}  // namespace dcp::rt
