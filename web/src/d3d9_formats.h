// Direct3D 9 surface format facts shared by the device and its WebGPU executor.
#pragma once

#include <d3d9.h>
#include <stdint.h>

#define BO1_FOURCC(a, b, c, d) ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

static const uint32_t BO1FMT_ATI1 = BO1_FOURCC('A', 'T', 'I', '1');
static const uint32_t BO1FMT_ATI2 = BO1_FOURCC('A', 'T', 'I', '2');
static const uint32_t BO1FMT_INTZ = BO1_FOURCC('I', 'N', 'T', 'Z');
static const uint32_t BO1FMT_RAWZ = BO1_FOURCC('R', 'A', 'W', 'Z');
static const uint32_t BO1FMT_DF24 = BO1_FOURCC('D', 'F', '2', '4');
static const uint32_t BO1FMT_DF16 = BO1_FOURCC('D', 'F', '1', '6');
static const uint32_t BO1FMT_NULL = BO1_FOURCC('N', 'U', 'L', 'L');

inline bool Bo1Fmt_IsBlock(uint32_t f)
{
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5 || f == BO1FMT_ATI1 ||
           f == BO1FMT_ATI2;
}

inline uint32_t Bo1Fmt_BlockBytes(uint32_t f) { return (f == D3DFMT_DXT1 || f == BO1FMT_ATI1) ? 8 : 16; }

inline bool Bo1Fmt_IsDepth(uint32_t f)
{
    switch (f)
    {
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D32:
    case D3DFMT_D15S1:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D16:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D24FS8:
        return true;
    default:
        return f == BO1FMT_INTZ || f == BO1FMT_RAWZ || f == BO1FMT_DF24 || f == BO1FMT_DF16;
    }
}

// bytes per texel of the uncompressed formats (0: unknown)
inline uint32_t Bo1Fmt_Bpp(uint32_t f)
{
    switch (f)
    {
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_P8:
    case D3DFMT_R3G3B2:
    case D3DFMT_A4L4:
        return 1;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8:
    case D3DFMT_L16:
    case D3DFMT_V8U8:
    case D3DFMT_R16F:
    case D3DFMT_L6V5U5:
    case D3DFMT_CxV8U8:
    case D3DFMT_A8P8:
    case D3DFMT_YUY2:
    case D3DFMT_UYVY:
    case D3DFMT_R8G8_B8G8:
    case D3DFMT_G8R8_G8B8:
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D15S1:
    case D3DFMT_A8R3G3B2:
        return 2;
    case D3DFMT_R8G8B8:
        return 3;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_G16R16:
    case D3DFMT_G16R16F:
    case D3DFMT_R32F:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:
    case D3DFMT_X8L8V8U8:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D24FS8:
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_INDEX32:
        return 4;
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_G32R32F:
    case D3DFMT_Q16W16V16U16:
        return 8;
    case D3DFMT_A32B32G32R32F:
        return 16;
    default:
        if (f == BO1FMT_INTZ || f == BO1FMT_RAWZ || f == BO1FMT_DF24)
            return 4;
        if (f == BO1FMT_DF16)
            return 2;
        return 4;
    }
}

// bytes per row of texels (block formats: per row of 4x4 blocks)
inline uint32_t Bo1Fmt_Pitch(uint32_t f, uint32_t width)
{
    if (Bo1Fmt_IsBlock(f))
        return ((width + 3) / 4) * Bo1Fmt_BlockBytes(f);
    return width * Bo1Fmt_Bpp(f);
}

// rows the pitch counts (block formats: rows of blocks)
inline uint32_t Bo1Fmt_Rows(uint32_t f, uint32_t height) { return Bo1Fmt_IsBlock(f) ? (height + 3) / 4 : height; }

inline uint32_t Bo1Fmt_LevelBytes(uint32_t f, uint32_t width, uint32_t height) { return Bo1Fmt_Pitch(f, width) * Bo1Fmt_Rows(f, height); }

inline uint32_t Bo1_MipSize(uint32_t size, uint32_t level)
{
    uint32_t s = size >> level;
    return s ? s : 1;
}

inline uint32_t Bo1_FullMipCount(uint32_t w, uint32_t h, uint32_t d = 1)
{
    uint32_t m = w > h ? w : h;
    m = m > d ? m : d;
    uint32_t levels = 1;
    while (m > 1)
    {
        m >>= 1;
        ++levels;
    }
    return levels;
}
