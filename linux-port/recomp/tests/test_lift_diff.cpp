// Differential test of the integer lifter: every case is real x86-64 machine code (assembled from Intel syntax at test
// time). It runs once natively (Rosetta 2 or an x86 CPU) and once as lifted C++, from the same random registers, flags
// and memory. Registers, memory and the architecturally defined flags must match. No game data involved.
//
// Needs an x86-64 host and a C++ compiler at run time (DCP_TEST_CXX). Skips with exit code 0 when it cannot build.
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "lift.h"
#include "dcp_rt.h"

using namespace dcp::recomp;
using dcp::rt::Ctx;

#ifdef __APPLE__
#define SYM(x) "_" #x
#define CSYM(x) "_" x
#else
#define SYM(x) #x
#define CSYM(x) x
#endif

// Runs `code` natively with the registers and flags of `in`; the final state goes to `out`. The code is entered with
// `call`, so it sees rsp = in.r[RSP] - 8 and must end with ret, exactly like a recompiled function whose caller pushed the
// return address.
extern "C" void dcp_native_exec(const void* code, const Ctx* in, Ctx* out);
asm(".text\n"
    ".globl " SYM(dcp_native_exec) "\n" SYM(dcp_native_exec) ":\n"
    "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
    "  movq %rsp, " SYM(g_host_rsp) "(%rip)\n"
    "  movq %rdi, " SYM(g_code) "(%rip)\n"
    "  movq %rdx, " SYM(g_out) "(%rip)\n"
    "  movq 128(%rsi), %rax\n  pushq %rax\n  popfq\n"
    "  vmovups 160(%rsi), %ymm0\n"
    "  vmovups 192(%rsi), %ymm1\n"
    "  vmovups 224(%rsi), %ymm2\n"
    "  vmovups 256(%rsi), %ymm3\n"
    "  vmovups 288(%rsi), %ymm4\n"
    "  vmovups 320(%rsi), %ymm5\n"
    "  vmovups 352(%rsi), %ymm6\n"
    "  vmovups 384(%rsi), %ymm7\n"
    "  vmovups 416(%rsi), %ymm8\n"
    "  vmovups 448(%rsi), %ymm9\n"
    "  vmovups 480(%rsi), %ymm10\n"
    "  vmovups 512(%rsi), %ymm11\n"
    "  vmovups 544(%rsi), %ymm12\n"
    "  vmovups 576(%rsi), %ymm13\n"
    "  vmovups 608(%rsi), %ymm14\n"
    "  vmovups 640(%rsi), %ymm15\n"
    "  movq 0(%rsi), %rax\n  movq 8(%rsi), %rcx\n  movq 16(%rsi), %rdx\n  movq 24(%rsi), %rbx\n  movq 40(%rsi), %rbp\n"
    "  movq 56(%rsi), %rdi\n  movq 64(%rsi), %r8\n  movq 72(%rsi), %r9\n  movq 80(%rsi), %r10\n  movq 88(%rsi), %r11\n"
    "  movq 96(%rsi), %r12\n  movq 104(%rsi), %r13\n  movq 112(%rsi), %r14\n  movq 120(%rsi), %r15\n"
    "  movq 32(%rsi), %rsp\n  movq 48(%rsi), %rsi\n"
    "  callq *" SYM(g_code) "(%rip)\n"
    // Leave the guest stack untouched: its contents are compared. mov does not change the flags.
    "  movq %rsp, " SYM(g_guest_rsp) "(%rip)\n"
    "  movq " SYM(g_host_rsp) "(%rip), %rsp\n"
    "  pushfq\n  pushq %r11\n  movq " SYM(g_out) "(%rip), %r11\n"
    "  vmovups %ymm0, 160(%r11)\n"
    "  vmovups %ymm1, 192(%r11)\n"
    "  vmovups %ymm2, 224(%r11)\n"
    "  vmovups %ymm3, 256(%r11)\n"
    "  vmovups %ymm4, 288(%r11)\n"
    "  vmovups %ymm5, 320(%r11)\n"
    "  vmovups %ymm6, 352(%r11)\n"
    "  vmovups %ymm7, 384(%r11)\n"
    "  vmovups %ymm8, 416(%r11)\n"
    "  vmovups %ymm9, 448(%r11)\n"
    "  vmovups %ymm10, 480(%r11)\n"
    "  vmovups %ymm11, 512(%r11)\n"
    "  vmovups %ymm12, 544(%r11)\n"
    "  vmovups %ymm13, 576(%r11)\n"
    "  vmovups %ymm14, 608(%r11)\n"
    "  vmovups %ymm15, 640(%r11)\n"
    "  movq %rax, 0(%r11)\n  movq %rcx, 8(%r11)\n  movq %rdx, 16(%r11)\n  movq %rbx, 24(%r11)\n  movq %rbp, 40(%r11)\n"
    "  movq %rsi, 48(%r11)\n  movq %rdi, 56(%r11)\n  movq %r8, 64(%r11)\n  movq %r9, 72(%r11)\n  movq %r10, 80(%r11)\n"
    "  movq %r12, 96(%r11)\n  movq %r13, 104(%r11)\n  movq %r14, 112(%r11)\n  movq %r15, 120(%r11)\n"
    "  popq %rax\n  movq %rax, 88(%r11)\n"
    "  movq " SYM(g_guest_rsp) "(%rip), %rax\n  movq %rax, 32(%r11)\n"
    "  popq %rax\n  movq %rax, 128(%r11)\n"
    "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n  retq\n"
    ".data\n.p2align 3\n"
    ".globl " SYM(g_host_rsp) "\n" SYM(g_host_rsp) ": .quad 0\n"
    ".globl " SYM(g_code) "\n" SYM(g_code) ": .quad 0\n"
    ".globl " SYM(g_out) "\n" SYM(g_out) ": .quad 0\n"
    ".globl " SYM(g_guest_rsp) "\n" SYM(g_guest_rsp) ": .quad 0\n"
    ".text\n");

namespace {

constexpr unsigned CF = 1u << 0, PF = 1u << 2, AF = 1u << 4, ZF = 1u << 6, SF = 1u << 7, OF = 1u << 11;
constexpr unsigned ALL = CF | PF | AF | ZF | SF | OF;
constexpr unsigned NOAF = ALL & ~AF;  // and / or / xor / test: AF is architecturally undefined

struct Case { const char* text; unsigned mask; };

// Prelude fragments that keep inputs legal: memory indexes small, divisors non-zero, quotients in range.
#define IDX "and esi, 7\n"
const Case kCases[] = {
    // ---- mov / lea / extensions ----
    {"mov rax, rbx", 0}, {"mov eax, ebx", 0}, {"mov ax, bx", 0}, {"mov al, bl", 0}, {"mov ah, bl", 0}, {"mov bh, ch", 0},
    {"mov rax, 0x123456789abcdef0", 0}, {"mov eax, -1", 0}, {"mov ax, 0x1234", 0}, {"mov cl, 0x80", 0}, {"mov dh, 7", 0},
    {"mov rax, -2", 0}, {"mov qword ptr [rbx+8], -2", 0}, {"mov dword ptr [rbx], 0x7fffffff", 0},
    {"mov byte ptr [rbx+3], 0x55", 0}, {"mov word ptr [rbx+2], 0x4242", 0},
    {"mov rax, qword ptr [rbx+16]", 0}, {"mov eax, dword ptr [rbx+4]", 0}, {"mov ax, word ptr [rbx+6]", 0},
    {"mov al, byte ptr [rbx+9]", 0}, {"mov ah, byte ptr [rbx+9]", 0},
    {"mov qword ptr [rbx+24], rcx", 0}, {"mov dword ptr [rbx+28], ecx", 0}, {"mov word ptr [rbx+30], cx", 0},
    {"mov byte ptr [rbx+31], cl", 0}, {"mov byte ptr [rbx+31], ch", 0},
    {IDX "mov rax, qword ptr [rbx+rsi*8]", 0}, {IDX "mov eax, dword ptr [rbx+rsi*4+4]", 0},
    {IDX "mov qword ptr [rbx+rsi*8-8], rdx", 0},
    {"lea rax, [rbx+rcx*8+0x10]", 0}, {"lea eax, [rbx+rcx*2-4]", 0}, {"lea rax, [rcx*4+0x100]", 0},
    {"lea ax, [rbx+rcx]", 0}, {"lea rax, [rbx-1]", 0}, {"lea rax, [rbx+rbx*2]", 0},
    {"movzx eax, bl", 0}, {"movzx rax, bl", 0}, {"movzx eax, bh", 0}, {"movzx eax, bx", 0}, {"movzx rax, cx", 0},
    {"movzx ax, bl", 0}, {"movzx rax, byte ptr [rbx+5]", 0}, {"movzx eax, word ptr [rbx+6]", 0},
    {"movsx eax, bl", 0}, {"movsx rax, bl", 0}, {"movsx eax, bx", 0}, {"movsx rax, cx", 0}, {"movsx ax, bl", 0},
    {"movsx rax, byte ptr [rbx+5]", 0}, {"movsx eax, word ptr [rbx+6]", 0}, {"movsxd rax, ecx", 0},
    {"movsxd rax, dword ptr [rbx+4]", 0},
    {"cdqe", 0}, {"cwde", 0}, {"cdq", 0}, {"cqo", 0},
    {"xchg rax, rbx", 0}, {"xchg ecx, edx", 0}, {"xchg qword ptr [rbx], rcx", 0}, {"xchg byte ptr [rbx+1], dl", 0},
    {"bswap eax", 0}, {"bswap rcx", 0}, {"movbe eax, dword ptr [rbx]", 0}, {"movbe qword ptr [rbx+8], rcx", 0},
    {"movbe ax, word ptr [rbx+2]", 0},
    // ---- stack ----
    {"push rbx\npop rcx", 0}, {"push 0x12345678\npop rax", 0}, {"push -3\npop rdx", 0}, {"push qword ptr [rbx]\npop rax", 0},
    {"push rax\npush rcx\npop rdx\npop rsi", 0}, {"push rbx\npop qword ptr [rbx+8]", 0},
    // ---- two-operand ALU, every width and form ----
    {"add rax, rbx", ALL}, {"add eax, ebx", ALL}, {"add ax, bx", ALL}, {"add al, bl", ALL}, {"add ah, cl", ALL},
    {"add rax, -1", ALL}, {"add eax, 0x80000000", ALL}, {"add ax, 0x7fff", ALL}, {"add al, 0x7f", ALL}, {"add rax, 0x7fffffff", ALL},
    {"add qword ptr [rbx+8], rcx", ALL}, {"add rcx, qword ptr [rbx+16]", ALL}, {"add byte ptr [rbx], dl", ALL},
    {"adc rax, rbx", ALL}, {"adc eax, ebx", ALL}, {"adc ax, bx", ALL}, {"adc al, bl", ALL}, {"adc rax, -1", ALL},
    {"adc qword ptr [rbx], rcx", ALL},
    {"sub rax, rbx", ALL}, {"sub eax, ebx", ALL}, {"sub ax, bx", ALL}, {"sub al, bl", ALL}, {"sub rax, -1", ALL},
    {"sub eax, 0x80000000", ALL}, {"sub dword ptr [rbx+4], ecx", ALL}, {"sub rcx, qword ptr [rbx]", ALL},
    {"sbb rax, rbx", ALL}, {"sbb eax, ebx", ALL}, {"sbb ax, bx", ALL}, {"sbb al, bl", ALL}, {"sbb rax, 1", ALL},
    {"sbb qword ptr [rbx], rcx", ALL},
    {"cmp rax, rbx", ALL}, {"cmp eax, ebx", ALL}, {"cmp ax, bx", ALL}, {"cmp al, bl", ALL}, {"cmp rax, 0", ALL},
    {"cmp rax, -1", ALL}, {"cmp qword ptr [rbx], rcx", ALL}, {"cmp rcx, qword ptr [rbx+8]", ALL}, {"cmp byte ptr [rbx], 0x80", ALL},
    {"and rax, rbx", NOAF}, {"and eax, ebx", NOAF}, {"and ax, bx", NOAF}, {"and al, bl", NOAF}, {"and rax, -16", NOAF},
    {"and eax, 0x80000000", NOAF}, {"and qword ptr [rbx], rcx", NOAF},
    {"or rax, rbx", NOAF}, {"or eax, ebx", NOAF}, {"or ax, bx", NOAF}, {"or al, bl", NOAF}, {"or rax, 0x80", NOAF},
    {"or dword ptr [rbx], 1", NOAF},
    {"xor rax, rbx", NOAF}, {"xor eax, eax", NOAF}, {"xor eax, ebx", NOAF}, {"xor ax, bx", NOAF}, {"xor al, bl", NOAF},
    {"xor rax, -1", NOAF}, {"xor qword ptr [rbx], rcx", NOAF},
    {"test rax, rbx", NOAF}, {"test eax, eax", NOAF}, {"test ax, bx", NOAF}, {"test al, bl", NOAF}, {"test rax, 0x80", NOAF},
    {"test byte ptr [rbx], 1", NOAF}, {"test dword ptr [rbx+4], ecx", NOAF},
    // ---- one-operand ----
    {"inc rax", ALL & ~CF}, {"inc eax", ALL & ~CF}, {"inc ax", ALL & ~CF}, {"inc al", ALL & ~CF}, {"inc dword ptr [rbx]", ALL & ~CF},
    {"dec rax", ALL & ~CF}, {"dec eax", ALL & ~CF}, {"dec ax", ALL & ~CF}, {"dec al", ALL & ~CF}, {"dec qword ptr [rbx]", ALL & ~CF},
    {"neg rax", ALL}, {"neg eax", ALL}, {"neg ax", ALL}, {"neg al", ALL}, {"neg qword ptr [rbx]", ALL},
    {"not rax", 0}, {"not eax", 0}, {"not ax", 0}, {"not al", 0}, {"not qword ptr [rbx]", 0},
    {"inc rax", ALL}, {"dec rax", ALL},  // CF must be preserved: compared too
    // ---- shifts and rotates: counts of 0, 1, many, and cl (OF/AF undefined unless the count is 1) ----
    {"shl rax, 1", ALL & ~AF}, {"shl rax, 5", ALL & ~(AF | OF)}, {"shl eax, 31", ALL & ~(AF | OF)}, {"shl eax, 0", ALL},
    {"shl ax, 3", ALL & ~(AF | OF)}, {"shl al, 1", ALL & ~AF}, {"shl al, 7", ALL & ~(AF | OF)}, {"shl rax, cl", ALL & ~(AF | OF)},
    {"shl eax, cl", ALL & ~(AF | OF)}, {"shl qword ptr [rbx], 4", ALL & ~(AF | OF)}, {"shl rax, 63", ALL & ~(AF | OF)},
    {"shr rax, 1", ALL & ~AF}, {"shr rax, 5", ALL & ~(AF | OF)}, {"shr eax, 31", ALL & ~(AF | OF)}, {"shr eax, 0", ALL},
    {"shr ax, 3", ALL & ~(AF | OF)}, {"shr al, 1", ALL & ~AF}, {"shr rax, cl", ALL & ~(AF | OF)}, {"shr eax, cl", ALL & ~(AF | OF)},
    {"shr dword ptr [rbx], 2", ALL & ~(AF | OF)}, {"shr rax, 63", ALL & ~(AF | OF)},
    {"sar rax, 1", ALL & ~AF}, {"sar rax, 5", ALL & ~(AF | OF)}, {"sar eax, 31", ALL & ~(AF | OF)}, {"sar ax, 3", ALL & ~(AF | OF)},
    {"sar al, 1", ALL & ~AF}, {"sar rax, cl", ALL & ~(AF | OF)}, {"sar eax, cl", ALL & ~(AF | OF)}, {"sar qword ptr [rbx], 7", ALL & ~(AF | OF)},
    {"rol rax, 1", CF | OF}, {"rol rax, 5", CF}, {"rol eax, 31", CF}, {"rol ax, 3", CF}, {"rol al, 1", CF | OF}, {"rol al, 3", CF},
    {"rol rax, cl", CF}, {"rol eax, cl", CF}, {"rol dword ptr [rbx], 9", CF},
    {"ror rax, 1", CF | OF}, {"ror rax, 5", CF}, {"ror eax, 31", CF}, {"ror ax, 3", CF}, {"ror al, 1", CF | OF}, {"ror al, 3", CF},
    {"ror rax, cl", CF}, {"ror eax, cl", CF}, {"ror dword ptr [rbx], 9", CF},
    {"rol rax, 5", ZF | SF | PF | AF}, {"ror eax, 7", ZF | SF | PF | AF},  // rotates leave these flags untouched
    // ---- multiply (only CF and OF are defined) ----
    {"imul rax, rbx", CF | OF}, {"imul eax, ebx", CF | OF}, {"imul ax, bx", CF | OF}, {"imul rax, qword ptr [rbx]", CF | OF},
    {"imul eax, ebx, 7", CF | OF}, {"imul rax, rbx, -3", CF | OF}, {"imul rax, qword ptr [rbx+8], 1000", CF | OF},
    {"imul ax, bx, 300", CF | OF}, {"imul rcx", CF | OF}, {"imul ecx", CF | OF}, {"imul cx", CF | OF}, {"imul cl", CF | OF},
    {"imul qword ptr [rbx]", CF | OF}, {"mul rcx", CF | OF}, {"mul ecx", CF | OF}, {"mul cx", CF | OF}, {"mul cl", CF | OF},
    {"mul qword ptr [rbx]", CF | OF}, {"mul dword ptr [rbx+4]", CF | OF},
    // ---- divide (inputs constrained so the hardware does not fault) ----
    {"xor edx, edx\nor rcx, 1\ndiv rcx", 0}, {"xor edx, edx\nor ecx, 3\ndiv ecx", 0},
    {"xor edx, edx\nor cx, 5\ndiv cx", 0}, {"movzx eax, al\nand ecx, 7\nor ecx, 3\ndiv cl", 0},
    {"xor edx, edx\nor qword ptr [rbx], 1\ndiv qword ptr [rbx]", 0},
    {"cqo\nand rcx, 0xffff\nor rcx, 2\nidiv rcx", 0}, {"cdq\nand ecx, 0xffff\nor ecx, 2\nidiv ecx", 0},
    {"cwde\ncdq\nand ecx, 0xff\nor ecx, 2\nidiv cx", 0}, {"movsx ax, al\nand ecx, 7\nor ecx, 3\nidiv cl", 0},
    {"cqo\nand qword ptr [rbx], 0xff\nor qword ptr [rbx], 2\nidiv qword ptr [rbx]", 0},
    // ---- bit tricks ----
    {"bt rax, rbx", CF}, {"bt eax, ebx", CF}, {"bt ax, bx", CF}, {"bt rax, 5", CF}, {"bt eax, 31", CF}, {"bt rax, 63", CF},
    {"bt qword ptr [rbx], 9", CF},
    {"bsf rax, rbx", ZF}, {"bsf eax, ebx", ZF}, {"bsf ax, bx", ZF}, {"bsr rax, rbx", ZF}, {"bsr eax, ebx", ZF}, {"bsr ax, bx", ZF},
    {"bsf rax, qword ptr [rbx]", ZF}, {"bsr eax, dword ptr [rbx+4]", ZF},
    {"lzcnt rax, rbx", CF | ZF}, {"lzcnt eax, ebx", CF | ZF}, {"lzcnt ax, bx", CF | ZF}, {"lzcnt rax, qword ptr [rbx]", CF | ZF},
    {"tzcnt rax, rbx", CF | ZF}, {"tzcnt eax, ebx", CF | ZF}, {"tzcnt ax, bx", CF | ZF},
    {"popcnt rax, rbx", ALL}, {"popcnt eax, ebx", ALL}, {"popcnt ax, bx", ALL},
    {"andn rax, rbx, rcx", CF | OF | SF | ZF}, {"andn eax, ebx, ecx", CF | OF | SF | ZF},
    {"andn rax, rbx, qword ptr [rbx+8]", CF | OF | SF | ZF},
    {"bextr rax, rbx, rcx", CF | OF | ZF}, {"bextr eax, ebx, ecx", CF | OF | ZF}, {"bextr rax, qword ptr [rbx], rcx", CF | OF | ZF},
    {"lahf", 0}, {"sahf", ALL},
    // ---- conditions: setcc / cmovcc / jcc after a compare ----
    {"cmp rax, rbx\nsetz al", 0}, {"cmp rax, rbx\nsetnz al", 0}, {"cmp rax, rbx\nsetb al", 0}, {"cmp rax, rbx\nsetnb al", 0},
    {"cmp rax, rbx\nsetbe al", 0}, {"cmp rax, rbx\nsetnbe al", 0}, {"cmp rax, rbx\nsetl al", 0}, {"cmp rax, rbx\nsetnl al", 0},
    {"cmp rax, rbx\nsetle al", 0}, {"cmp rax, rbx\nsetnle al", 0}, {"cmp rax, rbx\nsets al", 0}, {"cmp rax, rbx\nsetns al", 0},
    {"cmp rax, rbx\nsetp al", 0}, {"cmp rax, rbx\nsetnp al", 0}, {"cmp rax, rbx\nseto al", 0}, {"cmp rax, rbx\nsetno al", 0},
    {"cmp rax, rbx\nsetz byte ptr [rbx]", 0}, {"cmp eax, ebx\nsetb ah", 0},
    {"cmp rax, rbx\ncmovz rcx, rdx", 0}, {"cmp rax, rbx\ncmovnz ecx, edx", 0}, {"cmp rax, rbx\ncmovb rcx, rdx", 0},
    {"cmp rax, rbx\ncmovnb cx, dx", 0}, {"cmp rax, rbx\ncmovbe rcx, rdx", 0}, {"cmp rax, rbx\ncmovnbe ecx, edx", 0},
    {"cmp rax, rbx\ncmovl rcx, rdx", 0}, {"cmp rax, rbx\ncmovnl rcx, rdx", 0}, {"cmp rax, rbx\ncmovle ecx, edx", 0},
    {"cmp rax, rbx\ncmovnle rcx, rdx", 0}, {"cmp rax, rbx\ncmovs rcx, rdx", 0}, {"cmp rax, rbx\ncmovns ecx, edx", 0},
    {"cmp rax, rbx\ncmovp rcx, rdx", 0}, {"cmp rax, rbx\ncmovnp rcx, rdx", 0}, {"cmp rax, rbx\ncmovo rcx, rdx", 0},
    {"cmp rax, rbx\ncmovno rcx, rdx", 0}, {"cmp rax, rbx\ncmovz rcx, qword ptr [rbx]", 0},
    {"cmp rax, rbx\njz 1f\nmov rcx, 1\n1: nop", 0}, {"cmp rax, rbx\njnz 1f\nmov rcx, 1\n1: nop", 0},
    {"cmp rax, rbx\njb 1f\nmov rcx, 1\n1: nop", 0}, {"cmp rax, rbx\njnb 1f\nmov rcx, 1\n1: nop", 0},
    {"cmp rax, rbx\njbe 1f\nmov rcx, 1\n1: nop", 0}, {"cmp rax, rbx\njnbe 1f\nmov rcx, 1\n1: nop", 0},
    {"cmp rax, rbx\njl 1f\nmov rcx, 1\n1: nop", 0}, {"cmp rax, rbx\njnl 1f\nmov rcx, 1\n1: nop", 0},
    {"cmp rax, rbx\njle 1f\nmov rcx, 1\n1: nop", 0}, {"cmp rax, rbx\njnle 1f\nmov rcx, 1\n1: nop", 0},
    {"cmp rax, rbx\njs 1f\nmov rcx, 1\n1: nop", 0}, {"cmp rax, rbx\njns 1f\nmov rcx, 1\n1: nop", 0},
    {"cmp rax, rbx\njp 1f\nmov rcx, 1\n1: nop", 0}, {"cmp rax, rbx\njnp 1f\nmov rcx, 1\n1: nop", 0},
    {"cmp rax, rbx\njo 1f\nmov rcx, 1\n1: nop", 0}, {"cmp rax, rbx\njno 1f\nmov rcx, 1\n1: nop", 0},
    // ---- vector moves (VEX.128 zeroes bits 255:128 of the destination, VEX.256 does not) ----
    {"vmovups xmm0, xmm1", 0}, {"vmovups ymm2, ymm3", 0}, {"vmovups xmm4, xmmword ptr [rbx+16]", 0},
    {"vmovups ymm5, ymmword ptr [rbx+32]", 0}, {"vmovups xmmword ptr [rbx+48], xmm6", 0},
    {"vmovups ymmword ptr [rbx+64], ymm7", 0}, {"vmovups xmm15, xmm8", 0}, {"vmovups ymm9, ymm14", 0},
    {"vmovaps xmm0, xmm1", 0}, {"vmovaps ymm2, ymmword ptr [rbx+32]", 0}, {"vmovaps xmmword ptr [rbx+48], xmm3", 0},
    {"vmovdqa xmm0, xmmword ptr [rbx+16]", 0}, {"vmovdqa ymmword ptr [rbx+64], ymm1", 0},
    {"vmovdqu xmm0, xmmword ptr [rbx+5]", 0}, {"vmovdqu ymmword ptr [rbx+7], ymm1", 0}, {"vmovdqu xmm2, xmm3", 0},
    {"vmovupd xmm0, xmmword ptr [rbx+3]", 0}, {"vmovapd ymm1, ymm2", 0}, {"vmovapd xmmword ptr [rbx+16], xmm4", 0},
    {IDX "vmovups xmm0, xmmword ptr [rbx+rsi*4]", 0}, {IDX "vmovups xmmword ptr [rbx+rsi*8], xmm1", 0},
    // ---- scalar moves: loads zero the rest, the register form merges ----
    {"vmovss xmm0, dword ptr [rbx+4]", 0}, {"vmovss dword ptr [rbx+8], xmm1", 0}, {"vmovss xmm0, xmm1, xmm2", 0},
    {"vmovss xmm3, xmm3, xmm4", 0}, {"vmovss xmm12, xmm9, xmm10", 0},
    {"vmovsd xmm0, qword ptr [rbx+8]", 0}, {"vmovsd qword ptr [rbx+16], xmm1", 0}, {"vmovsd xmm0, xmm1, xmm2", 0},
    {"vmovd xmm0, eax", 0}, {"vmovd eax, xmm1", 0}, {"vmovd xmm0, dword ptr [rbx]", 0}, {"vmovd dword ptr [rbx+4], xmm2", 0},
    {"vmovd xmm9, ecx", 0}, {"vmovd r8d, xmm10", 0},
    {"vmovq xmm0, rax", 0}, {"vmovq rax, xmm1", 0}, {"vmovq xmm0, qword ptr [rbx]", 0}, {"vmovq qword ptr [rbx+8], xmm2", 0},
    {"vmovq xmm0, xmm1", 0},
    // ---- bitwise (floating-point and integer domain are the same bits) ----
    {"vxorps xmm0, xmm1, xmm2", 0}, {"vxorps xmm0, xmm0, xmm0", 0}, {"vxorps ymm0, ymm1, ymm2", 0},
    {"vxorps xmm3, xmm4, xmmword ptr [rbx+16]", 0}, {"vxorps ymm3, ymm4, ymmword ptr [rbx+32]", 0},
    {"vandps xmm0, xmm1, xmm2", 0}, {"vandps ymm0, ymm1, ymmword ptr [rbx+32]", 0},
    {"vandnps xmm0, xmm1, xmm2", 0}, {"vandnps ymm0, ymm1, ymm2", 0}, {"vorps xmm0, xmm1, xmm2", 0},
    {"vorps ymm0, ymm1, ymmword ptr [rbx+32]", 0}, {"vxorpd xmm0, xmm1, xmm2", 0}, {"vandpd ymm0, ymm1, ymm2", 0},
    {"vandnpd xmm0, xmm1, xmm2", 0}, {"vorpd xmm0, xmm1, xmm2", 0},
    {"vpxor xmm0, xmm1, xmm2", 0}, {"vpxor xmm0, xmm0, xmm0", 0}, {"vpand xmm0, xmm1, xmmword ptr [rbx+16]", 0},
    {"vpandn xmm0, xmm1, xmm2", 0}, {"vpor xmm0, xmm1, xmm2", 0}, {"vpxor xmm13, xmm14, xmm15", 0},
    // a counted loop (1..8 iterations: ecx is made non-zero first, otherwise dec wraps and it runs 2^32 times)
    {"and ecx, 7\ninc ecx\nxor eax, eax\n1: add rax, rcx\ndec ecx\njnz 1b", ALL},
    // flags set by one instruction and consumed several instructions later
    {"cmp rax, rbx\nmov rdx, rcx\nlea rsi, [rdx+1]\nsetb al\nadc rcx, 0", ALL},
};
constexpr size_t kNumCases = sizeof kCases / sizeof kCases[0];

uint64_t g_rng = 0x9e3779b97f4a7c15ull;
uint64_t rnd() {
    g_rng ^= g_rng << 13, g_rng ^= g_rng >> 7, g_rng ^= g_rng << 17;
    return g_rng;
}
uint64_t rand_value() {
    static const uint64_t edge[] = {0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffffull, 0x80000000ull,
                                    0xffffffffull, 0x7fffffffffffffffull, 0x8000000000000000ull, ~0ull, 31, 32, 63, 64};
    switch (rnd() % 4) {
    case 0: return edge[rnd() % (sizeof edge / sizeof edge[0])];
    case 1: return rnd() & 0xff;
    case 2: return rnd() & 0xffffffffull;
    default: return rnd();
    }
}

std::string run_cmd_log;
bool sh(const std::string& cmd, const std::string& log) {
    const std::string full = cmd + " > " + log + " 2>&1";
    return std::system(full.c_str()) == 0;
}

}  // namespace

int main() {
#if !defined(__x86_64__)
    std::puts("lift_diff: skipped (needs an x86-64 host)");
    return 0;
#else
    char tmpl[] = "/tmp/dcp_lift_diff_XXXXXX";
    const char* tmp = getenv("TMPDIR");
    std::string dir_t = std::string(tmp ? tmp : "/tmp") + "/dcp_lift_diff_XXXXXX";
    std::vector<char> dtv(dir_t.begin(), dir_t.end());
    dtv.push_back(0);
    if (!mkdtemp(dtv.data())) return std::puts("lift_diff: skipped (no temp dir)"), 0;
    (void)tmpl;
    const std::string dir = dtv.data();
    auto cleanup = [&] { std::system(("rm -rf '" + dir + "'").c_str()); };

    // 1. Assemble every case into a shared library so its bytes can be read back.
    {
        std::ofstream s(dir + "/cases.s");
        s << ".intel_syntax noprefix\n.text\n";
        for (size_t i = 0; i < kNumCases; ++i)
            s << ".globl " CSYM("case_") << i << "_s\n" CSYM("case_") << i << "_s:\n" << kCases[i].text << "\n.globl "
              << CSYM("case_") << i << "_e\n" CSYM("case_") << i << "_e:\n";
    }
    const std::string cxx = DCP_TEST_CXX, arch = DCP_TEST_ARCH;
#ifdef __APPLE__
    const char* asm_link = " -nodefaultlibs -lSystem";
#else
    const char* asm_link = " -nostdlib";
#endif
    if (!sh(cxx + " " + arch + " -shared" + asm_link + " -o " + dir + "/cases.so " + dir + "/cases.s", dir + "/asm.log")) {
        std::fprintf(stderr, "lift_diff: assembling the cases failed:\n");
        std::system(("cat " + dir + "/asm.log >&2").c_str());
        cleanup();
        return 1;
    }
    void* cases_lib = dlopen((dir + "/cases.so").c_str(), RTLD_NOW);
    if (!cases_lib) return std::fprintf(stderr, "lift_diff: dlopen cases: %s\n", dlerror()), cleanup(), 1;

    // 2. Lift each case (as a function of its own) and compile all of them into one library.
    struct Prog { std::vector<uint8_t> bytes; };
    std::vector<Prog> progs(kNumCases);
    std::vector<uint8_t> image(kNumCases * 0x400 + 0x400, 0xcc);
    std::string lifted = lift_prelude();
    lifted += "namespace dcp::rt { u64 dcp_img; void call_va(Ctx*, u64) { __builtin_trap(); } void call_ind(Ctx*, u64) "
              "{ __builtin_trap(); } void jmp_ind(Ctx*, u64) { __builtin_trap(); } }\n";
    int lift_fail = 0;
    for (size_t i = 0; i < kNumCases; ++i) {
        const auto* s = static_cast<const uint8_t*>(dlsym(cases_lib, ("case_" + std::to_string(i) + "_s").c_str()));
        const auto* e = static_cast<const uint8_t*>(dlsym(cases_lib, ("case_" + std::to_string(i) + "_e").c_str()));
        if (!s || !e || e <= s || e - s > 0x3f0) {
            std::fprintf(stderr, "lift_diff: case %zu has no bytes\n", i);
            return cleanup(), 1;
        }
        progs[i].bytes.assign(s, e);
        progs[i].bytes.push_back(0xc3);  // ret
        const uint64_t va = 0x400 + i * 0x400;
        std::copy(progs[i].bytes.begin(), progs[i].bytes.end(), image.begin() + long(va));
        Function fn = discover_function(image.data(), image.size(), 0, va, progs[i].bytes.size());
        LiftResult lr = lift_function(fn, {});
        if (fn.has_bad_bytes || !lr.unsupported.empty()) {
            ++lift_fail;
            std::fprintf(stderr, "FAIL case %zu [%s]: not liftable (%s)\n", i, kCases[i].text,
                         lr.unsupported.empty() ? "bad bytes" : lr.unsupported.begin()->first.c_str());
        }
        lifted += lr.code;
    }
    {
        std::ofstream f(dir + "/lifted.cpp");
        f << lifted;
    }
    if (!sh(cxx + " " + arch + " -std=c++20 -O1 -DDCP_TEST_FLAGS -shared -fPIC -I" DCP_TEST_RT_DIR " -o " + dir +
                "/lifted.so " + dir + "/lifted.cpp",
            dir + "/cc.log")) {
        std::fprintf(stderr, "lift_diff: compiling the lifted code failed:\n");
        std::system(("head -c 4000 " + dir + "/cc.log >&2").c_str());
        cleanup();
        return 1;
    }
    void* lib = dlopen((dir + "/lifted.so").c_str(), RTLD_NOW);
    if (!lib) return std::fprintf(stderr, "lift_diff: dlopen lifted: %s\n", dlerror()), cleanup(), 1;

    // 3. Run both versions from the same random state.
    auto* exec = static_cast<uint8_t*>(mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    alignas(64) static uint8_t mem[512], stack[8192];
    constexpr size_t kMemOff = 128, kStackTop = 6144;
    const uint64_t ptr = reinterpret_cast<uint64_t>(mem) + kMemOff, sp = reinterpret_cast<uint64_t>(stack) + kStackTop;

    int failures = lift_fail;
    size_t trials_total = 0;
    for (size_t i = 0; i < kNumCases; ++i) {
        const Case& cs = kCases[i];
        std::memcpy(exec, progs[i].bytes.data(), progs[i].bytes.size());
        char name[32];
        std::snprintf(name, sizeof name, "f_%llx", (unsigned long long)(0x400 + i * 0x400));
        using Fn = void (*)(Ctx*);
        Fn f = reinterpret_cast<Fn>(dlsym(lib, name));
        if (!f) {
            std::fprintf(stderr, "lift_diff: missing %s\n", name);
            ++failures;
            continue;
        }
        int bad = 0;
        for (int t = 0; t < 400 && bad < 2; ++t, ++trials_total) {
            Ctx in{};
            for (auto& r : in.r) r = rand_value();
            for (auto& v : in.x)
                for (auto& q : v.q) q = rand_value();
            in.r[dcp::rt::RBX] = ptr;
            in.r[dcp::rt::RSP] = sp;
            in.rflags = 2 | (rnd() & (CF | PF | AF | ZF | SF | OF));
            uint8_t mem0[512], stack0[8192];
            for (auto& b : mem0) b = uint8_t(rnd());
            std::memcpy(mem, mem0, sizeof mem);
            std::memset(stack, 0, sizeof stack);

            Ctx nat{};
            dcp_native_exec(exec, &in, &nat);
            uint8_t mem_nat[512];
            std::memcpy(mem_nat, mem, sizeof mem);
            std::memcpy(stack0, stack, sizeof stack);

            Ctx lf = in;
            lf.r[dcp::rt::RSP] = sp - 8;  // as if the caller had pushed the return address
            std::memcpy(mem, mem0, sizeof mem);
            std::memset(stack, 0, sizeof stack);
            f(&lf);

            bool ok = true;
            std::string why;
            for (int r = 0; r < 16 && ok; ++r)
                if (nat.r[r] != lf.r[r]) {
                    ok = false;
                    char b[96];
                    std::snprintf(b, sizeof b, "r[%d] native %llx lifted %llx", r, (unsigned long long)nat.r[r], (unsigned long long)lf.r[r]);
                    why = b;
                }
            if (ok && std::memcmp(nat.x, lf.x, sizeof nat.x)) {
                ok = false;
                for (int v = 0; v < 16; ++v)
                    if (std::memcmp(&nat.x[v], &lf.x[v], sizeof(dcp::rt::Vec))) {
                        char b[160];
                        std::snprintf(b, sizeof b, "ymm%d native %016llx%016llx lifted %016llx%016llx (low 128 bits)", v,
                                      (unsigned long long)nat.x[v].q[1], (unsigned long long)nat.x[v].q[0],
                                      (unsigned long long)lf.x[v].q[1], (unsigned long long)lf.x[v].q[0]);
                        why = b;
                        break;
                    }
            }
            if (ok && (nat.rflags ^ lf.rflags) & cs.mask) {
                ok = false;
                char b[96];
                std::snprintf(b, sizeof b, "flags native %llx lifted %llx (mask %x)", (unsigned long long)(nat.rflags & ALL),
                              (unsigned long long)(lf.rflags & ALL), cs.mask);
                why = b;
            }
            if (ok && std::memcmp(mem_nat, mem, sizeof mem)) ok = false, why = "memory differs";
            // The stack below the return-address slot: pushed values must match (the slot itself differs by design).
            if (ok && std::memcmp(stack0, stack, kStackTop - 8)) ok = false, why = "stack differs";
            if (!ok) {
                ++bad, ++failures;
                std::fprintf(stderr, "FAIL case %zu [%s]: %s\n   in: rax=%llx rcx=%llx rdx=%llx rsi=%llx flags=%llx\n", i,
                             cs.text, why.c_str(), (unsigned long long)in.r[0], (unsigned long long)in.r[1],
                             (unsigned long long)in.r[2], (unsigned long long)in.r[6], (unsigned long long)(in.rflags & ALL));
            }
        }
    }
    std::printf("lift_diff: %zu cases, %zu trials, %d failures\n", kNumCases, trials_total, failures);
    cleanup();
    return failures ? 1 : 0;
#endif
}
