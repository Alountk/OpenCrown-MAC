// Basic-block discovery inside one function, and function boundaries from .eh_frame.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "decoder.h"

namespace dcp::recomp {

struct Block {
    uint64_t start = 0, end = 0;          // [start, end) in guest addresses
    std::vector<Insn> insns;
    std::vector<uint64_t> succ;           // direct successors inside the function (fallthrough and branch targets)
};

// A switch compiled to `lea T,[table]; movsxd R,[T+I*4]; add R,T; jmp R` behind a `cmp I,N; ja default` bounds check.
struct JumpTable {
    uint64_t table = 0;                   // guest address of the int32 offset table (offsets are relative to it)
    std::vector<uint64_t> targets;        // entry k jumps to targets[k]; N + 1 entries
};

struct Function {
    uint64_t start = 0, size = 0;
    std::map<uint64_t, Block> blocks;     // keyed by block start
    std::set<uint64_t> calls;             // direct call targets
    std::set<uint64_t> tail_jumps;        // direct jumps that leave [start, start+size)
    std::map<uint64_t, JumpTable> jump_tables;  // keyed by the address of the `jmp R` they end in
    bool has_indirect_jump = false;       // an indirect jump that is not a recognised jump table (tail call, computed goto)
    bool has_bad_bytes = false;           // undecodable bytes, or control flow that left the function/image
    bool decode_failed = false;           // bytes that are not a valid instruction
    bool fell_off_end = false;            // straight-line code reaches the end of the range (noreturn call, no ud2): not an error
    uint64_t bad_at = 0;                  // address of the first problem
};

// Decodes the function at [start, start+size) from `code`, which holds the bytes of the image starting at guest
// address `code_base`. Follows direct branches only; every reachable instruction ends up in exactly one block.
Function discover_function(const uint8_t* code, size_t code_size, uint64_t code_base, uint64_t start, uint64_t size);

struct FdeRange { uint64_t start, size; };
// Function ranges from the FDEs of a raw .eh_frame section loaded at guest address `vaddr`. Stops at the terminator.
// Returns an empty vector on malformed input.
std::vector<FdeRange> parse_eh_frame(const uint8_t* data, size_t size, uint64_t vaddr);

}  // namespace dcp::recomp
