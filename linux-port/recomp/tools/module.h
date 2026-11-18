// Loading a decrypted PS4 module (eboot or PRX) for recompilation: its image laid out at base 0, and its functions.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "cfg.h"

namespace dcp::recomp {

struct LoadedModule {
    std::vector<uint8_t> image;        // the mapped image at virtual address 0 (what map_image would produce, without relocations)
    std::vector<FdeRange> functions;   // from .eh_frame, in address order
    uint64_t entry = 0, init = 0, eh_frame = 0;  // virtual addresses (DT_INIT is 0 when the module has none)
};

// Throw std::runtime_error with a message fit to show the user (unreadable file, not a SELF/ELF, no .eh_frame, ...).
// The functions of the module: its .eh_frame ranges plus code that has no unwind info but is entered anyway: the entry point (for
// libc.prx, module_start sits at 0, before its first FDE), DT_INIT, PLT stubs that are only ever called, and tail-jump targets.
// Found by following the direct calls and jumps of every function until nothing new turns up. Sorted, never overlapping.
std::vector<FdeRange> complete_functions(const LoadedModule& m);

LoadedModule load_module(const std::string& path);
LoadedModule load_module_bytes(std::vector<uint8_t> file);

}  // namespace dcp::recomp
