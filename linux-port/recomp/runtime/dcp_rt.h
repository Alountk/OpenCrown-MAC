// Runtime for recompiled guest code: guest context, x86-64 flag semantics and integer instruction helpers.
// Generated functions include this header. Everything here is header-only so the C++ compiler can inline it and
// delete the flags nobody reads (that is what makes the flags "lazy": they are plain locals).
#pragma once
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace dcp::rt {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using u128 = unsigned __int128;
using i128 = __int128;

// Guest register file at function boundaries. r[] is in x86 encoding order: rax rcx rdx rbx rsp rbp rsi rdi r8..r15.
// A 256-bit vector register: ymm n, whose low half is xmm n.
struct Vec {
    u64 q[4];
};

struct Ctx {
    u64 r[16];
    u64 rflags;  // only meaningful when built with DCP_TEST_FLAGS (the differential tests)
    u64 pad_[3];
    Vec x[16];   // ymm0..15
};
static_assert(__builtin_offsetof(Ctx, x) == 160, "the differential test's assembly trampoline hard-codes this layout");

enum Reg { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

// Dispatch for calls whose target is not a statically known recompiled function (T4 provides the definitions).
void call_va(Ctx* c, u64 guest_va);   // direct call to a guest virtual address (import stubs and the like)
void call_ind(Ctx* c, u64 target);    // indirect call through a host pointer
void jmp_ind(Ctx* c, u64 target);     // indirect jump (jump tables, tail dispatch): never returns to the caller

// Base address of the loaded image: guest virtual address + this = host pointer (guest pointers are host pointers).
extern u64 dcp_img;

struct Flags {
    bool cf = false, zf = false, sf = false, of = false, pf = false, af = false;
};

// ---- registers and memory -------------------------------------------------------------------------------------

template <class T> inline T rd(u64 r) { return T(r); }
// Writes follow x86-64: 32-bit writes zero-extend, 8 and 16-bit writes merge into the low bits.
template <class T> inline void wr(u64& r, T v) {
    if constexpr (sizeof(T) == 8) r = v;
    else if constexpr (sizeof(T) == 4) r = u64(u32(v));
    else if constexpr (sizeof(T) == 2) r = (r & ~u64(0xffff)) | u64(u16(v));
    else r = (r & ~u64(0xff)) | u64(u8(v));
}
inline u8 rdh(u64 r) { return u8(r >> 8); }  // ah ch dh bh
inline void wrh(u64& r, u8 v) { r = (r & ~u64(0xff00)) | (u64(v) << 8); }

template <class T> inline T ld(u64 a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof v);
    return v;
}
template <class T> inline void st(u64 a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof v); }

// ---- flags ------------------------------------------------------------------------------------------------------

template <class T> constexpr int bits_of = int(sizeof(T) * 8);
template <class T> using S = std::make_signed_t<T>;

inline bool parity8(u64 v) { return (__builtin_popcount(unsigned(v & 0xff)) & 1) == 0; }
template <class T> inline bool msb(T v) { return (v >> (bits_of<T> - 1)) & 1; }

template <class T> inline void zsp(T res, Flags& f) {
    f.zf = res == 0;
    f.sf = msb(res);
    f.pf = parity8(res);
}

inline u64 pack_flags(const Flags& f) {
    return 2 | (u64(f.cf) << 0) | (u64(f.pf) << 2) | (u64(f.af) << 4) | (u64(f.zf) << 6) | (u64(f.sf) << 7) |
           (u64(f.of) << 11);
}
inline Flags unpack_flags(u64 v) {
    Flags f;
    f.cf = v >> 0 & 1, f.pf = v >> 2 & 1, f.af = v >> 4 & 1, f.zf = v >> 6 & 1, f.sf = v >> 7 & 1, f.of = v >> 11 & 1;
    return f;
}

// ---- condition codes (Intel names: z nz b nb be nbe l nl le nle s ns p np o no) -----------------------------------

inline bool cc_z(const Flags& f) { return f.zf; }
inline bool cc_nz(const Flags& f) { return !f.zf; }
inline bool cc_b(const Flags& f) { return f.cf; }
inline bool cc_nb(const Flags& f) { return !f.cf; }
inline bool cc_be(const Flags& f) { return f.cf || f.zf; }
inline bool cc_nbe(const Flags& f) { return !f.cf && !f.zf; }
inline bool cc_l(const Flags& f) { return f.sf != f.of; }
inline bool cc_nl(const Flags& f) { return f.sf == f.of; }
inline bool cc_le(const Flags& f) { return f.zf || f.sf != f.of; }
inline bool cc_nle(const Flags& f) { return !f.zf && f.sf == f.of; }
inline bool cc_s(const Flags& f) { return f.sf; }
inline bool cc_ns(const Flags& f) { return !f.sf; }
inline bool cc_p(const Flags& f) { return f.pf; }
inline bool cc_np(const Flags& f) { return !f.pf; }
inline bool cc_o(const Flags& f) { return f.of; }
inline bool cc_no(const Flags& f) { return !f.of; }

// ---- arithmetic -------------------------------------------------------------------------------------------------

template <class T> inline T add(T a, T b, Flags& f) {
    const T r = T(a + b);
    f.cf = r < a;
    f.of = msb(T((a ^ r) & (b ^ r)));
    f.af = ((a ^ b ^ r) & 0x10) != 0;
    zsp(r, f);
    return r;
}
template <class T> inline T adc(T a, T b, Flags& f) {
    const T c = f.cf;
    const T r = T(a + b + c);
    f.cf = c ? r <= a : r < a;
    f.of = msb(T((a ^ r) & (b ^ r)));
    f.af = ((a ^ b ^ r) & 0x10) != 0;
    zsp(r, f);
    return r;
}
template <class T> inline T sub(T a, T b, Flags& f) {
    const T r = T(a - b);
    f.cf = a < b;
    f.of = msb(T((a ^ b) & (a ^ r)));
    f.af = ((a ^ b ^ r) & 0x10) != 0;
    zsp(r, f);
    return r;
}
template <class T> inline T sbb(T a, T b, Flags& f) {
    const T c = f.cf;
    const T r = T(a - b - c);
    f.cf = c ? a <= b : a < b;
    f.of = msb(T((a ^ b) & (a ^ r)));
    f.af = ((a ^ b ^ r) & 0x10) != 0;
    zsp(r, f);
    return r;
}
template <class T> inline T logic(T r, Flags& f) {  // and / or / xor / test: AF is undefined, cleared here
    f.cf = f.of = f.af = false;
    zsp(r, f);
    return r;
}
template <class T> inline T inc(T a, Flags& f) {  // CF is preserved
    const T r = T(a + 1);
    f.of = r == T(T(1) << (bits_of<T> - 1));
    f.af = ((a ^ 1 ^ r) & 0x10) != 0;
    zsp(r, f);
    return r;
}
template <class T> inline T dec(T a, Flags& f) {
    const T r = T(a - 1);
    f.of = a == T(T(1) << (bits_of<T> - 1));
    f.af = ((a ^ 1 ^ r) & 0x10) != 0;
    zsp(r, f);
    return r;
}
template <class T> inline T neg(T a, Flags& f) {
    const T r = T(T(0) - a);
    f.cf = a != 0;
    f.of = a == T(T(1) << (bits_of<T> - 1));
    f.af = ((a ^ r) & 0x10) != 0;
    zsp(r, f);
    return r;
}

// ---- shifts and rotates (count is masked to 5 bits, 6 for 64-bit operands; a masked count of 0 changes nothing) -----

template <class T> inline int count_mask(u8 n) { return n & (sizeof(T) == 8 ? 63 : 31); }

template <class T> inline T shl(T a, u8 n, Flags& f) {
    const int c = count_mask<T>(n);
    if (!c) return a;
    const T r = c >= bits_of<T> ? T(0) : T(a << c);
    f.cf = c > bits_of<T> ? false : ((a >> (bits_of<T> - c)) & 1);
    f.of = msb(r) != f.cf;  // only defined for c == 1
    f.af = false;
    zsp(r, f);
    return r;
}
template <class T> inline T shr(T a, u8 n, Flags& f) {
    const int c = count_mask<T>(n);
    if (!c) return a;
    const T r = c >= bits_of<T> ? T(0) : T(a >> c);
    f.cf = c > bits_of<T> ? false : ((a >> (c - 1)) & 1);
    f.of = msb(a);  // only defined for c == 1
    f.af = false;
    zsp(r, f);
    return r;
}
template <class T> inline T sar(T a, u8 n, Flags& f) {
    const int c = count_mask<T>(n);
    if (!c) return a;
    const S<T> sa = S<T>(a);
    const int cc = c >= bits_of<T> ? bits_of<T> - 1 : c;
    const T r = T(sa >> cc);
    f.cf = c >= bits_of<T> ? msb(a) : (T(sa >> (c - 1)) & 1);
    f.of = false;
    f.af = false;
    zsp(r, f);
    return r;
}
template <class T> inline T rol(T a, u8 n, Flags& f) {
    const int c = count_mask<T>(n);
    if (!c) return a;
    const int m = c % bits_of<T>;
    const T r = m ? T(T(a << m) | T(a >> (bits_of<T> - m))) : a;
    f.cf = r & 1;
    f.of = msb(r) != f.cf;  // only defined for c == 1
    return r;
}
template <class T> inline T ror(T a, u8 n, Flags& f) {
    const int c = count_mask<T>(n);
    if (!c) return a;
    const int m = c % bits_of<T>;
    const T r = m ? T(T(a >> m) | T(a << (bits_of<T> - m))) : a;
    f.cf = msb(r);
    f.of = msb(r) != bool((r >> (bits_of<T> - 2)) & 1);  // only defined for c == 1
    return r;
}

// ---- multiply and divide ----------------------------------------------------------------------------------------

// Two- and three-operand imul: truncated product; CF = OF = "the product does not fit". SF/ZF/PF are undefined.
template <class T> inline T imul(T a, T b, Flags& f) {
    using W = std::conditional_t<sizeof(T) == 8, i128, i64>;
    const W full = W(S<T>(a)) * W(S<T>(b));
    const T r = T(full);
    f.cf = f.of = full != W(S<T>(r));
    f.af = false;
    zsp(r, f);
    return r;
}
// One-operand mul / imul: returns the high half, low half in *lo. CF = OF = "the high half is significant".
template <class T> inline T mul_wide(T a, T b, T* lo, Flags& f) {
    using W = std::conditional_t<sizeof(T) == 8, u128, u64>;
    const W full = W(a) * W(b);
    *lo = T(full);
    const T hi = T(full >> bits_of<T>);
    f.cf = f.of = hi != 0;
    return hi;
}
template <class T> inline T imul_wide(T a, T b, T* lo, Flags& f) {
    using W = std::conditional_t<sizeof(T) == 8, i128, i64>;
    const W full = W(S<T>(a)) * W(S<T>(b));
    *lo = T(full);
    const T hi = T(full >> bits_of<T>);
    f.cf = f.of = full != W(S<T>(*lo));
    return hi;
}
// Divides hi:lo by d. Division by zero and quotient overflow raise #DE on hardware; here they trap.
template <class T> struct Wide;  // double-width types for the dividend
template <> struct Wide<u8> { using U = u16; using I = i16; };
template <> struct Wide<u16> { using U = u32; using I = i32; };
template <> struct Wide<u32> { using U = u64; using I = i64; };
template <> struct Wide<u64> { using U = u128; using I = i128; };

template <class T> inline void div_wide(T hi, T lo, T d, T* q, T* rem) {
    using W = typename Wide<T>::U;
    if (d == 0) __builtin_trap();
    const W n = W(W(hi) << bits_of<T> | W(lo));
    const W qq = n / d;
    if (qq >> bits_of<T>) __builtin_trap();
    *q = T(qq), *rem = T(n % d);
}
template <class T> inline void idiv_wide(T hi, T lo, T d, T* q, T* rem) {
    using W = typename Wide<T>::I;
    using UW = typename Wide<T>::U;
    if (d == 0) __builtin_trap();
    const W n = W(UW(UW(hi) << bits_of<T> | UW(lo)));
    const W sd = W(S<T>(d));
    const W qq = n / sd;  // W is wide enough that INT_MIN / -1 does not overflow here, the range check catches it
    if (qq > W(std::numeric_limits<S<T>>::max()) || qq < W(std::numeric_limits<S<T>>::min())) __builtin_trap();
    *q = T(qq), *rem = T(n % sd);
}

// ---- bit operations ---------------------------------------------------------------------------------------------

template <class T> inline void bt(T a, u8 n, Flags& f) { f.cf = (a >> (n % bits_of<T>)) & 1; }
template <class T> inline T bsf(T src, T old, Flags& f) {
    f.zf = src == 0;
    return src ? T(__builtin_ctzll(src)) : old;  // destination is left unchanged when the source is zero
}
template <class T> inline T bsr(T src, T old, Flags& f) {
    f.zf = src == 0;
    return src ? T(63 - __builtin_clzll(u64(src))) : old;
}
template <class T> inline T tzcnt(T src, Flags& f) {
    const T r = src ? T(__builtin_ctzll(src)) : T(bits_of<T>);
    f.cf = src == 0;
    f.zf = r == 0;
    return r;
}
template <class T> inline T lzcnt(T src, Flags& f) {
    const T r = src ? T(__builtin_clzll(u64(src)) - (64 - bits_of<T>)) : T(bits_of<T>);
    f.cf = src == 0;
    f.zf = r == 0;
    return r;
}
template <class T> inline T popcnt(T src, Flags& f) {
    f.cf = f.of = f.sf = f.af = f.pf = false;
    f.zf = src == 0;
    return T(__builtin_popcountll(u64(src)));
}
// BMI1: andn = ~a & b; bextr extracts `len` bits starting at `start` (both taken from the low 16 bits of ctl).
template <class T> inline T andn(T a, T b, Flags& f) {
    const T r = T(~a & b);
    f.cf = f.of = f.af = false;
    zsp(r, f);
    return r;
}
template <class T> inline T bextr(T src, T ctl, Flags& f) {
    const unsigned start = unsigned(ctl & 0xff), len = unsigned((ctl >> 8) & 0xff);
    T r = start >= unsigned(bits_of<T>) ? T(0) : T(src >> start);
    if (len < unsigned(bits_of<T>)) r = T(r & T((T(1) << len) - 1));
    f.cf = f.of = f.af = false;
    f.zf = r == 0;
    return r;
}
template <class T> inline T bswap(T v) {
    if constexpr (sizeof(T) == 8) return T(__builtin_bswap64(v));
    else if constexpr (sizeof(T) == 4) return T(__builtin_bswap32(v));
    else return T(__builtin_bswap16(v));  // movbe r16
}

// ---- vector registers -------------------------------------------------------------------------------------------
// VEX-encoded instructions with a 128-bit destination clear bits 255:128 of the register; 256-bit ones write all of it.

inline Vec vzero() { return Vec{{0, 0, 0, 0}}; }
// Keeps the low B bits of v and clears the rest.
template <int B> inline Vec vcut(Vec v) {
    if constexpr (B <= 128) v.q[2] = v.q[3] = 0;
    return v;
}
template <int B> inline Vec vld(u64 a) {
    Vec v = vzero();
    std::memcpy(v.q, reinterpret_cast<const void*>(a), B / 8);
    return v;
}
template <int B> inline void vst(u64 a, const Vec& v) { std::memcpy(reinterpret_cast<void*>(a), v.q, B / 8); }

// Lowest scalar element of a register (u32 or u64), and a register holding only that element.
template <class T> inline T vlo(const Vec& v) { return T(v.q[0]); }
template <class T> inline Vec vscalar(T e) {
    Vec v = vzero();
    v.q[0] = e;
    return v;
}
// `base` with its lowest element replaced (vmovss / vmovsd register form).
template <class T> inline Vec vins(Vec base, T e) {
    if constexpr (sizeof(T) == 8) base.q[0] = e;
    else base.q[0] = (base.q[0] & ~u64(0xffffffff)) | u64(e);
    return base;
}

inline Vec vxor(const Vec& a, const Vec& b) { return Vec{{a.q[0] ^ b.q[0], a.q[1] ^ b.q[1], a.q[2] ^ b.q[2], a.q[3] ^ b.q[3]}}; }
inline Vec vand(const Vec& a, const Vec& b) { return Vec{{a.q[0] & b.q[0], a.q[1] & b.q[1], a.q[2] & b.q[2], a.q[3] & b.q[3]}}; }
inline Vec vor(const Vec& a, const Vec& b) { return Vec{{a.q[0] | b.q[0], a.q[1] | b.q[1], a.q[2] | b.q[2], a.q[3] | b.q[3]}}; }
inline Vec vandn(const Vec& a, const Vec& b) { return Vec{{~a.q[0] & b.q[0], ~a.q[1] & b.q[1], ~a.q[2] & b.q[2], ~a.q[3] & b.q[3]}}; }

}  // namespace dcp::rt
