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
    uint64_t entry = 0, eh_frame = 0;  // virtual addresses
};

// Throw std::runtime_error with a message fit to show the user (unreadable file, not a SELF/ELF, no .eh_frame, ...).
LoadedModule load_module(const std::string& path);
LoadedModule load_module_bytes(std::vector<uint8_t> file);

}  // namespace dcp::recomp
