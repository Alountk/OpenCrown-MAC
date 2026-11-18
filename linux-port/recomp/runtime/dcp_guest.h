// Host code calling guest code: callbacks an HLE function invokes, and the entry point of a guest thread.
#pragma once
#include <cstddef>
#include <type_traits>
#include <utility>

#include "dcp_rt.h"

namespace dcp::rt {

// From inside a host function that the guest called (dcp_abi.h): calls the guest function at host pointer `fn` with up to six
// integer arguments and returns rax. The guest's registers and stack pointer are restored afterwards, so the host function
// returns to the guest exactly as if it had not called back. A `fn` that is not recompiled code (an HLE import) works too.
u64 call_guest(u64 fn, u64 a0 = 0, u64 a1 = 0, u64 a2 = 0, u64 a3 = 0, u64 a4 = 0, u64 a5 = 0);

// Runs guest code from the host on the calling thread (a guest thread's entry point, module start, main): a fresh register
// file and a private guest stack of `stack_size` bytes, `fn(arg, arg2, arg3)`, returns rax.
u64 run_guest(u64 fn, u64 arg, size_t stack_size, u64 arg2 = 0, u64 arg3 = 0);

// Typed form for host code that holds a guest function pointer (`void* (*)(void*)`, `void (*)(uint16_t, ..., void*)`): calls it
// the way call_guest / run_guest would, picking by where it is called from. Integer, enum and pointer arguments and results
// only, at most six arguments. Outside a host call (a thread's entry point) the callee gets a private guest stack.
inline constexpr size_t kDefaultGuestStack = 1 << 20;
// Guest stack size for entry points this thread starts from the host (a thread sets it from its requested stack size).
inline thread_local size_t g_thread_guest_stack = kDefaultGuestStack;
[[noreturn]] void guest_call_outside_host();

template <class R, class... A> R guest_call(R (*fn)(A...), std::type_identity_t<A>... args) {
    static_assert(sizeof...(A) <= 6, "guest calls take at most six integer arguments");
    auto word = [](auto v) -> u64 {
        if constexpr (std::is_pointer_v<decltype(v)>) return reinterpret_cast<u64>(v);
        else return static_cast<u64>(v);
    };
    const u64 target = reinterpret_cast<u64>(fn);
    u64 r;
    if (g_current_ctx) {
        r = call_guest(target, word(args)...);
    } else if constexpr (sizeof...(A) <= 1) {
        const u64 words[] = {word(args)..., 0};  // the entry point's single argument, or 0
        r = run_guest(target, words[0], g_thread_guest_stack);
    } else {
        guest_call_outside_host();  // callbacks with several arguments are only ever fired from inside a guest-called function
        r = 0;
    }
    if constexpr (std::is_void_v<R>) return;
    else if constexpr (std::is_pointer_v<R>) return reinterpret_cast<R>(r);
    else return static_cast<R>(r);
}

}  // namespace dcp::rt
