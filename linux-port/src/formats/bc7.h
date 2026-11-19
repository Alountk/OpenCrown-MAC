// BC7 (BPTC) texture decoding to RGBA8. Used where the GPU API has no BC7 (Apple's OpenGL).
#pragma once
#include <cstdint>

namespace dcp::formats {

// One 16-byte block to 4x4 RGBA8 pixels (row-major, 64 bytes). A reserved mode gives transparent black.
void bc7_decode_block(const uint8_t block[16], uint8_t rgba[64]);

// A width x height image of blocks (rows of ceil(width/4) blocks) to width x height RGBA8 pixels.
void bc7_decode(const uint8_t* blocks, int width, int height, uint8_t* rgba);

}  // namespace dcp::formats
