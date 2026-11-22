// Direct3D 9 texture formats in WebGPU's (d3d9_webgpu_formats.h).
#include <windows.h>
#include <d3d9.h>

#include <math.h>
#include <string.h>

#include "d3d9_formats.h"
#include "d3d9_webgpu_formats.h"

namespace bo1gpu
{
TexFormat Format(uint32_t f)
{
    TexFormat t;
    auto set = [&](GpuFormat gpu, uint8_t bpp, bool renderable, Conv conv = CONV_NONE) {
        t.gpu = gpu;
        t.outBpp = bpp;
        t.renderable = renderable;
        t.conv = conv;
        if (gpu == GF_BGRA8)
            t.srgb = GF_BGRA8_SRGB;
        else if (gpu == GF_RGBA8)
            t.srgb = GF_RGBA8_SRGB;
    };
    switch (f)
    {
    case D3DFMT_A8R8G8B8:
        set(GF_BGRA8, 4, true);
        break;
    case D3DFMT_X8R8G8B8:
        set(GF_BGRA8, 4, true, CONV_ALPHA1);
        break;
    case D3DFMT_A8B8G8R8:
        set(GF_RGBA8, 4, true);
        break;
    case D3DFMT_X8B8G8R8:
        set(GF_RGBA8, 4, true, CONV_ALPHA1);
        break;
    case D3DFMT_R8G8B8:
        set(GF_BGRA8, 4, true, CONV_RGB8);
        break;
    case D3DFMT_R5G6B5:
        set(GF_BGRA8, 4, true, CONV_565);
        break;
    case D3DFMT_A1R5G5B5:
        set(GF_BGRA8, 4, true, CONV_1555);
        break;
    case D3DFMT_X1R5G5B5:
        set(GF_BGRA8, 4, true, CONV_X555);
        break;
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
        set(GF_BGRA8, 4, true, CONV_4444);
        break;
    case D3DFMT_A8:
        set(GF_BGRA8, 4, true, CONV_A8);
        break;
    case D3DFMT_L8:
        set(GF_BGRA8, 4, true, CONV_L8);
        break;
    case D3DFMT_A8L8:
        set(GF_BGRA8, 4, true, CONV_A8L8);
        break;
    case D3DFMT_L16:
        set(GF_RGBA16F, 8, true, CONV_L16);
        t.clamp = true;
        break;
    case D3DFMT_G16R16:
        set(GF_RG16F, 4, true, CONV_UNORM16);
        t.clamp = true;
        break;
    case D3DFMT_A16B16G16R16:
        set(GF_RGBA16F, 8, true, CONV_UNORM16);
        t.clamp = true;
        break;
    case D3DFMT_R16F:
        set(GF_R16F, 2, true);
        break;
    case D3DFMT_G16R16F:
        set(GF_RG16F, 4, true);
        break;
    case D3DFMT_A16B16G16R16F:
        set(GF_RGBA16F, 8, true);
        break;
    case D3DFMT_R32F:
        set(GF_R32F, 4, true);
        t.float32 = true;
        break;
    case D3DFMT_G32R32F:
        set(GF_RG32F, 8, true);
        t.float32 = true;
        break;
    case D3DFMT_A32B32G32R32F:
        set(GF_RGBA32F, 16, true);
        t.float32 = true;
        break;
    case D3DFMT_A2B10G10R10:
        set(GF_RGB10A2, 4, true);
        break;
    case D3DFMT_A2R10G10B10:
        set(GF_RGB10A2, 4, true, CONV_A2R10);
        break;
    case D3DFMT_V8U8:
        set(GF_RG8_SNORM, 2, false);
        break;
    case D3DFMT_Q8W8V8U8:
        set(GF_RGBA8_SNORM, 4, false);
        break;
    case D3DFMT_R3G3B2:
        set(GF_BGRA8, 4, false, CONV_332);
        break;
    case D3DFMT_A8R3G3B2:
        set(GF_BGRA8, 4, false, CONV_8332);
        break;
    case D3DFMT_A4L4:
        set(GF_BGRA8, 4, false, CONV_A4L4);
        break;
    case D3DFMT_V16U16:
        set(GF_RG16F, 4, false, CONV_SNORM16);
        break;
    case D3DFMT_Q16W16V16U16:
        set(GF_RGBA16F, 8, false, CONV_SNORM16);
        break;
    case D3DFMT_L6V5U5:
        set(GF_RGBA16F, 8, false, CONV_L6V5U5);
        break;
    case D3DFMT_X8L8V8U8:
        set(GF_RGBA16F, 8, false, CONV_X8L8V8U8);
        break;
    case D3DFMT_A2W10V10U10:
        set(GF_RGBA16F, 8, false, CONV_A2W10V10U10);
        break;
    case D3DFMT_CxV8U8:
        set(GF_RGBA16F, 8, false, CONV_CXV8U8);
        break;
    case D3DFMT_YUY2:
        set(GF_BGRA8, 4, false, CONV_YUY2);
        break;
    case D3DFMT_UYVY:
        set(GF_BGRA8, 4, false, CONV_UYVY);
        break;
    case D3DFMT_R8G8_B8G8:
        set(GF_BGRA8, 4, false, CONV_RGBG);
        break;
    case D3DFMT_G8R8_G8B8:
        set(GF_BGRA8, 4, false, CONV_GRGB);
        break;
    case D3DFMT_DXT1:
        set(GF_BC1, 8, false);
        t.srgb = GF_BC1_SRGB;
        t.block = true;
        break;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
        set(GF_BC2, 16, false);
        t.srgb = GF_BC2_SRGB;
        t.block = true;
        break;
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        set(GF_BC3, 16, false);
        t.srgb = GF_BC3_SRGB;
        t.block = true;
        break;
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
        set(GF_D16, 2, true);
        t.depth = true;
        break;
    case D3DFMT_D24X8:
        set(GF_D24, 4, true);
        t.depth = true;
        break;
    case D3DFMT_D24S8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D24FS8:
    case D3DFMT_D15S1:
        set(GF_D24S8, 4, true);
        t.depth = t.stencil = true;
        break;
    case D3DFMT_D32:
    case D3DFMT_D32_LOCKABLE:
    case D3DFMT_D32F_LOCKABLE:
        set(GF_D32F, 4, true);
        t.depth = true;
        break;
    default:
        if (f == BO1FMT_ATI1 || f == BO1FMT_ATI2)
        {
            set(f == BO1FMT_ATI1 ? GF_BC4 : GF_BC5, f == BO1FMT_ATI1 ? 8 : 16, false);
            t.block = true;
        }
        else if (f == BO1FMT_INTZ || f == BO1FMT_RAWZ)
        {
            set(GF_D24S8, 4, true);
            t.depth = t.stencil = true;
        }
        else if (f == BO1FMT_DF24)
        {
            set(GF_D24, 4, true);
            t.depth = true;
        }
        else if (f == BO1FMT_DF16)
        {
            set(GF_D16, 2, true);
            t.depth = true;
        }
        else
            t.valid = false;
        break;
    }
    return t;
}

void Decompressed(TexFormat &t)
{
    t.gpu = GF_RGBA8;
    t.srgb = GF_RGBA8_SRGB;
    t.conv = CONV_DECODE;
    t.outBpp = 4;
    t.block = false;
}

// round to nearest even; below 2^-14, subnormal halves
uint16_t FloatToHalf(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    uint32_t abs = x & 0x7FFFFFFF;
    if (abs >= 0x7F800000)
        return (uint16_t)(sign | 0x7C00 | (abs > 0x7F800000 ? 0x200 : 0));
    if (abs >= 0x477FF000)
        return (uint16_t)(sign | 0x7C00);
    if (abs < 0x38800000)
    {
        float v;
        memcpy(&v, &abs, 4);
        return (uint16_t)(sign | (uint32_t)lrintf(v * 16777216.0f));
    }
    uint32_t h = (abs >> 13) - ((127 - 15) << 10);
    uint32_t rest = abs & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (h & 1)))
        ++h;
    return (uint16_t)(sign | h);
}

float HalfToFloat(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float f;
    if (e == 0)
        f = m / 16777216.0f;
    else if (e == 31)
        f = m ? NAN : INFINITY;
    else
    {
        uint32_t x = ((e + 112) << 23) | (m << 13);
        memcpy(&f, &x, 4);
    }
    return sign ? -f : f;
}

// ----- conversions -----

// a signed value of `bits` bits
static int Signed(uint32_t v, int bits) { return (int)(v << (32 - bits)) >> (32 - bits); }

// a signed normalized value: the most negative is -1, as is the next
static float Snorm(int v, int max) { return fmaxf((float)v / max, -1.0f); }

static uint8_t Byte(float f) { return (uint8_t)(f <= 0.0f ? 0 : f >= 255.0f ? 255 : f + 0.5f); }

static void Half4(uint8_t *d, float a, float b, float c, float e)
{
    uint16_t h[4] = {FloatToHalf(a), FloatToHalf(b), FloatToHalf(c), FloatToHalf(e)};
    memcpy(d, h, 8);
}

// ----- block decompression (DXT1..5, ATI1, ATI2) -----

static void DecodeColor(const uint8_t *b, uint8_t out[16][4], bool dxt1)
{
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    auto expand = [](uint16_t c, uint8_t *p) {
        p[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
        p[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
        p[2] = (uint8_t)((c & 31) * 255 / 31);
        p[3] = 255;
    };
    expand(c0, pal[0]);
    expand(c1, pal[1]);
    for (int i = 0; i < 3; ++i)
    {
        if (c0 > c1 || !dxt1)
        {
            pal[2][i] = (uint8_t)((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = (uint8_t)((pal[0][i] + 2 * pal[1][i]) / 3);
        }
        else
        {
            pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2);
            pal[3][i] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = (c0 > c1 || !dxt1) ? 255 : 0;
    uint32_t bits = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; ++i)
        memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}

static void DecodeAlpha(const uint8_t *b, uint8_t out[16])
{
    uint8_t a[8];
    a[0] = b[0];
    a[1] = b[1];
    if (a[0] > a[1])
        for (int i = 1; i < 7; ++i)
            a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
    else
    {
        for (int i = 1; i < 5; ++i)
            a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5);
        a[6] = 0;
        a[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i)
        bits |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; ++i)
        out[i] = a[(bits >> (3 * i)) & 7];
}

// a 4x4 block of `format` to RGBA texels
static void DecodeBlock(uint32_t format, const uint8_t *b, uint8_t out[16][4])
{
    uint8_t alpha[16];
    if (format == D3DFMT_DXT1)
        DecodeColor(b, out, true);
    else if (format == D3DFMT_DXT2 || format == D3DFMT_DXT3)
    {
        DecodeColor(b + 8, out, false);
        for (int i = 0; i < 16; ++i)
            out[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
    }
    else if (format == D3DFMT_DXT4 || format == D3DFMT_DXT5)
    {
        DecodeColor(b + 8, out, false);
        DecodeAlpha(b, alpha);
        for (int i = 0; i < 16; ++i)
            out[i][3] = alpha[i];
    }
    else if (format == BO1FMT_ATI1)
    {
        DecodeAlpha(b, alpha);
        for (int i = 0; i < 16; ++i)
        {
            out[i][0] = alpha[i];
            out[i][1] = out[i][2] = 0;
            out[i][3] = 255;
        }
    }
    else // ATI2
    {
        uint8_t green[16];
        DecodeAlpha(b, alpha);
        DecodeAlpha(b + 8, green);
        for (int i = 0; i < 16; ++i)
        {
            out[i][0] = alpha[i];
            out[i][1] = green[i];
            out[i][2] = 0;
            out[i][3] = 255;
        }
    }
}

const uint8_t *Convert(const TexFormat &tf, uint32_t d3dFormat, const uint8_t *src, uint32_t width, uint32_t height, uint32_t pitch,
    std::vector<uint8_t> &tmp)
{
    tmp.resize((size_t)width * height * tf.outBpp + 16);
    uint8_t *dst = tmp.data();
    if (tf.conv == CONV_DECODE)
    {
        uint32_t blockBytes = Bo1Fmt_BlockBytes(d3dFormat);
        uint8_t texels[16][4];
        for (uint32_t by = 0; by < (height + 3) / 4; ++by)
            for (uint32_t bx = 0; bx < (width + 3) / 4; ++bx)
            {
                DecodeBlock(d3dFormat, src + (size_t)by * pitch + (size_t)bx * blockBytes, texels);
                for (uint32_t i = 0; i < 16; ++i)
                {
                    uint32_t x = bx * 4 + (i & 3), y = by * 4 + i / 4;
                    if (x < width && y < height)
                        memcpy(dst + ((size_t)y * width + x) * 4, texels[i], 4);
                }
            }
        return dst;
    }
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t *s = src + (size_t)y * pitch;
        uint8_t *d = dst + (size_t)y * width * tf.outBpp;
        switch (tf.conv)
        {
        case CONV_NONE:
            memcpy(d, s, (size_t)width * tf.outBpp);
            break;
        case CONV_ALPHA1:
            for (uint32_t i = 0; i < width; ++i, s += 4, d += 4)
            {
                memcpy(d, s, 3);
                d[3] = 255;
            }
            break;
        case CONV_RGB8:
            for (uint32_t i = 0; i < width; ++i, s += 3, d += 4)
            {
                memcpy(d, s, 3);
                d[3] = 255;
            }
            break;
        case CONV_565:
            for (uint32_t i = 0; i < width; ++i, s += 2, d += 4)
            {
                uint16_t v = (uint16_t)(s[0] | s[1] << 8);
                d[0] = (uint8_t)((v & 31) * 255 / 31);
                d[1] = (uint8_t)(((v >> 5) & 63) * 255 / 63);
                d[2] = (uint8_t)(((v >> 11) & 31) * 255 / 31);
                d[3] = 255;
            }
            break;
        case CONV_1555:
        case CONV_X555:
            for (uint32_t i = 0; i < width; ++i, s += 2, d += 4)
            {
                uint16_t v = (uint16_t)(s[0] | s[1] << 8);
                d[0] = (uint8_t)((v & 31) * 255 / 31);
                d[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31);
                d[2] = (uint8_t)(((v >> 10) & 31) * 255 / 31);
                d[3] = (tf.conv == CONV_X555 || (v >> 15)) ? 255 : 0;
            }
            break;
        case CONV_4444:
            for (uint32_t i = 0; i < width; ++i, s += 2, d += 4)
            {
                uint16_t v = (uint16_t)(s[0] | s[1] << 8);
                d[0] = (uint8_t)((v & 15) * 17);
                d[1] = (uint8_t)(((v >> 4) & 15) * 17);
                d[2] = (uint8_t)(((v >> 8) & 15) * 17);
                d[3] = d3dFormat == D3DFMT_X4R4G4B4 ? 255 : (uint8_t)((v >> 12) * 17);
            }
            break;
        case CONV_A8:
            for (uint32_t i = 0; i < width; ++i, s += 1, d += 4)
            {
                d[0] = d[1] = d[2] = 0;
                d[3] = s[0];
            }
            break;
        case CONV_L8:
            for (uint32_t i = 0; i < width; ++i, s += 1, d += 4)
            {
                d[0] = d[1] = d[2] = s[0];
                d[3] = 255;
            }
            break;
        case CONV_A8L8:
            for (uint32_t i = 0; i < width; ++i, s += 2, d += 4)
            {
                d[0] = d[1] = d[2] = s[0];
                d[3] = s[1];
            }
            break;
        case CONV_A2R10:
            for (uint32_t i = 0; i < width; ++i, s += 4, d += 4)
            {
                uint32_t v;
                memcpy(&v, s, 4);
                uint32_t r = (v >> 20) & 0x3FF, b = v & 0x3FF;
                v = (v & 0xC00FFC00u) | r | (b << 20);
                memcpy(d, &v, 4);
            }
            break;
        case CONV_UNORM16:
            for (uint32_t i = 0; i < width * tf.outBpp / 2; ++i, s += 2, d += 2)
            {
                uint16_t h = FloatToHalf((uint16_t)(s[0] | s[1] << 8) / 65535.0f);
                memcpy(d, &h, 2);
            }
            break;
        case CONV_L16:
            for (uint32_t i = 0; i < width; ++i, s += 2, d += 8)
            {
                uint16_t h[4] = {0, 0, 0, 0x3C00};
                h[0] = h[1] = h[2] = FloatToHalf((uint16_t)(s[0] | s[1] << 8) / 65535.0f);
                memcpy(d, h, 8);
            }
            break;
        case CONV_332:
            for (uint32_t i = 0; i < width; ++i, s += 1, d += 4)
            {
                d[0] = (uint8_t)((s[0] & 3) * 255 / 3);
                d[1] = (uint8_t)(((s[0] >> 2) & 7) * 255 / 7);
                d[2] = (uint8_t)((s[0] >> 5) * 255 / 7);
                d[3] = 255;
            }
            break;
        case CONV_8332:
            for (uint32_t i = 0; i < width; ++i, s += 2, d += 4)
            {
                d[0] = (uint8_t)((s[0] & 3) * 255 / 3);
                d[1] = (uint8_t)(((s[0] >> 2) & 7) * 255 / 7);
                d[2] = (uint8_t)((s[0] >> 5) * 255 / 7);
                d[3] = s[1];
            }
            break;
        case CONV_A4L4:
            for (uint32_t i = 0; i < width; ++i, s += 1, d += 4)
            {
                d[0] = d[1] = d[2] = (uint8_t)((s[0] & 15) * 17);
                d[3] = (uint8_t)((s[0] >> 4) * 17);
            }
            break;
        case CONV_SNORM16:
            for (uint32_t i = 0; i < width * tf.outBpp / 2; ++i, s += 2, d += 2)
            {
                uint16_t h = FloatToHalf(Snorm((int16_t)(s[0] | s[1] << 8), 32767));
                memcpy(d, &h, 2);
            }
            break;
        case CONV_L6V5U5:
            for (uint32_t i = 0; i < width; ++i, s += 2, d += 8)
            {
                uint16_t v = (uint16_t)(s[0] | s[1] << 8);
                Half4(d, Snorm(Signed(v, 5), 15), Snorm(Signed(v >> 5, 5), 15), (v >> 10) / 63.0f, 1.0f);
            }
            break;
        case CONV_X8L8V8U8:
            for (uint32_t i = 0; i < width; ++i, s += 4, d += 8)
                Half4(d, Snorm((int8_t)s[0], 127), Snorm((int8_t)s[1], 127), s[2] / 255.0f, 1.0f);
            break;
        case CONV_A2W10V10U10:
            for (uint32_t i = 0; i < width; ++i, s += 4, d += 8)
            {
                uint32_t v;
                memcpy(&v, s, 4);
                Half4(d, Snorm(Signed(v, 10), 511), Snorm(Signed(v >> 10, 10), 511), Snorm(Signed(v >> 20, 10), 511), (v >> 30) / 3.0f);
            }
            break;
        case CONV_CXV8U8:
            for (uint32_t i = 0; i < width; ++i, s += 2, d += 8)
            {
                float u = Snorm((int8_t)s[0], 127), v = Snorm((int8_t)s[1], 127);
                Half4(d, u, v, sqrtf(fmaxf(0.0f, 1.0f - u * u - v * v)), 1.0f);
            }
            break;
        case CONV_YUY2:
        case CONV_UYVY:
        case CONV_RGBG:
        case CONV_GRGB:
            for (uint32_t i = 0; i < width; i += 2, s += 4, d += 8)
            {
                uint8_t px[2][4];
                if (tf.conv == CONV_RGBG || tf.conv == CONV_GRGB)
                {
                    bool g = tf.conv == CONV_GRGB;
                    for (int k = 0; k < 2; ++k)
                    {
                        px[k][0] = s[g ? 3 : 2];
                        px[k][1] = s[g ? 2 * k : 1 + 2 * k];
                        px[k][2] = s[g ? 1 : 0];
                        px[k][3] = 255;
                    }
                }
                else
                {
                    bool y = tf.conv == CONV_YUY2;
                    float u = s[y ? 1 : 0] - 128.0f, v = s[y ? 3 : 2] - 128.0f;
                    for (int k = 0; k < 2; ++k)
                    {
                        float l = s[y ? 2 * k : 1 + 2 * k];
                        px[k][0] = Byte(l + 1.772f * u);
                        px[k][1] = Byte(l - 0.344136f * u - 0.714136f * v);
                        px[k][2] = Byte(l + 1.402f * v);
                        px[k][3] = 255;
                    }
                }
                memcpy(d, px[0], 4);
                if (i + 1 < width)
                    memcpy(d + 4, px[1], 4);
            }
            break;
        default:
            break;
        }
    }
    return dst;
}

uint32_t GpuBpp(GpuFormat f)
{
    switch (f)
    {
    case GF_BGRA8:
    case GF_BGRA8_SRGB:
    case GF_RGBA8:
    case GF_RGBA8_SRGB:
    case GF_RGBA8_SNORM:
    case GF_RGB10A2:
    case GF_RG16F:
    case GF_R32F:
        return 4;
    case GF_RG8_SNORM:
    case GF_R16F:
        return 2;
    case GF_RGBA16F:
    case GF_RG32F:
        return 8;
    case GF_RGBA32F:
        return 16;
    default:
        return 0;
    }
}
} // namespace bo1gpu
