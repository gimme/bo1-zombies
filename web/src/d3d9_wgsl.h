// Direct3D 9 shaders (vs_2_0 .. vs_3_0, ps_2_0 .. ps_3_0 bytecode) in WGSL, for the WebGPU executor (d3d9_webgpu.cpp).
//
// A shader is parsed once, when the engine creates it; its WGSL is written per variant, when a draw first needs it.
// What Direct3D 9 does outside the shader is in the WGSL, set per pipeline with override constants: the alpha test, sRGB
// writes, clamping what is written to the 16-bit normalized render targets (half floats here), and reading D3DCOLOR
// (BGRA bytes) and UBYTE4 (whole numbers) vertex data. The variants are what changes declarations: pixel samplers
// reading depth textures (hardware shadow maps: depth comparisons), pixel shader outputs without a render target (left
// out), and vertex shader outputs a pixel shader reads that the vertex shader does not write (zeros).
//
// Bindings: group 0 holds the constants, a uniform block per stage (binding 0 vertex, 1 pixel, dynamic offsets):
//   fix  vertex: half a pixel in clip units (D3D9's pixel centers); pixel: z the alpha test's reference
//   bm   x: the bool constants b0..b15, a bit each
//   c    the float constants the shader reads, packed in runs (ShaderInfo::floatRuns)
//   i    the int constants i0..
// Group 1 holds the textures: pixel sampler s at binding s (texture) and 16 + s (sampler), vertex sampler s at 32 + s
// and 36 + s.
#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace bo1gpu
{
enum SamplerDim : uint8_t
{
    DIM_2D,
    DIM_CUBE,
    DIM_3D,
};

// vertex outputs and pixel inputs: TEXCOORD0..9 at locations 0..9, COLOR0..1 at 10..11
static const int VARYING_COUNT = 12;
int VaryingLocation(int usage, int index); // -1: none

struct ConstRun
{
    uint16_t reg, count, slot;
};

struct DefSlot
{
    uint16_t slot;
    float value[4];
};

struct ShaderInstr;

struct ShaderInfo
{
    bool pixel = false;
    uint32_t major = 0, minor = 0;
    std::string error; // empty: it translates
    // vertex shaders: the attributes, at @location(i)
    struct Input
    {
        uint8_t usage, index;
    };
    std::vector<Input> inputs;
    uint32_t varyings = 0; // vertex: the locations written; pixel: read
    struct Sampler
    {
        uint8_t stage; // pixel 0..15, vertex 0..3
        uint8_t dim;
    };
    std::vector<Sampler> samplers;
    std::vector<ConstRun> floatRuns; // the float constants read, in the order of the uniform block's c
    uint32_t floatSlots = 0;
    std::vector<DefSlot> defSlots; // constants the shader defines inside a run (read relative): their values
    uint32_t intCount = 0;         // i0..intCount-1 are in the block
    uint32_t colorOutputs = 0;     // pixel: oC0..3 written, a bit each
    bool writesDepth = false;
    uint32_t UniformBytes() const { return 32 + 16 * (floatSlots + intCount); }

    // the parsed program (ShaderEmit)
    std::vector<ShaderInstr> *instrs = nullptr;
    ShaderInfo() = default;
    ShaderInfo(const ShaderInfo &) = delete;
    ShaderInfo &operator=(const ShaderInfo &) = delete;
    ~ShaderInfo();
};

// the override constants a module has (ShaderEmit), which a pipeline sets
enum
{
    OVERRIDE_INPUTS = 1, // bo1_inColor, bo1_inUbyte: vertex attributes read as D3DCOLOR (.zyxw) and UBYTE4 (x 255), a bit each
    OVERRIDE_CLAMP = 2,  // bo1_clamp: color outputs clamped to 0..1, a bit each
    OVERRIDE_ALPHA = 4,  // bo1_alphaFunc: the alpha test's D3DCMP (8: none); bo1_srgbWrite: oC0 to sRGB
};

// parse a shader's bytecode; out.error says why it can't be translated
void ShaderParse(const uint32_t *code, size_t words, ShaderInfo &out);

// For stand-in pixel shaders (d3d9_webgpu.cpp: stand-ins). Where a pixel shader's samplers get their texture coordinates:
// coords[s], for sampler stage s, is the varying location (VARYING_COUNT: not found) and, in bits 0..3, the swizzle of
// the coordinates' x and y (2 bits each: the varying's component). A coordinate computed from one varying (scrolled,
// scaled) counts as that varying's.
struct SamplerCoord
{
    uint8_t loc, swz;
};
void PixelSamplerCoords(const ShaderInfo &info, SamplerCoord coords[16]);
// The varying locations (a bit each) whose values a vertex shader derives from constant register constReg (-1: none)
// or from its input of declared usage inputUsage (-1: none): which of its outputs is the light vector (from the light's
// position) or the normal.
uint32_t VertexVaryingsFrom(const ShaderInfo &info, int constReg, int inputUsage);
// The WGSL of a variant, and the override constants it has. shadowMask: pixel samplers that read a depth texture with
// comparisons, a bit each; extraVaryings: locations a vertex shader outputs as zeros, besides what it writes;
// targetMask: the render targets a pixel shader's color outputs go to (the others are left out). A shader reads its
// constants from a uniform block (group 0; a vertex shader's at binding 0, a pixel shader's at 1): the fix, the bools,
// the float constants' slots and the int constants, 16 bytes each (ShaderInfo::UniformBytes). byInstance: a vertex
// shader reads its block from a storage buffer instead (binding 0, an array of vec4u), the block the instance counts
// to, in blocks of its size from the buffer's start: a draw's first instance says where its block is, with no call to
// bind it, and a draw of several instances reads a block for each, one after the other.
std::string ShaderEmit(const ShaderInfo &s, uint32_t shadowMask, uint32_t extraVaryings, uint32_t targetMask, bool byInstance,
    uint32_t *overrides);
} // namespace bo1gpu
