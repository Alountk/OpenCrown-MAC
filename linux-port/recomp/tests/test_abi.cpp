// ABI bridge: calls a host C++ function with arguments taken from the guest's registers (SysV x86-64 convention) and puts
// its result back in the guest's registers. Plain C++: runs natively on arm64 and x86_64. No game data.
#include <cstdio>
#include <cstring>

#include "dcp_abi.h"

using namespace dcp::rt;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static int add3(int a, int b, int c) { return a + b + c; }
static i32 negative() { return -5; }
static u64 sum8(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f, u64 g, u64 h) { return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h; }
static double scale(double x, double y) { return x * y; }
static float mix(int a, double b, int c, float d) { return float(a) + float(b) * 10.0f + float(c) * 100.0f + d * 1000.0f; }
static int g_seen = 0;
static void store(int* p, int v) { *p = v; g_seen = v; }
static bool is_big(u64 v) { return v > 100; }
static u8 low_byte(u64 v) { return u8(v); }
static const char* name_of(int i) { return i ? "one" : "zero"; }
static i64 widen(i8 a, u16 b, i32 c) { return i64(a) + b + c; }

// A guest context whose stack holds [return address][stack args...]; r[RSP] points at the return address.
struct Frame {
    Ctx c{};
    u64 stack[16] = {};
    Frame() { c.r[RSP] = reinterpret_cast<u64>(&stack[0]); }
};

static void test_integers() {
    Frame f;
    f.c.r[RDI] = 1, f.c.r[RSI] = 20, f.c.r[RDX] = 300;
    const u64 sp = f.c.r[RSP];
    thunk<add3>(&f.c);
    CHECK(f.c.r[RAX] == 321);
    CHECK(f.c.r[RSP] == sp + 8);  // the call's return address was popped, as the callee's ret would

    Frame g;
    thunk<negative>(&g.c);
    CHECK(g.c.r[RAX] == u64(i64(-5)));

    Frame h;
    h.c.r[RDI] = 0x1ffffffff80ull;  // the upper bits of an int argument are garbage and must be ignored
    h.c.r[RSI] = 0xdead00000005ull;
    h.c.r[RDX] = 0x1234567800000007ull;
    thunk<add3>(&h.c);
    CHECK(h.c.r[RAX] == u64(int(0xffffff80) + 5 + 7));

    Frame w;
    w.c.r[RDI] = 0xfffffff0u;  // i8 -16 from the low byte
    w.c.r[RSI] = 0xabcd0001;   // u16 1
    w.c.r[RDX] = 0xffffffffu;  // i32 -1
    thunk<widen>(&w.c);
    CHECK(i64(w.c.r[RAX]) == -16 + 1 - 1);
}

static void test_stack_arguments() {
    Frame f;
    f.c.r[RDI] = 1, f.c.r[RSI] = 1, f.c.r[RDX] = 1, f.c.r[RCX] = 1, f.c.r[R8] = 1, f.c.r[R9] = 1;
    f.stack[1] = 10;  // seventh argument sits just above the return address
    f.stack[2] = 100;
    thunk<sum8>(&f.c);
    CHECK(f.c.r[RAX] == 1 + 2 + 3 + 4 + 5 + 6 + 70 + 800);
}

static void test_floating_point() {
    Frame f;
    f.c.x[0] = vscalar<u64>(0x4000000000000000ull);  // 2.0
    f.c.x[1] = vscalar<u64>(0x4008000000000000ull);  // 3.0
    thunk<scale>(&f.c);
    CHECK(vlo<u64>(f.c.x[0]) == 0x4018000000000000ull);  // 6.0 in xmm0
    CHECK(f.c.x[0].q[1] == 0 && f.c.x[0].q[2] == 0);

    Frame m;  // ints and floats are numbered separately: a=rdi, c=rsi, b=xmm0, d=xmm1
    m.c.r[RDI] = 1, m.c.r[RSI] = 3;
    m.c.x[0] = vscalar<u64>(0x4000000000000000ull);  // 2.0
    const float d = 4.0f;
    u32 db;
    std::memcpy(&db, &d, 4);
    m.c.x[1] = vscalar<u32>(db);
    thunk<mix>(&m.c);
    float r;
    const u32 rb = vlo<u32>(m.c.x[0]);
    std::memcpy(&r, &rb, 4);
    CHECK(r == 1.0f + 20.0f + 300.0f + 4000.0f);
}

static void test_pointers_and_small_types() {
    int slot = 0;
    Frame f;
    f.c.r[RDI] = reinterpret_cast<u64>(&slot), f.c.r[RSI] = 42;
    thunk<store>(&f.c);
    CHECK(slot == 42 && g_seen == 42);

    Frame b;
    b.c.r[RDI] = 101;
    thunk<is_big>(&b.c);
    CHECK(b.c.r[RAX] == 1);
    Frame b2;
    b2.c.r[RDI] = 100;
    thunk<is_big>(&b2.c);
    CHECK(b2.c.r[RAX] == 0);

    Frame l;
    l.c.r[RDI] = 0x1234;
    thunk<low_byte>(&l.c);
    CHECK(l.c.r[RAX] == 0x34);

    Frame n;
    n.c.r[RDI] = 1;
    thunk<name_of>(&n.c);
    CHECK(std::strcmp(reinterpret_cast<const char*>(n.c.r[RAX]), "one") == 0);
}

// A class that knows how to build itself from a guest register word (the HLE's guest-callback wrapper does).
struct Handle {
    u64 word = 0;
    static Handle from_guest_word(u64 w) { return Handle{w * 2}; }
};
static u64 take_handle(Handle h, int tail) { return h.word + u64(tail); }

static void test_word_constructible_arguments() {
    Frame f;
    f.c.r[RDI] = 21, f.c.r[RSI] = 5;
    thunk<take_handle>(&f.c);
    CHECK(f.c.r[RAX] == 47);  // the handle consumed rdi, the int the next register, rsi
}

int main() {
    test_integers();
    test_stack_arguments();
    test_floating_point();
    test_pointers_and_small_types();
    test_word_constructible_arguments();
    if (g_fail) return std::fprintf(stderr, "abi: %d check(s) failed\n", g_fail), 1;
    std::puts("abi: all tests passed");
    return 0;
}
