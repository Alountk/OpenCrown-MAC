// ABI bridge: lets recompiled guest code call host C++ functions. The guest follows the SysV x86-64 convention (integer
// arguments in rdi rsi rdx rcx r8 r9, floating-point ones in xmm0-7, the rest on the stack, results in rax / xmm0), which
// has nothing to do with the host's own convention (Apple arm64 puts variadic arguments on the stack, for one). Instead of a
// hand-written adapter per function, `thunk<fn>` is generated from the function's own type.
//
//   void sceFoo(Ctx* c) = thunk<hle_foo>;  // reads rdi/rsi/... from c, calls hle_foo, writes rax, pops the return address
//   observer<hle_foo>                      // same arguments, but nothing is written and nothing popped (see EntryHook)
//
// Supported: integers, enums, bool, pointers, float and double, as arguments and results; and, as arguments, any class with a
// static from_guest_word(u64) (the HLE's wrapper for guest function pointers). No structs by value, no varargs.
#pragma once
#include <tuple>
#include <type_traits>

#include "dcp_rt.h"

namespace dcp::rt {
namespace abi_detail {

constexpr int kIntRegs[6] = {RDI, RSI, RDX, RCX, R8, R9};

struct Cursor {
    Ctx* c;
    int gp = 0, fp = 0, stack = 0;
    // Stack arguments start just above the return address, which r[RSP] points at when the callee is entered.
    u64 next_stack() { return ld<u64>(c->r[RSP] + 8 + 8 * u64(stack++)); }
};

template <class T> T next_arg(Cursor& a) {
    if constexpr (std::is_floating_point_v<T>) {
        if (a.fp < 8) {
            T v;
            std::memcpy(&v, a.c->x[a.fp++].q, sizeof v);
            return v;
        }
        const u64 raw = a.next_stack();
        T v;
        std::memcpy(&v, &raw, sizeof v);
        return v;
    } else {
        static_assert(std::is_integral_v<T> || std::is_pointer_v<T> || std::is_enum_v<T> || requires(u64 w) { T::from_guest_word(w); },
                      "the ABI bridge handles integers, enums, bool, pointers, float, double and types with from_guest_word(u64)");
        const u64 raw = a.gp < 6 ? a.c->r[kIntRegs[a.gp++]] : a.next_stack();
        if constexpr (requires { T::from_guest_word(raw); }) return T::from_guest_word(raw);  // e.g. a guest callback wrapper
        else if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(raw);
        else if constexpr (std::is_same_v<T, bool>) return (raw & 0xff) != 0;  // only the low byte of a bool is defined
        else if constexpr (std::is_enum_v<T>) return static_cast<T>(static_cast<std::underlying_type_t<T>>(raw));
        else return static_cast<T>(raw);  // the upper bits of a narrower integer are garbage: truncate
    }
}

template <class R> void set_result(Ctx* c, R r) {
    if constexpr (std::is_floating_point_v<R>) {
        u64 bits = 0;
        std::memcpy(&bits, &r, sizeof r);
        c->x[0] = vscalar<u64>(bits);
    } else if constexpr (std::is_pointer_v<R>) {
        c->r[RAX] = reinterpret_cast<u64>(r);
    } else if constexpr (std::is_enum_v<R>) {
        c->r[RAX] = static_cast<u64>(static_cast<std::underlying_type_t<R>>(r));
    } else if constexpr (std::is_signed_v<R>) {
        c->r[RAX] = static_cast<u64>(static_cast<i64>(r));
    } else {
        c->r[RAX] = static_cast<u64>(r);
    }
}

template <bool Return, class R, class... A> void call(Ctx* c, R (*fn)(A...)) {
    struct Scope {  // lets the host function call back into guest code (call_guest)
        Ctx* prev;
        explicit Scope(Ctx* x) : prev(g_current_ctx) { g_current_ctx = x; }
        ~Scope() { g_current_ctx = prev; }
    } scope(c);
    Cursor cur{c};
    std::tuple<A...> args{next_arg<A>(cur)...};  // braced initialisation evaluates left to right
    if constexpr (std::is_void_v<R> || !Return) std::apply(fn, args);
    else set_result(c, std::apply(fn, args));
    if constexpr (Return) c->r[RSP] += 8;  // the callee's ret: the guest's return address is gone, execution resumes after the call
}

}  // namespace abi_detail

template <auto Fn> void thunk(Ctx* c) { abi_detail::call<true>(c, Fn); }

// For an entry hook (EntryHook): the same argument reading, but the host function only observes. The result is dropped and the
// guest's return address stays, because the hooked function's own body runs next.
template <auto Fn> void observer(Ctx* c) { abi_detail::call<false>(c, Fn); }

}  // namespace dcp::rt
