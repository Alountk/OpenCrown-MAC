#include "bc7.h"

#include <cstring>
#include <utility>

namespace dcp::formats {
namespace {

#include "bc7_tables.inc"

struct Mode {
    uint8_t subsets, partition_bits, rotation_bits, index_selection, color_bits, alpha_bits;
    uint8_t endpoint_pbits;  // 1: a p-bit per endpoint; 0: none
    uint8_t shared_pbits;    // 1: one p-bit per subset, shared by both endpoints
    uint8_t index_bits, index2_bits;
};
constexpr Mode kModes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
    {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2}, {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0},
};
constexpr uint8_t kWeights2[4] = {0, 21, 43, 64};
constexpr uint8_t kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Bits {  // little-endian bit reader over the 128-bit block
    const uint8_t* p;
    int pos = 0;
    unsigned read(int n) {
        unsigned v = 0;
        for (int i = 0; i < n; ++i, ++pos) v |= unsigned((p[pos >> 3] >> (pos & 7)) & 1) << i;
        return v;
    }
};

int weight(int index_bits, int i) { return index_bits == 2 ? kWeights2[i] : index_bits == 3 ? kWeights3[i] : kWeights4[i]; }
int lerp(int a, int b, int w) { return ((64 - w) * a + w * b + 32) >> 6; }

}  // namespace

void bc7_decode_block(const uint8_t block[16], uint8_t rgba[64]) {
    if (block[0] == 0) {  // reserved mode
        std::memset(rgba, 0, 64);
        return;
    }
    Bits b{block};
    int mode = 0;
    while (!b.read(1)) ++mode;  // the mode is the number of leading zero bits
    const Mode& m = kModes[mode];
    const int partition = int(b.read(m.partition_bits));
    const int rotation = int(b.read(m.rotation_bits));
    const int index_selection = int(b.read(m.index_selection));

    // endpoints[subset][endpoint 0/1][channel RGBA]: all R values of every endpoint first, then G, B, A
    int ep[3][2][4] = {};
    const int n_ep = m.subsets * 2;
    for (int ch = 0; ch < 3; ++ch)
        for (int e = 0; e < n_ep; ++e) ep[e / 2][e % 2][ch] = int(b.read(m.color_bits));
    if (m.alpha_bits)
        for (int e = 0; e < n_ep; ++e) ep[e / 2][e % 2][3] = int(b.read(m.alpha_bits));
    int pbit[3][2] = {};
    if (m.endpoint_pbits)
        for (int e = 0; e < n_ep; ++e) pbit[e / 2][e % 2] = int(b.read(1));
    else if (m.shared_pbits)
        for (int s = 0; s < m.subsets; ++s) pbit[s][0] = pbit[s][1] = int(b.read(1));
    const bool has_p = m.endpoint_pbits || m.shared_pbits;
    for (int e = 0; e < n_ep; ++e)
        for (int ch = 0; ch < 4; ++ch) {
            int bits = ch < 3 ? m.color_bits : m.alpha_bits;
            if (!bits) {
                ep[e / 2][e % 2][ch] = 255;  // no alpha channel in this mode
                continue;
            }
            int v = ep[e / 2][e % 2][ch];
            if (has_p) v = v << 1 | pbit[e / 2][e % 2], ++bits;
            ep[e / 2][e % 2][ch] = v << (8 - bits) | v >> (2 * bits - 8);  // replicate the top bits into the low ones
        }

    // subset of every pixel and the anchors (their index has one bit less: the top bit is implied zero)
    uint8_t subset[16] = {};
    bool anchor[16] = {};
    anchor[0] = true;
    if (m.subsets == 2) {
        for (int i = 0; i < 16; ++i) subset[i] = kPart2[partition][i];
        anchor[kAnchor2[partition]] = true;
    } else if (m.subsets == 3) {
        for (int i = 0; i < 16; ++i) subset[i] = kPart3[partition][i];
        anchor[kAnchor3a[partition]] = anchor[kAnchor3b[partition]] = true;
    }
    // Anchors: pixel 0 is always the first subset's; the others are given per partition.
    uint8_t idx[16], idx2[16] = {};
    for (int i = 0; i < 16; ++i) idx[i] = uint8_t(b.read(anchor[i] ? m.index_bits - 1 : m.index_bits));
    if (m.index2_bits)
        for (int i = 0; i < 16; ++i) idx2[i] = uint8_t(b.read(i == 0 ? m.index2_bits - 1 : m.index2_bits));

    for (int i = 0; i < 16; ++i) {
        const int s = subset[i];
        int color_bits = m.index_bits, color_idx = idx[i], alpha_bits = m.index_bits, alpha_idx = idx[i];
        if (m.index2_bits) {  // modes 4 and 5: colour and alpha have their own index sets
            if (index_selection) {  // mode 4: the selection bit gives the colour the second (3-bit) set
                color_bits = m.index2_bits, color_idx = idx2[i], alpha_bits = m.index_bits, alpha_idx = idx[i];
            } else {
                alpha_bits = m.index2_bits, alpha_idx = idx2[i];
            }
        }
        int px[4];
        for (int ch = 0; ch < 3; ++ch) px[ch] = lerp(ep[s][0][ch], ep[s][1][ch], weight(color_bits, color_idx));
        px[3] = m.alpha_bits ? lerp(ep[s][0][3], ep[s][1][3], weight(alpha_bits, alpha_idx)) : 255;
        if (rotation) std::swap(px[3], px[rotation - 1]);
        for (int ch = 0; ch < 4; ++ch) rgba[i * 4 + ch] = uint8_t(px[ch]);
    }
}

void bc7_decode(const uint8_t* blocks, int width, int height, uint8_t* rgba) {
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            uint8_t px[64];
            bc7_decode_block(blocks + 16 * size_t(by * bw + bx), px);
            for (int y = 0; y < 4 && by * 4 + y < height; ++y)
                for (int x = 0; x < 4 && bx * 4 + x < width; ++x)
                    std::memcpy(rgba + (size_t(by * 4 + y) * width + bx * 4 + x) * 4, px + (y * 4 + x) * 4, 4);
        }
}

}  // namespace dcp::formats
