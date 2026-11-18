#include "dcp_guest.h"

#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>

#include "dcp_dispatch.h"

namespace dcp::rt {

u64 call_guest(u64 fn, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    Ctx* c = g_current_ctx;
    if (!c) {
        std::fprintf(stderr, "dcp: call_guest outside a host function called by guest code\n");
        std::abort();
    }
    const Ctx saved = *c;
    // Push a return address below everything the guest has live (its red zone included) and keep the SysV alignment: at
    // function entry rsp + 8 is a multiple of 16. The address itself is never used: recompiled code returns by C++ return.
    u64 sp = (saved.r[RSP] - 256) & ~u64(15);
    sp -= 8;
    st<u64>(sp, 0);
    c->r[RSP] = sp;
    c->r[RDI] = a0, c->r[RSI] = a1, c->r[RDX] = a2, c->r[RCX] = a3, c->r[R8] = a4, c->r[R9] = a5;
    call_ind(c, fn);
    const u64 result = c->r[RAX];
    *c = saved;  // callee-saved registers, rsp and everything else are the caller's again
    return result;
}

void guest_call_outside_host() {
    std::fprintf(stderr, "dcp: a guest callback with several arguments was called outside a host call\n");
    std::abort();
}

u64 run_guest(u64 fn, u64 arg, size_t stack_size) {
    constexpr size_t kGuard = 1 << 14;  // one page of any size in use on macOS and Linux
    void* mem = mmap(nullptr, stack_size + kGuard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        std::fprintf(stderr, "dcp: cannot allocate a %zu byte guest stack\n", stack_size);
        std::abort();
    }
    mprotect(mem, kGuard, PROT_NONE);  // overflow faults instead of corrupting memory
    Ctx c{};
    u64 sp = (reinterpret_cast<u64>(mem) + kGuard + stack_size) & ~u64(15);
    sp -= 8;
    st<u64>(sp, 0);
    c.r[RSP] = sp;
    c.r[RDI] = arg;
    Ctx* const prev = g_current_ctx;
    g_current_ctx = &c;
    call_ind(&c, fn);
    g_current_ctx = prev;
    const u64 result = c.r[RAX];
    munmap(mem, stack_size + kGuard);
    return result;
}

}  // namespace dcp::rt
