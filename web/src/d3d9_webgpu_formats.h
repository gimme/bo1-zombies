// Direct3D 9 texture formats in WebGPU's (d3d9_webgpu.cpp): which format a texture has there, and how its texels are
// converted on upload where WebGPU has no such format.
#pragma once

#include <stdint.h>
#include <vector>

namespace bo1gpu
{
// library_gpu.js: BO1GPU.formats
enum GpuFormat : uint8_t
{
    GF_NONE,
    GF_BGRA8,
    GF_BGRA8_SRGB,
    GF_RGBA8,
    GF_RGBA8_SRGB,
    GF_RGBA8_SNORM,
    GF_RG8_SNORM,
    GF_RGB10A2,
    GF_R16F,
    GF_RG16F,
    GF_RGBA16F,
    GF_R32F,
    GF_RG32F,
    GF_RGBA32F,
    GF_BC1,
    GF_BC1_SRGB,
    GF_BC2,
    GF_BC2_SRGB,
    GF_BC3,
    GF_BC3_SRGB,
    GF_BC4,
    GF_BC5,
    GF_D16,
    GF_D24,
    GF_D24S8,
    GF_D32F,
};


// how D3D texels become the GPU format's
enum Conv : uint8_t
{
    CONV_NONE,
    CONV_ALPHA1, // X8R8G8B8, X8B8G8R8: alpha 255
    CONV_RGB8,   // R8G8B8 -> BGRA
    CONV_565,    // -> BGRA
    CONV_1555,
    CONV_X555,
    CONV_4444,
    CONV_A8,   // -> BGRA (0, 0, 0, A)
    CONV_L8,   // -> BGRA (L, L, L, 1)
    CONV_A8L8, // -> BGRA (L, L, L, A)
    CONV_A2R10,
    CONV_UNORM16, // each channel -> half float
    CONV_L16,     // -> RGBA half floats (L, L, L, 1)
    CONV_DECODE,  // block-compressed -> RGBA
    CONV_332,     // R3G3B2 -> BGRA
    CONV_8332,    // A8R3G3B2 -> BGRA
    CONV_A4L4,    // -> BGRA (L, L, L, A)
    // bump maps: signed channels, read as (U, V, then L or W, then 1 or A)
    CONV_SNORM16,     // V16U16, Q16W16V16U16: each channel -> half float
    CONV_L6V5U5,      // -> RGBA half floats (U, V, L, 1)
    CONV_X8L8V8U8,    // -> RGBA half floats (U, V, L, 1)
    CONV_A2W10V10U10, // -> RGBA half floats (U, V, W, A)
    CONV_CXV8U8,      // -> RGBA half floats (U, V, sqrt(1 - U^2 - V^2), 1)
    // video: pairs of pixels -> BGRA
    CONV_YUY2, // Y0 U Y1 V (BT.601)
    CONV_UYVY, // U Y0 V Y1
    CONV_RGBG, // R8G8_B8G8: R G0 B G1
    CONV_GRGB, // G8R8_G8B8: G0 R G1 B
};

struct TexFormat
{
    GpuFormat gpu = GF_NONE;
    GpuFormat srgb = GF_NONE; // the format of sRGB views (D3DSAMP_SRGBTEXTURE), if there is one
    Conv conv = CONV_NONE;
    uint8_t outBpp = 4;  // bytes a texel is uploaded as (block formats: a 4x4 block)
    bool block = false;  // uploaded in 4x4 blocks
    bool depth = false, stencil = false;
    bool clamp = false;  // a normalized D3D format in floats: what pixel shaders write to it is clamped to 0..1
    bool renderable = false;
    bool float32 = false; // filterable and blendable only with the features
    bool valid = true;
};

// the GPU format of a Direct3D 9 format (TexFormat::valid false: none)
TexFormat Format(uint32_t d3dFormat);
// block-compressed data decompressed to RGBA (no BC feature, or a size WebGPU does not take)
void Decompressed(TexFormat &t);
// D3D texels of `width` x `height` (pitch bytes a row; block formats: a row of blocks) in the GPU format, rows packed
const uint8_t *Convert(const TexFormat &tf, uint32_t d3dFormat, const uint8_t *src, uint32_t width, uint32_t height, uint32_t pitch,
    std::vector<uint8_t> &tmp);
// bytes a texel of a GPU format takes in a read back (0: not readable)
uint32_t GpuBpp(GpuFormat f);
uint16_t FloatToHalf(float f);
float HalfToFloat(uint16_t h);
} // namespace bo1gpu
