#include "module.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "loader/elf.h"

namespace dcp::recomp {
namespace {

uint64_t rd(const uint8_t* p, int n) {
    uint64_t v = 0;
    std::memcpy(&v, p, size_t(n));
    return v;
}

}  // namespace

LoadedModule load_module_bytes(std::vector<uint8_t> file) {
    std::vector<uint8_t> elf = loader::unwrap_self(std::move(file));
    const loader::ElfInfo info = loader::parse_elf(elf);

    LoadedModule m;
    m.entry = info.entry;
    m.image.assign(info.image_size, 0);  // laid out at base 0, like map_image
    for (const auto& s : info.load)
        if (s.offset + s.filesz <= elf.size() && s.vaddr + s.filesz <= m.image.size())
            std::memcpy(m.image.data() + s.vaddr, elf.data() + s.offset, s.filesz);

    // PT_GNU_EH_FRAME (0x6474e550) -> .eh_frame_hdr -> .eh_frame
    const uint64_t phoff = rd(elf.data() + 0x20, 8);
    const unsigned phentsize = unsigned(rd(elf.data() + 0x36, 2)), phnum = unsigned(rd(elf.data() + 0x38, 2));
    uint64_t hdr = 0;
    for (unsigned i = 0; i < phnum; ++i) {
        const uint8_t* ph = elf.data() + phoff + uint64_t(i) * phentsize;
        if (rd(ph, 4) == 0x6474e550) hdr = rd(ph + 16, 8);
    }
    if (!hdr || hdr + 8 > m.image.size()) throw std::runtime_error("the module has no PT_GNU_EH_FRAME (.eh_frame): it cannot be split into functions");
    if (m.image[hdr + 1] != 0x1b)
        throw std::runtime_error("unexpected .eh_frame_hdr encoding 0x" + std::to_string(m.image[hdr + 1]) + " (only pcrel sdata4 is supported)");
    const int32_t rel = int32_t(rd(m.image.data() + hdr + 4, 4));
    m.eh_frame = hdr + 4 + uint64_t(int64_t(rel));
    if (m.eh_frame >= m.image.size()) throw std::runtime_error("the .eh_frame address is outside the image");
    m.functions = parse_eh_frame(m.image.data() + m.eh_frame, m.image.size() - m.eh_frame, m.eh_frame);
    if (m.functions.empty()) throw std::runtime_error("the .eh_frame of the module is empty or malformed");
    return m;
}

LoadedModule load_module(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    return load_module_bytes(std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}));
}

}  // namespace dcp::recomp
