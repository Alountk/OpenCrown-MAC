// Host code calling guest code (callbacks, thread entry points) and guest code calling host code that calls back.
// The "guest" functions here are hand-written stand-ins for recompiled ones: they obey the same contract (arguments in
// rdi/rsi, result in rax, `ret` pops the return address). Plain C++: native on arm64 and x86_64. No game data.
#include <cstdio>
#include <thread>

#include "dcp_abi.h"
#include "dcp_dispatch.h"
#include "dcp_guest.h"

using namespace dcp::rt;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

alignas(16) static unsigned char g_image[0x1000];
static int g_tid_calls = 0;

// guest function 0x10: rax = rdi * 2 (and it trashes the caller-saved and a callee-saved register, like real code would
// before restoring it; here it "forgets" to restore rbx to prove the bridge does).
static void guest_double(Ctx* c) {
    c->r[RAX] = c->r[RDI] * 2;
    c->r[RBX] = 0xbad;
    c->r[RCX] = 0xbad;
    c->r[RSP] += 8;
}
// guest function 0x20: rax = host_apply(0x10, rdi) + 1, where host_apply calls guest 0x10 twice
static u64 host_apply(u64 fn, u64 x);
static void guest_caller(Ctx* c) {
    c->r[RSP] -= 8;  // the call pushes a return address
    c->r[RDI] = reinterpret_cast<u64>(g_image) + 0x10, c->r[RSI] = 21;
    c->r[RBX] = 0x1234;  // a callee-saved value that must survive the host call
    call_ind(c, reinterpret_cast<u64>(&host_apply));
    c->r[RAX] += 1;
    c->r[RSP] += 8;
}
static u64 host_apply(u64 fn, u64 x) { return call_guest(fn, x) + call_guest(fn, x + 1); }

// guest function 0x30 calls host_nested, whose callback (guest 0x40) calls the host again: nesting two levels deep
static u64 host_nested(u64 fn, u64 x) { return call_guest(fn, x); }
static void guest_inner(Ctx* c) {  // rax = host_apply(0x10, rdi) + 100
    c->r[RSP] -= 8;
    c->r[RSI] = c->r[RDI], c->r[RDI] = reinterpret_cast<u64>(g_image) + 0x10;
    call_ind(c, reinterpret_cast<u64>(&host_apply));
    c->r[RAX] += 100;
    c->r[RSP] += 8;
}
static void guest_outer(Ctx* c) {  // rax = host_nested(0x40, rdi)
    c->r[RSP] -= 8;
    c->r[RSI] = c->r[RDI], c->r[RDI] = reinterpret_cast<u64>(g_image) + 0x40;
    call_ind(c, reinterpret_cast<u64>(&host_nested));
    c->r[RSP] += 8;
}

static void guest_void(Ctx* c);

static void setup() {
    static const FnEntry table[] = {{0x10, guest_double}, {0x20, guest_caller}, {0x30, guest_outer}, {0x40, guest_inner}, {0x50, guest_void}};
    set_image(reinterpret_cast<u64>(g_image), sizeof g_image);
    set_function_table(table, 5);
    register_host_function(reinterpret_cast<void*>(&host_apply), thunk<host_apply>);
    register_host_function(reinterpret_cast<void*>(&host_nested), thunk<host_nested>);
}

static void test_thread_entry() {
    // A new thread enters guest code with one argument (a pthread-style entry) and gets its result back.
    CHECK(run_guest(reinterpret_cast<u64>(g_image) + 0x10, 7, 1 << 16) == 14);
    // guest_caller: host_apply(0x10, 21) = 42 + 44 = 86, +1 = 87; rbx must have been preserved by the bridge
    CHECK(run_guest(reinterpret_cast<u64>(g_image) + 0x20, 0, 1 << 16) == 87);
    // two levels: outer -> host_nested -> inner -> host_apply -> double: host_apply(0x10, 5) = 10 + 12 = 22, +100 = 122
    CHECK(run_guest(reinterpret_cast<u64>(g_image) + 0x30, 5, 1 << 16) == 122);
}

static void test_register_preservation() {
    // From inside a host function the guest's callee-saved registers and stack pointer are what they were before the call.
    static Ctx seen_before, seen_after;
    struct H {
        static u64 probe(u64 fn) {
            seen_before = *current_ctx();
            const u64 r = call_guest(fn, 3);
            seen_after = *current_ctx();
            return r;
        }
    };
    register_host_function(reinterpret_cast<void*>(&H::probe), thunk<H::probe>);
    alignas(16) static u64 stack[64];
    Ctx c{};
    c.r[RSP] = reinterpret_cast<u64>(&stack[32]);
    c.r[RBX] = 0x1111, c.r[RBP] = 0x2222, c.r[R12] = 0x3333;
    c.r[RDI] = reinterpret_cast<u64>(g_image) + 0x10;
    call_ind(&c, reinterpret_cast<u64>(&H::probe));
    CHECK(c.r[RAX] == 6);
    CHECK(seen_after.r[RBX] == 0x1111 && seen_after.r[RBP] == 0x2222 && seen_after.r[R12] == 0x3333);
    CHECK(seen_after.r[RSP] == seen_before.r[RSP]);
}

// A callback typed the way the HLE declares guest function pointers.
using GuestFn = u64 (*)(u64);
static u64 host_via_pointer(GuestFn cb, u64 x) { return guest_call(cb, x) + 1; }
static void guest_void(Ctx* c) { ++g_tid_calls, c->r[RSP] += 8; }
static u64 host_once(void (*cb)()) { guest_call(cb); guest_call(cb); return 0; }

static void test_typed_calls() {
    register_host_function(reinterpret_cast<void*>(&host_via_pointer), thunk<host_via_pointer>);
    register_host_function(reinterpret_cast<void*>(&host_once), thunk<host_once>);
    const GuestFn dbl = reinterpret_cast<GuestFn>(reinterpret_cast<u64>(g_image) + 0x10);
    // outside any host call (a thread's entry point): a fresh register file and stack
    CHECK(guest_call(dbl, u64(9)) == 18);
    // inside a host function the guest called: on the caller's own context
    alignas(16) static u64 stack[64];
    Ctx c{};
    c.r[RSP] = reinterpret_cast<u64>(&stack[32]);
    c.r[RDI] = reinterpret_cast<u64>(dbl), c.r[RSI] = 20;
    call_ind(&c, reinterpret_cast<u64>(&host_via_pointer));
    CHECK(c.r[RAX] == 41);
    // void callbacks without arguments (scePthreadOnce)
    c.r[RSP] = reinterpret_cast<u64>(&stack[32]);
    c.r[RDI] = reinterpret_cast<u64>(g_image) + 0x50;
    call_ind(&c, reinterpret_cast<u64>(&host_once));
    CHECK(g_tid_calls == 2);
}

// guest function 0x60: rax = rdi * 1000 + rsi * 10 + rdx (an entry point with a second and third argument)
static void guest_three(Ctx* c) { c->r[RAX] = c->r[RDI] * 1000 + c->r[RSI] * 10 + c->r[RDX], c->r[RSP] += 8; }

static void test_entry_with_arguments() {
    static const FnEntry table[] = {{0x60, guest_three}};
    add_module(reinterpret_cast<u64>(g_image) + 0x800, 0x100, table, 1);  // a separate module so the others stay as set up
    const u64 fn = reinterpret_cast<u64>(g_image) + 0x800 + 0x60;
    CHECK(run_guest(fn, 4, 1 << 16, 5, 6) == 4056);  // the process entry point takes (params, exit callback)
    CHECK(run_guest(fn, 1, 1 << 16) == 1000);
}

static void test_concurrent_threads() {
    // Each thread has its own Ctx and guest stack.
    std::thread t[4];
    u64 out[4] = {};
    for (int i = 0; i < 4; ++i)
        t[i] = std::thread([&, i] { for (int k = 0; k < 200; ++k) out[i] += run_guest(reinterpret_cast<u64>(g_image) + 0x20, 0, 1 << 16); });
    for (auto& x : t) x.join();
    for (u64 v : out) CHECK(v == 200 * 87);
}

int main() {
    setup();
    test_thread_entry();
    test_register_preservation();
    test_typed_calls();
    test_entry_with_arguments();
    test_concurrent_threads();
    if (g_fail) return std::fprintf(stderr, "guest: %d check(s) failed\n", g_fail), 1;
    std::puts("guest: all tests passed");
    return 0;
}
