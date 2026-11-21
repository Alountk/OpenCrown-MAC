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

static void test_rep_prefix_kind() {
    Insn i;
    const uint8_t plain[] = {0xa4};                 // movsb
    CHECK(decode(plain, sizeof plain, 0, i) && i.rep_kind == 0);
    const uint8_t rep[] = {0xf3, 0xa4};             // rep movsb
    CHECK(decode(rep, sizeof rep, 0, i) && i.rep_kind == 1 && i.mnemonic == "movsb");
    const uint8_t repe[] = {0xf3, 0xa6};            // repe cmpsb
    CHECK(decode(repe, sizeof repe, 0, i) && i.rep_kind == 1 && i.mnemonic == "cmpsb");
    const uint8_t repne[] = {0xf2, 0xae};           // repne scasb
    CHECK(decode(repne, sizeof repne, 0, i) && i.rep_kind == 2 && i.mnemonic == "scasb");
    const uint8_t stos[] = {0xf3, 0x48, 0xab};      // rep stosq
    CHECK(decode(stos, sizeof stos, 0, i) && i.rep_kind == 1 && i.mnemonic == "stosq");
}

static void test_x87_operands() {
    Insn i;
    const uint8_t fadd[] = {0xd8, 0xc3};  // fadd st0, st3
    CHECK(decode(fadd, sizeof fadd, 0, i) && i.mnemonic == "fadd" && i.nops == 2 && i.ops[0].st == 0 && i.ops[1].st == 3);
    CHECK(i.ops[0].reg == kNoReg && i.ops[0].vec == kNoReg);
    const uint8_t faddp[] = {0xde, 0xc1};  // faddp st1, st0
    CHECK(decode(faddp, sizeof faddp, 0, i) && i.mnemonic == "faddp" && i.ops[0].st == 1 && i.ops[1].st == 0);
    const uint8_t fld[] = {0xdd, 0x00};    // fld qword ptr [rax]
    CHECK(decode(fld, sizeof fld, 0, i) && i.mnemonic == "fld" && i.ops[0].kind == OpKind::Mem && i.ops[0].bits == 64 && i.ops[0].st == kNoReg);
    const uint8_t fld80[] = {0xdb, 0x28};  // fld tbyte ptr [rax]
    CHECK(decode(fld80, sizeof fld80, 0, i) && i.ops[0].bits == 80);
    const uint8_t mov[] = {0x48, 0x89, 0xd8};
    CHECK(decode(mov, sizeof mov, 0, i) && i.ops[0].st == kNoReg);
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

    // `cmp edi,3; jae default` bounds the index to 3 entries as well (memcpy's size dispatch is written that way)
    uint8_t jae[sizeof code];
    std::memcpy(jae, code, sizeof code);
    jae[2] = 0x03, jae[3] = 0x73;
    Function g = discover_function(jae, sizeof jae, 0x1000, 0x1000, sizeof jae);
    CHECK(g.jump_tables.size() == 1 && g.jump_tables.count(0x1013) && g.jump_tables.at(0x1013).targets.size() == 3);

    // Table base and result share a register: movsxd rdi,[rcx+rdi*4]; add rcx,rdi; jmp rcx (what memcpy's size dispatch does)
    const uint8_t shared[] = {0x83, 0xff, 0x03, 0x73, 0x10, 0x48, 0x8d, 0x0d, 0x20, 0x00, 0x00, 0x00, 0x48, 0x63, 0x3c, 0xb9,
                              0x48, 0x01, 0xf9, 0xff, 0xe1,
                              0x31, 0xc0, 0xc3, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3, 0xb8, 0x02, 0x00, 0x00, 0x00, 0xc3,
                              0xb8, 0x03, 0x00, 0x00, 0x00, 0xc3, 0xcc, 0xcc,
                              0xec, 0xff, 0xff, 0xff, 0xf2, 0xff, 0xff, 0xff, 0xf8, 0xff, 0xff, 0xff};
    Function h = discover_function(shared, sizeof shared, 0x1000, 0x1000, sizeof shared);
    CHECK(h.jump_tables.size() == 1 && h.jump_tables.count(0x1013) && h.jump_tables.at(0x1013).targets.size() == 3);

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

static void test_computed_jump_into_function() {
    // memcpy-style dispatch: jmp [table + rdi*8] with a table of absolute addresses of labels inside the function. The targets are only
    // reachable through that jump, so they have to be found by sweeping the code the control-flow walk did not reach.
    const uint8_t code[] = {0x48, 0x8d, 0x0d, 0x0e, 0x00, 0x00, 0x00,   // 0x1000 lea rcx, [rip+0x0e]   (table at 0x1015)
                            0xff, 0x24, 0xf9,                           // 0x1007 jmp qword ptr [rcx+rdi*8]
                            0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3,         // 0x100a case 0: mov eax, 7; ret
                            0xcc, 0xcc, 0xcc, 0xcc, 0xcc};              // 0x1010 padding
    // (the table itself is not decoded: it is data, and no instruction of the walk reaches it)
    Function f = discover_function(code, sizeof code, 0x1000, 0x1000, sizeof code);
    CHECK(f.has_indirect_jump);                       // the jump cannot be resolved statically
    CHECK(f.blocks.count(0x100a) == 1);               // but the code after it is decoded, so a switch can target it
    CHECK(f.blocks.at(0x100a).insns.size() == 2);
    // Compilers pad jump targets with a nop: the code after the padding is a case of its own, even when the sweep reaches it by
    // falling through the padding (a call to the middle of such a function crashed the game before).
    const uint8_t padded[] = {0xff, 0xe0,                            // 0x1000 jmp rax
                              0x0f, 0x1f, 0x44, 0x00, 0x00,          // 0x1002 nop dword ptr [rax+rax*1+0]
                              0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};   // 0x1007 mov eax, 1; ret
    Function pf = discover_function(padded, sizeof padded, 0x1000, 0x1000, sizeof padded);
    CHECK(pf.has_indirect_jump && pf.blocks.count(0x1002) && pf.blocks.count(0x1007));
    CHECK(pf.blocks.at(0x1002).insns.size() == 1 && pf.blocks.at(0x1007).insns.size() == 2);
    Function g = discover_function(code, 0x10, 0x1000, 0x1000, 0x10);  // a function that ends right after the case: nothing to sweep past
    CHECK(g.blocks.count(0x100a) == 1);
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

static void test_entry_hooks() {
    // A function listed as hooked calls a host slot before its body runs; the slot is declared, defined and registered by name.
    const std::set<uint64_t> hooks{0x1000};
    const std::string d = lift_declarations({0x1000, 0x20f0}, "", hooks);
    CHECK(d.find("extern dcp::rt::EntryHook dcp_hook_1000;") != std::string::npos && d.find("dcp_hook_20f0") == std::string::npos);
    const std::string t = lift_function_table({0x1000, 0x20f0}, "libc", hooks);
    CHECK(t.find("dcp::rt::EntryHook dcp_hook_libc_1000 = nullptr;") != std::string::npos);
    CHECK(t.find("register_hook_slot(\"libc\", 0x1000u, &dcp_hook_libc_1000);") != std::string::npos && t.find("dcp_hook_libc_20f0") == std::string::npos);
    CHECK(lift_declarations({0x1000}).find("EntryHook") == std::string::npos);  // nothing changes without hooks
    // Many hooks: every slot is registered (the registration code is long) and the register function is still closed.
    std::set<uint64_t> many;
    for (uint64_t a = 0x1000; a < 0x1000 + 0x10 * 60; a += 0x10) many.insert(a);
    const std::string tm = lift_function_table(many, "eboot", many);
    CHECK(tm.find("&dcp_hook_eboot_1000);") != std::string::npos && tm.find("&dcp_hook_eboot_13b0);") != std::string::npos);
    CHECK(tm.size() > 4 && tm.compare(tm.size() - 3, 3, ");\n}") != 0 && tm.substr(tm.size() - 2) == "}\n");
    const uint8_t code[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};  // mov eax,1; ret
    const std::set<uint64_t> fns{0x1000};
    const Function fn = discover_function(code, sizeof code, 0x1000, 0x1000, sizeof code);
    const std::string hooked = lift_function(fn, fns, "", false, hooks).code;
    const size_t at = hooked.find("if (dcp_hook_1000) dcp_hook_1000(c);");
    CHECK(at != std::string::npos && at < hooked.find("DCP_LOAD();"));  // before the registers are loaded: the hook sees the entry state
    CHECK(lift_function(fn, fns, "", false).code.find("dcp_hook") == std::string::npos);
}

static void test_byte_patches() {
    // A patched instruction is lifted twice, behind a runtime flag: the replacement when the flag is set, the original otherwise.
    const uint8_t code[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};  // mov eax,1; ret
    const std::set<uint64_t> fns{0x1000};
    const Function fn = discover_function(code, sizeof code, 0x1000, 0x1000, sizeof code);
    const uint8_t repl[] = {0xb8, 0x02, 0x00, 0x00, 0x00};  // mov eax,2
    Insn alt;
    CHECK(decode(repl, sizeof repl, 0x1000, alt));
    const PatchMap patches{{0x1000, {alt}}};
    const std::string p = lift_function(fn, fns, "eboot", false, {}, patches).code;
    const size_t on = p.find("if (dcp_patch_eboot_1000) {"), els = p.find("} else {");
    CHECK(on != std::string::npos && els != std::string::npos && on < els);
    CHECK(p.find("0x2", on) < els && p.find("0x1", els) != std::string::npos);  // replacement first, original in the else branch
    CHECK(lift_function(fn, fns, "eboot", false).code.find("dcp_patch") == std::string::npos);  // nothing changes without patches
    CHECK(lift_declarations(fns, "eboot", {}, patches).find("extern uint8_t dcp_patch_eboot_1000;") != std::string::npos);
    CHECK(lift_declarations(fns, "eboot").find("dcp_patch") == std::string::npos);
    const std::string t = lift_function_table(fns, "eboot", {}, patches);
    CHECK(t.find("uint8_t dcp_patch_eboot_1000 = 0;") != std::string::npos);
    CHECK(t.find("register_patch_slot(\"eboot\", 0x1000u, &dcp_patch_eboot_1000);") != std::string::npos);
    CHECK(lift_function_table(fns, "eboot").find("dcp_patch") == std::string::npos);
}

static void test_indirect_call_operand_order() {
    // call [rsp+8]: x86 reads the target before it pushes the return address, so the stack operand sees the old rsp.
    const uint8_t code[] = {0xff, 0x54, 0x24, 0x08, 0xc3};
    const std::set<uint64_t> fns{0x1000};
    const Function fn = discover_function(code, sizeof code, 0x1000, 0x1000, sizeof code);
    const std::string c = lift_function(fn, fns, "eboot", false).code;
    const size_t read = c.find("ld<u64>"), push = c.find("r[RSP] -= 8");
    CHECK(read != std::string::npos && push != std::string::npos && read < push);
}

static void test_site_hooks() {
    // 0x1000 cmp edi,0 ; 0x1003 jne 0x100b ; 0x1005 mov eax,10 ; 0x100a ret ; 0x100b mov eax,20 ; 0x1010 ret
    const uint8_t code[] = {0x83, 0xff, 0x00, 0x75, 0x06, 0xb8, 0x0a, 0x00, 0x00, 0x00, 0xc3, 0xb8, 0x14, 0x00, 0x00, 0x00, 0xc3};
    const std::set<uint64_t> fns{0x1000};
    const Function fn = discover_function(code, sizeof code, 0x1000, 0x1000, sizeof code);
    const SiteMap sites{{0x1000, kSiteNoTarget}, {0x1003, 0x100b}, {0x1005, kSiteRet}};
    const LiftResult r = lift_function(fn, fns, "eboot", false, {}, {}, sites);
    const std::string& c = r.code;
    // The hook runs before its instruction: no target ignores the result, a block start jumps there, ret leaves the function.
    const size_t s0 = c.find("if (dcp_site_eboot_1000) {"), s3 = c.find("if (dcp_site_eboot_1003) {"), s5 = c.find("if (dcp_site_eboot_1005) {");
    CHECK(s0 != std::string::npos && s3 != std::string::npos && s5 != std::string::npos && s0 < s3 && s3 < s5);
    CHECK(c.find("goto L_100b;", s3) < s5);
    CHECK(c.find("dcp_site_eboot_1000(c)") != std::string::npos && c.find("goto", s0) > s3);  // nothing to jump to for 0x1000
    CHECK(c.find("return;", s5) != std::string::npos);
    CHECK(c.find("L_100b:;") != std::string::npos);  // the target gets a label even though nothing else jumps there
    CHECK(r.sites_hit == std::set<uint64_t>({0x1000, 0x1003, 0x1005}));
    CHECK(lift_function(fn, fns, "eboot", false).code.find("dcp_site") == std::string::npos);  // nothing changes without sites
    CHECK(lift_declarations(fns, "eboot", {}, {}, sites).find("extern dcp::rt::SiteHook dcp_site_eboot_1003;") != std::string::npos);
    CHECK(lift_declarations(fns, "eboot").find("dcp_site") == std::string::npos);
    const std::string t = lift_function_table(fns, "eboot", {}, {}, sites);
    CHECK(t.find("dcp::rt::SiteHook dcp_site_eboot_1003 = nullptr;") != std::string::npos);
    CHECK(t.find("register_site_slot(\"eboot\", 0x1003u, &dcp_site_eboot_1003);") != std::string::npos);
    CHECK(lift_function_table(fns, "eboot").find("dcp_site") == std::string::npos);
    // kSiteSkip: the hook did the instruction's work, a non-zero result jumps over the instruction (and runs nothing else of it).
    const std::string k = lift_function(fn, fns, "eboot", false, {}, {}, SiteMap{{0x1005, kSiteSkip}}).code;
    const size_t k0 = k.find("if (dcp_site_eboot_1005) {"), kgo = k.find("goto S_1005;", k0), kl = k.find("S_1005:;");
    CHECK(k0 != std::string::npos && kgo != std::string::npos && kl != std::string::npos && kgo < kl);
    const size_t ins = k.find("0xaull", kgo);  // mov eax,10 sits between the jump and the label
    CHECK(ins != std::string::npos && ins < kl);
    CHECK(lift_function(fn, fns, "eboot", false, {}, {}, SiteMap{{0x1003, 0x100b}}).code.find("S_1003") == std::string::npos);  // other targets: no label
    // A target that does not start a basic block of the function cannot be a label: refused.
    bool threw = false;
    try { lift_function(fn, fns, "eboot", false, {}, {}, SiteMap{{0x1003, 0x1007}}); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    // A site in a function that does not hold the instruction is simply not hit (the emitter reports the ones nobody hit).
    CHECK(lift_function(fn, fns, "eboot", false, {}, {}, SiteMap{{0x5000, kSiteRet}}).sites_hit.empty());
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
    test_rep_prefix_kind();
    test_x87_operands();
    test_jump_table();
    test_hoisted_table_base();
    test_computed_jump_into_function();
    test_declarations();
    test_entry_hooks();
    test_byte_patches();
    test_site_hooks();
    test_indirect_call_operand_order();
    test_eh_frame();
    if (g_fail) return 1;
    std::puts("recomp: all tests passed");
    return 0;
}
