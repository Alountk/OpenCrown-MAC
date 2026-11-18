// Decoder, basic-block discovery and .eh_frame parsing on hand-assembled bytes. No game data.
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "cfg.h"
#include "lift.h"

using namespace dcp::recomp;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static void test_decode() {
    const uint8_t add[] = {0x48, 0x01, 0xd8};  // add rax, rbx
    Insn i;
    CHECK(decode(add, sizeof add, 0x1000, i));
    CHECK(i.len == 3 && i.flow == Flow::Normal && i.text == "add rax, rbx");

    const uint8_t call[] = {0xe8, 0xfb, 0x0f, 0x00, 0x00};  // call +0xffb -> 0x1000 + 5 + 0xffb
    CHECK(decode(call, sizeof call, 0x1000, i));
    CHECK(i.flow == Flow::Call && i.direct && i.target == 0x2000);

    const uint8_t jmp_rax[] = {0xff, 0xe0};
    CHECK(decode(jmp_rax, sizeof jmp_rax, 0x1000, i));
    CHECK(i.flow == Flow::Jump && !i.direct);

    const uint8_t vex[] = {0xc5, 0xf8, 0x58, 0xc1};  // vaddps xmm0, xmm0, xmm1 (AVX1)
    CHECK(decode(vex, sizeof vex, 0x1000, i));
    CHECK(i.len == 4 && i.text == "vaddps xmm0, xmm0, xmm1");

    // Vector registers are reported as Reg operands with `vec` = 0..15 and no GPR index.
    const uint8_t ymm[] = {0xc5, 0xec, 0x58, 0xcb};  // vaddps ymm1, ymm2, ymm3
    CHECK(decode(ymm, sizeof ymm, 0x1000, i));
    CHECK(i.nops == 3 && i.ops[0].kind == OpKind::Reg && i.ops[0].vec == 1 && i.ops[0].bits == 256 && i.ops[0].reg == kNoReg);
    CHECK(i.ops[1].vec == 2 && i.ops[2].vec == 3);
    const uint8_t xmm15[] = {0xc5, 0x78, 0x28, 0xf8};  // vmovaps xmm15, xmm0
    CHECK(decode(xmm15, sizeof xmm15, 0x1000, i));
    CHECK(i.ops[0].vec == 15 && i.ops[0].bits == 128 && i.ops[1].vec == 0);
    CHECK(decode(add, sizeof add, 0x1000, i) && i.ops[0].vec == kNoReg);  // a GPR operand is not a vector

    const uint8_t bmi[] = {0xc4, 0xe2, 0x78, 0xf2, 0xc3};  // andn eax, eax, ebx (BMI1)
    CHECK(decode(bmi, sizeof bmi, 0x1000, i));
    CHECK(i.text == "andn eax, eax, ebx");

    const uint8_t ud2[] = {0x0f, 0x0b};
    CHECK(decode(ud2, sizeof ud2, 0x1000, i) && i.flow == Flow::Trap);

    const uint8_t bad[] = {0x0f};  // truncated
    CHECK(!decode(bad, sizeof bad, 0x1000, i));
}

static void test_byte_registers() {
    // 8-bit registers beyond bh: spl bpl sil dil are 4..7 (REX encoding) and r8b..r15b are 8..15, not their enum order.
    Insn i;
    const uint8_t sil[] = {0x40, 0x80, 0xfe, 0x03};      // cmp sil, 3
    CHECK(decode(sil, sizeof sil, 0, i) && i.ops[0].reg == 6 && !i.ops[0].high8 && i.ops[0].bits == 8);
    const uint8_t dil[] = {0x40, 0xb7, 0x05};            // mov dil, 5
    CHECK(decode(dil, sizeof dil, 0, i) && i.ops[0].reg == 7);
    const uint8_t spl[] = {0x40, 0xb4, 0x05};            // mov spl, 5
    CHECK(decode(spl, sizeof spl, 0, i) && i.ops[0].reg == 4 && !i.ops[0].high8);
    const uint8_t bpl[] = {0x40, 0xb5, 0x05};            // mov bpl, 5
    CHECK(decode(bpl, sizeof bpl, 0, i) && i.ops[0].reg == 5 && !i.ops[0].high8);
    const uint8_t r8b[] = {0x41, 0xb0, 0x05};            // mov r8b, 5
    CHECK(decode(r8b, sizeof r8b, 0, i) && i.ops[0].reg == 8);
    const uint8_t r15b[] = {0x41, 0xb7, 0x05};           // mov r15b, 5
    CHECK(decode(r15b, sizeof r15b, 0, i) && i.ops[0].reg == 15);
    const uint8_t ah[] = {0xb4, 0x05};                   // mov ah, 5: still the high byte of rax
    CHECK(decode(ah, sizeof ah, 0, i) && i.ops[0].reg == 0 && i.ops[0].high8);
    const uint8_t bh[] = {0xb7, 0x05};                   // mov bh, 5: high byte of rbx
    CHECK(decode(bh, sizeof bh, 0, i) && i.ops[0].reg == 3 && i.ops[0].high8);
}

static void test_jump_table() {
    // switch (edi) { case 0: return 1; case 1: return 2; case 2: return 3; default: return 0; } as compilers emit it:
    // cmp edi,2; ja default; lea rcx,[rip+table]; movsxd rax,[rcx+rdi*4]; add rax,rcx; jmp rax
    const uint8_t code[] = {0x83, 0xff, 0x02, 0x77, 0x10, 0x48, 0x8d, 0x0d, 0x20, 0x00, 0x00, 0x00, 0x48, 0x63, 0x04, 0xb9,
                            0x48, 0x01, 0xc8, 0xff, 0xe0,
                            0x31, 0xc0, 0xc3,                          // 0x1015 default
                            0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3,       // 0x1018 case 0
                            0xb8, 0x02, 0x00, 0x00, 0x00, 0xc3,       // 0x101e case 1
                            0xb8, 0x03, 0x00, 0x00, 0x00, 0xc3,       // 0x1024 case 2
                            0xcc, 0xcc,
                            0xec, 0xff, 0xff, 0xff, 0xf2, 0xff, 0xff, 0xff, 0xf8, 0xff, 0xff, 0xff};  // 0x102c table
    Function f = discover_function(code, sizeof code, 0x1000, 0x1000, sizeof code);
    CHECK(!f.has_bad_bytes && !f.has_indirect_jump);
    CHECK(f.jump_tables.size() == 1 && f.jump_tables.count(0x1013));
    const JumpTable& t = f.jump_tables.at(0x1013);
    CHECK(t.table == 0x102c && t.targets.size() == 3);
    CHECK(t.targets.size() == 3 && t.targets[0] == 0x1018 && t.targets[1] == 0x101e && t.targets[2] == 0x1024);
    CHECK(f.blocks.size() == 6 && f.blocks.count(0x1018) && f.blocks.count(0x1024) && f.blocks.count(0x1015));
    CHECK(f.blocks.at(0x1005).succ.size() == 3);  // the jump block reaches every case

    // The same jump without the bounds check cannot be resolved: it stays an indirect jump.
    const uint8_t unbounded[] = {0x48, 0x8d, 0x0d, 0x0b, 0x00, 0x00, 0x00, 0x48, 0x63, 0x04, 0xb9, 0x48, 0x01, 0xc8, 0xff, 0xe0};
    Function u = discover_function(unbounded, sizeof unbounded, 0x2000, 0x2000, sizeof unbounded);
    CHECK(u.has_indirect_jump && u.jump_tables.empty());
}

static void test_hoisted_table_base() {
    // The table base is loaded once, before the bounds check, in a block the jump's straight-line path does not contain:
    //   lea rbx,[rip+table]; jmp next; next: cmp edi,2; ja default; movsxd rax,[rbx+rdi*4]; add rax,rbx; jmp rax
    const uint8_t code[] = {0x48, 0x8d, 0x1d, 0x25, 0x00, 0x00, 0x00, 0xeb, 0x00,
                            0x83, 0xff, 0x02, 0x77, 0x09, 0x48, 0x63, 0x04, 0xbb, 0x48, 0x01, 0xd8, 0xff, 0xe0,
                            0x31, 0xc0, 0xc3,                          // 0x1017 default
                            0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3,       // 0x101a case 0
                            0xb8, 0x02, 0x00, 0x00, 0x00, 0xc3,       // 0x1020 case 1
                            0xb8, 0x03, 0x00, 0x00, 0x00, 0xc3,       // 0x1026 case 2
                            0xee, 0xff, 0xff, 0xff, 0xf4, 0xff, 0xff, 0xff, 0xfa, 0xff, 0xff, 0xff};  // 0x102c table
    Function f = discover_function(code, sizeof code, 0x1000, 0x1000, sizeof code);
    CHECK(!f.has_bad_bytes && !f.has_indirect_jump);
    CHECK(f.jump_tables.size() == 1 && f.jump_tables.count(0x1015));
    CHECK(f.jump_tables.at(0x1015).table == 0x102c && f.jump_tables.at(0x1015).targets.size() == 3);
    CHECK(f.blocks.count(0x101a) && f.blocks.count(0x1020) && f.blocks.count(0x1026) && f.blocks.count(0x1017));
}

static void test_declarations() {
    // Generated functions call each other in any order, so the output starts with one declaration per function.
    const std::string d = lift_declarations({0x1000, 0x20f0});
    CHECK(d.find("extern \"C\" void f_1000(Ctx*);\n") != std::string::npos);
    CHECK(d.find("extern \"C\" void f_20f0(Ctx*);\n") != std::string::npos);
    CHECK(lift_declarations({}).empty());
    // Several modules (eboot, libc.prx, ...) share offsets, so their symbols carry the module name.
    CHECK(function_name(0x1000, "libc") == "f_libc_1000" && function_name(0x1000) == "f_1000");
    const std::string dm = lift_declarations({0x1000}, "libc");
    CHECK(dm.find("extern \"C\" void f_libc_1000(Ctx*);") != std::string::npos);
    CHECK(dm.find("extern dcp::rt::u64 dcp_img_libc;") != std::string::npos);
    const std::string tm = lift_function_table({0x1000}, "libc");
    CHECK(tm.find("{0x1000u, f_libc_1000}") != std::string::npos && tm.find("dcp_register_libc(") != std::string::npos);
    CHECK(tm.find("dcp_img_libc = img_base") != std::string::npos && tm.find("add_module(") != std::string::npos);
    // The function table is sorted (std::set), registers itself, and survives an empty program.
    const std::string t = lift_function_table({0x20f0, 0x1000});
    CHECK(t.find("{0x1000u, f_1000},\n  {0x20f0u, f_20f0}") != std::string::npos);
    CHECK(t.find("set_function_table(kFunctions, 2)") != std::string::npos);
    CHECK(lift_function_table({}).find("set_function_table(kFunctions, 0)") != std::string::npos);
}

static void test_cfg() {
    // push rbp; mov rbp,rsp; test edi,edi; jz L; call 0x2000; pop rbp; ret; L: xor eax,eax; pop rbp; ret
    const uint8_t code[] = {0x55, 0x48, 0x89, 0xe5, 0x85, 0xff, 0x74, 0x07, 0xe8, 0xf3, 0x0f, 0x00, 0x00,
                            0x5d, 0xc3, 0x31, 0xc0, 0x5d, 0xc3};
    Function f = discover_function(code, sizeof code, 0x1000, 0x1000, sizeof code);
    CHECK(!f.has_bad_bytes && !f.has_indirect_jump);
    CHECK(f.blocks.size() == 3);
    CHECK(f.calls.count(0x2000) == 1 && f.calls.size() == 1);
    CHECK(f.blocks.count(0x1000) && f.blocks.count(0x1008) && f.blocks.count(0x100f));
    const Block& entry = f.blocks.at(0x1000);
    CHECK(entry.insns.size() == 4 && entry.succ.size() == 2);
    CHECK(f.blocks.at(0x1008).end == 0x100f && f.blocks.at(0x100f).end == 0x1013);

    // A jump into the middle of a straight-line run must split that run into two blocks.
    // L0: xor eax,eax; L1: inc eax; cmp eax,3; jne L1; ret
    const uint8_t loop[] = {0x31, 0xc0, 0xff, 0xc0, 0x83, 0xf8, 0x03, 0x75, 0xf9, 0xc3};
    Function l = discover_function(loop, sizeof loop, 0x3000, 0x3000, sizeof loop);
    CHECK(l.blocks.size() == 3 && l.blocks.count(0x3002));
    CHECK(l.blocks.at(0x3000).succ.size() == 1 && l.blocks.at(0x3000).succ[0] == 0x3002);

    // Tail jump out of the function, and an indirect jump (jump table).
    const uint8_t tail[] = {0xe9, 0xfb, 0x0f, 0x00, 0x00};
    Function t = discover_function(tail, sizeof tail, 0x1000, 0x1000, sizeof tail);
    CHECK(t.tail_jumps.count(0x2000) == 1 && t.blocks.size() == 1);
    const uint8_t ind[] = {0xff, 0xe0};
    Function n = discover_function(ind, sizeof ind, 0x1000, 0x1000, sizeof ind);
    CHECK(n.has_indirect_jump);

    // Reaching the end exactly (a call to a noreturn function with no ud2 after it) is recorded, not an error.
    const uint8_t noret[] = {0xe8, 0xfb, 0x0f, 0x00, 0x00};
    Function nr = discover_function(noret, sizeof noret, 0x1000, 0x1000, sizeof noret);
    CHECK(nr.fell_off_end && !nr.has_bad_bytes && nr.calls.count(0x2000) == 1);
    // A conditional jump that lands outside the image is an error, and so is an instruction cut by the range end.
    const uint8_t wild[] = {0x74, 0x7e, 0xc3};  // jz +0x7e -> 0x1080, past the 3-byte image
    CHECK(discover_function(wild, sizeof wild, 0x1000, 0x1000, sizeof wild).tail_jumps.count(0x1080) == 1);
    const uint8_t cut[] = {0x48, 0x01};
    CHECK(discover_function(cut, sizeof cut, 0x1000, 0x1000, sizeof cut).decode_failed);
}

struct Buf {
    std::vector<uint8_t> v;
    void u8(uint8_t x) { v.push_back(x); }
    void u32(uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
    void pad(size_t align) { while (v.size() % align) v.push_back(0); }
};

static void test_eh_frame() {
    Buf b;
    // CIE: version 1, "zR", code align 1, data align -8, ra reg 16, aug data {R = pcrel|sdata4}, def_cfa r7+8, offset r16
    Buf cie;
    cie.u32(0);  // CIE id
    for (uint8_t x : std::initializer_list<uint8_t>{1, 'z', 'R', 0, 1, 0x78, 16, 1, 0x1b, 0x0c, 7, 8, 0x90, 1}) cie.u8(x);
    cie.pad(4);  // length field (4) + body must be 8-aligned overall; DW_CFA_nop is 0
    b.u32(uint32_t(cie.v.size()));
    b.v.insert(b.v.end(), cie.v.begin(), cie.v.end());

    const uint64_t vaddr = 0x5000;
    auto fde = [&](int32_t begin_va, uint32_t range) {
        const size_t at = b.v.size();
        Buf f;
        f.u32(uint32_t(at + 4));  // distance from this field back to the CIE (CIE is at offset 0)
        const uint64_t field_va = vaddr + at + 8;
        f.u32(uint32_t(int32_t(begin_va - int64_t(field_va))));  // pc_begin, pcrel sdata4
        f.u32(range);
        f.u8(0);  // augmentation length
        f.pad(4);
        b.u32(uint32_t(f.v.size()));
        b.v.insert(b.v.end(), f.v.begin(), f.v.end());
    };
    fde(0x1000, 0x40);
    fde(0x1040, 0x13);
    b.u32(0);  // terminator

    std::vector<FdeRange> r = parse_eh_frame(b.v.data(), b.v.size(), vaddr);
    CHECK(r.size() == 2);
    if (r.size() == 2) {
        CHECK(r[0].start == 0x1000 && r[0].size == 0x40);
        CHECK(r[1].start == 0x1040 && r[1].size == 0x13);
    }
    CHECK(parse_eh_frame(b.v.data(), 6, vaddr).empty());  // truncated input is rejected, not read past
}

int main() {
    test_decode();
    test_cfg();
    test_byte_registers();
    test_jump_table();
    test_hoisted_table_base();
    test_declarations();
    test_eh_frame();
    if (g_fail) return 1;
    std::puts("recomp: all tests passed");
    return 0;
}
