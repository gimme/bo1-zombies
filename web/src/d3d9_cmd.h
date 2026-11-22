// The command stream between the Direct3D 9 device (d3d9_device.cpp, on the engine's threads) and its executor on the
// page's thread, where the canvas' WebGPU context lives (d3d9_webgpu.cpp).
//
// The device writes each call as a command into a ring in shared memory and returns; the page replays a frame's
// commands on each animation frame. Large data (texture and buffer contents, shader code) travels as a
// malloc'd block the executor frees once used. The few calls that need an answer from the GPU (render target reads)
// wait on a futex the executor releases.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace bo1cmd
{
enum Op : uint16_t
{
    OP_NOP = 0,
    OP_WRAP,            // the rest of the ring is unused; continue at offset 0
    OP_CREATE_TEXTURE,  // CmdCreateTexture
    OP_CREATE_SURFACE,  // CmdCreateSurface (render target / depth stencil not in a texture)
    OP_CREATE_BUFFER,   // CmdCreateBuffer
    OP_CREATE_VDECL,    // CmdCreateVDecl
    OP_CREATE_SHADER,   // CmdCreateShader
    OP_DESTROY,         // CmdDestroy
    OP_UPLOAD_TEXTURE,  // CmdUploadTexture
    OP_UPLOAD_BUFFER,   // CmdUploadBuffer
    OP_GENERATE_MIPS,   // CmdId
    OP_RENDER_STATE,    // CmdState (state, value)
    OP_SAMPLER_STATE,   // CmdSamplerState
    OP_TEXTURE,         // CmdSetTexture
    OP_STREAM,          // CmdStream
    OP_STREAM_FREQ,     // CmdState (stream, setting)
    OP_INDICES,         // CmdId
    OP_VDECL,           // CmdId
    OP_VS,              // CmdId
    OP_PS,              // CmdId
    OP_VS_CONST_F,      // CmdConst + floats
    OP_PS_CONST_F,      // CmdConst + floats
    OP_VS_CONST_I,      // CmdConst + ints
    OP_PS_CONST_I,      // CmdConst + ints
    OP_VS_CONST_B,      // CmdConst + ints
    OP_PS_CONST_B,      // CmdConst + ints
    OP_VIEWPORT,        // CmdViewport
    OP_SCISSOR,         // CmdRect
    OP_RENDER_TARGET,   // CmdRenderTarget
    OP_DEPTH_STENCIL,   // CmdRenderTarget (index unused)
    OP_CLEAR,           // CmdClear
    OP_DRAW,            // CmdDraw
    OP_DRAW_INDEXED,    // CmdDrawIndexed
    OP_DRAW_UP,         // CmdDrawUp (+ data block)
    OP_DRAW_INDEXED_UP, // CmdDrawIndexedUp (+ data blocks)
    OP_STRETCH_RECT,    // CmdStretchRect
    OP_READBACK,        // CmdReadback
    OP_QUERY_END,       // CmdQuery
    OP_QUERY_BEGIN,     // CmdQuery (occlusion)
    OP_PRESENT,         // CmdPresent
    OP_COLOR_FILL,      // CmdColorFill
    OP_RESET,           // CmdReset: the back buffer changed size
    OP_GAMMA,           // CmdGamma: the game's brightness
    OP_PASS_HINTS,      // CmdPassHints: what a material pass's samplers and constants are, for stand-in pipelines
    OP_ENUM_PASS,       // CmdEnumPass: a pass of the zone's materials set up as a draw would, for its stand-in (or its end)
    OP_COUNT
};

// a surface as a render target / copy endpoint
enum SurfKind : uint32_t
{
    SURF_NONE = 0,
    SURF_STANDALONE, // CreateRenderTarget / CreateDepthStencilSurface / CreateOffscreenPlainSurface: id is the surface
    SURF_TEXTURE,    // a level (and cube face) of a texture: id is the texture
    SURF_BACKBUFFER,
};

struct SurfRef
{
    uint32_t kind;
    uint32_t id;
    uint32_t level;
    uint32_t face;
};

enum TexType : uint32_t
{
    TEX_2D = 0,
    TEX_CUBE,
    TEX_3D,
};

struct CmdHeader
{
    uint16_t op;
    uint16_t pad;
    uint32_t size; // bytes, header included, multiple of 8
};

struct CmdId
{
    uint32_t id;
};

struct CmdState
{
    uint32_t state;
    uint32_t value;
};

struct CmdSamplerState
{
    uint32_t sampler;
    uint32_t type;
    uint32_t value;
};

struct CmdCreateTexture
{
    uint32_t id;
    uint32_t type;
    uint32_t width, height, depth;
    uint32_t levels;
    uint32_t format; // D3DFORMAT
    uint32_t usage;
    uint32_t pool;
};

struct CmdCreateSurface
{
    uint32_t id;
    uint32_t width, height;
    uint32_t format;
    uint32_t multisample;
    uint32_t depth; // depth / stencil surface
};

struct CmdCreateBuffer
{
    uint32_t id;
    uint32_t index; // index buffer
    uint32_t size;
    uint32_t usage;
    uint32_t format; // index buffers: D3DFMT_INDEX16 / INDEX32
};

struct CmdCreateVDecl
{
    uint32_t id;
    uint32_t count;     // elements before D3DDECL_END
    void *elements;     // D3DVERTEXELEMENT9[count], malloc'd, the executor keeps it
};

struct CmdCreateShader
{
    uint32_t id;
    uint32_t pixel;
    void *code; // malloc'd bytecode, the executor keeps it
    uint32_t size;
};

enum ObjKind : uint32_t
{
    OBJ_TEXTURE = 1,
    OBJ_SURFACE,
    OBJ_BUFFER,
    OBJ_VDECL,
    OBJ_SHADER,
};

struct CmdDestroy
{
    uint32_t kind;
    uint32_t id;
};

struct CmdUploadTexture
{
    uint32_t id;
    uint32_t face;
    uint32_t level;
    uint32_t x, y, z;
    uint32_t width, height, depth; // texels (DXT: multiples of 4 except at small mips)
    uint32_t pitch;                // bytes per row (DXT: per row of blocks)
    uint32_t slicePitch;
    void *data; // malloc'd, freed by the executor
};

struct CmdUploadBuffer
{
    uint32_t id;
    uint32_t offset;
    uint32_t size;
    uint32_t discard; // D3DLOCK_DISCARD: orphan the old contents
    void *data;       // malloc'd, freed by the executor
};

struct CmdSetTexture
{
    uint32_t stage; // 0..15 pixel, 257..260 vertex (D3DVERTEXTEXTURESAMPLER0..3)
    uint32_t id;    // 0: none
    uint32_t type;
};

struct CmdStream
{
    uint32_t stream;
    uint32_t id;
    uint32_t offset;
    uint32_t stride;
};

struct CmdConst
{
    uint32_t start;
    uint32_t count; // vec4 registers (bool: single values)
    // followed by the values
};

struct CmdViewport
{
    uint32_t x, y, width, height;
    float minZ, maxZ;
};

struct CmdRect
{
    int32_t left, top, right, bottom;
};

struct CmdRenderTarget
{
    uint32_t index;
    SurfRef surf;
};

struct CmdClear
{
    uint32_t flags;
    uint32_t color; // D3DCOLOR
    float z;
    uint32_t stencil;
    uint32_t rectCount;
    CmdRect rect; // the first rect (the engine clears at most one)
};

struct CmdDraw
{
    uint32_t primType;
    uint32_t startVertex;
    uint32_t primCount;
};

struct CmdDrawIndexed
{
    uint32_t primType;
    int32_t baseVertex;
    uint32_t minIndex;
    uint32_t numVertices;
    uint32_t startIndex;
    uint32_t primCount;
};

struct CmdDrawUp
{
    uint32_t primType;
    uint32_t primCount;
    uint32_t stride;
    uint32_t vertexBytes;
    void *vertices; // malloc'd
};

struct CmdDrawIndexedUp
{
    uint32_t primType;
    uint32_t minIndex;
    uint32_t numVertices;
    uint32_t primCount;
    uint32_t index32;
    uint32_t stride;
    uint32_t vertexBytes;
    uint32_t indexBytes;
    void *vertices; // malloc'd
    void *indices;  // malloc'd
};

struct CmdStretchRect
{
    SurfRef src;
    SurfRef dst;
    CmdRect srcRect;
    CmdRect dstRect;
    uint32_t hasSrcRect, hasDstRect;
    uint32_t filter;
};

struct CmdReadback
{
    SurfRef src;
    uint32_t width, height;
    uint32_t format;      // D3DFORMAT the engine wants (A8R8G8B8 / X8R8G8B8 / R32F / A16B16G16R16F ...)
    uint32_t pitch;
    void *dst;
    volatile int32_t *done; // set to 1 and woken when the pixels are in dst
};

// An occlusion query's answer, from the executor (d3d9_webgpu.cpp: occlusion queries): BO1_OCCLUSION_VISIBLE samples
// if any of its draws' passed the depth test, else 0
struct OcclusionAnswer
{
    volatile uint32_t samples;
    volatile uint32_t answers; // counts up with each
};
#define BO1_OCCLUSION_VISIBLE 100000u

struct CmdQuery
{
    uint32_t id;
    volatile int32_t *done;  // an event: set to 1 when the executor passes this point
    OcclusionAnswer *answer; // an occlusion query: answered frames later
};

struct CmdPresent
{
    uint32_t frame;
    float renderMs;   // the render thread drawing the frame, from BeginScene to Present
    double inputTime; // when the mouse had last moved as the game thread read it for the frame, 0 if it had not
                      // (Bo1_FrameTakeInput)
};

struct CmdColorFill
{
    SurfRef surf;
    CmdRect rect;
    uint32_t hasRect;
    uint32_t color;
};

struct CmdReset
{
    uint32_t width, height;
    uint32_t multisample; // D3DMULTISAMPLE_TYPE: the present parameters'
};

// the gamma ramp (SetGammaRamp), as a display applies it to the frame: for each 8-bit value of a channel, the 8-bit
// value it shows (the ramp's 16-bit entry, rounded), red, green and blue, then 255
struct CmdGamma
{
    uint8_t rgba[256 * 4];
};

// What the engine knows of the material pass it set up (src/gfx_d3d/r_shade.cpp: R_SetupPass), which the shaders alone
// don't say, for the executor's stand-in pipelines (d3d9_webgpu.cpp: stand-ins), which draw a colour map lit by the
// primary light where a draw's own pipeline is still compiling. Sent as it changes; the draws after are of that pass.
enum PassHint
{
    HINT_COLOR_MAP = 0,          // pixel sampler stage of the material's colorMap, 255 none
    HINT_LIGHTMAP_PRIMARY = 1,   // pixel sampler stage of the primary lightmap (world surfaces), 255 none
    HINT_LIGHTMAP_SECONDARY = 2, // of the secondary lightmap
    HINT_PS_LIGHT_POSITION = 3,  // pixel constant register of the primary light's position (CONST_SRC_CODE_LIGHT_POSITION), 255 none
    HINT_PS_LIGHT_DIFFUSE = 4,   // of its diffuse colour
    HINT_VS_LIGHT_POSITION = 5,  // the vertex constant registers of the same
    HINT_VS_LIGHT_DIFFUSE = 6,
    HINT_FLAGS = 7,              // HINT_READS_SCENE, HINT_READS_DEPTH
    HINT_MODEL_LIGHTING = 8,     // pixel sampler stage of a model's light from the light grid (TEXTURE_SRC_CODE_MODEL_LIGHTING, a
                                 // 3D texture: a cube of 4x4x4 texels per model, one for each direction of the normal), 255 none
    HINT_PS_LOOKUP_SCALE = 9,    // pixel constant register of the cube's scale (CONST_SRC_CODE_LIGHTING_LOOKUP_SCALE)
    HINT_PS_HDR = 10,            // pixel constant register whose x scales the scene's light before the shaders' final square root
                                 // (CONST_SRC_CODE_HDRCONTROL_0)
    HINT_VS_BASE_LIGHTING = 11,  // vertex constant register of the cube's coordinates (CONST_SRC_CODE_BASE_LIGHTING_COORDS)
    HINT_PS_HERO_SCALE = 12,     // pixel constant register whose x scales the primary light on models (CONST_SRC_CODE_LIGHT_HERO_SCALE)
    HINT_SHADOW_SUN = 13,        // pixel sampler stage of the sun's shadow map (TEXTURE_SRC_CODE_SHADOWMAP_SUN), 255 none
    HINT_VS_SHADOW_MATRIX = 14,  // vertex constant register of the shadow map's lookup matrix (CONST_SRC_CODE_TRANSPOSE_SHADOW_LOOKUP_MATRIX,
                                 // four rows): the varying it makes holds the map's coordinates, the depth, and which partition
    HINT_PS_SHADOW_PARTITION = 15, // pixel constant register of the far partition's place in the map (CONST_SRC_CODE_SHADOWMAP_SWITCH_PARTITION)
    HINT_COUNT = 16,
};
enum
{
    HINT_READS_SCENE = 1, // a sampler reads the scene's colour copy (distortion, post effects): no stand-in draws for the pass
    HINT_READS_DEPTH = 2, // a sampler reads the depth buffer's copy (effects fading where they near a surface, water, depth of
                          // field): its stand-in draws without that
};

// a pass of the zone's materials, set up on the device as a draw of it would be (states, shaders, declaration, textures,
// hints), enumerated as the map loads (src/gfx_d3d/rb_draw3d.cpp: R_Bo1EnumeratePasses): the executor makes its stand-in,
// or its pipeline where the pass has none (it reads the scene's colour copy), right after the player goes in, or before
// where the map has no list; end: the enumeration is over
struct CmdEnumPass
{
    uint8_t end;
};

struct CmdPassHints
{
    uint8_t hints[HINT_COUNT];
    // the material's and technique's names (the engine's strings, which outlive the frame: the zone's), and the kind
    // of surface drawn (surfaceType_t; 16: none, a 2D or code draw), which name a draw in the log of waits
    const char *material, *technique;
    uint8_t surfType;
};

// ---------------------------------------------------------------------------------------------------------
// the ring
// ---------------------------------------------------------------------------------------------------------

#define BO1CMD_RING_SIZE (32u << 20)

struct Ring
{
    // byte offsets, monotonically increasing (the position is offset % size)
    volatile uint32_t writePos;
    volatile uint32_t readPos;
    volatile uint32_t framesSubmitted; // PRESENT commands written, each counted before it is in the ring
    volatile uint32_t framesTaken;     // frames the page has begun to replay, written whole: it replays each up to its
                                       // PRESENT on one animation frame
    volatile int32_t executorAlive;
    // how the page paces the engine (d3d9_device.cpp: Pace): the time between its animation frames, in microseconds, the
    // animation frames that found no whole frame to take, and the pacing now: 0 serial, 1 pipelined, 2 queued
    volatile uint32_t refreshUs;
    volatile uint32_t misses;
    volatile uint32_t pacing;
    // the player's frame rate limit, frames a second at most (0: none), which the engine writes at each Present
    // (src/qcommon/common.cpp: bo1_maxfps) and the page keeps to by the animation frames it takes (d3d9_webgpu.cpp:
    // Frame)
    volatile uint32_t maxFps;
    // the page's side, for what the engine says at a stall (d3d9_device.cpp: Present): its replays so far (an animation
    // frame's or a hidden page's wake each: d3d9_webgpu.cpp, Replay), the frames it replayed to their PRESENT, and those
    // of them the GPU has yet to finish
    volatile uint32_t replays;
    volatile uint32_t pageShown;
    volatile int32_t pagePending;
    uint8_t *data;
};

extern Ring g_ring;

// device side (d3d9_device.cpp): space for one command, then commit
void *BeginCmd(Op op, uint32_t payloadBytes);
void EndCmd();
} // namespace bo1cmd

// the engine's side of CmdPassHints (src/gfx_d3d/r_shade.cpp calls it, declared there as well: it includes no header of
// the web client's)
void Bo1_PassHints(const unsigned char *hints, const char *material, const char *technique, unsigned char surfType);
void Bo1_EnumPass(int end); // CmdEnumPass's
int Bo1_PipelineListState(); // d3d9_webgpu.cpp: the map's pipeline list: 0 on the way, 1 here, 2 none (or of another build)
void Bo1_PipelineSettings(int shadowMaps); // d3d9_device.cpp: the player's shadow maps, which the list's spawn tier goes by

namespace bo1gpu
{
// executor side (d3d9_webgpu.cpp): starts replaying the ring on the page's thread, if the page made a WebGPU device
// (zombies-hub's web/public/player.js makes one: gpuInit; not for a dedicated server, which draws nothing)
void ExecutorStart(int width, int height);
// and stops, as the engine quits (web_main.cpp: Bo1_Exit)
void ExecutorStop();
} // namespace bo1gpu
