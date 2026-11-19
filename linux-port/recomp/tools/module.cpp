#include "module.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
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
    m.entry = info.entry, m.init = info.init;
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

std::vector<FdeRange> complete_functions(const LoadedModule& m) {
    constexpr uint64_t kMaxExtra = 0x10000;  // an unwind-less function is cut at the next function or this many bytes
    std::map<uint64_t, uint64_t> fde;       // start -> size, as the module declares them
    for (const FdeRange& r : m.functions) fde[r.start] = r.size;
    auto inside_fde = [&](uint64_t a) {  // a lies strictly inside a declared function (not at its start)
        auto it = fde.upper_bound(a);
        if (it == fde.begin()) return false;
        --it;
        return a > it->first && a < it->first + it->second;
    };
    std::set<uint64_t> starts;
    for (const auto& [a, sz] : fde) starts.insert(a);
    std::vector<uint64_t> seeds{m.entry};
    if (m.init) seeds.push_back(m.init);
    for (uint64_t a : seeds)
        if (a < m.image.size() && !inside_fde(a)) starts.insert(a);

    auto size_of = [&](uint64_t s) {
        // The declared function that covers s (s itself or one it lies inside): an entry added in the middle of a function keeps
        // the rest of that function as its range, and entries inside a declared range never cut another range short.
        uint64_t cover_end = 0;
        auto cd = fde.upper_bound(s);
        if (cd != fde.begin()) {
            --cd;
            if (s < cd->first + cd->second) cover_end = cd->first + cd->second;
        }
        auto next = starts.upper_bound(s);
        while (cover_end && next != starts.end() && *next < cover_end && !fde.count(*next)) ++next;
        const uint64_t limit = next == starts.end() ? m.image.size() : *next;
        const uint64_t want = cover_end ? cover_end - s : kMaxExtra;
        return std::min<uint64_t>(want, limit - s);
    };
    std::map<uint64_t, uint64_t> scanned;  // start -> the size it was scanned with
    for (bool changed = true; changed;) {
        changed = false;
        const std::vector<uint64_t> snapshot(starts.begin(), starts.end());
        for (uint64_t s : snapshot) {
            const uint64_t sz = size_of(s);
            auto it = scanned.find(s);
            if (it != scanned.end() && it->second == sz) continue;
            scanned[s] = sz;
            const Function fn = discover_function(m.image.data(), m.image.size(), 0, s, sz);
            auto consider = [&](uint64_t t) {
                if (t < m.image.size() && !starts.count(t) && !inside_fde(t)) starts.insert(t), changed = true;
            };
            for (uint64_t t : fn.calls) consider(t);
            // A direct jmp into the middle of a declared function is a shared tail: its target is an entry of its own (unlike a
            // call target there, which could be a misdecoded one).
            for (uint64_t t : fn.tail_jumps)
                if (t < m.image.size() && !starts.count(t)) starts.insert(t), changed = true;
            changed = changed || false;
        }
        // a new start splits the range of its predecessor: sizes change, so rescan
        for (uint64_t s : starts)
            if (scanned.count(s) && scanned[s] != size_of(s)) changed = true;
    }
    std::vector<FdeRange> out;
    for (uint64_t s : starts) out.push_back({s, size_of(s)});
    return out;
}

LoadedModule load_module(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read " + path);
    return load_module_bytes(std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}));
}

}  // namespace dcp::recomp
