// Runtime for recompiled guest code: guest context, x86-64 flag semantics and integer instruction helpers.
// Generated functions include this header. Everything here is header-only so the C++ compiler can inline it and
// delete the flags nobody reads (that is what makes the flags "lazy": they are plain locals).
#pragma once
#include <cmath>
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

// The guest context of the guest code that is currently calling into the host on this thread (set while an HLE thunk runs),
// so a host function can call back into guest code. Null on a thread that is not inside a host call.
inline thread_local Ctx* g_current_ctx = nullptr;
inline Ctx* current_ctx() { return g_current_ctx; }

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

// ---- scalar floating point -------------------------------------------------------------------------------------
// SSE rules that arm64 does not share, written out so the result is the same on both: a NaN result is the first NaN
// operand made quiet, or the "default NaN" (negative quiet NaN) when no operand was NaN; min / max return the second
// operand unless the comparison holds; conversions to integer return the "integer indefinite" value when out of range.
// Operands and results travel as raw bits (u32 for float, u64 for double).

template <class F> struct FT;
template <> struct FT<float> {
    using U = u32;
    static constexpr U kQuiet = 0x00400000u, kDefaultNan = 0xffc00000u;
};
template <> struct FT<double> {
    using U = u64;
    static constexpr U kQuiet = 0x0008000000000000ull, kDefaultNan = 0xfff8000000000000ull;
};
template <class F> using FU = typename FT<F>::U;

template <class F> inline F fbits(FU<F> u) {
    F f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}
template <class F> inline FU<F> fraw(F f) {
    FU<F> u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}
template <class F> inline FU<F> fnan_result(FU<F> a, FU<F> b) {
    if (std::isnan(fbits<F>(a))) return a | FT<F>::kQuiet;
    if (std::isnan(fbits<F>(b))) return b | FT<F>::kQuiet;
    return FT<F>::kDefaultNan;
}
template <class F> inline FU<F> fresult(F r, FU<F> a, FU<F> b) { return std::isnan(r) ? fnan_result<F>(a, b) : fraw<F>(r); }

template <class F> inline FU<F> fadd(FU<F> a, FU<F> b) { return fresult<F>(fbits<F>(a) + fbits<F>(b), a, b); }
template <class F> inline FU<F> fsub(FU<F> a, FU<F> b) { return fresult<F>(fbits<F>(a) - fbits<F>(b), a, b); }
template <class F> inline FU<F> fmul(FU<F> a, FU<F> b) { return fresult<F>(fbits<F>(a) * fbits<F>(b), a, b); }
template <class F> inline FU<F> fdiv(FU<F> a, FU<F> b) { return fresult<F>(fbits<F>(a) / fbits<F>(b), a, b); }
template <class F> inline FU<F> fmin(FU<F> a, FU<F> b) { return fbits<F>(a) < fbits<F>(b) ? a : b; }
template <class F> inline FU<F> fmax(FU<F> a, FU<F> b) { return fbits<F>(a) > fbits<F>(b) ? a : b; }
template <class F> inline FU<F> fsqrt(FU<F> b) { return fresult<F>(std::sqrt(fbits<F>(b)), b, b); }

// ucomiss / comiss: unordered sets ZF PF CF; OF SF AF are always cleared.
template <class F> inline void fucomi(FU<F> a, FU<F> b, Flags& f) {
    const F x = fbits<F>(a), y = fbits<F>(b);
    f.of = f.sf = f.af = false;
    if (std::isnan(x) || std::isnan(y)) return f.zf = f.pf = f.cf = true, void();
    f.pf = false;
    f.zf = x == y;
    f.cf = x < y;
}

// cmpss / cmpsd with the 16 distinct predicates (imm bit 4 only changes exception behaviour). All ones when true.
template <class F> inline FU<F> fcmp(FU<F> a, FU<F> b, unsigned pred) {
    const F x = fbits<F>(a), y = fbits<F>(b);
    const bool u = std::isnan(x) || std::isnan(y);
    bool t = false;
    switch (pred & 15) {
    case 0: t = !u && x == y; break;
    case 1: t = !u && x < y; break;
    case 2: t = !u && x <= y; break;
    case 3: t = u; break;
    case 4: t = u || x != y; break;
    case 5: t = u || !(x < y); break;
    case 6: t = u || !(x <= y); break;
    case 7: t = !u; break;
    case 8: t = u || x == y; break;
    case 9: t = u || !(x >= y); break;
    case 10: t = u || !(x > y); break;
    case 11: t = false; break;
    case 12: t = !u && x != y; break;
    case 13: t = !u && x >= y; break;
    case 14: t = !u && x > y; break;
    default: t = true; break;
    }
    return t ? ~FU<F>(0) : FU<F>(0);
}

// Integer -> float (round to nearest even) and float -> integer by truncation (cvttss2si / cvttsd2si).
template <class F, class I> inline FU<F> fcvti(I v) { return fraw<F>(F(v)); }
template <class F, class I> inline I fcvtt(FU<F> a) {
    const F x = fbits<F>(a);
    const F hi = F(2) * F(I(1) << (bits_of<I> - 2));  // 2^(n-1), exact in float and double
    if (!(x >= -hi && x < hi)) return std::numeric_limits<I>::min();  // NaN too: the "integer indefinite"
    return I(x);
}
inline u64 fcvt_f2d(u32 a) {
    if (std::isnan(fbits<float>(a))) return u64(a & 0x80000000u) << 32 | 0x7ff8000000000000ull | u64(a & 0x7fffffu) << 29;
    return fraw<double>(double(fbits<float>(a)));
}
inline u32 fcvt_d2f(u64 a) {
    if (std::isnan(fbits<double>(a))) return (u32(a >> 32) & 0x80000000u) | 0x7fc00000u | u32((a & 0xfffffffffffffull) >> 29);
    return fraw<float>(float(fbits<double>(a)));
}

// ---- packed floating point -------------------------------------------------------------------------------------
// Lane-wise versions of the scalar helpers above. B is the register width in bits (128 or 256); bits above B are cleared.

template <class U> inline U lane(const Vec& v, int i) {
    U e;
    std::memcpy(&e, reinterpret_cast<const u8*>(v.q) + i * sizeof(U), sizeof e);
    return e;
}
template <class U> inline void setlane(Vec& v, int i, U e) { std::memcpy(reinterpret_cast<u8*>(v.q) + i * sizeof(U), &e, sizeof e); }
template <class F, int B> constexpr int lanes_of = B / (8 * int(sizeof(F)));

template <class F, int B, FU<F> (*Op)(FU<F>, FU<F>)> inline Vec vpf(const Vec& a, const Vec& b) {
    Vec r = vzero();
    for (int i = 0; i < lanes_of<F, B>; ++i) setlane<FU<F>>(r, i, Op(lane<FU<F>>(a, i), lane<FU<F>>(b, i)));
    return r;
}
template <class F, int B> inline Vec vpsqrt(const Vec& a) {
    Vec r = vzero();
    for (int i = 0; i < lanes_of<F, B>; ++i) setlane<FU<F>>(r, i, fsqrt<F>(lane<FU<F>>(a, i)));
    return r;
}
template <class F, int B> inline Vec vpcmp(const Vec& a, const Vec& b, unsigned pred) {
    Vec r = vzero();
    for (int i = 0; i < lanes_of<F, B>; ++i) setlane<FU<F>>(r, i, fcmp<F>(lane<FU<F>>(a, i), lane<FU<F>>(b, i), pred));
    return r;
}
// Horizontal add / subtract inside each 128-bit half: the low half of a half's result pairs up a, the high half pairs up b.
template <class F, int B, bool Sub> inline Vec vphop(const Vec& a, const Vec& b) {
    constexpr int m = 16 / int(sizeof(F));  // elements per 128-bit half
    Vec r = vzero();
    for (int g = 0; g < B / 128; ++g)
        for (int j = 0; j < m / 2; ++j) {
            const int lo = g * m + 2 * j, hi = lo + 1;
            auto op = [](FU<F> x, FU<F> y) { return Sub ? fsub<F>(x, y) : fadd<F>(x, y); };
            setlane<FU<F>>(r, g * m + j, op(lane<FU<F>>(a, lo), lane<FU<F>>(a, hi)));
            setlane<FU<F>>(r, g * m + m / 2 + j, op(lane<FU<F>>(b, lo), lane<FU<F>>(b, hi)));
        }
    return r;
}
// vaddsubps / vaddsubpd: even lanes subtract, odd lanes add.
template <class F, int B> inline Vec vpaddsub(const Vec& a, const Vec& b) {
    Vec r = vzero();
    for (int i = 0; i < lanes_of<F, B>; ++i)
        setlane<FU<F>>(r, i, i % 2 ? fadd<F>(lane<FU<F>>(a, i), lane<FU<F>>(b, i)) : fsub<F>(lane<FU<F>>(a, i), lane<FU<F>>(b, i)));
    return r;
}
template <int B> inline Vec vpcvtdq2ps(const Vec& a) {
    Vec r = vzero();
    for (int i = 0; i < B / 32; ++i) setlane<u32>(r, i, fcvti<float, i32>(lane<i32>(a, i)));
    return r;
}
template <int B> inline Vec vpcvttps2dq(const Vec& a) {
    Vec r = vzero();
    for (int i = 0; i < B / 32; ++i) setlane<i32>(r, i, fcvtt<float, i32>(lane<u32>(a, i)));
    return r;
}

// roundss / roundps: imm bits 1:0 select nearest-even, floor, ceil, truncate; bit 2 defers to MXCSR (nearest-even here).
template <class F> inline FU<F> fround(FU<F> a, unsigned imm) {
    const F x = fbits<F>(a);
    if (std::isnan(x)) return a | FT<F>::kQuiet;
    switch ((imm & 4) ? 0 : (imm & 3)) {
    case 0: return fraw<F>(std::nearbyint(x));
    case 1: return fraw<F>(std::floor(x));
    case 2: return fraw<F>(std::ceil(x));
    default: return fraw<F>(std::trunc(x));
    }
}
template <class F, int B> inline Vec vpround(const Vec& a, unsigned imm) {
    Vec r = vzero();
    for (int i = 0; i < lanes_of<F, B>; ++i) setlane<FU<F>>(r, i, fround<F>(lane<FU<F>>(a, i), imm));
    return r;
}

// ---- blends, shuffles and lane moves (float domain) --------------------------------------------------------------
// "Group" = one 128-bit half: the AVX shuffles never cross halves except vperm2f128, vinsertf128 and vextractf128.

inline Vec vmk(u64 a, u64 b) { return Vec{{a, b, 0, 0}}; }

template <class U, int B> inline Vec vblendv(const Vec& a, const Vec& b, const Vec& m) {
    Vec r = vzero();
    for (int i = 0; i < B / (8 * int(sizeof(U))); ++i) setlane<U>(r, i, msb(lane<U>(m, i)) ? lane<U>(b, i) : lane<U>(a, i));
    return r;
}
template <class U, int B> inline Vec vblendi(const Vec& a, const Vec& b, unsigned imm) {
    Vec r = vzero();
    for (int i = 0; i < B / (8 * int(sizeof(U))); ++i) setlane<U>(r, i, (imm >> i) & 1 ? lane<U>(b, i) : lane<U>(a, i));
    return r;
}
template <int B> inline Vec vpermilps_i(const Vec& a, unsigned imm) {
    Vec r = vzero();
    for (int g = 0; g < B / 128; ++g)
        for (int j = 0; j < 4; ++j) setlane<u32>(r, g * 4 + j, lane<u32>(a, g * 4 + int((imm >> (2 * j)) & 3)));
    return r;
}
template <int B> inline Vec vpermilps_v(const Vec& a, const Vec& c) {
    Vec r = vzero();
    for (int g = 0; g < B / 128; ++g)
        for (int j = 0; j < 4; ++j) setlane<u32>(r, g * 4 + j, lane<u32>(a, g * 4 + int(lane<u32>(c, g * 4 + j) & 3)));
    return r;
}
template <int B> inline Vec vpermilpd_i(const Vec& a, unsigned imm) {
    Vec r = vzero();
    for (int i = 0; i < B / 64; ++i) setlane<u64>(r, i, lane<u64>(a, (i & ~1) + int((imm >> i) & 1)));
    return r;
}
template <int B> inline Vec vpermilpd_v(const Vec& a, const Vec& c) {
    Vec r = vzero();
    for (int i = 0; i < B / 64; ++i) setlane<u64>(r, i, lane<u64>(a, (i & ~1) + int((lane<u64>(c, i) >> 1) & 1)));
    return r;
}
// imm[7:6] picks the source lane (register source only), imm[5:4] the destination lane, imm[3:0] zeroes lanes.
inline Vec vinsertps(const Vec& a, const Vec& b, unsigned imm, bool b_is_mem) {
    Vec r = a;
    r.q[2] = r.q[3] = 0;
    setlane<u32>(r, int((imm >> 4) & 3), lane<u32>(b, b_is_mem ? 0 : int((imm >> 6) & 3)));
    for (int i = 0; i < 4; ++i)
        if ((imm >> i) & 1) setlane<u32>(r, i, 0u);
    return r;
}
inline Vec vinsert128(Vec a, const Vec& b, unsigned imm) {
    a.q[(imm & 1) * 2] = b.q[0];
    a.q[(imm & 1) * 2 + 1] = b.q[1];
    return a;
}
inline Vec vextract128(const Vec& s, unsigned imm) { return vmk(s.q[(imm & 1) * 2], s.q[(imm & 1) * 2 + 1]); }
inline Vec vperm2f128(const Vec& a, const Vec& b, unsigned imm) {
    Vec r = vzero();
    for (int h = 0; h < 2; ++h) {
        const unsigned f = (imm >> (4 * h)) & 15;
        if (f & 8) continue;
        const Vec& src = (f & 2) ? b : a;
        r.q[h * 2] = src.q[(f & 1) * 2];
        r.q[h * 2 + 1] = src.q[(f & 1) * 2 + 1];
    }
    return r;
}
template <class U, int B> inline Vec vbcast(U e) {
    Vec r = vzero();
    for (int i = 0; i < B / (8 * int(sizeof(U))); ++i) setlane<U>(r, i, e);
    return r;
}
inline Vec vbcast128(const Vec& b) { return Vec{{b.q[0], b.q[1], b.q[0], b.q[1]}}; }
template <int B> inline Vec vmovshdup(const Vec& a) {
    Vec r = vzero();
    for (int i = 0; i < B / 32; ++i) setlane<u32>(r, i, lane<u32>(a, i | 1));
    return r;
}
template <int B> inline Vec vmovsldup(const Vec& a) {
    Vec r = vzero();
    for (int i = 0; i < B / 32; ++i) setlane<u32>(r, i, lane<u32>(a, i & ~1));
    return r;
}
template <int B> inline Vec vmovddup(const Vec& a) {
    Vec r = vzero();
    for (int i = 0; i < B / 64; ++i) setlane<u64>(r, i, lane<u64>(a, i & ~1));
    return r;
}
// unpcklps / unpckhps / unpcklpd / unpckhpd: interleave the low (or high) elements of each group of a and b.
template <class U, bool Hi, int B> inline Vec vunpck(const Vec& a, const Vec& b) {
    constexpr int n = 16 / int(sizeof(U));
    Vec r = vzero();
    for (int g = 0; g < B / 128; ++g)
        for (int j = 0; j < n / 2; ++j) {
            const int idx = g * n + (Hi ? n / 2 : 0) + j;
            setlane<U>(r, g * n + 2 * j, lane<U>(a, idx));
            setlane<U>(r, g * n + 2 * j + 1, lane<U>(b, idx));
        }
    return r;
}
template <int B> inline Vec vshufps(const Vec& a, const Vec& b, unsigned imm) {
    Vec r = vzero();
    for (int g = 0; g < B / 128; ++g)
        for (int j = 0; j < 4; ++j)
            setlane<u32>(r, g * 4 + j, lane<u32>(j < 2 ? a : b, g * 4 + int((imm >> (2 * j)) & 3)));
    return r;
}
template <int B> inline Vec vshufpd(const Vec& a, const Vec& b, unsigned imm) {
    Vec r = vzero();
    for (int i = 0; i < B / 64; ++i) setlane<u64>(r, i, lane<u64>(i & 1 ? b : a, (i & ~1) + int((imm >> i) & 1)));
    return r;
}
// movmskps / movmskpd: the sign bit of every element.
template <class U, int B> inline u32 vmovmsk(const Vec& a) {
    u32 m = 0;
    for (int i = 0; i < B / (8 * int(sizeof(U))); ++i) m |= u32(msb(lane<U>(a, i))) << i;
    return m;
}

// ---- 128-bit integer SIMD ---------------------------------------------------------------------------------------
// AVX1 only has 128-bit integer instructions, so these always clear bits 255:128. Lane type U is u8, u16, u32 or u64.

template <class U, class Op> inline Vec vpmap(const Vec& a, const Vec& b, Op op) {
    Vec r = vzero();
    for (int i = 0; i < 16 / int(sizeof(U)); ++i) setlane<U>(r, i, op(lane<U>(a, i), lane<U>(b, i)));
    return r;
}
template <class U> inline Vec vpadd(const Vec& a, const Vec& b) { return vpmap<U>(a, b, [](U x, U y) { return U(x + y); }); }
template <class U> inline Vec vpsub(const Vec& a, const Vec& b) { return vpmap<U>(a, b, [](U x, U y) { return U(x - y); }); }
template <class U> inline Vec vpmul(const Vec& a, const Vec& b) { return vpmap<U>(a, b, [](U x, U y) { return U(x * y); }); }
template <class U> inline Vec vpcmpeq(const Vec& a, const Vec& b) { return vpmap<U>(a, b, [](U x, U y) { return x == y ? U(~U(0)) : U(0); }); }
template <class U> inline Vec vpcmpgt(const Vec& a, const Vec& b) {
    return vpmap<U>(a, b, [](U x, U y) { return S<U>(x) > S<U>(y) ? U(~U(0)) : U(0); });
}
// phaddd / phaddw / phsubd / phsubw
template <class U, bool Sub> inline Vec vphaddi(const Vec& a, const Vec& b) {
    constexpr int n = 16 / int(sizeof(U));
    Vec r = vzero();
    for (int j = 0; j < n / 2; ++j) {
        auto op = [](U x, U y) { return Sub ? U(x - y) : U(x + y); };
        setlane<U>(r, j, op(lane<U>(a, 2 * j), lane<U>(a, 2 * j + 1)));
        setlane<U>(r, n / 2 + j, op(lane<U>(b, 2 * j), lane<U>(b, 2 * j + 1)));
    }
    return r;
}
inline Vec vpshuflw(const Vec& a, unsigned imm) {
    Vec r = vzero();
    r.q[1] = a.q[1];
    for (int j = 0; j < 4; ++j) setlane<u16>(r, j, lane<u16>(a, int((imm >> (2 * j)) & 3)));
    return r;
}
inline Vec vpshufhw(const Vec& a, unsigned imm) {
    Vec r = vzero();
    r.q[0] = a.q[0];
    for (int j = 0; j < 4; ++j) setlane<u16>(r, 4 + j, lane<u16>(a, 4 + int((imm >> (2 * j)) & 3)));
    return r;
}
inline Vec vpshufb(const Vec& a, const Vec& c) {
    Vec r = vzero();
    for (int i = 0; i < 16; ++i) {
        const u8 k = lane<u8>(c, i);
        setlane<u8>(r, i, k & 0x80 ? u8(0) : lane<u8>(a, k & 15));
    }
    return r;
}
// Byte shifts of the whole 128-bit register.
inline Vec vpslldq(const Vec& a, unsigned n) {
    Vec r = vzero();
    if (n > 15) return r;
    const u128 v = ((u128(a.q[1]) << 64) | a.q[0]) << (8 * n);
    r.q[0] = u64(v), r.q[1] = u64(v >> 64);
    return r;
}
inline Vec vpsrldq(const Vec& a, unsigned n) {
    Vec r = vzero();
    if (n > 15) return r;
    const u128 v = ((u128(a.q[1]) << 64) | a.q[0]) >> (8 * n);
    r.q[0] = u64(v), r.q[1] = u64(v >> 64);
    return r;
}
// Shifts of every lane by one count; a count at or above the lane width gives 0 (sign fill for the arithmetic shift).
template <class U> inline Vec vpsll(const Vec& a, u64 c) {
    Vec r = vzero();
    for (int i = 0; i < 16 / int(sizeof(U)); ++i) setlane<U>(r, i, c >= bits_of<U> ? U(0) : U(lane<U>(a, i) << c));
    return r;
}
template <class U> inline Vec vpsrl(const Vec& a, u64 c) {
    Vec r = vzero();
    for (int i = 0; i < 16 / int(sizeof(U)); ++i) setlane<U>(r, i, c >= bits_of<U> ? U(0) : U(lane<U>(a, i) >> c));
    return r;
}
template <class U> inline Vec vpsra(const Vec& a, u64 c) {
    Vec r = vzero();
    for (int i = 0; i < 16 / int(sizeof(U)); ++i) {
        const S<U> v = S<U>(lane<U>(a, i));
        setlane<U>(r, i, c >= bits_of<U> ? U(v < 0 ? ~U(0) : U(0)) : U(S<U>(v >> c)));
    }
    return r;
}
// pmovzx / pmovsx: widen the low elements of a (Src is signed for the sign-extending form); Dst is unsigned.
template <class Src, class Dst> inline Vec vpmovx(const Vec& a) {
    Vec r = vzero();
    for (int i = 0; i < 16 / int(sizeof(Dst)); ++i) setlane<Dst>(r, i, Dst(lane<Src>(a, i)));
    return r;
}
// packsswb / packuswb / packssdw / packusdw: saturate the signed Src lanes of a then b to the range of D.
template <class Src, class D> inline Vec vpack(const Vec& a, const Vec& b) {
    constexpr int n = 16 / int(sizeof(Src));
    auto sat = [](Src v) -> D {
        if (v < Src(std::numeric_limits<D>::min())) return std::numeric_limits<D>::min();
        if (v > Src(std::numeric_limits<D>::max())) return std::numeric_limits<D>::max();
        return D(v);
    };
    Vec r = vzero();
    for (int i = 0; i < n; ++i) {
        setlane<D>(r, i, sat(lane<Src>(a, i)));
        setlane<D>(r, n + i, sat(lane<Src>(b, i)));
    }
    return r;
}
template <class U> inline Vec vpins(const Vec& a, U e, unsigned idx) {
    Vec r = a;
    r.q[2] = r.q[3] = 0;
    setlane<U>(r, int(idx % (16 / sizeof(U))), e);
    return r;
}

// ---- reciprocal estimates ---------------------------------------------------------------------------------------
// rcpps / rsqrtps are approximations (relative error up to 1.5 * 2^-12) whose exact bits depend on the CPU model, so no
// bit-exact answer exists to reproduce. These return the correctly rounded value instead (always inside the architectural
// bound) and follow the defined special cases: zero and denormal inputs give a signed infinity, results below the
// smallest normal number flush to a signed zero, negative inputs to rsqrt give the default NaN.
inline u32 frcp(u32 a) {
    const float x = fbits<float>(a);
    if (std::isnan(x)) return a | FT<float>::kQuiet;
    const u32 sign = a & 0x80000000u;
    if (std::fabs(x) < std::numeric_limits<float>::min()) return sign | 0x7f800000u;  // zero or denormal
    const float r = 1.0f / x;
    return std::fabs(r) < std::numeric_limits<float>::min() ? sign : fraw<float>(r);
}
inline u32 frsqrt(u32 a) {
    const float x = fbits<float>(a);
    if (std::isnan(x)) return a | FT<float>::kQuiet;
    if (std::fabs(x) < std::numeric_limits<float>::min()) return (a & 0x80000000u) | 0x7f800000u;
    if (x < 0) return FT<float>::kDefaultNan;
    const float r = 1.0f / std::sqrt(x);
    return std::fabs(r) < std::numeric_limits<float>::min() ? 0u : fraw<float>(r);
}
template <int B> inline Vec vprcp(const Vec& a) {
    Vec r = vzero();
    for (int i = 0; i < B / 32; ++i) setlane<u32>(r, i, frcp(lane<u32>(a, i)));
    return r;
}
template <int B> inline Vec vprsqrt(const Vec& a) {
    Vec r = vzero();
    for (int i = 0; i < B / 32; ++i) setlane<u32>(r, i, frsqrt(lane<u32>(a, i)));
    return r;
}

}  // namespace dcp::rt
