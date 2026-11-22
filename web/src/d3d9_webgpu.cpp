// The WebGPU side of the Direct3D 9 device: replays d3d9_device.cpp's command ring on the page's thread, one engine
// frame per animation frame, on the WebGPU device the page made (zombies-hub's web/public/player.js: gpuInit,
// Module.bo1GpuDevice); the GPU calls themselves are library_gpu.js.
//
// Direct3D 9 and WebGPU agree on clip space (y up, depth 0..1), on the framebuffer (top row first) and on winding, so
// viewports, scissor rectangles, texture rows and read backs carry over unchanged; D3D9's pixel centers are handled in
// the vertex shaders (d3d9_wgsl.cpp). What differs:
//   - Pipelines: a draw's shaders, vertex layout, blending, depth and stencil states are one pipeline, which compiles
//     in the background the first time. Until it is ready, the draws that need it are drawn with a stand-in, the same
//     pipeline with a generated pixel shader that lights the colour map (stand-ins), or left out where that isn't
//     ready either: the page does not wait.
//     A list of the pipelines the game draws with on the map (web/pipelines/<map>.bin) compiles while it loads, the
//     first ones before the player goes in (recipes, preloading).
//   - Render passes: draws go into a pass over the render targets set. Changing them, a copy or a read back ends it.
//     Clearing whole targets is the next pass' load operation; clearing part of one draws a quad. A pass can't read
//     what it draws to: a draw that does reads a copy of it, or, for its depth buffer, has the buffer read only.
//   - Uploads: a queue write lands before everything recorded but not yet submitted. A write to a buffer range or a
//     texture that a recorded draw reads submits first, or, when the lock discards, gives the buffer new memory.
//     Constants and DrawPrimitiveUP's data go into one buffer that is written once per submit.
//   - Formats: there are no 16-bit normalized, 5:6:5, 5:5:5:1, 4:4:4:4, luminance, alpha-only, bump map or video
//     formats; those are converted on upload (d3d9_webgpu_formats.cpp), the 16-bit normalized ones to half floats
//     (finer than 16 bits below 1/64 and coarser above, 2^-11 at worst: finer than the 8-bit picture). Block-compressed
//     textures whose size is not a multiple of 4 are decompressed.
#include <windows.h>
#include <d3d9.h>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3d9_cmd.h"
#include "d3d9_formats.h"
#include "d3d9_webgpu_formats.h"
#include "d3d9_wgsl.h"
#include "web_platform.h"

// windows.h's, which std::min and std::max are not
#undef min
#undef max

// library_gpu.js
extern "C"
{
int bo1gpu_init();
int bo1gpu_canvas_format();
void bo1gpu_canvas_size(int *w, int *h);
uint32_t bo1gpu_texture(int dimension, int format, int width, int height, int layers, int mips, int usage, int viewFormat, int samples);
uint32_t bo1gpu_view(uint32_t tex, int format, int dimension, int aspect, int baseMip, int mips, int baseLayer, int layers);
void bo1gpu_write_texture(uint32_t tex, int mip, int x, int y, int z, const void *data, int bytesPerRow, int rowsPerImage, int width, int height,
    int depth);
uint32_t bo1gpu_buffer(int size, int usage);
void bo1gpu_write_buffer(uint32_t buf, int offset, const void *data, int size);
void bo1gpu_destroy(uint32_t h);
void bo1gpu_release(uint32_t h);
uint32_t bo1gpu_sampler(int u, int v, int w, int mag, int min, int mip, float lodMin, float lodMax, int compare, int aniso);
uint32_t bo1gpu_shader(const char *code, const char *label);
uint32_t bo1gpu_bind_group_layout(const uint32_t *entries, int count);
uint32_t bo1gpu_pipeline_layout(const uint32_t *layouts, int count);
uint32_t bo1gpu_bind_group(uint32_t layout, const uint32_t *entries, int count);
uint32_t bo1gpu_pipeline(const char *json, int32_t *status, int async);
void bo1gpu_begin_pass(const uint32_t *desc);
void bo1gpu_resolve(uint32_t view, uint32_t target);
void bo1gpu_end_pass();
uint32_t bo1gpu_query_set(int count);
uint32_t bo1gpu_time_set(int count);
void bo1gpu_begin_occlusion(int index);
void bo1gpu_end_occlusion();
void bo1gpu_resolve_queries(uint32_t set, int count, uint32_t resolve, uint32_t read);
void bo1gpu_set_pipeline(uint32_t p);
void bo1gpu_set_bind_group(int index, uint32_t group, const uint32_t *offsets, int count);
void bo1gpu_set_vertex_buffer(int slot, uint32_t buf, uint32_t offset, uint32_t size);
void bo1gpu_set_index_buffer(uint32_t buf, int index32, uint32_t offset, uint32_t size);
void bo1gpu_set_viewport(float x, float y, float w, float h, float minZ, float maxZ);
void bo1gpu_set_scissor(int x, int y, int w, int h);
void bo1gpu_set_stencil_ref(int ref);
void bo1gpu_set_blend_constant(float r, float g, float b, float a);
void bo1gpu_draw(int count, int instances, int first, int firstInstance);
void bo1gpu_draw_indexed(int count, int instances, int firstIndex, int baseVertex, int firstInstance, int list);
void bo1gpu_copy_texture(uint32_t src, int srcMip, int srcZ, int sx, int sy, uint32_t dst, int dstMip, int dstZ, int dx, int dy, int width,
    int height);
void bo1gpu_copy_to_buffer(uint32_t tex, int mip, int z, int width, int height, uint32_t buf, int bytesPerRow);
void bo1gpu_submit(int frame);
void bo1gpu_take_counts(double *calls, double *bytes);
void bo1gpu_take_drawn(uint32_t *out);
void bo1gpu_take_compiles(double *count, double *ms, double *longest);
int bo1gpu_frames_pending();
void bo1gpu_take_done(double *ms, double *frames);
void bo1gpu_wake_hidden(int ms, void (*wake)());
uint32_t bo1gpu_canvas_view();
void bo1gpu_canvas_see_through(int on);
void bo1gpu_map_read(uint32_t buf, int size, void *dst, int32_t *flag);
void bo1gpu_fetch_recipes();
void *bo1gpu_take_recipes(int *len);
}

using namespace bo1cmd;

int Com_FrameLimitStep(int displayHz, int limit); // src/qcommon/common.cpp

namespace bo1gpu
{
// ============================================================================================================
// vertex formats, usages, features
// ============================================================================================================

// library_gpu.js: BO1GPU.vertexFormats
enum VertexFormat : uint8_t
{
    VF_FLOAT32,
    VF_FLOAT32X2,
    VF_FLOAT32X3,
    VF_FLOAT32X4,
    VF_UNORM8X4,
    VF_SNORM16X2,
    VF_SNORM16X4,
    VF_UNORM16X2,
    VF_UNORM16X4,
    VF_FLOAT16X2,
    VF_FLOAT16X4,
    VF_NONE = 0xFF,
};

// GPUTextureUsage, GPUBufferUsage, GPUShaderStage
enum
{
    TU_COPY_SRC = 1,
    TU_COPY_DST = 2,
    TU_TEXTURE = 4,
    TU_RENDER = 16,
    BU_MAP_READ = 1,
    BU_COPY_SRC = 4,
    BU_COPY_DST = 8,
    BU_INDEX = 16,
    BU_VERTEX = 32,
    BU_UNIFORM = 64,
    BU_STORAGE = 128,
    BU_QUERY_RESOLVE = 512,
    VIS_VERTEX = 1,
    VIS_FRAGMENT = 2,
};

// bo1gpu_init's features
enum
{
    CAP_BC = 2,
    CAP_FLOAT32_FILTER = 4,
    CAP_FLOAT32_BLEND = 8,
};
static int s_caps;

// ============================================================================================================
// objects
// ============================================================================================================

struct Tex
{
    uint32_t tex = 0;
    TexType type = TEX_2D;
    uint32_t w = 1, h = 1, d = 1, levels = 1;
    uint32_t fmt = 0;
    TexFormat tf;
    uint32_t usage = 0;
    uint32_t views[2] = {}; // sampling: plain, sRGB
    std::unordered_map<uint32_t, uint32_t> targetViews; // level | layer << 8 -> view
    uint32_t used = 0;      // the serial of the encoder that last used it
    std::vector<uint64_t> bindGroups; // cached bind groups that hold its views
    struct Tex *copy = nullptr;        // what a draw reads of it while drawing to it (Snapshot)
    // Anti-aliasing: the samples a pixel has, 1 or 4 (MSAA: the back buffer and the depth buffer drawn with it, as the
    // engine makes them, src/gfx_d3d/r_rendertarget.cpp). A multisampled surface is only drawn to: what copies or
    // reads it reads its resolved copy, of one sample a pixel (Resolved)
    uint32_t samples = 1;
    struct Tex *resolved = nullptr;
};

struct Buf
{
    uint32_t gpu = 0;
    uint32_t size = 0, alloc = 0;
    bool index = false, index32 = false;
    bool dynamic = false; // D3DUSAGE_DYNAMIC
    uint32_t used = 0;    // the encoder whose draws read `ranges`
    std::vector<std::pair<uint32_t, uint32_t>> ranges; // [first byte, end), a few (dynamic buffers: both ends of a ring)
    // index and dynamic buffers: their contents, for writes that do not start or end on 4 bytes, and for what waits to
    // be written (WritePending)
    std::vector<uint8_t> mirror;
    uint32_t pendingLo = 0, pendingHi = 0; // dynamic buffers: the bytes of the mirror to write before the next submit
    bool queued = false;                   // in s_pendingBuffers
    int pool = -1;                         // static buffers: a part of a pool's GPU buffer (s_pools), from `base`
    uint32_t base = 0, gpuSize = 0;        // gpuSize: the GPU buffer's, bound whole
};

struct Decl
{
    std::vector<D3DVERTEXELEMENT9> el;
};

// A shader's WGSL in one variant (ShaderEmit). Its module exists while pipelines are made with it: Chrome keeps about a
// megabyte for each module, and none for it once the pipelines are made, so an idle one is released, and made again from
// the parsed shader when a new pipeline needs it.
struct Module
{
    uint32_t handle = 0;
    uint32_t id = 0; // the variant's, for pipeline keys (never reused, unlike handles)
    uint32_t overrides = 0;
    bool failed = false;
    double used = 0.0; // when a pipeline last needed it
};

// What a stand-in pixel shader reads (stand-ins, below): the colour map and the lightmaps at their sampler stages, each
// with the varying its coordinates come from and the swizzle of the two (SamplerCoord), the vertex colour, the light
// vector and the normal the vertex shader passes, and the primary light's constants
struct StandInRouting
{
    uint8_t colorStage, colorLoc, colorSwz;
    uint8_t lmPrimaryStage, lmPrimaryLoc, lmPrimarySwz;     // 255: none
    uint8_t lmSecondaryStage, lmSecondaryLoc, lmSecondarySwz;
    uint8_t probeStage, probeLoc; // a model's light from the light grid (a 3D texture) at the varying its cube's coordinates come in; 255: none
    uint8_t shadowStage, shadowLoc; // the sun's shadow map (a depth texture, compared) at the varying its coordinates come in; 255: none
    uint8_t lightLoc, normalLoc; // 255: none
    uint8_t vertexColor;         // reads COLOR0
    uint8_t premultiply;         // an additive blend: the colour times its alpha (the shape of a glow is in its alpha)
    uint8_t diffuseFrom;         // the light's diffuse colour: 0 none, 1 a pixel constant register, 2 a vertex one
    uint8_t positionFrom;        // its position (direction)
    uint8_t lookup, hdr, hero, partition; // the pass has the cube's scale, the HDR scale, the hero scale, the shadow map's partition
    // (which registers hold these, the draw's hints say at upload: UploadStandInConstants; one stand-in serves every
    // register of them)
    bool operator==(const StandInRouting &o) const { return !memcmp(this, &o, sizeof(StandInRouting)); }
};

// Direct3D shaders with the same bytecode (the engine loads a shader once per fastfile that has it) share one: its
// translation, modules and pipelines
struct Program
{
    ShaderInfo info;
    std::unordered_map<uint64_t, Module> modules; // variant ->
    uint32_t refs = 0;
    uint32_t id = 0;   // the first shader's, for labels
    uint64_t hash = 0; // of the bytecode, which recipes name it by
    const StandInRouting *standIn = nullptr; // a stand-in pixel shader: generated from this, not from bytecode
};

struct Shader
{
    Program *prog = nullptr;
};

static std::unordered_map<uint32_t, Tex> s_textures;
static std::unordered_map<uint32_t, Tex> s_surfaces; // render targets and depth stencils not in a texture
static std::unordered_map<uint32_t, Buf> s_buffers;
static std::unordered_map<uint32_t, Decl> s_decls;
static std::unordered_map<uint32_t, Shader> s_shaders;
static std::unordered_map<std::string, Program *> s_programs; // bytecode ->
static std::unordered_map<uint64_t, Program *> s_programsByHash;
static Tex s_back; // the back buffer, drawn onto the canvas by Present

// ============================================================================================================
// state
// ============================================================================================================

static const int MAX_SAMPLERS = 261;

struct StreamState
{
    uint32_t id, offset, stride;
};

static struct State
{
    DWORD rs[256];
    DWORD ss[MAX_SAMPLERS][D3DSAMP_DMAPOFFSET + 1];
    uint32_t tex[MAX_SAMPLERS];
    StreamState stream[16];
    uint32_t freq[16];
    uint32_t indices, decl, vs, ps;
    float vsF[256 * 4], psF[224 * 4];
    int vsI[16 * 4], psI[16 * 4];
    int vsB[16], psB[16];
    uint32_t vsGen, psGen; // constant changes
    CmdViewport vp;
    CmdRect scissor;
    SurfRef rt[4];
    SurfRef ds;
    uint8_t hints[HINT_COUNT]; // the pass set up last (CmdPassHints), for stand-ins
    const char *passMaterial, *passTechnique; // its names, and the kind of surface drawn, which name a draw in the log
    uint8_t passSurfType;
} st;

// the draw being set up, for the log: its material, technique and kind of surface (CmdPassHints)
static std::string DrawName()
{
    // the engine's surface types (src/gfx_d3d/r_material.h: SF_): a world's surfaces and a static model's come batched
    // (several in a draw: pretess), and a static model's cached too (its vertices placed in the world once, so that
    // the cached ones of a material are a draw: r_staticmodelcache.cpp), else a draw each with its own placement
    static const char *const kinds[17] = {"world", "world batched", "static model", "static model batched", "static model cached",
        "static model skinned", "brush model", "model", "model", "model", "water model", "effect", "mark", "particle cloud", "rope", "glass",
        "2D or code"};
    std::string s = st.passMaterial ? st.passMaterial : "?";
    s += ", ";
    s += st.passTechnique && st.passTechnique[0] ? st.passTechnique : "?";
    s += ", ";
    s += kinds[st.passSurfType < 17 ? st.passSurfType : 16];
    return s;
}

static uint32_t s_serial = 1; // the command encoder being recorded; a submit starts the next
static uint32_t s_frameDraws, s_frameSkips, s_frameWaits, s_frameStandIns, s_frameFlushes;
static bool s_capStandIn; // the draw PrepareDraw set up last drew with its pipeline's stand-in (CaptureDraw)
static uint32_t s_leftOutWhy[5]; // draw-frames left out since the last report, by StandInFor's reason (0: no pipeline at all)
// Module.bo1GpuFlags (page, ?gpu=): 1 no culling, 2 inverted culling, 4 log every draw of the next frame (then cleared), 8
// log the frame rate every 10 s, 16 log the next frame's profile (then cleared), 32 draw with a call to WebGPU for each
// state the engine sets, as before fewer did (to compare: each write to a dynamic buffer at once, each vertex buffer bound
// where its stream starts, a new uniform block for each change of constants, and static buffers not in pools, which
// needs it from the start), 128 work each draw out in full too, as draws did before they kept what they worked out, and
// log where the two differ (PrepareDraw: CheckSetup), 256 drop every pipeline (then cleared), so that draws make them
// anew, with stand-ins at work meanwhile (DropPipelines), 1024 each draw's vertex shader constants bound as a uniform
// block, as before a draw's first instance said where they are (SetByInstance), 2048 a call to WebGPU for each draw, as
// before draws alike went as one (library_gpu.js: bo1gpu_draw_indexed), 4096 the page two frames ahead of the GPU at
// most, as before it went a third while the GPU keeps pace (AheadMost)
enum
{
    DBG_NO_CULL = 1,
    DBG_FLIP_CULL = 2,
    DBG_CAPTURE = 4,
    DBG_PROFILE = 16,
    DBG_PLAIN = 32,
    DBG_CHECK = 128,
    DBG_DROP = 256,
    DBG_BOUND_CONSTANTS = 1024,
    DBG_TWO_AHEAD = 4096,
};
static uint32_t s_debugFlags;
static bool s_capturing;
static int s_logBudget = 200;

// A frame's profile (&gpu=16), logged as it ends ("webgpu profile:" lines): each pass' targets, its draws and the state
// set for them (each a call to WebGPU), and what the frame wrote to the GPU, and where
struct ProfPass
{
    uint32_t w, h, color, depth; // the targets' size, and the GpuFormat of the first color target and of the depth
    uint32_t draws, pipelines, constants, textures, vertexBuffers, indexBuffers, viewports, scissors, other;
    // draws that set vertex buffers: any, one at least another buffer, the same buffers further on by a whole number of
    // vertices (each vertex buffer's stride; what a draw's first vertex could say instead); draws of several instances
    uint32_t vbDraws, vbNewBuffer, vbShift, instanced;
};
struct ProfBuffer
{
    uint32_t writes, bytes, discards;
};
enum
{
    PROF_STATIC_VB,
    PROF_DYNAMIC_VB,
    PROF_STATIC_IB,
    PROF_DYNAMIC_IB,
    PROF_TEXTURES,
    PROF_WRITES
};
struct Profile
{
    bool on;
    std::vector<ProfPass> passes;
    uint32_t vsUploads, psUploads, constantBytes, upBytes, submits, snapshots;
    uint32_t waits, standIns; // draws whose pipelines were compiling, and those of them drawn with a stand-in meanwhile
    uint32_t queueWrites; // calls to the queue's writeBuffer and writeTexture
    // what draws worked out again, where they kept the rest from the draws before (PrepareDraw: SetupKept): their
    // shaders, targets, recipes, vertex layouts, pipelines (and made), texture bind groups
    uint32_t workedShaders, workedTargets, workedRecipes, workedLayouts, workedPipelines, madePipelines, workedGroups;
    uint32_t writes[PROF_WRITES], bytes[PROF_WRITES];
    uint32_t ops[OP_COUNT]; // the engine's commands replayed, by kind (Execute)
    std::unordered_map<uint32_t, ProfBuffer> buffers, textures; // id ->
};
static Profile s_prof;
#define PROF(field) (s_prof.on && !s_prof.passes.empty() ? (void)++s_prof.passes.back().field : (void)0)

// what WebGPU can't do as the engine asked: the picture is wrong ("gpu error:", which fails a test: zombies-hub's
// e2e/game/player.ts)
#define BO1_GPU_ERROR(fmt, ...)                                                                                       \
    do                                                                                                                \
    {                                                                                                                 \
        if (s_logBudget > 0)                                                                                          \
        {                                                                                                             \
            --s_logBudget;                                                                                            \
            Bo1_Log("gpu error: " fmt, __VA_ARGS__);                                                                  \
        }                                                                                                             \
    } while (0)

// ============================================================================================================
// submitting, and what waits for it
// ============================================================================================================

// Constants and DrawPrimitiveUP data: written here, and to the GPU's copy at each submit. Uniform blocks are bound at
// an offset into it, and read up to UNIFORM_BINDING bytes.
static const uint32_t ARENA_SIZE = 16u << 20;
static const uint32_t UNIFORM_BINDING = 4608; // the largest uniform block: 32 + 16 * (256 + 16) bytes
static uint8_t *s_arena;
static uint32_t s_arenaUsed, s_arenaGpu, s_arenaGroup, s_arenaLayout; // the group and its layout: s_byInstance's
// A draw's vertex shader reads its constants from the arena itself, bound whole and once as a storage buffer, from where
// the draw's first instance says, in blocks of the shader's size (d3d9_wgsl.h: ShaderEmit; FirstInstance), not from a
// uniform block bound at the block's offset: most draws of a scene have constants of their own, a model's place at the
// least, and binding a block is a call to WebGPU for each, which Chrome's GPU process carries out one by one (Kino's
// heaviest view: 2,174 of a frame's 7,334 calls). The pixel shader's block stays a uniform, bound when it changes,
// which is with the material. &gpu=1024 binds the vertex shader's block as before, to compare, as the game runs too
// (SetByInstance): group 0 and its layout, each vertex shader's module (a variant: Variant) and so each pipeline are of
// the one way or the other, [s_byInstance]. Draws alike, one after the other, then differ by their first instance
// alone, each block following the last's: the page draws them as instances of one draw (library_gpu.js:
// bo1gpu_draw_indexed).
static bool s_byInstance;
static uint32_t s_arenaGroups[2], s_arenaLayouts[2];

struct Retired
{
    uint32_t handle, size;
};
static std::vector<Retired> s_retiredBuffers;                          // given new memory: reusable once submitted
static std::unordered_map<uint32_t, std::vector<uint32_t>> s_bufferPool; // size -> buffers
static std::vector<uint32_t> s_destroyAfterSubmit;                      // textures and buffers the encoder may use
static std::vector<uint32_t> s_pendingBuffers;                          // dynamic buffers with writes waiting (ids)
struct Freed
{
    int pool;
    uint32_t offset, bytes;
};
static std::vector<Freed> s_freedParts; // of pools, by destroyed buffers the encoder may read: free once submitted
static void PoolFree(const Freed &f);

static void EndPass();
struct OccFrame;
static OccFrame *OcclusionFrameEnds();
static void OcclusionReadBack(OccFrame *f);
struct TimeFrame;
static TimeFrame *TimeFrameEnds();
static void TimeReadBack(TimeFrame *f);
static void WritePending(Buf &b);

static void Submit(bool frame)
{
    EndPass();
    OccFrame *occlusion = frame ? OcclusionFrameEnds() : nullptr;
    TimeFrame *times = frame ? TimeFrameEnds() : nullptr;
    if (s_arenaUsed)
    {
        bo1gpu_write_buffer(s_arenaGpu, 0, s_arena, (s_arenaUsed + 3) & ~3u);
        s_prof.queueWrites += s_prof.on;
    }
    for (uint32_t id : s_pendingBuffers)
    {
        auto it = s_buffers.find(id);
        if (it == s_buffers.end())
            continue;
        WritePending(it->second);
        it->second.queued = false;
    }
    s_pendingBuffers.clear();
    bo1gpu_submit(frame);
    s_prof.submits += s_prof.on;
    if (occlusion)
        OcclusionReadBack(occlusion);
    if (times)
        TimeReadBack(times);
    ++s_serial;
    s_arenaUsed = 0;
    for (uint32_t h : s_destroyAfterSubmit)
        bo1gpu_destroy(h);
    s_destroyAfterSubmit.clear();
    for (const Freed &f : s_freedParts)
        PoolFree(f);
    s_freedParts.clear();
    for (const Retired &r : s_retiredBuffers)
    {
        std::vector<uint32_t> &pool = s_bufferPool[r.size];
        if (pool.size() < 8)
            pool.push_back(r.handle);
        else
            bo1gpu_destroy(r.handle);
    }
    s_retiredBuffers.clear();
}

// a write needs what is recorded to go first
static void Flush()
{
    ++s_frameFlushes;
    Submit(false);
}

// room for a draw's data in the arena: a submit makes it if there is not enough
static void ArenaReserve(uint32_t bytes)
{
    if (s_arenaUsed + bytes + 512 > ARENA_SIZE)
        Flush();
}

// align: any number of bytes (a vertex shader's block is on its own size: UploadConstants)
static uint32_t ArenaAlloc(uint32_t bytes, uint32_t align, uint8_t **ptr)
{
    uint32_t at = (s_arenaUsed + align - 1) / align * align;
    if (at + bytes > ARENA_SIZE)
    {
        BO1_GPU_ERROR("arena overflow (%u bytes)", bytes);
        at = 0;
    }
    s_arenaUsed = at + bytes;
    *ptr = s_arena + at;
    return at;
}

// ============================================================================================================
// textures and surfaces
// ============================================================================================================

static void MakeTexture(Tex &t, uint32_t usage)
{
    if (!t.tf.valid)
    {
        BO1_GPU_ERROR("texture format %u not supported", t.fmt);
        t.tf = Format(D3DFMT_A8R8G8B8);
    }
    if (t.tf.block && (!(s_caps & CAP_BC) || t.type == TEX_3D || (t.w & 3) || (t.h & 3)))
        Decompressed(t.tf);
    uint32_t gpuUsage = TU_TEXTURE | (t.tf.depth ? 0 : TU_COPY_DST | TU_COPY_SRC);
    if ((usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP)) && t.tf.renderable)
        gpuUsage |= TU_RENDER;
    if (!(gpuUsage & TU_RENDER) || t.type != TEX_2D || t.levels != 1)
        t.samples = 1;
    // drawn to, and resolved; one of 32-bit floats, which WebGPU resolves none of, is read for that (FirstSampleInto)
    if (t.samples > 1)
        gpuUsage = TU_RENDER | (t.tf.float32 ? TU_TEXTURE : 0);
    t.usage = gpuUsage;
    uint32_t layers = t.type == TEX_CUBE ? 6 : t.type == TEX_3D ? t.d : 1;
    t.tex = bo1gpu_texture(t.type == TEX_3D, t.tf.gpu, t.w, t.h, layers, t.levels, gpuUsage, t.tf.srgb, t.samples);
}

// Direct3D's multisample type as WebGPU's samples: 4, or 1 (d3d9_device.cpp: CheckDeviceMultiSampleType)
static uint32_t SamplesOf(uint32_t multisample) { return multisample >= D3DMULTISAMPLE_4_SAMPLES ? 4 : 1; }

static void CreateTexture(const CmdCreateTexture &c)
{
    Tex &t = s_textures[c.id];
    t.type = (TexType)c.type;
    t.w = c.width;
    t.h = c.height;
    t.d = c.depth ? c.depth : 1;
    t.levels = c.levels ? c.levels : 1;
    t.fmt = c.format;
    t.tf = Format(c.format);
    MakeTexture(t, c.usage);
}

static void CreateSurface(const CmdCreateSurface &c)
{
    Tex &s = s_surfaces[c.id];
    s.w = c.width;
    s.h = c.height;
    s.fmt = c.format;
    s.tf = Format(c.format);
    s.samples = SamplesOf(c.multisample);
    if (!s.tf.valid || c.format == BO1FMT_NULL)
        return; // no storage: a NULL render target
    MakeTexture(s, c.depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET);
}

// a view for sampling: every level and face
static uint32_t SampleView(Tex &t, bool srgb)
{
    srgb = srgb && t.tf.srgb;
    uint32_t &v = t.views[srgb];
    if (!v)
    {
        int dim = t.type == TEX_CUBE ? 1 : t.type == TEX_3D ? 2 : 0;
        v = bo1gpu_view(t.tex, srgb ? t.tf.srgb : 0, dim, t.tf.depth ? 1 : 0, 0, t.levels, 0, t.type == TEX_CUBE ? 6 : 1);
    }
    return v;
}

// a view of one level and face (a slice of a volume) to draw to, copy or blit from
static uint32_t LevelView(Tex &t, uint32_t level, uint32_t layer)
{
    uint32_t &v = t.targetViews[level | layer << 8];
    if (!v)
        v = bo1gpu_view(t.tex, 0, 0, 0, level, 1, layer, 1);
    return v;
}

// cached bind groups that hold a texture's views go with it
static void ForgetBindGroups(Tex &t);

static void ReleaseTexture(Tex &t)
{
    ForgetBindGroups(t);
    for (uint32_t &v : t.views)
        if (v)
            bo1gpu_release(v);
    for (auto &v : t.targetViews)
        bo1gpu_release(v.second);
    if (t.tex)
        s_destroyAfterSubmit.push_back(t.tex);
    if (t.copy)
    {
        ReleaseTexture(*t.copy);
        delete t.copy;
    }
    if (t.resolved)
    {
        ReleaseTexture(*t.resolved);
        delete t.resolved;
    }
    t = Tex();
}

static void UploadTexture(const CmdUploadTexture &c)
{
    auto it = s_textures.find(c.id);
    if (it == s_textures.end() || !it->second.tex)
    {
        free(c.data);
        return;
    }
    Tex &t = it->second;
    if (t.tf.depth)
    {
        BO1_GPU_ERROR("texture %u: writes to depth textures are not supported", c.id);
        free(c.data);
        return;
    }
    if (t.used == s_serial)
        Flush(); // a recorded draw reads the texture as it is now
    uint32_t z = t.type == TEX_CUBE ? c.face : c.z;
    uint32_t depth = t.type == TEX_3D ? c.depth : 1;
    if (s_prof.on)
    {
        uint32_t bytes = c.pitch * (t.tf.block ? (c.height + 3) / 4 : c.height) * depth;
        ++s_prof.writes[PROF_TEXTURES];
        s_prof.bytes[PROF_TEXTURES] += bytes;
        ++s_prof.queueWrites;
        ProfBuffer &pt = s_prof.textures[c.id];
        ++pt.writes;
        pt.bytes += bytes;
    }
    if (t.tf.block)
    {
        // whole blocks, even where the level is smaller than one
        uint32_t lw = (Bo1_MipSize(t.w, c.level) + 3) & ~3u, lh = (Bo1_MipSize(t.h, c.level) + 3) & ~3u;
        uint32_t w = std::min((c.width + 3) & ~3u, lw - std::min(lw, c.x));
        uint32_t h = std::min((c.height + 3) & ~3u, lh - std::min(lh, c.y));
        if (w && h)
            bo1gpu_write_texture(t.tex, c.level, c.x, c.y, z, c.data, c.pitch, h / 4, w, h, 1);
    }
    else if (t.tf.conv == CONV_NONE)
        bo1gpu_write_texture(t.tex, c.level, c.x, c.y, z, c.data, c.pitch, c.height, c.width, c.height, depth);
    else
    {
        static std::vector<uint8_t> tmp, slices;
        size_t slice = (size_t)c.width * c.height * t.tf.outBpp;
        slices.resize(slice * depth + 16);
        for (uint32_t s = 0; s < depth; ++s)
        {
            const uint8_t *px = Convert(t.tf, t.fmt, (const uint8_t *)c.data + (size_t)s * c.slicePitch, c.width, c.height, c.pitch, tmp);
            memcpy(slices.data() + slice * s, px, slice);
        }
        bo1gpu_write_texture(t.tex, c.level, c.x, c.y, z, slices.data(), c.width * t.tf.outBpp, c.height, c.width, c.height, depth);
    }
    free(c.data);
}

// ============================================================================================================
// buffers
// ============================================================================================================

static uint32_t NewBuffer(uint32_t alloc)
{
    std::vector<uint32_t> &pool = s_bufferPool[alloc];
    if (!pool.empty())
    {
        uint32_t h = pool.back();
        pool.pop_back();
        return h;
    }
    return bo1gpu_buffer(alloc, BU_VERTEX | BU_INDEX | BU_COPY_DST);
}

// Static buffers share a few big GPU buffers, each from an offset of its own (Buf::base). The engine draws each model
// from vertex and index buffers of its own, and binding them is a call to WebGPU each; the buffers of a pool are bound
// once, and the draw's first index and first vertex say where in it (DrawIndexed, PrepareDraw).
struct Pool
{
    uint32_t gpu;
    std::map<uint32_t, uint32_t> free; // offset -> bytes
};
static std::vector<Pool> s_pools;
static const uint32_t POOL_SIZE = 64u << 20, POOL_ALIGN = 256;

static bool PoolAlloc(uint32_t bytes, int *pool, uint32_t *offset)
{
    bytes = (bytes + POOL_ALIGN - 1) & ~(POOL_ALIGN - 1);
    if (bytes > POOL_SIZE / 4)
        return false;
    for (int round = 0; round < 2; ++round)
    {
        for (size_t i = 0; i < s_pools.size(); ++i)
            for (auto f = s_pools[i].free.begin(); f != s_pools[i].free.end(); ++f)
                if (f->second >= bytes)
                {
                    *pool = (int)i;
                    *offset = f->first;
                    uint32_t at = f->first + bytes, left = f->second - bytes;
                    s_pools[i].free.erase(f);
                    if (left)
                        s_pools[i].free[at] = left;
                    return true;
                }
        s_pools.push_back({bo1gpu_buffer(POOL_SIZE, BU_VERTEX | BU_INDEX | BU_COPY_DST), {{0u, POOL_SIZE}}});
    }
    return false;
}

static void PoolFree(const Freed &f)
{
    std::map<uint32_t, uint32_t> &free = s_pools[f.pool].free;
    auto at = free.emplace(f.offset, (f.bytes + POOL_ALIGN - 1) & ~(POOL_ALIGN - 1)).first;
    auto next = std::next(at);
    if (next != free.end() && at->first + at->second == next->first)
    {
        at->second += next->second;
        free.erase(next);
    }
    if (at != free.begin())
    {
        auto prev = std::prev(at);
        if (prev->first + prev->second == at->first)
        {
            prev->second += at->second;
            free.erase(at);
        }
    }
}

static void CreateBuffer(const CmdCreateBuffer &c)
{
    Buf &b = s_buffers[c.id];
    b.size = c.size;
    b.alloc = (c.size + 3) & ~3u;
    if (!b.alloc)
        b.alloc = 4;
    b.index = c.index != 0;
    b.index32 = c.format == D3DFMT_INDEX32;
    b.dynamic = (c.usage & D3DUSAGE_DYNAMIC) != 0;
    if (b.index || b.dynamic)
        b.mirror.assign(b.alloc, 0);
    if (!b.dynamic && !(s_debugFlags & DBG_PLAIN) && PoolAlloc(b.alloc, &b.pool, &b.base))
    {
        b.gpu = s_pools[b.pool].gpu;
        b.gpuSize = POOL_SIZE;
    }
    else
    {
        b.gpu = NewBuffer(b.alloc);
        b.gpuSize = b.alloc;
    }
}

// What a dynamic buffer was written since the last submit goes to the GPU's copy, in one write. The engine writes each
// draw's indices as it draws (R_SetIndexData), and each write to WebGPU is a call; the queue's writes all land before
// the submit's draws anyway. Each submit writes what waits (Submit), and so does a write the draws recorded would see
// otherwise (UploadBuffer).
static void WritePending(Buf &b)
{
    if (b.pendingHi <= b.pendingLo)
        return;
    bo1gpu_write_buffer(b.gpu, b.pendingLo, b.mirror.data() + b.pendingLo, b.pendingHi - b.pendingLo);
    s_prof.queueWrites += s_prof.on;
    b.pendingLo = b.pendingHi = 0;
}

static void UploadBuffer(const CmdUploadBuffer &c)
{
    auto it = s_buffers.find(c.id);
    if (it == s_buffers.end() || c.offset + c.size > it->second.size || !c.size)
    {
        free(c.data);
        return;
    }
    Buf &b = it->second;
    bool read = false;
    if (b.used == s_serial)
        for (auto &r : b.ranges)
            read = read || (c.offset < r.second && c.offset + c.size > r.first);
    if (read)
    {
        // a recorded draw reads this range
        if (c.discard && b.pool < 0)
        {
            WritePending(b); // for the draws recorded, which read the old memory
            s_retiredBuffers.push_back({b.gpu, b.alloc});
            b.gpu = NewBuffer(b.alloc);
            b.used = 0;
        }
        else
            Flush();
    }
    // queue writes start and end on 4 bytes: index buffers fill in around from what they hold
    uint32_t start = c.offset & ~3u, end = (c.offset + c.size + 3) & ~3u;
    if (b.dynamic && !(s_debugFlags & DBG_PLAIN))
    {
        if (s_prof.on)
        {
            int kind = b.index ? PROF_DYNAMIC_IB : PROF_DYNAMIC_VB;
            ++s_prof.writes[kind];
            s_prof.bytes[kind] += end - start;
            ProfBuffer &pb = s_prof.buffers[c.id];
            ++pb.writes;
            pb.bytes += end - start;
            pb.discards += c.discard != 0;
        }
        memcpy(b.mirror.data() + c.offset, c.data, c.size);
        free(c.data);
        // what waits far from this write goes first, rather than the bytes in between
        static const uint32_t GAP = 64u << 10;
        if (b.pendingHi > b.pendingLo && (start > b.pendingHi + GAP || end + GAP < b.pendingLo))
            WritePending(b);
        if (b.pendingHi > b.pendingLo)
        {
            b.pendingLo = std::min(b.pendingLo, start);
            b.pendingHi = std::max(b.pendingHi, end);
        }
        else
        {
            b.pendingLo = start;
            b.pendingHi = end;
        }
        if (!b.queued)
        {
            b.queued = true;
            s_pendingBuffers.push_back(c.id);
        }
        return;
    }
    const uint8_t *src = (const uint8_t *)c.data;
    if (!b.mirror.empty())
    {
        memcpy(b.mirror.data() + c.offset, c.data, c.size);
        src = b.mirror.data() + start;
    }
    else if (start != c.offset || end != c.offset + c.size)
    {
        static std::vector<uint8_t> padded;
        padded.assign(end - start, 0);
        memcpy(padded.data() + (c.offset - start), c.data, c.size);
        src = padded.data();
        if (start != c.offset || end < b.size)
            BO1_GPU_ERROR("buffer %u: a write at %u, %u bytes, not on 4 bytes", c.id, c.offset, c.size);
    }
    if (s_prof.on)
    {
        int kind = (b.index ? PROF_STATIC_IB : PROF_STATIC_VB) + b.dynamic;
        ++s_prof.writes[kind];
        s_prof.bytes[kind] += end - start;
        ProfBuffer &pb = s_prof.buffers[c.id];
        ++pb.writes;
        pb.bytes += end - start;
        pb.discards += c.discard != 0;
    }
    bo1gpu_write_buffer(b.gpu, b.base + start, src, end - start);
    s_prof.queueWrites += s_prof.on;
    free(c.data);
}

// a recorded draw reads bytes lo..hi of the buffer
static void UseBuffer(Buf &b, uint64_t lo64, uint64_t hi64)
{
    uint32_t lo = (uint32_t)std::min<uint64_t>(lo64, b.alloc), hi = (uint32_t)std::min<uint64_t>(hi64, b.alloc);
    if (b.used != s_serial)
    {
        b.used = s_serial;
        b.ranges.clear();
    }
    for (auto &r : b.ranges)
        if (lo <= r.second && hi >= r.first)
        {
            r.first = std::min(r.first, lo);
            r.second = std::max(r.second, hi);
            return;
        }
    b.ranges.push_back({lo, hi});
    if (b.ranges.size() > 8)
    {
        // too many: the two closest become one
        std::sort(b.ranges.begin(), b.ranges.end());
        size_t best = 0;
        for (size_t i = 1; i + 1 < b.ranges.size(); ++i)
            if (b.ranges[i + 1].first - b.ranges[i].second < b.ranges[best + 1].first - b.ranges[best].second)
                best = i;
        b.ranges[best].second = std::max(b.ranges[best].second, b.ranges[best + 1].second);
        b.ranges.erase(b.ranges.begin() + best + 1);
    }
}

// ============================================================================================================
// render targets and passes
// ============================================================================================================

// a surface to draw to or copy: a level (and face) of a texture
struct Target
{
    Tex *tex = nullptr;
    uint32_t level = 0, layer = 0;
    uint32_t w = 0, h = 0;
};

static bool Resolve(const SurfRef &r, Target &out)
{
    out = Target();
    Tex *t = nullptr;
    switch (r.kind)
    {
    case SURF_BACKBUFFER:
        t = &s_back;
        break;
    case SURF_STANDALONE: {
        auto it = s_surfaces.find(r.id);
        if (it != s_surfaces.end())
            t = &it->second;
        break;
    }
    case SURF_TEXTURE: {
        auto it = s_textures.find(r.id);
        if (it != s_textures.end())
        {
            t = &it->second;
            out.level = r.level;
            out.layer = r.face;
        }
        break;
    }
    }
    if (!t || !t->tex)
        return false;
    out.tex = t;
    out.w = Bo1_MipSize(t->w, out.level);
    out.h = Bo1_MipSize(t->h, out.level);
    return true;
}

struct PassKey
{
    SurfRef color[4];
    SurfRef depth;
    uint32_t special;       // the executor's own passes (blits, Present): never the same as a draw's
    uint32_t readOnlyDepth; // the depth buffer is read in the pass, and not written
    bool operator==(const PassKey &o) const { return !memcmp(this, &o, sizeof(PassKey)); }
};

// what a pass draws to
struct PassTargets
{
    uint32_t colorView[4] = {}, depthView = 0;
    GpuFormat colorFormat[4] = {}, depthFormat = GF_NONE;
    bool stencil = false;
    bool clamp[4] = {};
    Tex *colorTex[4] = {};
    Tex *depthTex = nullptr;
    uint32_t w = 0, h = 0;
    uint32_t samples = 1; // a pixel's, as the targets have them
};

static std::unordered_map<uint64_t, Tex> s_depthProxies; // (width, height, format) -> depth buffer

static bool ResolvePass(const PassKey &key, PassTargets &out)
{
    out = PassTargets();
    for (int i = 0; i < 4; ++i)
    {
        Target t;
        if (key.color[i].kind == SURF_NONE || !Resolve(key.color[i], t))
            continue;
        if (!(t.tex->usage & TU_RENDER) || t.tex->tf.depth || t.tex->type == TEX_3D)
        {
            BO1_GPU_ERROR("render target %u:%u can't be drawn to (format %u)", key.color[i].kind, key.color[i].id, t.tex->fmt);
            continue;
        }
        if (out.w && (t.w != out.w || t.h != out.h))
            continue;
        if (out.w && t.tex->samples != out.samples)
        {
            BO1_GPU_ERROR("render target %u:%u has %u samples a pixel, the others %u", key.color[i].kind, key.color[i].id, t.tex->samples,
                out.samples);
            continue;
        }
        out.colorView[i] = LevelView(*t.tex, t.level, t.layer);
        out.colorFormat[i] = t.tex->tf.gpu;
        out.colorTex[i] = t.tex;
        out.clamp[i] = t.tex->tf.clamp;
        out.w = t.w;
        out.h = t.h;
        out.samples = t.tex->samples;
    }
    Target d;
    if (key.depth.kind != SURF_NONE && Resolve(key.depth, d) && d.tex->tf.depth)
    {
        Tex *tex = d.tex;
        if (out.w && tex->samples != out.samples)
        {
            // as Direct3D 9, which draws nothing with them
            BO1_GPU_ERROR("depth buffer %u:%u has %u samples a pixel, the render targets %u", key.depth.kind, key.depth.id, tex->samples,
                out.samples);
            return false;
        }
        if (out.w && (d.w != out.w || d.h != out.h))
        {
            // D3D9 draws to a render target smaller than the depth buffer using the depth buffer's top-left corner;
            // WebGPU needs attachments of one size: such targets get a depth buffer of their own size
            uint64_t k = (uint64_t)out.w << 32 | (uint64_t)out.h << 8 | tex->tf.gpu | (uint64_t)(tex->samples > 1) << 7;
            Tex &proxy = s_depthProxies[k];
            if (!proxy.tex)
            {
                proxy.w = out.w;
                proxy.h = out.h;
                proxy.fmt = tex->fmt;
                proxy.tf = tex->tf;
                proxy.samples = tex->samples;
                MakeTexture(proxy, D3DUSAGE_DEPTHSTENCIL);
            }
            tex = &proxy;
            d.level = d.layer = 0;
        }
        out.depthView = LevelView(*tex, d.level, d.layer);
        out.depthFormat = tex->tf.gpu;
        out.stencil = tex->tf.stencil;
        out.depthTex = tex;
        out.w = Bo1_MipSize(tex->w, d.level);
        out.h = Bo1_MipSize(tex->h, d.level);
        out.samples = tex->samples;
    }
    return out.w != 0;
}

// what the open pass has set (WebGPU resets it at each pass)
static struct PassState
{
    uint32_t pipeline;
    uint32_t vsOffset, psOffset, group0;
    uint32_t group1;
    uint32_t vbuf[9], voff[9], vsize[9];
    uint32_t ibuf, ioff;
    bool index32;
    float viewport[6];
    int scissor[4];
    int stencilRef;
    float blend[4];
} ps;

static bool s_passOpen;
static PassKey s_passKey;
static PassTargets s_pass;

// clears of whole targets, waiting to be the next pass' load operations
static struct PendingClear
{
    bool any;
    PassKey key;
    bool color, depth, stencil;
    float rgba[4];
    float z;
    uint32_t s;
} s_clear;

static uint32_t OcclusionPassBegins();
static void OcclusionPassEnds();
static void TimePassBegins(const PassTargets &t, uint32_t *desc);
static void TimePassEnds();

static void BeginPassWith(const PassKey &key, const PassTargets &t, const PendingClear *clear)
{
    uint32_t desc[33] = {};
    for (int i = 0; i < 4; ++i)
    {
        uint32_t *c = desc + i * 6;
        c[0] = t.colorView[i];
        if (t.colorView[i] && clear && clear->color)
        {
            c[1] = 1;
            memcpy(c + 2, clear->rgba, 16);
        }
        if (t.colorTex[i])
            t.colorTex[i]->used = s_serial;
    }
    uint32_t *d = desc + 24;
    d[0] = t.depthView;
    d[1] = clear && clear->depth;
    float z = clear ? clear->z : 1.0f;
    memcpy(d + 2, &z, 4);
    d[3] = !t.stencil ? 2 : (clear && clear->stencil) ? 1 : 0;
    d[4] = clear ? clear->s : 0;
    d[5] = key.readOnlyDepth;
    if (t.depthTex)
        t.depthTex->used = s_serial;
    desc[30] = OcclusionPassBegins();
    TimePassBegins(t, desc + 31);
    bo1gpu_begin_pass(desc);
    if (s_prof.on)
        s_prof.passes.push_back({t.w, t.h, t.colorFormat[0], t.depthFormat});
    s_passOpen = true;
    s_passKey = key;
    s_pass = t;
    memset(&ps, 0, sizeof(ps));
    ps.viewport[2] = -1.0f;
    ps.scissor[2] = -1;
    ps.stencilRef = -1;
    ps.blend[0] = -1.0f;
}

// the open pass ends, and an occlusion query in it
static void ClosePass()
{
    OcclusionPassEnds();
    TimePassEnds();
    bo1gpu_end_pass();
    s_passOpen = false;
}

// the pending clears, in a pass of their own
static void FlushClears()
{
    if (!s_clear.any)
        return;
    s_clear.any = false;
    PassTargets t;
    if (ResolvePass(s_clear.key, t))
    {
        BeginPassWith(s_clear.key, t, &s_clear);
        ClosePass();
    }
}

static void EndPass()
{
    if (s_passOpen)
        ClosePass();
    FlushClears();
}

// a pass over these targets, open
static bool EnsurePass(const PassKey &key)
{
    if (s_passOpen && s_passKey == key)
        return true;
    if (s_passOpen)
        ClosePass();
    if (s_clear.any && !(s_clear.key == key))
        FlushClears();
    PassTargets t;
    if (!ResolvePass(key, t))
    {
        s_clear.any = false;
        return false;
    }
    bool clear = s_clear.any;
    s_clear.any = false;
    BeginPassWith(key, t, clear ? &s_clear : nullptr);
    return true;
}

static PassKey DrawPassKey()
{
    PassKey k;
    memset(&k, 0, sizeof(k));
    for (int i = 0; i < 4; ++i)
        k.color[i] = st.rt[i];
    k.depth = st.ds;
    return k;
}

// ============================================================================================================
// occlusion queries
// ============================================================================================================

// D3D9's occlusion query counts the samples its draws pass; WebGPU's says only whether any did. The engine compares
// two counts (coronas: a sprite drawn with the depth test and without) or a count with the first one it took (the
// sun's sprite, as the renderer starts), so any samples are BO1_OCCLUSION_VISIBLE, none 0. A frame's queries have a
// query set of their own, which each of the frame's passes is given. A query begins in a pass at its first draw there,
// ends before the pass does, and so has a slot in each pass it draws in; one query is open at a time, so its slots
// follow each other. Present resolves the set into a buffer, read back once the GPU is done (FinishOcclusion).
static const uint32_t OCC_SLOTS = 1024; // a frame's
struct OccQuery
{
    OcclusionAnswer *answer;
    uint32_t first, count; // slots
};
struct OccFrame
{
    uint32_t set, resolve, read; // the query set, the buffer it resolves into, and the copy read back
    uint64_t *results;           // read back: a slot's samples (WebGPU: 0, or more)
    int32_t flag;                // 1 once read back, -1 if that failed
    uint32_t used;               // slots
    std::vector<OccQuery> queries;
    bool busy; // recording, or on the GPU
};
static OccFrame s_occFrames[4];
static uint32_t s_occAnswered, s_occVisible; // since the last report (&gpu=8)
static OccFrame *s_occFrame; // this frame's, from its first pass (none while all are busy)
static bool s_passOccludes;  // the open pass has the frame's set
static struct
{
    OcclusionAnswer *answer; // begun, not yet ended
    uint32_t first, count;   // its slots in this frame's set
    bool inPass;             // begun in the open pass
} s_occ;

static uint32_t OcclusionPassBegins()
{
    if (!s_occFrame)
        for (OccFrame &f : s_occFrames)
            if (!f.busy)
            {
                if (!f.set)
                {
                    f.set = bo1gpu_query_set(OCC_SLOTS);
                    f.resolve = bo1gpu_buffer(OCC_SLOTS * 8, BU_QUERY_RESOLVE | BU_COPY_SRC);
                    f.read = bo1gpu_buffer(OCC_SLOTS * 8, BU_MAP_READ | BU_COPY_DST);
                    f.results = (uint64_t *)malloc(OCC_SLOTS * 8);
                }
                f.busy = true;
                f.used = 0;
                f.queries.clear();
                s_occFrame = &f;
                break;
            }
    s_passOccludes = s_occFrame != nullptr;
    s_occ.inPass = false;
    return s_occFrame ? s_occFrame->set : 0;
}

static void OcclusionPassEnds()
{
    if (s_occ.inPass)
        bo1gpu_end_occlusion();
    s_occ.inPass = false;
    s_passOccludes = false;
}

// before one of the engine's draws: the open query, in this pass
static void OcclusionDraw()
{
    if (!s_occ.answer || s_occ.inPass || !s_passOccludes || s_occFrame->used == OCC_SLOTS)
        return;
    if (!s_occ.count)
        s_occ.first = s_occFrame->used;
    bo1gpu_begin_occlusion(s_occFrame->used++);
    PROF(other);
    ++s_occ.count;
    s_occ.inPass = true;
}

// A query ends: the frame answers it. One without a draw in a pass with the set keeps the answer it had.
static void OcclusionEnd(OcclusionAnswer *answer)
{
    if (s_occ.answer != answer)
        return;
    if (s_occ.inPass)
        bo1gpu_end_occlusion();
    if (s_occ.count)
        s_occFrame->queries.push_back({answer, s_occ.first, s_occ.count});
    s_occ = {};
}

static void OcclusionBegin(OcclusionAnswer *answer)
{
    if (s_occ.answer)
        OcclusionEnd(s_occ.answer);
    s_occ = {};
    s_occ.answer = answer;
}

// Present, before the submit: the frame's slots resolve into its buffer. A query still open goes on in the next frame.
static OccFrame *OcclusionFrameEnds()
{
    OccFrame *f = s_occFrame;
    s_occFrame = nullptr;
    if (!f)
        return nullptr;
    if (s_occ.count)
        f->queries.push_back({s_occ.answer, s_occ.first, s_occ.count});
    s_occ.count = 0;
    if (!f->used)
    {
        f->busy = false;
        return nullptr;
    }
    bo1gpu_resolve_queries(f->set, f->used, f->resolve, f->read);
    return f;
}

// after the submit: read back once the GPU is done
static void OcclusionReadBack(OccFrame *f)
{
    f->flag = 0;
    bo1gpu_map_read(f->read, f->used * 8, f->results, &f->flag);
}

static void FinishOcclusion()
{
    for (OccFrame &f : s_occFrames)
    {
        if (!f.busy || &f == s_occFrame || !__atomic_load_n(&f.flag, __ATOMIC_ACQUIRE))
            continue;
        if (f.flag > 0)
            for (const OccQuery &q : f.queries)
            {
                bool any = false;
                for (uint32_t i = q.first; i < q.first + q.count; ++i)
                    any |= f.results[i] != 0;
                __atomic_store_n(&q.answer->samples, any ? BO1_OCCLUSION_VISIBLE : 0u, __ATOMIC_RELEASE);
                __atomic_add_fetch(&q.answer->answers, 1, __ATOMIC_RELEASE);
                ++s_occAnswered;
                s_occVisible += any;
            }
        f.flag = 0;
        f.busy = false;
    }
}

// ============================================================================================================
// the GPU's time (&perf=1)
// ============================================================================================================

// What a frame took the GPU, for the frame rate overlay (ReportPerf). With &perf=1 the page asks for a device with
// timestamp queries (zombies-hub's web/public/player.js: gpuInit), and each pass of a frame has the GPU write its clock
// as it begins and as it ends, into two slots of a set which Present resolves into a buffer, read back once the GPU is
// done, as the occlusion queries' are. A frame's time is its passes' and the time between those of one submit, and it
// is told apart by pass: the scene's (the back buffer's size, with TIME_SCENE_DRAWS draws or more: the float-z pass,
// the lit one, the effects'), the full-screen ones (the back buffer's size with fewer: each post effect's, the HUD's,
// the canvas'), and those of other sizes (the shadow maps, the bloom's). The rest of the frame's time is between its
// passes: the copies of the scene, and whatever else the GPU did meanwhile, another process' drawing among it. Chrome
// rounds the clock to a tenth of a millisecond. The GPU clocks down when it has time to spare, as under a frame rate
// limit: the same frame then takes it longer.
static const uint32_t TIME_PASSES = 96;      // a frame's; passes past them go untimed
static const uint32_t TIME_SCENE_DRAWS = 32; // a pass with this many draws is the scene's, with fewer a full-screen one
enum
{
    TIME_SCENE,
    TIME_FULL,
    TIME_OTHER,
    TIME_KINDS
};
struct TimeFrame
{
    uint32_t read;     // the buffer the frame's slots are copied into, to read back
    uint64_t *results; // read back: each pass' beginning and end, in ns
    int32_t flag;      // 1 once read back, -1 if that failed
    uint32_t passes;
    uint8_t kind[TIME_PASSES];
    uint32_t submit[TIME_PASSES]; // s_serial as each began: the submit it went with
    bool busy; // recording, or on the GPU
};
static TimeFrame s_timeFrames[4];
static TimeFrame *s_timeFrame;          // this frame's, from its first pass (none while all are busy)
static uint32_t s_timeSet, s_timeResolve; // the set, and the buffer it resolves into
static int s_timeState;                 // 0 not looked for yet, 1 the device has timestamp queries, -1 it has none
static int s_timePass = -1;             // the open pass' place among the frame's timed ones (-1: not timed)
static uint32_t s_timePassDraws;        // s_frameDraws as it began
static bool s_timePassFull;             // it is the back buffer's size

// since the last report: frames timed, their time in ms, and each kind's passes and time
static struct
{
    uint32_t frames;
    double ms;
    uint32_t passes[TIME_KINDS];
    double kindMs[TIME_KINDS];
} s_gpuTimes;

static void TimePassBegins(const PassTargets &t, uint32_t *desc)
{
    s_timePass = -1;
    if (s_timeState < 0)
        return;
    if (!s_timeState)
    {
        s_timeSet = bo1gpu_time_set(TIME_PASSES * 2);
        s_timeState = s_timeSet ? 1 : -1;
        if (!s_timeSet)
            return;
        s_timeResolve = bo1gpu_buffer(TIME_PASSES * 16, BU_QUERY_RESOLVE | BU_COPY_SRC);
    }
    if (!s_timeFrame)
        for (TimeFrame &f : s_timeFrames)
            if (!f.busy)
            {
                if (!f.read)
                {
                    f.read = bo1gpu_buffer(TIME_PASSES * 16, BU_MAP_READ | BU_COPY_DST);
                    f.results = (uint64_t *)malloc(TIME_PASSES * 16);
                }
                f.busy = true;
                f.passes = 0;
                s_timeFrame = &f;
                break;
            }
    if (!s_timeFrame || s_timeFrame->passes == TIME_PASSES)
        return;
    s_timePass = (int)s_timeFrame->passes++;
    s_timeFrame->submit[s_timePass] = s_serial;
    s_timePassDraws = s_frameDraws;
    s_timePassFull = t.w == s_back.w && t.h == s_back.h;
    desc[0] = s_timeSet;
    desc[1] = (uint32_t)s_timePass * 2;
}

static void TimePassEnds()
{
    if (s_timePass < 0)
        return;
    s_timeFrame->kind[s_timePass] = !s_timePassFull ? TIME_OTHER : s_frameDraws - s_timePassDraws >= TIME_SCENE_DRAWS ? TIME_SCENE : TIME_FULL;
    s_timePass = -1;
}

// Present, before the submit: the frame's slots resolve into its buffer
static TimeFrame *TimeFrameEnds()
{
    TimeFrame *f = s_timeFrame;
    s_timeFrame = nullptr;
    if (!f)
        return nullptr;
    if (!f->passes)
    {
        f->busy = false;
        return nullptr;
    }
    bo1gpu_resolve_queries(s_timeSet, f->passes * 2, s_timeResolve, f->read);
    return f;
}

// after the submit: read back once the GPU is done
static void TimeReadBack(TimeFrame *f)
{
    f->flag = 0;
    bo1gpu_map_read(f->read, f->passes * 16, f->results, &f->flag);
}

// the frames read back, added to the report's; a clock that went backwards or stood at 0 (a device that has none after
// all) counts for nothing
static void FinishTimes()
{
    for (TimeFrame &f : s_timeFrames)
    {
        if (!f.busy || &f == s_timeFrame || !__atomic_load_n(&f.flag, __ATOMIC_ACQUIRE))
            continue;
        const uint64_t *r = f.results;
        uint64_t first = r[0], last = r[f.passes * 2 - 1];
        bool sound = f.flag > 0 && first && last >= first && last - first < 10000000000ull;
        for (uint32_t i = 0; i < f.passes && sound; ++i)
            sound = r[i * 2 + 1] >= r[i * 2] && r[i * 2] >= first && r[i * 2 + 1] <= last;
        if (sound)
        {
            ++s_gpuTimes.frames;
            for (uint32_t i = 0; i < f.passes; ++i)
            {
                ++s_gpuTimes.passes[f.kind[i]];
                s_gpuTimes.kindMs[f.kind[i]] += (r[i * 2 + 1] - r[i * 2]) / 1e6;
                // the frame's time: its passes, and the time between those that went to the GPU together. A frame
                // goes in two submits under a frame rate limit, a refresh apart (Replay: all but its PRESENT, then
                // that), and in more where a write to a buffer a draw reads comes between; the GPU is idle between.
                s_gpuTimes.ms += (r[i * 2 + 1] - r[i * 2]) / 1e6;
                if (i && f.submit[i] == f.submit[i - 1])
                    s_gpuTimes.ms += (r[i * 2] - r[i * 2 - 1]) / 1e6;
            }
        }
        f.flag = 0;
        f.busy = false;
    }
}

// ============================================================================================================
// the executor's own pipelines: clears of part of a target, blits
// ============================================================================================================

static uint32_t s_zeroBuffer; // (0, 0, 0, 1): vertex attributes the declaration lacks
static uint32_t s_dummy[3];   // views of black textures (2D, cube, volume) for samplers without a texture
static GpuFormat s_canvasFormat;

static uint32_t OwnShader(const std::string &code, const char *label) { return bo1gpu_shader(code.c_str(), label); }

static const char *kQuadVs = "struct VOut {\n"
                             "    @builtin(position) pos: vec4f,\n"
                             "    @location(0) value: vec4f,\n"
                             "    @location(1) uv: vec2f,\n"
                             "}\n"
                             // a triangle over the viewport; value: per instance (a clear's color, a blit's source rectangle)
                             "@vertex\nfn main(@builtin(vertex_index) i: u32, @location(0) value: vec4f) -> VOut {\n"
                             "    let t = vec2f(f32((i << 1u) & 2u), f32(i & 2u));\n"
                             "    var o: VOut;\n"
                             "    o.pos = vec4f(t.x * 2.0 - 1.0, 1.0 - t.y * 2.0, 0.0, 1.0);\n"
                             "    o.value = value;\n"
                             "    o.uv = mix(value.xy, value.zw, t);\n"
                             "    return o;\n"
                             "}\n";

static uint32_t s_quadVs;
static std::unordered_map<uint64_t, std::pair<uint32_t, int32_t *>> s_ownPipelines;
static std::unordered_map<uint32_t, uint32_t> s_ownModules;
static uint32_t s_blitLayout[2], s_blitPipelineLayout[2], s_emptyPipelineLayout;
static uint32_t s_firstSampleLayout, s_firstSamplePipelineLayout; // FirstSampleInto's: a multisampled texture
// the game's brightness (OP_GAMMA): its ramp as a 256x1 texture, which Present's blit looks each channel up in (a blit
// with a third binding), as a display applies the ramp to the frame; none while it is the identity (brightness 1)
static uint32_t s_gammaTex, s_gammaView, s_gammaLayout, s_gammaPipelineLayout;
static bool s_gammaOn;

static std::string Json(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

// a clear of part of the open pass' targets: the color of the vertex attribute, the depth of the viewport's depth range,
// the stencil value of the reference
static uint32_t ClearPipeline(const PassTargets &t, bool color, bool depth, bool stencil)
{
    uint32_t mask = 0;
    for (int i = 0; i < 4; ++i)
        if (t.colorView[i])
            mask |= 1u << i;
    uint64_t key = 1 | (uint64_t)mask << 4 | (uint64_t)color << 8 | (uint64_t)depth << 9 | (uint64_t)stencil << 10 | (uint64_t)(t.samples > 1) << 11 |
                   (uint64_t)t.depthFormat << 12;
    for (int i = 0; i < 4; ++i)
        key |= (uint64_t)t.colorFormat[i] << (20 + 8 * i);
    auto it = s_ownPipelines.find(key);
    if (it != s_ownPipelines.end())
        return it->second.first;
    uint32_t &fs = s_ownModules[mask];
    if (!fs)
    {
        std::string out = "struct FOut {\n", body;
        for (int i = 0; i < 4; ++i)
            if (mask >> i & 1)
            {
                out += "    @location(" + std::to_string(i) + ") c" + std::to_string(i) + ": vec4f,\n";
                body += "    o.c" + std::to_string(i) + " = value;\n";
            }
        std::string code = mask ? out + "}\n@fragment\nfn main(@location(0) value: vec4f) -> FOut {\n    var o: FOut;\n" + body + "    return o;\n}\n"
                                : "@fragment\nfn main() {\n}\n";
        fs = OwnShader(code, "clear");
    }
    std::string targets;
    int last = -1;
    for (int i = 0; i < 4; ++i)
        if (t.colorView[i])
            last = i;
    for (int i = 0; i <= last; ++i)
        targets += std::string(i ? "," : "") + (t.colorView[i] ? Json("[%d,%d,null]", t.colorFormat[i], color ? 15 : 0) : "null");
    std::string depthJson = "null";
    if (t.depthView)
    {
        depthJson = Json("{\"f\":%d,\"w\":%d,\"c\":7", t.depthFormat, depth ? 1 : 0);
        if (t.stencil)
            depthJson += stencil ? ",\"sf\":[7,2,2,2],\"sb\":[7,2,2,2],\"rm\":255,\"wm\":255" : ",\"sf\":[7,0,0,0],\"sb\":[7,0,0,0],\"rm\":255,\"wm\":0";
        depthJson += "}";
    }
    std::string json = Json("{\"label\":\"clear\",\"layout\":%u,\"vs\":%u,\"vc\":{},\"buffers\":[[16,1,[[3,0,0]]]],\"topology\":3,\"cull\":0,"
                            "\"ms\":%u,\"ps\":%u,\"pc\":{},\"targets\":[",
                           s_emptyPipelineLayout, s_quadVs, t.samples, fs) +
                       targets + "],\"depth\":" + depthJson + "}";
    int32_t *status = new int32_t(0);
    uint32_t p = bo1gpu_pipeline(json.c_str(), status, 0);
    s_ownPipelines[key] = {p, status};
    return p;
}

// a copy of a texture's level into the viewport, sampled (linear or nearest); gamma: each channel through the game's
// gamma ramp (s_gammaView), by its 8-bit value, as a display looks it up (a filterable source only)
static uint32_t BlitPipeline(GpuFormat target, uint32_t samples, bool filterable, bool gamma = false)
{
    uint64_t key = 2 | (uint64_t)target << 4 | (uint64_t)filterable << 12 | (uint64_t)gamma << 13 | (uint64_t)(samples > 1) << 14;
    auto it = s_ownPipelines.find(key);
    if (it != s_ownPipelines.end())
        return it->second.first;
    uint32_t &fs = s_ownModules[gamma ? 0x101 : 0x100];
    if (!fs && !gamma)
        fs = OwnShader("@group(0) @binding(0) var src: texture_2d<f32>;\n"
                    "@group(0) @binding(1) var smp: sampler;\n"
                    "@fragment\nfn main(@location(1) uv: vec2f) -> @location(0) vec4f {\n"
                    "    return textureSampleLevel(src, smp, uv, 0.0);\n"
                    "}\n",
            "blit");
    if (!fs && gamma)
        fs = OwnShader("@group(0) @binding(0) var src: texture_2d<f32>;\n"
                    "@group(0) @binding(1) var smp: sampler;\n"
                    "@group(0) @binding(2) var ramp: texture_2d<f32>;\n"
                    "@fragment\nfn main(@location(1) uv: vec2f) -> @location(0) vec4f {\n"
                    "    let c = textureSampleLevel(src, smp, uv, 0.0);\n"
                    "    let i = vec3i(round(clamp(c.rgb, vec3f(0.0), vec3f(1.0)) * 255.0));\n"
                    "    return vec4f(textureLoad(ramp, vec2i(i.r, 0), 0).r, textureLoad(ramp, vec2i(i.g, 0), 0).g,\n"
                    "                 textureLoad(ramp, vec2i(i.b, 0), 0).b, c.a);\n"
                    "}\n",
            "blit through the gamma ramp");
    std::string json = Json("{\"label\":\"blit\",\"layout\":%u,\"vs\":%u,\"vc\":{},\"buffers\":[[16,1,[[3,0,0]]]],\"topology\":3,\"cull\":0,"
                            "\"ms\":%u,\"ps\":%u,\"pc\":{},\"targets\":[[%d,15,null]],\"depth\":null}",
        gamma ? s_gammaPipelineLayout : s_blitPipelineLayout[filterable ? 0 : 1], s_quadVs, samples, fs, target);
    int32_t *status = new int32_t(0);
    uint32_t p = bo1gpu_pipeline(json.c_str(), status, 0);
    s_ownPipelines[key] = {p, status};
    return p;
}

static std::unordered_map<uint64_t, uint32_t> s_samplers;

static uint32_t BlitSampler(bool linear)
{
    uint64_t key = (uint64_t)1 << 63 | linear;
    uint32_t &s = s_samplers[key];
    if (!s)
        s = bo1gpu_sampler(2, 2, 2, linear, linear, 0, 0.0f, 32.0f, 0, 1);
    return s;
}

static bool Filterable(const TexFormat &tf) { return !tf.float32 || (s_caps & CAP_FLOAT32_FILTER); }

// draw level/layer `src` of a texture into the open pass, over the viewport: the source rectangle in texels; gamma:
// through the game's gamma ramp (Present's, while it is not the identity)
static void BlitInto(Tex &src, uint32_t level, uint32_t layer, const CmdRect &srcRect, GpuFormat target, bool linear,
                     bool gamma = false)
{
    bool filterable = Filterable(src.tf);
    gamma = gamma && filterable && s_gammaOn;
    uint32_t view = LevelView(src, level, layer);
    uint32_t entries[15] = {0, 1, view, 0, 0, 1, 2, BlitSampler(linear && filterable), 0, 0, 2, 1, s_gammaView, 0, 0};
    uint32_t group = bo1gpu_bind_group(gamma ? s_gammaLayout : s_blitLayout[filterable ? 0 : 1], entries, gamma ? 3 : 2);
    uint8_t *p;
    uint32_t off = ArenaAlloc(16, 16, &p);
    float sw = (float)Bo1_MipSize(src.w, level), sh = (float)Bo1_MipSize(src.h, level);
    float rect[4] = {srcRect.left / sw, srcRect.top / sh, srcRect.right / sw, srcRect.bottom / sh};
    memcpy(p, rect, 16);
    bo1gpu_set_pipeline(BlitPipeline(target, s_pass.samples, filterable, gamma));
    bo1gpu_set_bind_group(0, group, nullptr, 0);
    bo1gpu_set_vertex_buffer(0, s_arenaGpu, off, 16);
    bo1gpu_draw(3, 1, 0, 0);
    bo1gpu_release(group);
    src.used = s_serial;
    memset(&ps, 0, sizeof(ps));
    ps.viewport[2] = -1.0f;
    ps.scissor[2] = -1;
    ps.stencilRef = -1;
    ps.blend[0] = -1.0f;
}

// Each pixel's first sample of a multisampled texture, into the open pass, which is of its size: the resolve of a
// format WebGPU resolves none of, 32-bit floats. That is the float-z with anti-aliasing, drawn multisampled so that its
// pass leaves the scene's depth buffer as the scene's own depth prepass would (src/gfx_d3d/rb_draw3d.cpp:
// R_FloatZTargetBegin); its pixels are depths, which a mean would put between a surface and the one behind it at an
// edge, where one sample is a depth that is there.
static void FirstSampleInto(Tex &src, GpuFormat target)
{
    uint64_t key = 3 | (uint64_t)target << 4;
    auto it = s_ownPipelines.find(key);
    uint32_t pipeline;
    if (it != s_ownPipelines.end())
        pipeline = it->second.first;
    else
    {
        uint32_t &fs = s_ownModules[0x102];
        if (!fs)
            fs = OwnShader("@group(0) @binding(0) var src: texture_multisampled_2d<f32>;\n"
                        "@fragment\nfn main(@builtin(position) at: vec4f) -> @location(0) vec4f {\n"
                        "    return textureLoad(src, vec2i(at.xy), 0);\n"
                        "}\n",
                "first sample");
        std::string json = Json("{\"label\":\"first sample\",\"layout\":%u,\"vs\":%u,\"vc\":{},\"buffers\":[[16,1,[[3,0,0]]]],\"topology\":3,"
                                "\"cull\":0,\"ms\":1,\"ps\":%u,\"pc\":{},\"targets\":[[%d,15,null]],\"depth\":null}",
            s_firstSamplePipelineLayout, s_quadVs, fs, target);
        int32_t *status = new int32_t(0);
        pipeline = bo1gpu_pipeline(json.c_str(), status, 0);
        s_ownPipelines[key] = {pipeline, status};
    }
    uint32_t entries[5] = {0, 1, LevelView(src, 0, 0), 0, 0};
    uint32_t group = bo1gpu_bind_group(s_firstSampleLayout, entries, 1);
    uint8_t *p;
    uint32_t off = ArenaAlloc(16, 16, &p);
    const float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    memcpy(p, rect, 16);
    bo1gpu_set_pipeline(pipeline);
    bo1gpu_set_bind_group(0, group, nullptr, 0);
    bo1gpu_set_vertex_buffer(0, s_arenaGpu, off, 16);
    bo1gpu_draw(3, 1, 0, 0);
    bo1gpu_release(group);
    src.used = s_serial;
    memset(&ps, 0, sizeof(ps));
    ps.viewport[2] = -1.0f;
    ps.scissor[2] = -1;
    ps.stencilRef = -1;
    ps.blend[0] = -1.0f;
}

// ============================================================================================================
// samplers
// ============================================================================================================

static int Address(DWORD a)
{
    switch (a)
    {
    case D3DTADDRESS_MIRROR:
        return 1;
    case D3DTADDRESS_CLAMP:
    case D3DTADDRESS_BORDER:
    case D3DTADDRESS_MIRRORONCE:
        return 2;
    default:
        return 0;
    }
}

// kind: 0 filtering, 1 nearest only (an unfilterable texture), 2 depth comparisons
static uint32_t SamplerFor(int stage, bool mipmapped, int kind)
{
    const DWORD *ss = st.ss[stage];
    DWORD minf = ss[D3DSAMP_MINFILTER], magf = ss[D3DSAMP_MAGFILTER], mipf = mipmapped ? ss[D3DSAMP_MIPFILTER] : D3DTEXF_NONE;
    DWORD aniso = (minf == D3DTEXF_ANISOTROPIC || magf == D3DTEXF_ANISOTROPIC) ? ss[D3DSAMP_MAXANISOTROPY] : 1;
    aniso = std::max<DWORD>(1, std::min<DWORD>(16, aniso));
    if (kind == 1)
    {
        minf = magf = D3DTEXF_POINT;
        mipf = mipf == D3DTEXF_NONE ? D3DTEXF_NONE : D3DTEXF_POINT;
        aniso = 1;
    }
    uint32_t maxLevel = ss[D3DSAMP_MAXMIPLEVEL] & 0xF;
    uint64_t key = (uint64_t)(ss[D3DSAMP_ADDRESSU] & 7) | (uint64_t)(ss[D3DSAMP_ADDRESSV] & 7) << 3 | (uint64_t)(ss[D3DSAMP_ADDRESSW] & 7) << 6 |
                   (uint64_t)(minf & 7) << 9 | (uint64_t)(magf & 7) << 12 | (uint64_t)(mipf & 7) << 15 | (uint64_t)aniso << 18 |
                   (uint64_t)maxLevel << 23 | (uint64_t)kind << 27;
    uint32_t &s = s_samplers[key];
    if (s)
        return s;
    bool minLinear = minf != D3DTEXF_POINT, magLinear = magf != D3DTEXF_POINT, mipLinear = mipf == D3DTEXF_LINEAR;
    if (aniso > 1)
        minLinear = magLinear = mipLinear = true; // WebGPU filters anisotropically only with every filter linear
    // no mip filter: the most detailed level allowed only
    float lodMin = (float)maxLevel, lodMax = mipf == D3DTEXF_NONE ? (float)maxLevel : 32.0f;
    s = bo1gpu_sampler(Address(ss[D3DSAMP_ADDRESSU]), Address(ss[D3DSAMP_ADDRESSV]), Address(ss[D3DSAMP_ADDRESSW]), magLinear, minLinear,
        mipLinear, lodMin, lodMax, kind == 2 ? 4 : 0, aniso); // compare: less-equal (lit where the reference is not beyond)
    return s;
}

// a draw's sampler as the last draw that read it had it, by what else it was for (draws keep it: PrepareDraw); forgotten
// as its states change
static struct
{
    bool known, mipmapped;
    int8_t kind;
    uint32_t handle;
} s_samplerKept[MAX_SAMPLERS];

static uint32_t SamplerKept(int stage, bool mipmapped, int kind)
{
    auto &k = s_samplerKept[stage];
    if (!k.known || k.mipmapped != mipmapped || k.kind != kind)
        k = {true, mipmapped, (int8_t)kind, SamplerFor(stage, mipmapped, kind)};
    return k.handle;
}

// ============================================================================================================
// pipelines
// ============================================================================================================

static VertexFormat VertexFormatOf(BYTE type)
{
    switch (type)
    {
    case D3DDECLTYPE_FLOAT1:
        return VF_FLOAT32;
    case D3DDECLTYPE_FLOAT2:
        return VF_FLOAT32X2;
    case D3DDECLTYPE_FLOAT3:
        return VF_FLOAT32X3;
    case D3DDECLTYPE_FLOAT4:
        return VF_FLOAT32X4;
    case D3DDECLTYPE_D3DCOLOR:
    case D3DDECLTYPE_UBYTE4:
    case D3DDECLTYPE_UBYTE4N:
        return VF_UNORM8X4;
    case D3DDECLTYPE_SHORT2N:
        return VF_SNORM16X2;
    case D3DDECLTYPE_SHORT4N:
        return VF_SNORM16X4;
    case D3DDECLTYPE_USHORT2N:
        return VF_UNORM16X2;
    case D3DDECLTYPE_USHORT4N:
        return VF_UNORM16X4;
    case D3DDECLTYPE_FLOAT16_2:
        return VF_FLOAT16X2;
    case D3DDECLTYPE_FLOAT16_4:
        return VF_FLOAT16X4;
    default:
        return VF_NONE;
    }
}

// where a draw's vertex attributes come from
struct VertexLayout
{
    uint8_t slots;
    int8_t stream[9]; // the D3D stream of each vertex buffer (-1: the zero buffer)
    uint16_t stride[9];
    uint8_t instance[9];
    uint8_t attrs;
    struct
    {
        uint8_t format, slot, location, pad;
        uint16_t offset;
    } attr[16];
    uint32_t colorMask, ubyteMask; // D3DCOLOR and UBYTE4 attributes (the shader's bo1_inColor, bo1_inUbyte)
    bool operator==(const VertexLayout &o) const { return !memcmp(this, &o, sizeof(VertexLayout)); }
};

struct BytesHash
{
    template <class T> size_t operator()(const T &k) const
    {
        const uint8_t *p = (const uint8_t *)&k;
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < sizeof(T); ++i)
            h = (h ^ p[i]) * 0x100000001B3ull;
        return (size_t)(h ^ (h >> 32));
    }
};

static std::unordered_map<VertexLayout, uint32_t, BytesHash> s_layoutIds;
static std::vector<VertexLayout> s_layouts;

static const D3DVERTEXELEMENT9 *FindElement(const Decl *decl, int usage, int index)
{
    if (!decl)
        return nullptr;
    for (const D3DVERTEXELEMENT9 &e : decl->el)
    {
        if (e.Stream == 0xFF)
            break;
        if ((int)e.Usage == usage && (int)e.UsageIndex == index)
            return &e;
    }
    return nullptr;
}

static uint32_t InternLayout(const VertexLayout &l)
{
    auto it = s_layoutIds.find(l);
    if (it != s_layoutIds.end())
        return it->second;
    uint32_t id = (uint32_t)s_layouts.size();
    s_layouts.push_back(l);
    s_layoutIds[l] = id;
    return id;
}

// the vertex layout of the vertex shader's inputs from the declaration and streams (upStride: DrawPrimitiveUP's stream 0)
static uint32_t LayoutFor(const ShaderInfo &vs, const Decl *decl, uint32_t upStride)
{
    VertexLayout l;
    memset(&l, 0, sizeof(l));
    int zero = -1;
    for (size_t k = 0; k < vs.inputs.size() && k < 16; ++k)
    {
        const D3DVERTEXELEMENT9 *e = FindElement(decl, vs.inputs[k].usage, vs.inputs[k].index);
        VertexFormat f = e ? VertexFormatOf(e->Type) : VF_NONE;
        if (e && f == VF_NONE)
            BO1_GPU_ERROR("vertex declaration type %u not supported", e->Type);
        int slot;
        bool zeroed = f == VF_NONE;
        if (zeroed)
        {
            if (zero < 0)
            {
                zero = l.slots++;
                l.stream[zero] = -1;
            }
            slot = zero;
            f = VF_FLOAT32X4;
        }
        else
        {
            slot = -1;
            for (int s = 0; s < l.slots; ++s)
                if (l.stream[s] == e->Stream)
                    slot = s;
            if (slot < 0)
            {
                slot = l.slots++;
                l.stream[slot] = (int8_t)e->Stream;
                l.stride[slot] = (uint16_t)(upStride && e->Stream == 0 ? upStride : st.stream[e->Stream & 15].stride);
                l.instance[slot] = (st.freq[e->Stream & 15] & D3DSTREAMSOURCE_INSTANCEDATA) != 0;
            }
            if (e->Type == D3DDECLTYPE_D3DCOLOR)
                l.colorMask |= 1u << k;
            if (e->Type == D3DDECLTYPE_UBYTE4)
                l.ubyteMask |= 1u << k;
        }
        auto &a = l.attr[l.attrs++];
        a.format = f;
        a.slot = (uint8_t)slot;
        a.location = (uint8_t)k;
        a.offset = (uint16_t)(zeroed ? 0 : e->Offset);
    }
    return InternLayout(l);
}

struct PipeKey
{
    uint32_t vsModule, psModule; // Module::id
    uint32_t layout, vertexLayout;
    uint32_t vsOverrides, psOverrides; // which override constants the modules have
    uint32_t alphaFunc, srgbWrite, clampMask;
    uint32_t samples; // a pixel's, as the targets have them
    uint8_t topology, strip, cull, depthFormat;
    uint8_t colorFormat[4];
    uint8_t writeMask[4];
    uint32_t blend; // 0: off
    uint32_t depth; // enabled, write, compare
    uint32_t stencilFront, stencilBack; // compare, fail, depth fail, pass
    uint32_t stencilMasks;              // on, read, write
    int32_t bias;
    float slope;
    bool operator==(const PipeKey &o) const { return !memcmp(this, &o, sizeof(PipeKey)); }
};

static const int MAX_DRAW_SAMPLERS = 20; // the vertex shader's 4, the pixel shader's 16

struct Pipe
{
    uint32_t handle = 0;
    int32_t status = 0;     // library_gpu.js: 0 compiling, 1 ready, -1 failed
    float compileMs = 0.0f; // how long it compiled (library_gpu.js writes it, after the status)
    int8_t tier = -1;       // made from the list before a draw needed it: 0 the spawn's, 1 a stand-in, 2 the rest (-1: for a draw)
    bool recorded = false;  // a draw used it: its recipe is among s_usedRecipes
    bool waitStand = false; // while draws waited for it, some were drawn with a stand-in
    bool standIn = false;   // a stand-in pipeline (stand-ins): never recorded
    bool standTried = false; // its own stand-in was looked for (stand: none could be made)
    uint8_t hintFlags = 0;  // its recipe's HINT_FLAGS: whether its pass reads the scene's colour copy or the depth copy
    uint8_t leftOutWhy = 0; // why the last draw that waited for it was left out (StandInFor), for the log: 1 it has no stand-in,
                            // 2 its stand-in was compiling too, 3 its stand-in failed, 4 its stand-in didn't fit the draw's textures
    uint64_t vsVariant = 0, psVariant = 0; // its modules' variants (ModuleFor), to start it
    double compileStart = 0.0; // when it started compiling: it was ready compileMs after
    double waitSince = 0.0;    // when a draw first waited for it, until a draw finds it ready (0: none waits)
    char waitName[96] = "";    // the first draw that waited: its material, technique and kind (DrawName), for the log
    // what it was made of, for its stand-in (StandInFor, StandInBind) and the draws drawn with it
    PipeKey key;
    Program *vs = nullptr, *ps = nullptr;
    uint32_t groupLayout = 0;
    Pipe *stand = nullptr; // the stand-in draws use while it compiles (standTried)
};
static_assert(offsetof(Pipe, compileMs) == offsetof(Pipe, status) + 4, "library_gpu.js writes the compile time after the status");

// ----- recipes -----
//
// What a draw's pipeline is made from, in Direct3D's terms, so that it holds on any GPU: the shaders' bytecode, the
// vertex layout, the formats of the targets and of the textures read, and the render states pipelines take. Each
// pipeline's recipe is recorded the first time a draw uses it (Bo1_PipelineRecipes), and zombies-hub's
// docker/pipelines.sh plays each map to make the list of it the server serves (web/pipelines/<map>.bin): a header
// (RECIPE_MAGIC, RECIPE_VERSION, sizeof(Recipe)), then the records, in the order the game first drew with them. While a
// map loads, the list's recipes for the shaders it has make their pipelines (preloading): tier 0's and every recipe's
// stand-in, which the player waits for before going in (Bo1_PreloadPending), and the zone's passes' stand-ins
// (EnumPass) right after going in. The rest of the list's pipelines are made as draws first need them, as any the list
// lacks. Plain data, the same in every build of one RECIPE_VERSION.
// version 2: Recipe::hints of 8; version 3: of 16 (the model lighting, the HDR scale), which a version 2 list is read as
static const uint32_t RECIPE_VERSION = 3, RECIPE_MAGIC = 0x50314F42; // "BO1P"
static const uint32_t RECIPE_V2_SIZE = 408, RECIPE_V2_HINTS = 8;
// Recipe::tier in the file (zombies-hub's docker/player/pipelines.mjs)
static const uint8_t TIER_SHADOWS = 4, TIER_NO_SHADOWS = 5;

// the render states a pipeline takes
static const D3DRENDERSTATETYPE kPipeStates[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAFUNC,
    D3DRS_SRGBWRITEENABLE, D3DRS_CULLMODE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
    D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA, D3DRS_COLORWRITEENABLE,
    D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_STENCILENABLE, D3DRS_STENCILFUNC,
    D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL, D3DRS_STENCILPASS, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_CCW_STENCILFUNC,
    D3DRS_CCW_STENCILFAIL, D3DRS_CCW_STENCILZFAIL, D3DRS_CCW_STENCILPASS, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK,
    D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS};
static const int PIPE_STATES = sizeof(kPipeStates) / sizeof(kPipeStates[0]);

struct Recipe
{
    uint64_t vs, ps;                    // Program::hash
    uint32_t color[4], depth;           // the targets' D3D formats (0: none)
    uint32_t sampler[MAX_DRAW_SAMPLERS]; // the texture each of the shaders' samplers reads (the vertex shader's first): its
                                        // D3D format, 0: none
    uint32_t depthTarget;               // samplers that read the depth buffer drawn to, a bit each
    uint32_t rs[PIPE_STATES];           // kPipeStates
    VertexLayout layout;
    uint8_t topology, strip;
    uint8_t tier; // the list's: 0 what the player sees on spawning, compiled before going in; 2 the rest, of which the
                  // stand-ins alone are made ahead (1 in lists made before the stand-ins became tier 1, read as 2). In
                  // the file the spawn's are 0 where the spawn draws with them with shadow maps on and off, 4 with them
                  // on alone and 5 with them off alone (TIER_SHADOWS, TIER_NO_SHADOWS; lists made before say 0 for
                  // all, drawn with them on), and TakeRecipes makes those of the player's setting 0 and the others 2.
                  // The zone's passes' (EnumPass): 3. A draw's: 0
    // 1: drawn to multisampled targets, the back buffer's (anti-aliasing), whose pipelines have its samples: a list made
    // with anti-aliasing serves players without, and lists made before it have 0 here
    uint8_t multisampled;
    // the engine's word on the pass (CmdPassHints): which samplers are the colour map, the lightmaps and the model
    // lighting, which constants the primary light and the scales, whether it reads the scene; the same for every draw of
    // a pass, so a recipe is one pipeline still. What its stand-in (below) draws with, made from the list before a draw
    // needs it
    uint8_t hints[HINT_COUNT];
};
static_assert(sizeof(Recipe) % 8 == 0, "records follow each other");
static_assert(sizeof(Recipe) == 416, "zombies-hub's docker/player/pipelines.mjs reads the records (RECIPE, TIER)");
static_assert(offsetof(Recipe, tier) == 398, "zombies-hub's docker/player/pipelines.mjs: TIER");
static_assert(offsetof(Recipe, hints) == RECIPE_V2_SIZE - RECIPE_V2_HINTS, "a version 2 record is this one's first 408 bytes");

static std::unordered_map<PipeKey, Pipe *, BytesHash> s_pipelines;
static std::vector<Pipe *> s_orphans; // of destroyed shaders, still compiling

static int Compare(DWORD f)
{
    // library_gpu.js: never less equal less-equal greater not-equal greater-equal always (D3DCMP_ - 1)
    return f >= D3DCMP_NEVER && f <= D3DCMP_ALWAYS ? (int)f - 1 : 7;
}

static int StencilOp(DWORD op)
{
    switch (op)
    {
    case D3DSTENCILOP_ZERO:
        return 1;
    case D3DSTENCILOP_REPLACE:
        return 2;
    case D3DSTENCILOP_INVERT:
        return 3;
    case D3DSTENCILOP_INCRSAT:
        return 4;
    case D3DSTENCILOP_DECRSAT:
        return 5;
    case D3DSTENCILOP_INCR:
        return 6;
    case D3DSTENCILOP_DECR:
        return 7;
    default:
        return 0;
    }
}

// library_gpu.js: zero one src one-minus-src src-alpha one-minus-src-alpha dst-alpha one-minus-dst-alpha dst one-minus-dst
// src-alpha-saturated constant one-minus-constant
static int BlendFactor(DWORD b)
{
    switch (b)
    {
    case D3DBLEND_ZERO:
        return 0;
    case D3DBLEND_SRCCOLOR:
        return 2;
    case D3DBLEND_INVSRCCOLOR:
        return 3;
    case D3DBLEND_SRCALPHA:
        return 4;
    case D3DBLEND_INVSRCALPHA:
        return 5;
    case D3DBLEND_DESTALPHA:
        return 6;
    case D3DBLEND_INVDESTALPHA:
        return 7;
    case D3DBLEND_DESTCOLOR:
        return 8;
    case D3DBLEND_INVDESTCOLOR:
        return 9;
    case D3DBLEND_SRCALPHASAT:
        return 10;
    case D3DBLEND_BLENDFACTOR:
        return 11;
    case D3DBLEND_INVBLENDFACTOR:
        return 12;
    default:
        return 1;
    }
}

static int BlendOp(DWORD op)
{
    switch (op)
    {
    case D3DBLENDOP_SUBTRACT:
        return 1;
    case D3DBLENDOP_REVSUBTRACT:
        return 2;
    case D3DBLENDOP_MIN:
        return 3;
    case D3DBLENDOP_MAX:
        return 4;
    default:
        return 0;
    }
}

static float AsFloat(DWORD v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static bool BlendFactorUsesConstant(int f) { return f == 11 || f == 12; }

// the blend state, packed: enabled, then color src, dst, op and alpha src, dst, op (5 bits each)
static uint32_t BlendState(const DWORD *rs)
{
    if (!rs[D3DRS_ALPHABLENDENABLE])
        return 0;
    DWORD src = rs[D3DRS_SRCBLEND], dst = rs[D3DRS_DESTBLEND];
    if (src == D3DBLEND_BOTHSRCALPHA)
    {
        src = D3DBLEND_SRCALPHA;
        dst = D3DBLEND_INVSRCALPHA;
    }
    else if (src == D3DBLEND_BOTHINVSRCALPHA)
    {
        src = D3DBLEND_INVSRCALPHA;
        dst = D3DBLEND_SRCALPHA;
    }
    DWORD srcA = src, dstA = dst, opA = rs[D3DRS_BLENDOP];
    if (rs[D3DRS_SEPARATEALPHABLENDENABLE])
    {
        srcA = rs[D3DRS_SRCBLENDALPHA];
        dstA = rs[D3DRS_DESTBLENDALPHA];
        opA = rs[D3DRS_BLENDOPALPHA];
    }
    // the alpha channel's factors take alpha (src color is src alpha there)
    auto alpha = [](int f) { return f == 2 ? 4 : f == 3 ? 5 : f == 8 ? 6 : f == 9 ? 7 : f; };
    return 1u | (uint32_t)BlendFactor(src) << 1 | (uint32_t)BlendFactor(dst) << 6 | (uint32_t)BlendOp(rs[D3DRS_BLENDOP]) << 11 |
           (uint32_t)alpha(BlendFactor(srcA)) << 16 | (uint32_t)alpha(BlendFactor(dstA)) << 21 | (uint32_t)BlendOp(opA) << 26;
}

static std::string PipelineJson(const PipeKey &k, uint32_t vsId, uint32_t psId, uint32_t vsModule, uint32_t psModule)
{
    const VertexLayout &l = s_layouts[k.vertexLayout];
    std::string j = Json("{\"label\":\"vs%u ps%u\",\"layout\":%u,\"vs\":%u,\"vc\":{", vsId, psId, k.layout, vsModule);
    if (k.vsOverrides & OVERRIDE_INPUTS)
        j += Json("\"bo1_inColor\":%u,\"bo1_inUbyte\":%u", l.colorMask, l.ubyteMask);
    j += "},\"buffers\":[";
    for (int s = 0; s < l.slots; ++s)
    {
        j += Json("%s[%u,%u,[", s ? "," : "", l.stream[s] < 0 ? 0 : l.stride[s], l.instance[s]);
        bool first = true;
        for (int a = 0; a < l.attrs; ++a)
            if (l.attr[a].slot == s)
            {
                j += Json("%s[%u,%u,%u]", first ? "" : ",", l.attr[a].format, l.attr[a].offset, l.attr[a].location);
                first = false;
            }
        j += "]]";
    }
    j += Json("],\"topology\":%u,\"strip\":%u,\"cull\":%u,\"ms\":%u,\"ps\":%u,\"pc\":{", k.topology, k.strip, k.cull, k.samples,
        psModule);
    bool comma = false;
    if (k.psOverrides & OVERRIDE_CLAMP)
    {
        j += Json("\"bo1_clamp\":%u", k.clampMask);
        comma = true;
    }
    if (k.psOverrides & OVERRIDE_ALPHA)
        j += Json("%s\"bo1_alphaFunc\":%u,\"bo1_srgbWrite\":%u", comma ? "," : "", k.alphaFunc, k.srgbWrite);
    j += "},\"targets\":[";
    int last = -1;
    for (int i = 0; i < 4; ++i)
        if (k.colorFormat[i])
            last = i;
    for (int i = 0; i <= last; ++i)
    {
        if (i)
            j += ",";
        if (!k.colorFormat[i])
        {
            j += "null";
            continue;
        }
        std::string blend = "null";
        if (k.blend)
            blend = Json("[%u,%u,%u,%u,%u,%u]", k.blend >> 1 & 31, k.blend >> 6 & 31, k.blend >> 11 & 31, k.blend >> 16 & 31, k.blend >> 21 & 31,
                k.blend >> 26 & 31);
        j += Json("[%u,%u,%s]", k.colorFormat[i], k.writeMask[i], blend.c_str());
    }
    j += "],\"depth\":";
    if (k.depthFormat)
    {
        j += Json("{\"f\":%u,\"w\":%u,\"c\":%u,\"bias\":%d,\"slope\":%.9g", k.depthFormat, k.depth >> 1 & 1, k.depth >> 2 & 7, k.bias,
            isfinite(k.slope) ? k.slope : 0.0f);
        if (k.stencilMasks & 1)
            j += Json(",\"sf\":[%u,%u,%u,%u],\"sb\":[%u,%u,%u,%u],\"rm\":%u,\"wm\":%u", k.stencilFront & 7, k.stencilFront >> 3 & 7,
                k.stencilFront >> 6 & 7, k.stencilFront >> 9 & 7, k.stencilBack & 7, k.stencilBack >> 3 & 7, k.stencilBack >> 6 & 7,
                k.stencilBack >> 9 & 7, k.stencilMasks >> 8 & 0xFF, k.stencilMasks >> 16 & 0xFF);
        j += "}";
    }
    else
        j += "null";
    return j + "}";
}

// ----- modules and layouts -----

static uint32_t s_nextModuleId = 1;
static std::vector<std::pair<Program *, uint64_t>> s_liveModules; // the variants whose modules exist
static std::string StandInCode(const StandInRouting &rt, uint32_t targetMask, uint32_t *overrides); // stand-ins

// byInstance: a vertex shader's, that reads its constants where the draw's first instance says (s_byInstance)
static uint64_t Variant(uint32_t shadowMask, uint32_t extraVaryings, uint32_t targetMask, bool byInstance = false)
{
    return (uint64_t)shadowMask | (uint64_t)extraVaryings << 16 | (uint64_t)targetMask << 32 | (uint64_t)byInstance << 48;
}

// the variant's module, made if it was released
static uint32_t ModuleHandle(Program &s, uint64_t variant, Module &m)
{
    m.used = emscripten_get_now();
    if (m.handle || m.failed)
        return m.handle;
    double start = m.used;
    uint32_t ov = 0;
    uint32_t targetMask = (uint32_t)(variant >> 32 & 0xFFFF);
    std::string code = s.standIn ? StandInCode(*s.standIn, targetMask, &ov)
                                 : ShaderEmit(s.info, (uint32_t)(variant & 0xFFFF), (uint32_t)(variant >> 16 & 0xFFFF), targetMask,
                                       (variant >> 48 & 1) != 0, &ov);
    if (code.empty())
    {
        BO1_GPU_ERROR("shader %u (%s): %s", s.id, s.info.pixel ? "ps" : "vs", s.info.error.c_str());
        m.failed = true;
    }
    else
    {
        char label[32];
        snprintf(label, sizeof(label), "%s%u", s.info.pixel ? "ps" : "vs", s.id);
        m.handle = bo1gpu_shader(code.c_str(), label);
        m.overrides = ov;
        s_liveModules.push_back({&s, variant});
    }
    g_bo1CompileUs += (uint64_t)((emscripten_get_now() - start) * 1000.0);
    return m.handle;
}

// a variant of a shader (its module made the first time, for the pipeline that needs it)
static Module &ModuleFor(Program &s, uint32_t shadowMask, uint32_t extraVaryings, uint32_t targetMask, bool byInstance = false)
{
    uint64_t variant = Variant(shadowMask, extraVaryings, targetMask, byInstance);
    auto it = s.modules.find(variant);
    if (it != s.modules.end())
        return it->second;
    Module &m = s.modules[variant];
    m.id = s_nextModuleId++;
    ModuleHandle(s, variant, m);
    return m;
}

// modules no pipeline has needed for a while go
static void ReleaseIdleModules(double now)
{
    for (size_t i = 0; i < s_liveModules.size();)
    {
        Module &m = s_liveModules[i].first->modules[s_liveModules[i].second];
        if (now - m.used > 5000.0)
        {
            bo1gpu_release(m.handle);
            m.handle = 0;
            s_liveModules[i] = s_liveModules.back();
            s_liveModules.pop_back();
        }
        else
            ++i;
    }
}

struct GroupLayouts
{
    uint32_t group;       // group 1's layout (0: none)
    uint32_t pipeline[2]; // the pipeline layout with it and group 0's, [s_byInstance], made as a pipeline first needs it
};
static std::unordered_map<std::string, GroupLayouts> s_groupLayouts; // by group 1's entries

// group 1's layout for the samplers of a vertex and a pixel shader: (binding, visibility, kind, a, b) each
static void TextureLayout(const std::vector<uint32_t> &entries, uint32_t *groupLayout, uint32_t *pipelineLayout)
{
    std::string key((const char *)entries.data(), entries.size() * 4);
    auto it = s_groupLayouts.find(key);
    if (it == s_groupLayouts.end())
    {
        uint32_t g = entries.empty() ? 0 : bo1gpu_bind_group_layout(entries.data(), (int)entries.size() / 5);
        it = s_groupLayouts.emplace(key, GroupLayouts{g, {0, 0}}).first;
    }
    GroupLayouts &gl = it->second;
    if (!gl.pipeline[s_byInstance])
    {
        uint32_t layouts[2] = {s_arenaLayout, gl.group};
        gl.pipeline[s_byInstance] = bo1gpu_pipeline_layout(layouts, gl.group ? 2 : 1);
    }
    *groupLayout = gl.group;
    *pipelineLayout = gl.pipeline[s_byInstance];
}

// ----- texture bind groups -----

struct GroupKey
{
    uint32_t layout;
    uint32_t n;
    uint32_t items[40]; // view, sampler, ...
    bool operator==(const GroupKey &o) const { return layout == o.layout && n == o.n && !memcmp(items, o.items, n * 4); }
};

struct GroupKeyHash
{
    size_t operator()(const GroupKey &k) const
    {
        uint64_t h = k.layout * 0x9E3779B97F4A7C15ull;
        for (uint32_t i = 0; i < k.n; ++i)
            h = (h ^ k.items[i]) * 0x100000001B3ull;
        return (size_t)(h ^ (h >> 29));
    }
};

struct Group
{
    uint32_t handle;
    uint64_t id;
};

static std::unordered_map<GroupKey, Group, GroupKeyHash> s_groups;
static std::unordered_map<uint64_t, GroupKey> s_groupsById;
static uint64_t s_nextGroupId = 1;

static void ForgetBindGroups(Tex &t)
{
    for (uint64_t id : t.bindGroups)
    {
        auto k = s_groupsById.find(id);
        if (k == s_groupsById.end())
            continue;
        auto g = s_groups.find(k->second);
        if (g != s_groups.end())
        {
            if (ps.group1 == g->second.handle)
                ps.group1 = 0;
            bo1gpu_release(g->second.handle);
            s_groups.erase(g);
        }
        s_groupsById.erase(k);
    }
    t.bindGroups.clear();
}

// ============================================================================================================
// drawing
// ============================================================================================================

static const ShaderInfo *s_lastVs, *s_lastPs;
static uint32_t s_vsUpload, s_psUpload; // arena offsets of the last uniform blocks
static uint32_t s_vsBlock = 16;         // the size of the last vertex shader's (FirstInstance)
static uint32_t s_vsUploadBytes, s_psUploadBytes;
static uint32_t s_vsUploadSerial, s_psUploadSerial, s_vsUploadGen, s_psUploadGen;
static float s_vsUploadFix[2], s_psUploadRef;

// A stage's uniform block in the arena (UNIFORM_BINDING bytes are readable from its offset), on `align` bytes: 256 for
// one bound at its offset, its own size for one a draw's first instance counts to (s_byInstance). The block before
// (`prev`, in this submit's arena, `*bytes` long; ~0u: none) if it starts with the same, and is on `align` bytes: the
// engine sets constants again with the values they have, and sets ones the shader doesn't read, and binding a new
// block is a call to WebGPU. *bytes: the returned block's.
static uint32_t UploadConstants(const ShaderInfo &info, const float *f, const int *ints, const int *bools, const float fix[4], uint32_t align,
    uint32_t prev, uint32_t *bytes)
{
    uint32_t before = s_arenaUsed, size = info.UniformBytes();
    uint8_t *p;
    uint32_t off = ArenaAlloc(size, align, &p);
    memcpy(p, fix, 16);
    uint32_t mask = 0;
    for (int b = 0; b < 16; ++b)
        if (bools[b])
            mask |= 1u << b;
    uint32_t bm[4] = {mask, 0, 0, 0};
    memcpy(p + 16, bm, 16);
    uint8_t *c = p + 32;
    for (const ConstRun &r : info.floatRuns)
        memcpy(c + r.slot * 16, f + r.reg * 4, r.count * 16);
    for (const DefSlot &d : info.defSlots)
        memcpy(c + d.slot * 16, d.value, 16);
    memcpy(c + info.floatSlots * 16, ints, info.intCount * 16);
    if (prev != ~0u && size <= *bytes && prev % align == 0 && !memcmp(s_arena + prev, p, size))
    {
        s_arenaUsed = before;
        return prev;
    }
    if (s_prof.on)
        s_prof.constantBytes += size;
    *bytes = size;
    return off;
}

static Tex *BoundTexture(int stage)
{
    auto it = s_textures.find(st.tex[stage]);
    return it != s_textures.end() && it->second.tex ? &it->second : nullptr;
}

// A copy of a texture the draw's pass draws to, for the draw to read: WebGPU reads a texture in a pass only if the pass
// does not write it. What such a read gets in D3D9 is undefined; here it is what was drawn before the draw.
static Tex *Snapshot(Tex &t)
{
    if (!t.copy)
    {
        t.copy = new Tex;
        Tex &c = *t.copy;
        c.type = t.type;
        c.w = t.w;
        c.h = t.h;
        c.d = t.d;
        c.levels = t.levels;
        c.fmt = t.fmt;
        c.tf = t.tf;
        MakeTexture(c, 0);
    }
    EndPass();
    for (uint32_t layer = 0; layer < (t.type == TEX_CUBE ? 6u : 1u); ++layer)
        for (uint32_t level = 0; level < t.levels; ++level)
            bo1gpu_copy_texture(t.tex, level, layer, 0, 0, t.copy->tex, level, layer, 0, 0, Bo1_MipSize(t.w, level), Bo1_MipSize(t.h, level));
    t.used = t.copy->used = s_serial;
    return t.copy;
}

// the GPU format of a texture of a D3D format, as far as pipelines go (MakeTexture: an unsupported one is A8R8G8B8)
static TexFormat KeyFormat(uint32_t fmt)
{
    TexFormat tf = Format(fmt);
    return tf.valid ? tf : Format(D3DFMT_A8R8G8B8);
}

// what a pipeline made from a recipe is, for the draw that needs it
struct Derived
{
    Pipe *pipe;
    uint32_t groupLayout, vertexLayout;
    int8_t kind[MAX_DRAW_SAMPLERS]; // each sampler's: -1 no texture (a black stand-in), 0 filtering, 1 unfilterable, 2 depth
                                    // comparisons
    uint32_t blend, stencilMasks;   // PipeKey's
};

// the recipes of the pipelines draws used, in the order they first did, after the list's header: for
// zombies-hub's docker/pipelines.sh, which reads them from the page (Bo1_PipelineRecipes)
static std::vector<uint8_t> s_usedRecipes;
static std::unordered_set<std::string> s_listed; // the list's tier 2 (Recipe bytes), for the log of waits and Bo1_PipelinesLacked

extern "C" EMSCRIPTEN_KEEPALIVE const uint8_t *Bo1_PipelineRecipes() { return s_usedRecipes.data(); }
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t Bo1_PipelineRecipesSize() { return (uint32_t)s_usedRecipes.size(); }
// draws left out while their pipelines compiled, since the page started (zombies-hub's e2e/game/smoke/coop.spec.ts
// checks the spawn's)
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t Bo1_PipelineWaits() { return g_bo1PipelineWaits.load(); }
// and with a stand-in pipeline (stand-ins)
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t Bo1_PipelineStandIns() { return g_bo1PipelineStandIns.load(); }

// for zombies-hub's e2e/game/smoke/coop.spec.ts: draws left out for a pipeline of the list's first tier, which should
// have compiled before the player went in, and the pipelines needed that the list lacks
static uint32_t s_firstTierWaits, s_lacked;
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t Bo1_FirstTierWaits() { return s_firstTierWaits; }
extern "C" EMSCRIPTEN_KEEPALIVE uint32_t Bo1_PipelinesLacked() { return s_lacked; }

// A pipeline's compile, in the browser's background. Every pipeline starts as soon as it is made, a draw's and its
// stand-in's alike: holding a draw's back, 8 at a time, so that the stand-ins had the compiler first in a burst (a map
// played with no list) left as much black (the draws of Kino's spawn left out for a frame, Chrome on 16 cores: 5,010
// draw-frames against 4,453; on 4: 5,174 against 6,113), since a stand-in compiles no sooner than the real pipeline in
// the same queue, and held the real picture back (the longest wait 1.2-1.4 s against 0.4 s; on 4 cores 2.9 s against
// 1.3 s). Stand-ins pay when they are made before the draw: the list's.
static void StartPipeline(Pipe &p)
{
    double start = emscripten_get_now();
    Module &vm = p.vs->modules[p.vsVariant], &pm = p.ps->modules[p.psVariant];
    uint32_t vsHandle = ModuleHandle(*p.vs, p.vsVariant, vm);
    uint32_t psHandle = ModuleHandle(*p.ps, p.psVariant, pm);
    std::string json = PipelineJson(p.key, p.vs->id, p.ps->id, vsHandle, psHandle);
    p.compileStart = emscripten_get_now();
    p.handle = bo1gpu_pipeline(json.c_str(), &p.status, 1);
    ++g_bo1Compiles;
    g_bo1CompileUs += (uint64_t)((emscripten_get_now() - start) * 1000.0);
}

static void ForgetDerived();

// &gpu=256: every pipeline goes, and draws make them anew, drawn with stand-ins meanwhile, to see those at work
// after the map's list is compiled (one still compiling is released once it is done: an orphan)
static void DropPipelines()
{
    ForgetDerived();
    ps.pipeline = 0;
    size_t compiling = 0;
    for (auto &p : s_pipelines)
    {
        Pipe *pipe = p.second;
        if (pipe->status != 0)
        {
            bo1gpu_release(pipe->handle);
            delete pipe;
        }
        else
        {
            s_orphans.push_back(pipe);
            ++compiling;
        }
    }
    Bo1_Log("webgpu: %zu pipelines dropped (&gpu=256), %zu of them still compiling; draws make them anew", s_pipelines.size(), compiling);
    s_pipelines.clear();
}

// The pipeline of a recipe, made if it is new; false: the shaders can't make one. fromDraw: a draw needs it, and a new
// one's recipe is recorded. Everything comes from the recipe, so a pipeline made from one in a later session is the one
// the draw that recorded it made.
static bool MakePipeline(const Recipe &r, Program &vs, Program &psh, bool fromDraw, Derived &out)
{
    static DWORD rs[256];
    for (int i = 0; i < PIPE_STATES; ++i)
        rs[kPipeStates[i]] = r.rs[i];

    // the targets
    GpuFormat colorFormat[4] = {}, depthFormat = GF_NONE;
    bool clamp[4] = {}, stencil = false;
    uint32_t targetMask = 0;
    for (int i = 0; i < 4; ++i)
        if (r.color[i])
        {
            TexFormat tf = KeyFormat(r.color[i]);
            colorFormat[i] = tf.gpu;
            clamp[i] = tf.clamp;
            targetMask |= 1u << i;
        }
    if (r.depth)
    {
        TexFormat tf = KeyFormat(r.depth);
        depthFormat = tf.gpu;
        stencil = tf.stencil;
    }

    // the samplers, the vertex shader's first, and group 1's layout of them
    static std::vector<uint32_t> layoutEntries;
    layoutEntries.clear();
    uint32_t shadowMask = 0;
    int n = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (const ShaderInfo::Sampler &s : (pass ? psh.info : vs.info).samplers)
        {
            if (n == MAX_DRAW_SAMPLERS)
                break;
            int kind = -1;
            if (r.sampler[n])
            {
                TexFormat tf = KeyFormat(r.sampler[n]);
                if (!tf.depth)
                    kind = Filterable(tf) ? 0 : 1;
                else if (pass && s.dim == DIM_2D)
                {
                    kind = 2;
                    // the depth buffer drawn to: read only, so not while the draw writes it
                    if ((r.depthTarget >> n & 1) &&
                        ((rs[D3DRS_ZENABLE] && rs[D3DRS_ZWRITEENABLE]) || (rs[D3DRS_STENCILENABLE] && (rs[D3DRS_STENCILWRITEMASK] & 0xFF))))
                    {
                        if (fromDraw)
                            BO1_GPU_ERROR("the depth texture at sampler %u is read while written", s.stage);
                        kind = -1;
                    }
                }
            }
            if (kind == 2)
                shadowMask |= 1u << s.stage;
            out.kind[n++] = (int8_t)kind;
            uint32_t binding = pass ? s.stage : 32 + s.stage;
            uint32_t vis = pass ? VIS_FRAGMENT : VIS_VERTEX;
            uint32_t k = kind < 0 ? 0 : (uint32_t)kind;
            uint32_t texEntry[5] = {binding, vis, 1, k, (uint32_t)s.dim};
            uint32_t smpEntry[5] = {binding + (pass ? 16u : 4u), vis, 2, k, 0};
            layoutEntries.insert(layoutEntries.end(), texEntry, texEntry + 5);
            layoutEntries.insert(layoutEntries.end(), smpEntry, smpEntry + 5);
        }

    // the modules
    uint32_t extraVaryings = psh.info.varyings & ~vs.info.varyings;
    Module &vsModule = ModuleFor(vs, 0, extraVaryings, 0, s_byInstance);
    Module &psModule = ModuleFor(psh, shadowMask, 0, targetMask);
    if (vsModule.failed || psModule.failed)
        return false;
    uint32_t groupLayout, pipelineLayout;
    TextureLayout(layoutEntries, &groupLayout, &pipelineLayout);

    // the pipeline
    PipeKey k;
    memset(&k, 0, sizeof(k));
    k.vsModule = vsModule.id;
    k.psModule = psModule.id;
    k.layout = pipelineLayout;
    k.vertexLayout = InternLayout(r.layout);
    k.vsOverrides = vsModule.overrides;
    k.psOverrides = psModule.overrides;
    k.alphaFunc = rs[D3DRS_ALPHATESTENABLE] ? (rs[D3DRS_ALPHAFUNC] & 0xF) : 8;
    if (k.alphaFunc < 1 || k.alphaFunc > 8)
        k.alphaFunc = 8;
    k.srgbWrite = rs[D3DRS_SRGBWRITEENABLE] ? 1 : 0;
    k.topology = r.topology;
    k.strip = r.strip;
    // a draw's, as its targets have them; a list's, as the back buffer has them now
    k.samples = r.multisampled ? (fromDraw ? 4 : s_back.samples) : 1;
    DWORD cull = rs[D3DRS_CULLMODE];
    if (s_debugFlags & DBG_NO_CULL)
        cull = D3DCULL_NONE;
    else if ((s_debugFlags & DBG_FLIP_CULL) && cull != D3DCULL_NONE)
        cull = cull == D3DCULL_CW ? D3DCULL_CCW : D3DCULL_CW;
    // front faces are clockwise (frontFace "cw"): D3DCULL_CW culls them
    k.cull = r.topology < 3 || cull == D3DCULL_NONE ? 0 : cull == D3DCULL_CW ? 1 : 2;
    uint32_t blend = BlendState(rs);
    static const D3DRENDERSTATETYPE writeStates[4] = {D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2,
        D3DRS_COLORWRITEENABLE3};
    for (int i = 0; i < 4; ++i)
    {
        if (!colorFormat[i])
            continue;
        k.colorFormat[i] = colorFormat[i];
        // nothing to write where the shader has no output
        k.writeMask[i] = (psh.info.colorOutputs >> i & 1) ? (uint8_t)(rs[writeStates[i]] & 0xF) : 0;
        if (clamp[i])
            k.clampMask |= 1u << i;
    }
    bool blendable = true;
    for (int i = 0; i < 4; ++i)
        if (k.colorFormat[i] >= GF_R32F && k.colorFormat[i] <= GF_RGBA32F && !(s_caps & CAP_FLOAT32_BLEND))
            blendable = false;
    k.blend = blendable ? blend : 0;
    if (r.depth)
    {
        k.depthFormat = depthFormat;
        bool z = rs[D3DRS_ZENABLE] != 0;
        k.depth = (z ? 1u : 0u) | (z && rs[D3DRS_ZWRITEENABLE] ? 2u : 0u) | (uint32_t)(z ? Compare(rs[D3DRS_ZFUNC]) : 7) << 2;
        if (stencil && rs[D3DRS_STENCILENABLE])
        {
            auto face = [&](DWORD func, DWORD fail, DWORD zfail, DWORD pass) {
                return (uint32_t)Compare(func) | (uint32_t)StencilOp(fail) << 3 | (uint32_t)StencilOp(zfail) << 6 | (uint32_t)StencilOp(pass) << 9;
            };
            k.stencilFront = face(rs[D3DRS_STENCILFUNC], rs[D3DRS_STENCILFAIL], rs[D3DRS_STENCILZFAIL], rs[D3DRS_STENCILPASS]);
            k.stencilBack = rs[D3DRS_TWOSIDEDSTENCILMODE]
                                ? face(rs[D3DRS_CCW_STENCILFUNC], rs[D3DRS_CCW_STENCILFAIL], rs[D3DRS_CCW_STENCILZFAIL], rs[D3DRS_CCW_STENCILPASS])
                                : k.stencilFront;
            k.stencilMasks = 1u | (rs[D3DRS_STENCILMASK] & 0xFF) << 8 | (rs[D3DRS_STENCILWRITEMASK] & 0xFF) << 16;
        }
        float bias = AsFloat(rs[D3DRS_DEPTHBIAS]), slope = AsFloat(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
        if (r.topology >= 3 && (bias != 0.0f || slope != 0.0f))
        {
            // D3D's bias is in depth units; WebGPU's in steps of a 24-bit depth buffer (16-bit: of that)
            k.bias = (int32_t)lrintf(bias * (k.depthFormat == GF_D16 ? 65535.0f : 16777215.0f));
            k.slope = slope;
        }
    }
    Pipe *&pipe = s_pipelines[k];
    if (!pipe)
    {
        double start = emscripten_get_now();
        pipe = new Pipe;
        pipe->tier = fromDraw ? -1 : (int8_t)r.tier;
        pipe->key = k;
        pipe->vs = &vs;
        pipe->ps = &psh;
        pipe->groupLayout = groupLayout;
        pipe->vsVariant = Variant(0, extraVaryings, 0, s_byInstance);
        pipe->psVariant = Variant(shadowMask, 0, targetMask);
        pipe->hintFlags = r.hints[HINT_FLAGS];
        g_bo1CompileUs += (uint64_t)((emscripten_get_now() - start) * 1000.0);
        StartPipeline(*pipe);
    }
    out.pipe = pipe;
    out.groupLayout = groupLayout;
    out.vertexLayout = k.vertexLayout;
    out.blend = k.blend;
    out.stencilMasks = k.stencilMasks;
    return true;
}

// ----- what draws keep of the draws before -----
//
// Most draws change few of the states a draw reads: the constants, and where in its buffers a model is. So what a draw
// works out from the state is kept, and worked out again only when what it reads changes: Execute notes what changed
// (s_changed), and each part of the work is kept with what changed since in what it reads (s_dirty). &gpu=128 works each
// draw out in full too, as draws did before they kept anything, and logs where the two differ (CheckSetup).
enum
{
    CH_SHADERS = 1,   // the vertex or pixel shader
    CH_DECL = 2,      // the vertex declaration, a stream's stride or frequency
    CH_TARGETS = 4,   // the render targets and depth stencil
    CH_TEXTURES = 8,  // the textures set
    CH_SAMPLERS = 16, // sampler states
    CH_STATES = 32,   // the render states pipelines take (kPipeStates)
    CH_OBJECTS = 64,  // textures, surfaces, buffers, declarations or shaders made or destroyed
    CH_HINTS = 128,   // the pass's hints (CmdPassHints)
    CH_ALL = 255,
    // what each part reads (the recipe's vertex layout is a part of its own)
    READS_SHADERS = CH_SHADERS | CH_DECL | CH_OBJECTS,
    READS_TARGETS = CH_TARGETS | CH_OBJECTS,
    READS_RECIPE = CH_SHADERS | CH_TARGETS | CH_TEXTURES | CH_STATES | CH_OBJECTS | CH_HINTS,
    READS_LAYOUT = CH_SHADERS | CH_DECL | CH_OBJECTS,
    READS_GROUP = CH_SHADERS | CH_TARGETS | CH_TEXTURES | CH_SAMPLERS | CH_STATES | CH_OBJECTS,
};
static uint32_t s_changed = CH_ALL; // since the last draw
static struct
{
    uint32_t shaders, targets, recipe, layout, group; // since each part was worked out
} s_dirty = {CH_ALL, CH_ALL, CH_ALL, CH_ALL, CH_ALL};
static uint8_t s_pipeState[256]; // kPipeStates: 1

// the streams' buffers and the index buffer, looked up again when they are set to others, or buffers are made or go
static Buf *s_streamBuf[16], *s_indexBuf;
static uint32_t s_streamKnown; // a bit each
static bool s_indexKnown;

// LayoutFor's layouts, by what it reads: the vertex shader's inputs, the declaration, and the streams' strides and
// frequencies (cleared as objects are made or go: the addresses of declarations and shaders come back)
struct LayoutKey
{
    const ShaderInfo *vs;
    const Decl *decl;
    uint32_t upStride, instance; // instance: the streams of instance data, a bit each
    uint32_t stride[16];
    bool operator==(const LayoutKey &o) const { return !memcmp(this, &o, sizeof(LayoutKey)); }
};
struct LayoutKeyHash
{
    size_t operator()(const LayoutKey &k) const
    {
        const uint32_t *w = (const uint32_t *)&k;
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < sizeof(LayoutKey) / 4; ++i)
            h = (h ^ w[i]) * 0x100000001B3ull;
        return (size_t)(h ^ (h >> 32));
    }
};
static std::unordered_map<LayoutKey, uint32_t, LayoutKeyHash> s_layoutsKept;

static uint32_t LayoutKept(const ShaderInfo &vs, const Decl *decl, uint32_t upStride)
{
    LayoutKey k;
    k.vs = &vs;
    k.decl = decl;
    k.upStride = upStride;
    k.instance = 0;
    for (int s = 0; s < 16; ++s)
    {
        k.stride[s] = st.stream[s].stride;
        k.instance |= (st.freq[s] & D3DSTREAMSOURCE_INSTANCEDATA) ? 1u << s : 0u;
    }
    auto it = s_layoutsKept.find(k);
    if (it == s_layoutsKept.end())
        it = s_layoutsKept.emplace(k, LayoutFor(vs, decl, upStride)).first;
    return it->second;
}

static void ObjectsChanged()
{
    s_changed |= CH_OBJECTS;
    s_streamKnown = 0;
    s_indexKnown = false;
    s_layoutsKept.clear();
}

static Buf *FindBuffer(uint32_t id)
{
    auto it = s_buffers.find(id);
    return it != s_buffers.end() ? &it->second : nullptr;
}

static Buf *StreamBuffer(int stream)
{
    if (!(s_streamKnown >> stream & 1))
    {
        s_streamBuf[stream] = FindBuffer(st.stream[stream].id);
        s_streamKnown |= 1u << stream;
    }
    return s_streamBuf[stream];
}

static Buf *IndexBuffer()
{
    if (!s_indexKnown)
    {
        s_indexBuf = FindBuffer(st.indices);
        s_indexKnown = true;
    }
    return s_indexBuf;
}

// The pipelines of the recipes draws needed: draws of the same models and materials need the same few. The last draw's
// is the first looked at, as draws in a row often share it. Cleared when pipelines go.
struct RecipeHash
{
    size_t operator()(const Recipe &r) const
    {
        const uint32_t *w = (const uint32_t *)&r;
        uint64_t h = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < sizeof(Recipe) / 4; ++i)
            h = (h ^ w[i]) * 0x100000001B3ull;
        return (size_t)(h ^ (h >> 32));
    }
};
struct RecipeEqual
{
    bool operator()(const Recipe &a, const Recipe &b) const { return !memcmp(&a, &b, sizeof(Recipe)); }
};
static std::unordered_map<Recipe, Derived, RecipeHash, RecipeEqual> s_derived;
static Recipe s_lastRecipe;
static Derived s_lastDerived;
static bool s_lastValid;

// pipelines went (a shader's), or would be made otherwise (the culling's debug flags)
static void ForgetDerived()
{
    s_lastValid = false;
    s_derived.clear();
}

// The vertex shader's constants where a draw's first instance says, or bound for the draw (s_byInstance; &gpu=1024).
// Draws make the other way's pipelines as they need them, drawn with stand-ins meanwhile, and both ways' stay.
static void SetByInstance(bool on)
{
    if (s_byInstance == on)
        return;
    s_byInstance = on;
    s_arenaGroup = s_arenaGroups[on];
    s_arenaLayout = s_arenaLayouts[on];
    s_vsUploadSerial = 0; // the next draw's block: on its way's alignment (PrepareDraw)
    ForgetDerived();
}

// a recipe's pipeline, and what it is for the draw: the last draw's, one a draw before needed, or made
static bool DerivedFor(const Recipe &r, Program &vs, Program &psh, Derived &d)
{
    if (s_lastValid && !memcmp(&r, &s_lastRecipe, sizeof(r)))
    {
        d = s_lastDerived;
        return true;
    }
    s_prof.workedPipelines += s_prof.on;
    auto it = s_derived.find(r);
    if (it != s_derived.end())
        d = it->second;
    else
    {
        s_prof.madePipelines += s_prof.on;
        if (!MakePipeline(r, vs, psh, true, d))
            return false;
        s_derived.emplace(r, d);
    }
    s_lastRecipe = r;
    s_lastDerived = d;
    s_lastValid = true;
    return true;
}

struct Bound
{
    Tex *tex;
    int stage, dim, kind; // kind: 0 filtering, 1 unfilterable, 2 depth comparisons
    bool vertex;
};

// what a draw works out from the state before it sets any: its shaders, what its pass draws to, the textures its
// samplers read, and its pipeline's recipe
struct DrawSetup
{
    Program *vs, *ps; // none: the draw is left out
    const Decl *decl;
    bool targetsOk; // false: the draw is left out
    PassKey passKey;
    PassTargets targets;
    Recipe recipe;
    uint32_t upStride; // the recipe's
    int nbound;
    Bound bound[MAX_DRAW_SAMPLERS];
};

// the draw's shaders (none if it can't be drawn with them), and its vertex declaration
static void SetupShaders(DrawSetup &w)
{
    w.vs = w.ps = nullptr;
    w.decl = nullptr;
    auto vsIt = s_shaders.find(st.vs);
    auto psIt = s_shaders.find(st.ps);
    if (vsIt == s_shaders.end() || psIt == s_shaders.end())
        return; // the fixed-function pipeline is not emulated
    Program &vs = *vsIt->second.prog;
    Program &psh = *psIt->second.prog;
    if (!vs.info.error.empty() || !psh.info.error.empty() || vs.info.pixel || !psh.info.pixel)
        return;
    w.vs = &vs;
    w.ps = &psh;
    auto dIt = s_decls.find(st.decl);
    if (dIt != s_decls.end())
        w.decl = &dIt->second;
}

static void SetupTargets(DrawSetup &w)
{
    w.passKey = DrawPassKey();
    w.targetsOk = true;
    if (s_passOpen && s_passKey == w.passKey)
        w.targets = s_pass;
    else
        w.targetsOk = ResolvePass(w.passKey, w.targets);
}

// the recipe, and the textures: the pixel shader's and the vertex shader's (layout: the vertex layout of the vertex
// shader's inputs, with upStride, LayoutFor's; none: the recipe's stays)
static void SetupRecipe(DrawSetup &w, int topology, bool indexed, bool index32, uint32_t upStride, const VertexLayout *layout)
{
    Program &vs = *w.vs, &psh = *w.ps;
    const PassTargets &targets = w.targets;
    Recipe &r = w.recipe;
    VertexLayout kept;
    if (!layout)
        kept = r.layout;
    memset(&r, 0, sizeof(r));
    r.vs = vs.hash;
    r.ps = psh.hash;
    for (int i = 0; i < 4; ++i)
        r.color[i] = targets.colorTex[i] ? targets.colorTex[i]->fmt : 0;
    r.depth = targets.depthTex ? targets.depthTex->fmt : 0;
    r.multisampled = targets.samples > 1;
    memcpy(r.hints, st.hints, HINT_COUNT);
    w.nbound = 0;
    for (int pass = 0; pass < 2; ++pass)
        for (const ShaderInfo::Sampler &s : (pass ? psh.info : vs.info).samplers)
        {
            if (w.nbound == MAX_DRAW_SAMPLERS)
                break;
            int stage = pass ? s.stage : 257 + s.stage;
            Tex *t = BoundTexture(stage);
            int type = s.dim == DIM_CUBE ? TEX_CUBE : s.dim == DIM_3D ? TEX_3D : TEX_2D;
            if (t && t->type != type)
                t = nullptr;
            if (t)
                r.sampler[w.nbound] = t->fmt;
            if (t && t == targets.depthTex)
                r.depthTarget |= 1u << w.nbound;
            w.bound[w.nbound++] = {t, stage, s.dim, 0, !pass};
        }
    for (int i = 0; i < PIPE_STATES; ++i)
        r.rs[i] = st.rs[kPipeStates[i]];
    if (layout)
    {
        r.layout = *layout;
        w.upStride = upStride;
    }
    else
        r.layout = kept;
    r.topology = (uint8_t)topology;
    r.strip = (topology == 2 || topology == 4) && indexed ? (index32 ? 2 : 1) : 0;
}

// the setup worked out in full, as each draw did before draws kept it; false leaves the draw out
static bool SetupFull(DrawSetup &w, int topology, bool indexed, bool index32, uint32_t upStride)
{
    SetupShaders(w);
    if (!w.vs)
        return false;
    SetupTargets(w);
    if (!w.targetsOk)
        return false;
    SetupRecipe(w, topology, indexed, index32, upStride, &s_layouts[LayoutFor(w.vs->info, w.decl, upStride)]);
    return true;
}

static DrawSetup s_setup;   // the draws before's
static bool s_setupDerived; // s_lastDerived is s_setup.recipe's

// the setup, as the draws before worked it out where what it reads didn't change since; false leaves the draw out
static bool SetupKept(int topology, bool indexed, bool index32, uint32_t upStride)
{
    if (uint32_t ch = s_changed)
    {
        s_dirty.shaders |= ch;
        s_dirty.targets |= ch;
        s_dirty.recipe |= ch;
        s_dirty.layout |= ch;
        s_dirty.group |= ch;
        s_changed = 0;
    }
    DrawSetup &w = s_setup;
    if (s_dirty.shaders & READS_SHADERS)
    {
        SetupShaders(w);
        s_dirty.shaders = 0;
        s_prof.workedShaders += s_prof.on;
    }
    if (!w.vs)
        return false;
    if (s_dirty.targets & READS_TARGETS)
    {
        SetupTargets(w);
        s_dirty.targets = 0;
        s_prof.workedTargets += s_prof.on;
    }
    if (!w.targetsOk)
        return false;
    uint8_t strip = (topology == 2 || topology == 4) && indexed ? (index32 ? 2 : 1) : 0;
    bool layout = (s_dirty.layout & READS_LAYOUT) || w.upStride != upStride;
    if (layout || (s_dirty.recipe & READS_RECIPE))
    {
        SetupRecipe(w, topology, indexed, index32, upStride, layout ? &s_layouts[LayoutKept(w.vs->info, w.decl, upStride)] : nullptr);
        s_dirty.recipe = 0;
        if (layout)
            s_dirty.layout = 0;
        s_setupDerived = false;
        s_prof.workedRecipes += s_prof.on;
        s_prof.workedLayouts += s_prof.on && layout;
    }
    else if (w.recipe.topology != topology || w.recipe.strip != strip)
    {
        w.recipe.topology = (uint8_t)topology;
        w.recipe.strip = strip;
        s_setupDerived = false;
    }
    return true;
}

// the bind group of the textures and samplers a draw reads, made the first time (kept: with the samplers draws keep)
static uint32_t TextureGroup(uint32_t layout, const Bound *bound, int nbound, bool kept)
{
    GroupKey gk;
    gk.layout = layout;
    gk.n = 0;
    uint32_t groupEntries[40 * 5];
    for (int i = 0; i < nbound; ++i)
    {
        const Bound &b = bound[i];
        uint32_t view = b.tex ? SampleView(*b.tex, b.kind != 2 && st.ss[b.stage][D3DSAMP_SRGBTEXTURE]) : s_dummy[b.dim];
        bool mipmapped = b.tex && b.tex->levels > 1;
        uint32_t smp = kept ? SamplerKept(b.stage, mipmapped, b.kind) : SamplerFor(b.stage, mipmapped, b.kind);
        uint32_t binding = b.vertex ? 32 + (b.stage - 257) : b.stage;
        uint32_t *e = groupEntries + gk.n * 5;
        e[0] = binding;
        e[1] = 1;
        e[2] = view;
        e[3] = e[4] = 0;
        e[5] = binding + (b.vertex ? 4 : 16);
        e[6] = 2;
        e[7] = smp;
        e[8] = e[9] = 0;
        gk.items[gk.n++] = view;
        gk.items[gk.n++] = smp;
    }
    auto g = s_groups.find(gk);
    if (g == s_groups.end())
    {
        if (s_groups.size() > 20000)
        {
            // long sessions: start over
            for (auto &e : s_groups)
                bo1gpu_release(e.second.handle);
            s_groups.clear();
            s_groupsById.clear();
            for (auto &t : s_textures)
                t.second.bindGroups.clear();
            ps.group1 = 0;
        }
        Group grp = {bo1gpu_bind_group(layout, groupEntries, (int)gk.n), s_nextGroupId++};
        g = s_groups.emplace(gk, grp).first;
        s_groupsById[grp.id] = gk;
        for (int i = 0; i < nbound; ++i)
            if (bound[i].tex)
                bound[i].tex->bindGroups.push_back(grp.id);
    }
    return g->second.handle;
}

// the last draw's texture bind group, and what it was made for (kinds: Derived's)
static struct
{
    uint32_t handle, layout;
    int n;
    int8_t kind[MAX_DRAW_SAMPLERS];
} s_keptGroup;

// &gpu=128: a difference between what a draw kept of the draws before and the draw worked out in full is a gpu error (the
// draw is drawn as kept); how many draws were checked, and how many differed, is logged every 10 s
static uint32_t s_checked, s_differed, s_differedAt;

static void Differs(const char *what)
{
    if (s_differedAt != s_checked)
    {
        s_differedAt = s_checked;
        ++s_differed;
    }
    BO1_GPU_ERROR("a draw worked out in full differs from what it kept: %s (vs %u, ps %u)", what, st.vs, st.ps);
}

static bool SameTargets(const PassTargets &a, const PassTargets &b)
{
    for (int i = 0; i < 4; ++i)
        if (a.colorView[i] != b.colorView[i] || a.colorFormat[i] != b.colorFormat[i] || a.clamp[i] != b.clamp[i] || a.colorTex[i] != b.colorTex[i])
            return false;
    return a.depthView == b.depthView && a.depthFormat == b.depthFormat && a.stencil == b.stencil && a.depthTex == b.depthTex && a.w == b.w &&
           a.h == b.h && a.samples == b.samples;
}

// the setup kept (`ok`: the draw goes on) against the draw's worked out in full; true: both go on, alike
static bool CheckSetup(bool ok, const DrawSetup &k, int topology, bool indexed, bool index32, uint32_t upStride, DrawSetup &f)
{
    ++s_checked;
    bool full = SetupFull(f, topology, indexed, index32, upStride);
    if (k.vs != f.vs || k.ps != f.ps)
        Differs("its shaders");
    else if (f.vs && k.decl != f.decl)
        Differs("its vertex declaration");
    else if (f.vs && (k.targetsOk != f.targetsOk || (f.targetsOk && (!(k.passKey == f.passKey) || !SameTargets(k.targets, f.targets)))))
        Differs("what it draws to");
    else if (full && memcmp(&k.recipe, &f.recipe, sizeof(Recipe)))
        Differs("its pipeline's recipe");
    else if (ok != full)
        Differs("whether it is left out");
    else
    {
        bool same = !full || k.nbound == f.nbound;
        for (int i = 0; same && full && i < f.nbound; ++i)
        {
            const Bound &a = k.bound[i], &b = f.bound[i];
            same = a.tex == b.tex && a.stage == b.stage && a.dim == b.dim && a.kind == b.kind && a.vertex == b.vertex;
        }
        if (!same)
            Differs("the textures it reads");
        return full && same;
    }
    return false;
}

// the pipeline kept (`ok`: there is one) against the one MakePipeline gives the recipe worked out in full, `d`; true:
// both have one, alike
static bool CheckDerived(bool ok, const Derived &k, const DrawSetup &f, Derived &d)
{
    bool made = MakePipeline(f.recipe, *f.vs, *f.ps, true, d);
    if (made != ok)
        Differs("whether it has a pipeline");
    else if (made && (k.pipe != d.pipe || k.groupLayout != d.groupLayout || k.vertexLayout != d.vertexLayout || k.blend != d.blend ||
                         k.stencilMasks != d.stencilMasks || memcmp(k.kind, d.kind, f.nbound)))
        Differs("its pipeline");
    else
        return made;
    return false;
}

// What the textures a draw reads come to with its pipeline: a black stand-in where the pipeline has none, a copy of one
// the pass draws to (made for each draw), the depth buffer read only. Kept while the setup and pipeline are.
struct TexturePlan
{
    int nbound;
    Bound bound[MAX_DRAW_SAMPLERS];
    uint32_t snapshots; // the textures to copy, a bit each
    PassKey passKey;    // readOnlyDepth: a texture is the depth buffer
};
static TexturePlan s_plan;
static uint32_t s_planSerial, s_setupSerial = 1; // s_setupSerial: counts the setups and pipelines draws worked out

static void PlanTextures(const DrawSetup &w, const Derived &d, TexturePlan &p)
{
    const PassTargets &targets = w.targets;
    p.nbound = w.nbound;
    p.snapshots = 0;
    p.passKey = w.passKey;
    for (int i = 0; i < w.nbound; ++i)
    {
        Bound &b = p.bound[i] = w.bound[i];
        b.kind = std::max<int>(d.kind[i], 0);
        if (d.kind[i] < 0)
            b.tex = nullptr;
        else if (b.tex && (b.tex == targets.colorTex[0] || b.tex == targets.colorTex[1] || b.tex == targets.colorTex[2] ||
                              b.tex == targets.colorTex[3]))
            p.snapshots |= 1u << i;
        else if (b.tex && b.tex == targets.depthTex)
            p.passKey.readOnlyDepth = 1;
    }
}

// the plan kept against the plan of the draw worked out in full
static void CheckPlan(const TexturePlan &k, const DrawSetup &f, const Derived &d)
{
    static TexturePlan p;
    PlanTextures(f, d, p);
    bool same = k.nbound == p.nbound && k.snapshots == p.snapshots && k.passKey == p.passKey;
    for (int i = 0; same && i < p.nbound; ++i)
    {
        const Bound &a = k.bound[i], &b = p.bound[i];
        same = a.tex == b.tex && a.stage == b.stage && a.dim == b.dim && a.kind == b.kind && a.vertex == b.vertex;
    }
    if (!same)
        Differs("what the textures it reads come to");
}

// ----- stand-ins -----
//
// Where a draw's pipeline is still compiling, the draw is drawn with a stand-in: a pipeline of the
// draw's own vertex shader module, vertex layout, states and targets (samples included) with a generated pixel shader,
// which lights the pass's colour map about as the real one does (StandInCode): the colour map (times the vertex colour
// where the real pixel shader reads one on a world surface or a blended effect, times its alpha where the blend is
// additive) lit as the game's shaders light it: they square the colour, light it, scale it by the HDR control constant
// and take the root. On a world surface the light is the light around from the luminance lightmap plus the sun's colour
// by how much of it reaches the texel (the primary lightmap) along the light vector the vertex shader passes; on a model
// the light grid's cube of 4x4x4 texels at its normal (the model lighting texture, a 3D one, which the light-probe and
// character techniques read) plus the primary light's colour, the sun's in those techniques, by how much of it the
// surface faces and whether the sun's shadow map lets it through (one compared tap, where the real shader filters four
// in two partitions). Which sampler stage holds the colour map (by its name, or its texture's semantic: a character's
// skin), the model lighting and the shadow map, and which constant registers the light, the cube's coordinates and
// scale, the shadow map's partition, the HDR scale and the hero scale, the engine says per pass (CmdPassHints,
// Recipe::hints); the lightmaps it binds at samplers 12-14 outside the pass's arguments (r_draw_bsp.cpp), which the
// routing knows (StandInRoute); where the colour map's coordinates come from, the real pixel shader says
// (PixelSamplerCoords: the varying its texld reads), and which output is the light vector, the normal, the cube's
// coordinates or the shadow map's, the vertex shader (VertexVaryingsFrom: the outputs computed from the light's
// position, the normal, or a constant). Checked against the settled picture of the same frozen view (Kino, every scene
// pipeline drawn with its stand-in): world surfaces come out as they are but for the packed colour lightmap (sampler
// 13) the stand-in leaves alone, zombies, the hands and the guns close (the guns without their shine: no specular or
// reflection), dust and glows as they are. A pass that reads the scene's colour copy (distortion, post effects) has no
// stand-in: its draws stay left out; one that reads the depth buffer's copy (most smoke and fire, to fade where they
// near a surface; water) has one, which draws without the fade. The stand-in's pixel shader reads its textures at the
// real stages, so the draw's sampler states serve it, and its five constants are packed for it
// (UploadStandInConstants). One stand-in serves every pipeline of its vertex shader,
// layout, states, targets and routing (StandInRouting), through MakePipeline's own table: the list's are its tier 1, made
// once tier 0 is compiled as the map loads and first from then on, before the player goes in, the zone's passes' (EnumPass) tier 3, made
// right after the player went in, or before where the map has no list (Preload), the rest as a draw first needs one. A draw with no
// stand-in ready is left out. Drawing with a twin meanwhile, a ready pipeline of a sibling material's alike in all but
// its pixel shader, was dropped: the sibling reads the draw's textures for other maps (Kino's lobby came out white,
// black and green with every draw drawn with one, a gun lilac, a zombie pink), and with the zone's stand-ins it drew
// once in two games of a Mac's.

static std::unordered_map<std::string, Program *> s_standInPrograms; // the stand-in pixel shaders, by their code
static uint32_t s_standInsMade;                                      // stand-in pipelines made

static std::string Swizzle2(uint8_t swz)
{
    const char *c = "xyzw";
    std::string r = ".";
    r += c[swz & 3];
    r += c[(swz >> 2) & 3];
    return r;
}

// the stand-in pixel shader's WGSL: the routing's, writing colour target 0 where the pass has one (targetMask)
static std::string StandInCode(const StandInRouting &rt, uint32_t targetMask, uint32_t *overrides)
{
    bool out = (targetMask & 1) != 0;
    *overrides = OVERRIDE_ALPHA | (out ? OVERRIDE_CLAMP : 0);
    std::string m = "diagnostic(off, derivative_uniformity);\n\n";
    if (rt.colorStage == 255)
        m += "// stand-in: the depth alone";
    else
        m += Json("// stand-in: colour map s%u (vary%u%s)", rt.colorStage, rt.colorLoc, Swizzle2(rt.colorSwz).c_str());
    if (rt.lmPrimaryStage != 255)
        m += Json(", primary lightmap s%u (vary%u%s)", rt.lmPrimaryStage, rt.lmPrimaryLoc, Swizzle2(rt.lmPrimarySwz).c_str());
    if (rt.lmSecondaryStage != 255)
        m += Json(", secondary lightmap s%u (vary%u%s)", rt.lmSecondaryStage, rt.lmSecondaryLoc, Swizzle2(rt.lmSecondarySwz).c_str());
    if (rt.probeStage != 255)
        m += Json(", model lighting s%u (vary%u)", rt.probeStage, rt.probeLoc);
    if (rt.vertexColor)
        m += ", vertex colour";
    if (rt.premultiply)
        m += ", premultiplied";
    if (rt.lightLoc != 255)
        m += Json(", light vector vary%u", rt.lightLoc);
    if (rt.normalLoc != 255)
        m += Json(", normal vary%u", rt.normalLoc);
    if (rt.diffuseFrom)
        m += Json(", light diffuse %cs", rt.diffuseFrom == 1 ? 'p' : 'v');
    if (rt.positionFrom)
        m += Json(", light position %cs", rt.positionFrom == 1 ? 'p' : 'v');
    if (rt.lookup)
        m += ", lookup scale";
    if (rt.hdr)
        m += ", HDR scale";
    if (rt.hero)
        m += ", hero scale";
    if (rt.partition)
        m += ", shadow partition";
    if (rt.shadowStage != 255)
        m += Json(", sun shadow map s%u (vary%u)", rt.shadowStage, rt.shadowLoc);
    m += "\n\nstruct Consts {\n    bo1_fix: vec4f,\n    bo1_bools: vec4u,\n    c: array<vec4f, 5>,\n}\n\n"
         "@group(0) @binding(1) var<uniform> U: Consts;\n";
    uint8_t stages[3] = {rt.colorStage, rt.lmPrimaryStage, rt.lmSecondaryStage};
    std::sort(stages, stages + 3);
    for (int i = 0; i < 3; ++i)
        if (stages[i] < 16 && (i == 0 || stages[i] != stages[i - 1]))
            m += Json("@group(1) @binding(%u) var tex%u: texture_2d<f32>;\n@group(1) @binding(%u) var smp%u: sampler;\n", stages[i], stages[i],
                16 + stages[i], stages[i]);
    if (rt.colorStage == 255) // no colour map: a depth prepass's stand-in, the depth alone, writing no colour
    {
        if (out)
            m += "\nstruct PsOut {\n    @location(0) c0: vec4f,\n}\n\noverride bo1_clamp: u32 = 0u;\n";
        m += "override bo1_alphaFunc: u32 = 8u;\noverride bo1_srgbWrite: bool = false;\n\n";
        m += std::string("@fragment\nfn main()") + (out ? " -> PsOut" : "") + " {\n";
        if (out)
            m += "    var res: PsOut;\n    res.c0 = vec4f(0.0, 0.0, 0.0, 1.0);\n    return res;\n";
        m += "}\n";
        return m;
    }
    if (rt.probeStage != 255)
        m += Json("@group(1) @binding(%u) var tex%u: texture_3d<f32>;\n@group(1) @binding(%u) var smp%u: sampler;\n", rt.probeStage, rt.probeStage,
            16 + rt.probeStage, rt.probeStage);
    if (rt.shadowStage != 255)
        m += Json("@group(1) @binding(%u) var tex%u: texture_depth_2d;\n@group(1) @binding(%u) var smp%u: sampler_comparison;\n", rt.shadowStage,
            rt.shadowStage, 16 + rt.shadowStage, rt.shadowStage);
    uint32_t read = 1u << rt.colorLoc;
    if (rt.lmPrimaryStage != 255)
        read |= 1u << rt.lmPrimaryLoc;
    if (rt.lmSecondaryStage != 255)
        read |= 1u << rt.lmSecondaryLoc;
    if (rt.probeStage != 255)
        read |= 1u << rt.probeLoc;
    if (rt.shadowStage != 255)
        read |= 1u << rt.shadowLoc;
    if (rt.vertexColor)
        read |= 1u << 10;
    if (rt.lightLoc != 255)
        read |= 1u << rt.lightLoc;
    if (rt.normalLoc != 255)
        read |= 1u << rt.normalLoc;
    m += "\nstruct PsIn {\n";
    for (int loc = 0; loc < VARYING_COUNT; ++loc)
        if (read >> loc & 1)
            m += Json("    @location(%d) vary%d: vec4f,\n", loc, loc);
    m += "}\n\n";
    if (out)
        m += "struct PsOut {\n    @location(0) c0: vec4f,\n}\n\noverride bo1_clamp: u32 = 0u;\n";
    m += "override bo1_alphaFunc: u32 = 8u;\noverride bo1_srgbWrite: bool = false;\n\n"
         "fn bo1_srgb(c: vec3f) -> vec3f {\n"
         "    let s = saturate(c);\n"
         "    return select(1.055 * pow(s, vec3f(1.0 / 2.4)) - 0.055, s * 12.92, s < vec3f(0.0031308));\n"
         "}\n\n";
    m += std::string("@fragment\nfn main(pin: PsIn)") + (out ? " -> PsOut" : "") + " {\n";
    bool lightmapped = rt.lmPrimaryStage != 255 || rt.lmSecondaryStage != 255, probe = rt.probeStage != 255;
    m += Json("    var color = textureSample(tex%u, smp%u, pin.vary%u%s);\n", rt.colorStage, rt.colorStage, rt.colorLoc, Swizzle2(rt.colorSwz).c_str());
    if (rt.vertexColor)
        m += "    color = color * saturate(pin.vary10);\n";
    if (rt.premultiply)
        m += "    color = vec4f(color.rgb * color.a, color.a);\n";
    // how much of the light the surface faces: along the light vector (tangent space: the surface faces z; with a normal:
    // along it), or by a direction from the light's position with the normal, or half of it where neither is known (a zero
    // vector normalized is not a number, which would black the pixel: half light then)
    std::string nl = "0.5";
    if (rt.lightLoc != 255)
    {
        m += Json("    let lv = pin.vary%u.xyz;\n    let l = lv * inverseSqrt(max(dot(lv, lv), 1e-12));\n", rt.lightLoc);
        nl = rt.normalLoc != 255 ? Json("select(0.5, saturate(dot(normalize(pin.vary%u.xyz), l)), dot(pin.vary%u.xyz, pin.vary%u.xyz) > 0.0)",
                                       rt.normalLoc, rt.normalLoc, rt.normalLoc)
                                 : "select(0.5, saturate(l.z), dot(lv, lv) > 0.0)";
    }
    else if (rt.normalLoc != 255 && rt.positionFrom)
    {
        m += "    let lv = U.c[1].xyz;\n    let l = lv * inverseSqrt(max(dot(lv, lv), 1e-12));\n";
        nl = Json("select(0.5, saturate(dot(normalize(pin.vary%u.xyz), l)), dot(pin.vary%u.xyz, pin.vary%u.xyz) > 0.0 && dot(lv, lv) > 0.0)",
            rt.normalLoc, rt.normalLoc, rt.normalLoc);
    }
    // The game's shaders square the colour map (to linear light), light it, scale it by the HDR control's x (0.15 in
    // Kino), saturate and take the root (back to the display's). The light, in the shaders' units: a world surface's the
    // light around from the luminance lightmap (its red times 255/8, as the game stores it) and the primary light's colour
    // by how much of it reaches the texel (the primary lightmap's green) along the light vector where there is one; a
    // model's the light grid's cube at its normal (the texel squared, times 255/8) and the primary light's colour (the
    // sun's, in the light-probe techniques, times the hero scale) by how much of it the surface faces; a model with no
    // cube an ambient and that light; else unlit: the colour as it is.
    if (lightmapped || probe || rt.diffuseFrom)
    {
        m += "    var light = vec3f(0.0);\n    let lc = max(U.c[0].rgb, vec3f(0.0));\n";
        if (probe)
        {
            m += Json("    var pc = pin.vary%u.xyz;\n", rt.probeLoc);
            if (rt.normalLoc != 255 && rt.lookup)
                m += Json("    {\n        let pn = pin.vary%u.xyz;\n        let pm = max(max(abs(pn.x), abs(pn.y)), abs(pn.z));\n"
                          "        pc = pc + select(vec3f(0.0), pn * U.c[2].xyz / pm, pm > 0.0);\n    }\n", rt.normalLoc);
            m += Json("    let probe = textureSample(tex%u, smp%u, pc).rgb;\n    light = light + probe * probe * 31.875;\n", rt.probeStage, rt.probeStage);
        }
        if (lightmapped)
        {
            if (rt.lmSecondaryStage != 255)
                m += Json("    light = light + vec3f(textureSample(tex%u, smp%u, pin.vary%u%s).x * 31.875);\n", rt.lmSecondaryStage, rt.lmSecondaryStage,
                    rt.lmSecondaryLoc, Swizzle2(rt.lmSecondarySwz).c_str());
            if (rt.lmPrimaryStage != 255)
                m += Json("    light = light + %s * textureSample(tex%u, smp%u, pin.vary%u%s).y * %s;\n", rt.diffuseFrom ? "lc" : "vec3f(1.0)",
                    rt.lmPrimaryStage, rt.lmPrimaryStage, rt.lmPrimaryLoc, Swizzle2(rt.lmPrimarySwz).c_str(), nl == "0.5" ? "1.0" : nl.c_str());
        }
        else if (rt.diffuseFrom)
        {
            if (!probe)
                m += "    light = light + vec3f(2.0);\n";
            std::string shadow;
            if (rt.shadowStage != 255)
            {
                // the near partition's coordinates as they come (w up to 1), the far one's placed by the constant
                m += Json("    var sc = pin.vary%u;\n", rt.shadowLoc);
                if (rt.partition)
                    m += "    if (sc.w > 1.0) { sc = vec4f(sc.xy * U.c[4].w + U.c[4].xy, sc.zw); }\n";
                m += Json("    let shadow = textureSampleCompareLevel(tex%u, smp%u, sc.xy, sc.z);\n", rt.shadowStage, rt.shadowStage);
                shadow = "shadow * ";
            }
            m += std::string("    light = light + lc * ") + shadow + (rt.hero ? "max(U.c[3].y, 0.0) * " : "") + nl + ";\n";
        }
        m += std::string("    let hdr = ") + (rt.hdr ? "max(U.c[3].x, 0.0)" : "1.0") + ";\n"
             "    var oC0 = vec4f(sqrt(saturate(color.rgb * color.rgb * light * hdr)), color.a);\n";
    }
    else
        m += "    var oC0 = color;\n";
    m += "    if (bo1_alphaFunc != 8u) {\n"
         "        let a = oC0.a;\n"
         "        let aref = U.bo1_fix.z;\n"
         "        var keep = false;\n"
         "        switch bo1_alphaFunc {\n"
         "            case 2u: { keep = a < aref; }\n"
         "            case 3u: { keep = a == aref; }\n"
         "            case 4u: { keep = a <= aref; }\n"
         "            case 5u: { keep = a > aref; }\n"
         "            case 6u: { keep = a != aref; }\n"
         "            case 7u: { keep = a >= aref; }\n"
         "            default: { keep = false; }\n"
         "        }\n"
         "        if (!keep) { discard; }\n"
         "    }\n"
         "    if (bo1_srgbWrite) { oC0 = vec4f(bo1_srgb(oC0.rgb), oC0.a); }\n";
    if (out)
        m += "    var res: PsOut;\n    res.c0 = select(oC0, saturate(oC0), (bo1_clamp & 1u) != 0u);\n    return res;\n";
    m += "}\n";
    return m;
}

// the stand-in pixel shader of a routing, as a program (one per distinct code), whose module StandInCode writes
static Program *StandInProgram(const StandInRouting &rt)
{
    uint32_t ov;
    std::string code = StandInCode(rt, 1, &ov);
    Program *&prog = s_standInPrograms[code];
    if (prog)
        return prog;
    prog = new Program;
    prog->standIn = new StandInRouting(rt);
    prog->id = 900000 + (uint32_t)s_standInPrograms.size();
    ShaderInfo &i = prog->info;
    i.pixel = true;
    i.major = 3;
    // its samplers by stage (MakePipeline and StandInBind go through them in this order), the 2D ones and the model
    // lighting's 3D one
    std::vector<ShaderInfo::Sampler> samplers;
    for (uint8_t stage : {rt.colorStage, rt.lmPrimaryStage, rt.lmSecondaryStage})
        if (stage < 16)
            samplers.push_back({stage, (uint8_t)DIM_2D});
    if (rt.probeStage != 255)
        samplers.push_back({rt.probeStage, (uint8_t)DIM_3D});
    if (rt.shadowStage != 255)
        samplers.push_back({rt.shadowStage, (uint8_t)DIM_2D});
    std::sort(samplers.begin(), samplers.end(), [](const ShaderInfo::Sampler &a, const ShaderInfo::Sampler &b) { return a.stage < b.stage; });
    for (size_t k = 0; k < samplers.size(); ++k)
        if (k == 0 || samplers[k].stage != samplers[k - 1].stage)
            i.samplers.push_back(samplers[k]);
    i.varyings = rt.colorStage != 255 ? 1u << rt.colorLoc : 0u;
    if (rt.lmPrimaryStage != 255)
        i.varyings |= 1u << rt.lmPrimaryLoc;
    if (rt.lmSecondaryStage != 255)
        i.varyings |= 1u << rt.lmSecondaryLoc;
    if (rt.probeStage != 255)
        i.varyings |= 1u << rt.probeLoc;
    if (rt.shadowStage != 255)
        i.varyings |= 1u << rt.shadowLoc;
    if (rt.vertexColor)
        i.varyings |= 1u << 10;
    if (rt.lightLoc != 255)
        i.varyings |= 1u << rt.lightLoc;
    if (rt.normalLoc != 255)
        i.varyings |= 1u << rt.normalLoc;
    i.floatSlots = 5; // UploadStandInConstants' five
    i.colorOutputs = 1;
    uint64_t h = 0xCBF29CE484222325ull;
    for (unsigned char ch : code)
        h = (h ^ ch) * 0x100000001B3ull;
    prog->hash = h;
    return prog;
}

static int LowestBit(uint32_t m)
{
    int i = 0;
    while (!(m >> i & 1))
        ++i;
    return i;
}

// what a stand-in for a recipe's pipeline reads; the reason it has none (the pass reads the scene's colour copy, or has no
// colour map), or null
static const char *StandInRoute(const Recipe &r, const Program &vs, const Program &psh, StandInRouting &rt)
{
    memset(&rt, 255, sizeof(rt));
    rt.vertexColor = 0;
    rt.premultiply = (uint8_t)(r.rs[7] != 0 && r.rs[9] == D3DBLEND_ONE); // kPipeStates[7], [9]: D3DRS_ALPHABLENDENABLE, D3DRS_DESTBLEND
    rt.diffuseFrom = rt.positionFrom = 0;
    rt.lookup = rt.hdr = rt.hero = rt.partition = 0;
    const uint8_t *h = r.hints;
    if (h[HINT_FLAGS] & HINT_READS_SCENE)
        return "the pass reads the scene's colour copy";
    const ShaderInfo &ps = psh.info;
    SamplerCoord coords[16];
    PixelSamplerCoords(ps, coords);
    auto has2d = [&](int s) {
        if (s > 15)
            return false;
        for (const ShaderInfo::Sampler &x : ps.samplers)
            if (x.stage == s)
                return x.dim == (uint8_t)DIM_2D;
        return false;
    };
    // the colour map: the pass's; with none named (or the pass set up last not this draw's), the 2D sampler read at
    // TEXCOORD0's xy, else the first read at a varying's coordinates (a shadow map's are a varying's too)
    int color = has2d(h[HINT_COLOR_MAP]) ? h[HINT_COLOR_MAP] : -1;
    for (int pass = 0; color < 0 && pass < 2; ++pass)
        for (const ShaderInfo::Sampler &x : ps.samplers)
            if (x.dim == DIM_2D && coords[x.stage].loc < VARYING_COUNT && (pass || (coords[x.stage].loc == 0 && coords[x.stage].swz == 4)))
            {
                color = x.stage;
                break;
            }
    if (color < 0)
    {
        // a pass that reads no texture and writes no colour (a depth prepass): its stand-in draws the depth alone
        if (ps.samplers.empty() && !(r.rs[15] & 0xF)) // kPipeStates[15]: D3DRS_COLORWRITEENABLE
        {
            rt.colorStage = 255;
            rt.colorLoc = 0;
            rt.colorSwz = 4;
            return nullptr;
        }
        return ps.samplers.empty() ? "the pixel shader reads no texture" : "no sampler reads a 2D texture at a varying's coordinates";
    }
    uint8_t loc = coords[color].loc, swz = coords[color].swz;
    if (loc >= VARYING_COUNT)
    {
        // coordinates computed (parallax): TEXCOORD0's xy, if there is one
        if (!(ps.varyings & vs.info.varyings & 1))
            return "the colour map's coordinates are computed, and there is no TEXCOORD0";
        loc = 0;
        swz = 4; // .xy
    }
    rt.colorStage = (uint8_t)color;
    rt.colorLoc = loc;
    rt.colorSwz = swz;
    uint32_t used = 1u << loc | 3u << 10;
    auto lightmap = [&](int at, uint8_t &stage, uint8_t &l, uint8_t &s) {
        if (!has2d(at) || at == color || coords[at].loc >= VARYING_COUNT)
            return;
        stage = (uint8_t)at;
        l = coords[stage].loc;
        s = coords[stage].swz;
        used |= 1u << l;
    };
    lightmap(h[HINT_LIGHTMAP_PRIMARY], rt.lmPrimaryStage, rt.lmPrimaryLoc, rt.lmPrimarySwz);
    lightmap(h[HINT_LIGHTMAP_SECONDARY], rt.lmSecondaryStage, rt.lmSecondaryLoc, rt.lmSecondarySwz);
    // the world's lightmaps the engine binds at samplers 12, 13 and 14 outside the pass's arguments (src/gfx_d3d/r_draw_bsp.cpp:
    // R_HW_SetSamplerTexture 0xC-0xE), so the pass's hints say nothing of them: a pixel shader that reads those reads them.
    // The primary's green is how much of the sun reaches the texel; the one at 14 (G16R16) has the light around in its red,
    // and the one at 13 the colour of that packed with its direction, which the stand-in leaves alone
    if (rt.lmPrimaryStage == 255 && rt.lmSecondaryStage == 255 && h[HINT_LIGHTMAP_PRIMARY] == 255 && h[HINT_LIGHTMAP_SECONDARY] == 255)
    {
        lightmap(12, rt.lmPrimaryStage, rt.lmPrimaryLoc, rt.lmPrimarySwz);
        lightmap(14, rt.lmSecondaryStage, rt.lmSecondaryLoc, rt.lmSecondarySwz);
    }
    // a model's light from the light grid: the pass's 3D texture of it (a cube of 4x4x4 texels a model, one for each
    // direction of the normal), at the varying the vertex shader passes the cube's coordinates in
    if (h[HINT_MODEL_LIGHTING] < 16 && h[HINT_VS_BASE_LIGHTING] != 255)
    {
        bool volume = false;
        for (const ShaderInfo::Sampler &x : ps.samplers)
            if (x.stage == h[HINT_MODEL_LIGHTING])
                volume = x.dim == (uint8_t)DIM_3D;
        uint32_t at = VertexVaryingsFrom(vs.info, h[HINT_VS_BASE_LIGHTING], -1) & ps.varyings & vs.info.varyings & ~used;
        if (volume && at)
        {
            rt.probeStage = h[HINT_MODEL_LIGHTING];
            rt.probeLoc = (uint8_t)LowestBit(at);
            used |= 1u << rt.probeLoc;
        }
    }
    rt.lookup = h[HINT_PS_LOOKUP_SCALE] != 255;
    rt.hdr = h[HINT_PS_HDR] != 255;
    rt.hero = h[HINT_PS_HERO_SCALE] != 255;
    rt.partition = h[HINT_PS_SHADOW_PARTITION] != 255;
    // the sun's shadow map, where the pass reads it as a depth texture: one compared tap at the coordinates the vertex
    // shader computes from the lookup matrix (the real shaders take four, in two partitions blended)
    if (h[HINT_SHADOW_SUN] < 16 && h[HINT_VS_SHADOW_MATRIX] != 255)
    {
        size_t nvs = std::min<size_t>(vs.info.samplers.size(), MAX_DRAW_SAMPLERS);
        bool depth = false;
        for (size_t k = 0; k < ps.samplers.size() && nvs + k < MAX_DRAW_SAMPLERS; ++k)
            if (ps.samplers[k].stage == h[HINT_SHADOW_SUN])
                depth = ps.samplers[k].dim == (uint8_t)DIM_2D && r.sampler[nvs + k] && KeyFormat(r.sampler[nvs + k]).depth;
        uint32_t at = VertexVaryingsFrom(vs.info, h[HINT_VS_SHADOW_MATRIX], -1) & ps.varyings & vs.info.varyings & ~used;
        if (depth && at)
        {
            rt.shadowStage = h[HINT_SHADOW_SUN];
            rt.shadowLoc = (uint8_t)LowestBit(at);
            used |= 1u << rt.shadowLoc;
        }
    }
    // the vertex colour, where both shaders have one: the real pixel shaders multiply by it (a world surface's or an
    // effect's tint, a model's colour)
    rt.vertexColor = (uint8_t)((ps.varyings & vs.info.varyings) >> 10 & 1);
    // the light's constants, the pixel shader's first
    if (h[HINT_PS_LIGHT_DIFFUSE] != 255)
        rt.diffuseFrom = 1;
    else if (h[HINT_VS_LIGHT_DIFFUSE] != 255)
        rt.diffuseFrom = 2;
    if (h[HINT_PS_LIGHT_POSITION] != 255)
        rt.positionFrom = 1;
    else if (h[HINT_VS_LIGHT_POSITION] != 255)
        rt.positionFrom = 2;
    // the vertex shader's outputs the pixel shader reads: one computed from the light's position is the light vector;
    // one from the normal but not the tangent is the normal (the tangent and binormal come from both)
    uint32_t candidates = vs.info.varyings & ps.varyings & ~used;
    if (h[HINT_VS_LIGHT_POSITION] != 255)
    {
        uint32_t light = VertexVaryingsFrom(vs.info, h[HINT_VS_LIGHT_POSITION], -1) & candidates;
        if (light)
        {
            rt.lightLoc = (uint8_t)LowestBit(light);
            candidates &= ~light;
        }
    }
    uint32_t normal = VertexVaryingsFrom(vs.info, -1, 3) & ~VertexVaryingsFrom(vs.info, -1, 6) & candidates; // D3DDECLUSAGE_NORMAL, _TANGENT
    if (normal)
        rt.normalLoc = (uint8_t)LowestBit(normal);
    return nullptr;
}

// the stand-in pipeline of a recipe's, made if new (fromDraw as MakePipeline's); none: the pass has none (the first
// few said why, for a draw's)
static Pipe *MakeStandIn(const Recipe &r, Program &vs, Program &psh, bool fromDraw)
{
    StandInRouting rt;
    if (const char *why = StandInRoute(r, vs, psh, rt))
    {
        static int budget = 20;
        if (fromDraw && budget > 0)
        {
            --budget;
            Bo1_Log("webgpu: no stand-in for a pipeline (%s): %s (shaders %016llx %016llx)", DrawName().c_str(), why, (unsigned long long)vs.hash,
                (unsigned long long)psh.hash);
        }
        return nullptr;
    }
    Program *sp = StandInProgram(rt);
    Recipe sr = r;
    sr.ps = sp->hash;
    // the vertex shader's samplers as they are; the stand-in's pixel samplers filterable (A8R8G8B8), whatever is bound
    size_t nvs = std::min<size_t>(vs.info.samplers.size(), MAX_DRAW_SAMPLERS);
    for (size_t i = nvs; i < MAX_DRAW_SAMPLERS; ++i)
        sr.sampler[i] = 0;
    for (size_t i = 0; i < sp->info.samplers.size() && nvs + i < MAX_DRAW_SAMPLERS; ++i)
    {
        sr.sampler[nvs + i] = D3DFMT_A8R8G8B8;
        if (sp->info.samplers[i].stage == rt.shadowStage) // the depth texture, compared: its real format
            for (size_t k = 0; k < psh.info.samplers.size() && nvs + k < MAX_DRAW_SAMPLERS; ++k)
                if (psh.info.samplers[k].stage == rt.shadowStage)
                    sr.sampler[nvs + i] = r.sampler[nvs + k];
    }
    sr.depthTarget = nvs ? r.depthTarget & ((1u << nvs) - 1) : 0;
    Derived d;
    if (!MakePipeline(sr, vs, *sp, fromDraw, d))
        return nullptr;
    if (!d.pipe->standIn)
    {
        d.pipe->standIn = true;
        d.pipe->tier = fromDraw ? -1 : r.tier == 3 ? 3 : 1;
        ++s_standInsMade;
    }
    return d.pipe;
}

// What a recipe's stand-in is made of (MakeStandIn: rt, its routing), without making it: the recipe but for its pixel
// shader, which comes in through the routing alone, the stand-in's pixel samplers being its own but for the shadow map's
// format. Recipes of one key share a stand-in, so preloading queues each key once: Kino's list's 851 recipes make some
// 270 stand-ins, and its zone's passes' 10,359 some 1,160 (EnumPass). Keys that differ may make the same stand-in yet,
// which MakeStandIn finds made.
static std::string StandInKey(const Recipe &r, const Program &vs, const Program &psh, const StandInRouting &rt)
{
    size_t nvs = std::min<size_t>(vs.info.samplers.size(), MAX_DRAW_SAMPLERS);
    uint32_t shadow = 0;
    for (size_t k = 0; k < psh.info.samplers.size() && nvs + k < MAX_DRAW_SAMPLERS; ++k)
        if (psh.info.samplers[k].stage == rt.shadowStage)
            shadow = r.sampler[nvs + k];
    Recipe base = r;
    base.ps = 0;
    base.tier = 0;
    memset(base.hints, 0, sizeof(base.hints));
    for (size_t i = nvs; i < MAX_DRAW_SAMPLERS; ++i)
        base.sampler[i] = 0;
    base.depthTarget = nvs ? r.depthTarget & ((1u << nvs) - 1) : 0;
    std::string key((const char *)&base, sizeof(base));
    key.append((const char *)&rt, sizeof(rt));
    key.append((const char *)&shadow, sizeof(shadow));
    return key;
}

// The draw's textures for a stand-in's samplers (bound, *n of them): the vertex shader's as the draw has them, the pixel
// shader's by stage, a filterable texture or none (the black stand-in). false: it can't draw it (a depth comparison with
// no depth texture).
static bool StandInBind(const Pipe &stand, const DrawSetup &w, const Derived &d, const PassTargets &targets, Bound *bound, int *n)
{
    *n = 0;
    auto target = [&](const Tex *t) {
        return t == targets.depthTex || t == targets.colorTex[0] || t == targets.colorTex[1] || t == targets.colorTex[2] || t == targets.colorTex[3];
    };
    int nvs = (int)std::min<size_t>(stand.vs->info.samplers.size(), (size_t)w.nbound);
    for (int i = 0; i < nvs; ++i)
    {
        Bound b = w.bound[i];
        b.kind = std::max<int>(d.kind[i], 0);
        if (d.kind[i] < 0 || (b.tex && target(b.tex)))
            b.tex = nullptr;
        if (b.kind == 2 && !b.tex)
            return false;
        bound[(*n)++] = b;
    }
    const StandInRouting &rt = *stand.ps->standIn;
    for (const ShaderInfo::Sampler &s : stand.ps->info.samplers)
    {
        if (*n == MAX_DRAW_SAMPLERS)
            break;
        Tex *t = BoundTexture(s.stage);
        if (s.stage == rt.shadowStage)
        {
            // the sun's shadow map: a depth texture to compare against, or the draw can't be drawn with this
            if (!t || t->type != TEX_2D || target(t) || !KeyFormat(t->fmt).depth)
                return false;
            bound[(*n)++] = {t, s.stage, (int)s.dim, 2, false};
            continue;
        }
        if (t)
        {
            TexFormat tf = KeyFormat(t->fmt);
            if (t->type != (s.dim == (uint8_t)DIM_3D ? TEX_3D : TEX_2D) || target(t) || tf.depth || !Filterable(tf))
                t = nullptr;
        }
        bound[(*n)++] = {t, s.stage, (int)s.dim, 0, false};
    }
    return true;
}

// the stand-in a draw of a compiling pipeline is drawn with, if it is ready and fits (bound, *n: its textures); made the
// first time the pipeline needs one
static Pipe *StandInFor(Pipe &pipe, const Recipe &r, const DrawSetup &w, const Derived &d, const PassTargets &targets, Bound *bound, int *n)
{
    if (!pipe.standTried)
    {
        pipe.standTried = true;
        pipe.stand = MakeStandIn(r, *pipe.vs, *pipe.ps, true);
    }
    Pipe *s = pipe.stand;
    if (!s)
        pipe.leftOutWhy = 1;
    else if (s->status == 0 || !s->handle)
        pipe.leftOutWhy = 2;
    else if (s->status < 0)
        pipe.leftOutWhy = 3;
    else if (!StandInBind(*s, w, d, targets, bound, n))
        pipe.leftOutWhy = 4;
    else
        return s;
    return nullptr;
}

// a stand-in's uniform block: the fix, and from the registers the draw's pass has them in (its hints), the light's
// diffuse colour and position (white, and nothing, where the pass has none), the model lighting cube's scale (nothing),
// the HDR scale and the hero scale (1), and the shadow map's far partition's place (nothing)
static uint32_t UploadStandInConstants(const StandInRouting &rt, const float fix[4], const uint8_t *h)
{
    uint8_t *p;
    uint32_t off = ArenaAlloc(112, 256, &p);
    memcpy(p, fix, 16);
    memset(p + 16, 0, 16);
    float *c = (float *)(p + 32);
    static const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f}, none[4] = {0.0f, 0.0f, 1.0f, 0.0f}, zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    auto ps = [&](int hint, const float *otherwise) { return h[hint] != 255 ? st.psF + h[hint] * 4 : otherwise; };
    const float *diffuse = rt.diffuseFrom == 1 ? ps(HINT_PS_LIGHT_DIFFUSE, white) : rt.diffuseFrom == 2 && h[HINT_VS_LIGHT_DIFFUSE] != 255 ? st.vsF + h[HINT_VS_LIGHT_DIFFUSE] * 4 : white;
    const float *position = rt.positionFrom == 1 ? ps(HINT_PS_LIGHT_POSITION, none) : rt.positionFrom == 2 && h[HINT_VS_LIGHT_POSITION] != 255 ? st.vsF + h[HINT_VS_LIGHT_POSITION] * 4 : none;
    memcpy(c, diffuse, 16);
    memcpy(c + 4, position, 16);
    memcpy(c + 8, ps(HINT_PS_LOOKUP_SCALE, zero), 16);
    c[12] = ps(HINT_PS_HDR, white)[0]; // the HDR scale's x and the hero scale's x
    c[13] = ps(HINT_PS_HERO_SCALE, white)[0];
    c[14] = c[15] = 0.0f;
    memcpy(c + 16, ps(HINT_PS_SHADOW_PARTITION, zero), 16);
    if (s_prof.on)
        s_prof.constantBytes += 112;
    return off;
}

// the waits for pipelines that ended: the first ones logged, and stats for the log every 10 s (&gpu=8). A wait lasts
// until the pipeline was ready, not until the next draw that finds it so, which may come seconds after (an effect drawn
// now and then).
static uint32_t s_waitsEnded, s_waitsStoodIn, s_waitsLeftOutWhy[5];
static double s_waitMs, s_waitMaxMs;

static void WaitEnded(Pipe &p, const Program &vs, const Program &psh)
{
    double ms = std::max(0.0, p.compileStart + p.compileMs - p.waitSince);
    p.waitSince = 0.0;
    ++s_waitsEnded;
    s_waitsStoodIn += p.waitStand;
    bool leftOut = !p.waitStand;
    s_waitsLeftOutWhy[leftOut ? std::min<int>(p.leftOutWhy, 4) : 0] += leftOut;
    s_waitMs += ms;
    s_waitMaxMs = std::max(s_waitMaxMs, ms);
    static int budget = 30, leftOutBudget = 20; // the first waits, and the first that ended with draws left out
    if (budget > 0 || (leftOut && leftOutBudget > 0))
    {
        if (budget > 0)
            --budget;
        else
            --leftOutBudget;
        static const char *const why[5] = {"left out", "left out: it has no stand-in", "left out: its stand-in was compiling too",
            "left out: its stand-in failed", "left out: its stand-in didn't fit the draw's textures"};
        const char *how = p.waitStand ? (p.hintFlags & HINT_READS_DEPTH ? "drawn with a stand-in (without the fade at the depth copy)" : "drawn with a stand-in")
                                      : why[std::min<int>(p.leftOutWhy, 4)];
        Bo1_Log("webgpu: a pipeline compiled in %.0f ms; draws (%s) waited %.0f ms for it, %s meanwhile (shaders %016llx %016llx)", p.compileMs,
            p.waitName, ms, how, (unsigned long long)vs.hash, (unsigned long long)psh.hash);
    }
    p.waitStand = false;
    p.leftOutWhy = 0;
}

// Everything a draw needs; false leaves it out. It reads vertices first .. first + count of each vertex buffer (instance
// data: `instances` of them). *shift: vertices the draw's first vertex goes further on (a stream bound where its buffer
// starts).
static bool PrepareDraw(int topology, bool indexed, bool index32, uint32_t upStride, uint32_t upOffset, uint32_t upBytes, int64_t first,
    int64_t count, uint32_t *instances, uint32_t *shift)
{
    *instances = 1;
    *shift = 0;
    bool check = (s_debugFlags & DBG_CHECK) != 0;
    bool ok = SetupKept(topology, indexed, index32, upStride);
    const DrawSetup &w = s_setup;
    static DrawSetup full; // &gpu=128's
    if (check && !CheckSetup(ok, w, topology, indexed, index32, upStride, full))
        check = false; // nothing further to compare
    if (!ok)
        return false;
    Program &vs = *w.vs;
    Program &psh = *w.ps;
    const PassTargets &targets = w.targets;
    const Recipe &r = w.recipe;
    const DWORD *rs = st.rs;

    // the pipeline (DerivedFor leaves it in s_lastDerived)
    static Derived fullDerived; // &gpu=128's
    if (!s_setupDerived || !s_lastValid)
    {
        Derived made;
        if (!DerivedFor(r, vs, psh, made))
        {
            if (check)
                CheckDerived(false, made, full, fullDerived);
            return false;
        }
        s_setupDerived = true;
        ++s_setupSerial;
    }
    const Derived &d = s_lastDerived;
    if (check && !CheckDerived(true, d, full, fullDerived))
        check = false;
    Pipe *pipe = d.pipe;
    if (!pipe->recorded)
    {
        pipe->recorded = true;
        s_usedRecipes.insert(s_usedRecipes.end(), (const uint8_t *)&r, (const uint8_t *)(&r + 1));
        // the first times a draw is left out for want of its pipeline: why it isn't ready (zombies-hub's
        // docker/pipelines.sh)
        static int budget = 30;
        Recipe listed = r;
        listed.tier = 2;
        listed.multisampled = 0;
        bool lacked = pipe->tier < 0 && !s_listed.count(std::string((const char *)&listed, sizeof(listed)));
        s_lacked += lacked;
        if (pipe->status == 0 && budget > 0)
        {
            --budget;
            const char *why = pipe->tier == 0 ? "from the map's list's tier 0, its shaders made after the player went in"
                              : !lacked ? "of the map's list's tier 2, which compiles as a draw needs it"
                                        : "which the map's list (web/pipelines/<map>.bin) lacks";
            Bo1_Log("webgpu: a draw (%s) waits for its pipeline, %s (shaders %016llx %016llx)", DrawName().c_str(), why,
                (unsigned long long)vs.hash, (unsigned long long)psh.hash);
        }
    }
    if (pipe->status == 0)
    {
        if (!pipe->waitSince)
        {
            pipe->waitSince = emscripten_get_now();
            snprintf(pipe->waitName, sizeof(pipe->waitName), "%s", DrawName().c_str());
        }
    }
    else if (pipe->waitSince)
        WaitEnded(*pipe, vs, psh);
    // compiling: drawn with its stand-in meanwhile, if that is ready and fits, else left out (failed: left out)
    const Pipe *drawn = pipe;
    Bound altBound[MAX_DRAW_SAMPLERS];
    int altBoundN = 0;
    bool standIn = false;
    s_capStandIn = false;
    if (pipe->status != 1)
    {
        if (pipe->status != 0)
            return false;
        ++s_frameWaits;
        s_prof.waits += s_prof.on;
        ++g_bo1PipelineWaits;
        s_firstTierWaits += pipe->tier == 0;
        drawn = StandInFor(*pipe, r, w, d, targets, altBound, &altBoundN);
        if (!drawn)
        {
            ++s_leftOutWhy[std::min<int>(pipe->leftOutWhy, 4)];
            return false;
        }
        standIn = true;
        s_capStandIn = true;
        ++s_frameStandIns;
        s_prof.standIns += s_prof.on;
        ++g_bo1PipelineStandIns;
        pipe->waitStand = true;
        check = false;
    }

    // the textures: a stand-in's by role (StandInBind); a texture the pass draws to: a copy of it, or the depth buffer
    // read only (it can't be copied)
    int nbound = altBoundN;
    const Bound *bound = altBound;
    PassKey passKey = w.passKey;
    bool snapshot = false;
    Bound copies[MAX_DRAW_SAMPLERS];
    if (!standIn)
    {
        if (s_planSerial != s_setupSerial)
        {
            PlanTextures(w, d, s_plan);
            s_planSerial = s_setupSerial;
        }
        if (check)
            CheckPlan(s_plan, full, fullDerived);
        nbound = s_plan.nbound;
        bound = s_plan.bound;
        passKey = s_plan.passKey;
        snapshot = s_plan.snapshots != 0;
        if (snapshot)
        {
            memcpy(copies, s_plan.bound, nbound * sizeof(Bound));
            for (int i = 0; i < nbound; ++i)
                if (s_plan.snapshots >> i & 1)
                {
                    copies[i].tex = Snapshot(*copies[i].tex);
                    s_prof.snapshots += s_prof.on;
                }
            bound = copies;
        }
    }

    // constants (the vertex shader's half pixel follows the viewport; the pixel shader's alpha reference)
    float vsFix[4] = {st.vp.width ? 1.0f / st.vp.width : 0.0f, st.vp.height ? -1.0f / st.vp.height : 0.0f, 0.0f, 0.0f};
    float psFix[4] = {0.0f, 0.0f, (rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f, 0.0f};
    if (s_lastVs != &vs.info || s_vsUploadSerial != s_serial || s_vsUploadGen != st.vsGen || s_vsUploadFix[0] != vsFix[0] ||
        s_vsUploadFix[1] != vsFix[1])
    {
        uint32_t prev = s_vsUploadSerial == s_serial && !(s_debugFlags & DBG_PLAIN) ? s_vsUpload : ~0u;
        s_vsBlock = vs.info.UniformBytes();
        s_vsUpload = UploadConstants(vs.info, st.vsF, st.vsI, st.vsB, vsFix, s_byInstance ? s_vsBlock : 256, prev, &s_vsUploadBytes);
        s_prof.vsUploads += s_prof.on && s_vsUpload != prev;
        s_lastVs = &vs.info;
        s_vsUploadSerial = s_serial;
        s_vsUploadGen = st.vsGen;
        s_vsUploadFix[0] = vsFix[0];
        s_vsUploadFix[1] = vsFix[1];
    }
    if (s_lastPs != &psh.info || s_psUploadSerial != s_serial || s_psUploadGen != st.psGen || s_psUploadRef != psFix[2])
    {
        uint32_t prev = s_psUploadSerial == s_serial && !(s_debugFlags & DBG_PLAIN) ? s_psUpload : ~0u;
        s_psUpload = UploadConstants(psh.info, st.psF, st.psI, st.psB, psFix, 256, prev, &s_psUploadBytes);
        s_prof.psUploads += s_prof.on && s_psUpload != prev;
        s_lastPs = &psh.info;
        s_psUploadSerial = s_serial;
        s_psUploadGen = st.psGen;
        s_psUploadRef = psFix[2];
    }
    uint32_t psUpload = s_psUpload;
    if (standIn)
        psUpload = UploadStandInConstants(*drawn->ps->standIn, psFix, r.hints);

    // the texture bind group: the last draw's, unless what it reads changed (a copy of a target drawn to: another each time)
    uint32_t group = 0;
    if (standIn)
    {
        if (drawn->groupLayout)
            group = TextureGroup(drawn->groupLayout, bound, nbound, true);
    }
    else if (d.groupLayout)
    {
        if (!snapshot && !(s_dirty.group & READS_GROUP) && s_keptGroup.layout == d.groupLayout && s_keptGroup.n == nbound &&
            !memcmp(s_keptGroup.kind, d.kind, nbound))
            group = s_keptGroup.handle;
        else
        {
            group = TextureGroup(d.groupLayout, bound, nbound, true);
            s_prof.workedGroups += s_prof.on;
            s_keptGroup.layout = 0;
            if (!snapshot)
            {
                s_keptGroup.handle = group;
                s_keptGroup.layout = d.groupLayout;
                s_keptGroup.n = nbound;
                memcpy(s_keptGroup.kind, d.kind, nbound);
                s_dirty.group = 0;
            }
        }
        if (check && group != TextureGroup(d.groupLayout, bound, nbound, false))
            Differs("its texture bind group");
    }

    // vertex buffers
    const VertexLayout &l = s_layouts[d.vertexLayout];
    uint32_t vbuf[9], voff[9], vsize[9];
    Buf *vb[9];
    for (int s = 0; s < l.slots; ++s)
    {
        vb[s] = nullptr;
        if (l.stream[s] < 0)
        {
            vbuf[s] = s_zeroBuffer;
            voff[s] = 0;
            vsize[s] = 16;
            continue;
        }
        if (upStride && l.stream[s] == 0)
        {
            vbuf[s] = s_arenaGpu;
            voff[s] = upOffset;
            vsize[s] = upBytes;
            continue;
        }
        const StreamState &ss = st.stream[l.stream[s] & 15];
        Buf *b = StreamBuffer(l.stream[s] & 15);
        if (check && b != FindBuffer(ss.id))
            Differs("a stream's vertex buffer");
        if (!b || ss.offset >= b->alloc)
            return false;
        if (ss.offset & 3)
        {
            BO1_GPU_ERROR("stream %d at offset %u, not on 4 bytes", l.stream[s], ss.offset);
            return false;
        }
        vb[s] = b;
        vbuf[s] = b->gpu;
        voff[s] = ss.offset;
    }
    if (st.freq[0] & D3DSTREAMSOURCE_INDEXEDDATA)
        *instances = std::max<uint32_t>(1, st.freq[0] & 0x3FFFFFFF);
    if (s_byInstance)
    {
        // the first instance is the constants' place, for one instance (the engine draws no more: it sets no stream's
        // frequency)
        bool instanced = *instances > 1;
        for (int s = 0; s < l.slots; ++s)
            instanced = instanced || l.instance[s];
        if (instanced)
        {
            BO1_GPU_ERROR("a draw of %u instances is not supported (&gpu=1024 draws it)", *instances);
            return false;
        }
    }
    // A draw's one stream of vertices is bound where it starts in its GPU buffer, as far on in it as whole vertices
    // allow, and the draw's first vertex goes the rest of the way. The world's surfaces each start further on in one
    // buffer, the static models' and skinned models' caches hold many models each, and static buffers share pools: bound
    // once, not for each draw. Several streams are bound where they are: one first vertex fits them all only by chance.
    uint32_t boff[9];
    int vertexStreams = 0, at = -1;
    for (int s = 0; s < l.slots; ++s)
    {
        boff[s] = voff[s] + (vb[s] ? vb[s]->base : 0);
        if (vb[s] && !l.instance[s] && l.stride[s])
        {
            ++vertexStreams;
            at = s;
        }
    }
    if (vertexStreams == 1 && !upStride && !(s_debugFlags & DBG_PLAIN))
    {
        *shift = boff[at] / l.stride[at];
        boff[at] -= *shift * l.stride[at];
    }
    // bound to the end of the GPU buffer: a binding the next draw finds already made (the same buffer from the same
    // offset) serves it, so what it takes in can't depend on the draw. Bound to the end of a pooled buffer's own part, a
    // model further on in its pool would be past it: Direct3D 12 reads zeros there, and the model is gone.
    for (int s = 0; s < l.slots; ++s)
        if (vb[s])
            vsize[s] = vb[s]->gpuSize - boff[s];

    // the pass and its state
    if (!EnsurePass(passKey))
        return false;
    if (ps.pipeline != drawn->handle)
    {
        bo1gpu_set_pipeline(drawn->handle);
        ps.pipeline = drawn->handle;
        PROF(pipelines);
    }
    // the vertex shader's block: where the draw's first instance says (FirstInstance), or bound here too
    if (ps.group0 != s_arenaGroup || ps.psOffset != psUpload || (!s_byInstance && ps.vsOffset != s_vsUpload))
    {
        uint32_t offsets[2] = {s_vsUpload, psUpload};
        if (s_byInstance)
            bo1gpu_set_bind_group(0, s_arenaGroup, offsets + 1, 1);
        else
            bo1gpu_set_bind_group(0, s_arenaGroup, offsets, 2);
        PROF(constants);
        ps.group0 = s_arenaGroup;
        ps.vsOffset = s_vsUpload;
        ps.psOffset = psUpload;
    }
    if (group && ps.group1 != group)
    {
        bo1gpu_set_bind_group(1, group, nullptr, 0);
        PROF(textures);
        ps.group1 = group;
    }
    if (s_prof.on && !s_prof.passes.empty())
    {
        bool any = false, newBuffer = false, shift = true;
        int64_t k = -1;
        for (int s = 0; s < l.slots; ++s)
        {
            any = any || ps.vbuf[s] != vbuf[s] || ps.voff[s] != boff[s] || ps.vsize[s] != vsize[s];
            if (ps.vbuf[s] != vbuf[s])
            {
                newBuffer = true;
                continue;
            }
            int64_t delta = (int64_t)boff[s] - (int64_t)ps.voff[s];
            if (!l.stride[s])
                continue;
            if (l.instance[s])
                shift = shift && !delta;
            else if (delta < 0 || delta % l.stride[s] || (k >= 0 && k != delta / l.stride[s]))
                shift = false;
            else
                k = delta / l.stride[s];
        }
        ProfPass &q = s_prof.passes.back();
        q.vbDraws += any;
        q.vbNewBuffer += any && newBuffer;
        q.vbShift += any && !newBuffer && shift;
        q.instanced += *instances > 1;
    }
    for (int s = 0; s < l.slots; ++s)
        if (ps.vbuf[s] != vbuf[s] || ps.voff[s] != boff[s] || ps.vsize[s] != vsize[s])
        {
            bo1gpu_set_vertex_buffer(s, vbuf[s], boff[s], vsize[s]);
            PROF(vertexBuffers);
            ps.vbuf[s] = vbuf[s];
            ps.voff[s] = boff[s];
            ps.vsize[s] = vsize[s];
        }
    float vp[6] = {(float)st.vp.x, (float)st.vp.y, (float)st.vp.width, (float)st.vp.height, st.vp.minZ, st.vp.maxZ};
    if (vp[0] + vp[2] > targets.w || vp[1] + vp[3] > targets.h)
    {
        BO1_GPU_ERROR("viewport %g,%g %gx%g outside the %ux%u render target", vp[0], vp[1], vp[2], vp[3], targets.w, targets.h);
        vp[2] = std::min(vp[2], (float)targets.w - std::min(vp[0], (float)targets.w));
        vp[3] = std::min(vp[3], (float)targets.h - std::min(vp[1], (float)targets.h));
    }
    if (vp[2] <= 0.0f || vp[3] <= 0.0f)
        return false;
    vp[4] = std::min(std::max(vp[4], 0.0f), 1.0f);
    vp[5] = std::min(std::max(vp[5], vp[4]), 1.0f);
    if (memcmp(ps.viewport, vp, sizeof(vp)))
    {
        bo1gpu_set_viewport(vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
        PROF(viewports);
        memcpy(ps.viewport, vp, sizeof(vp));
    }
    int sc[4] = {0, 0, (int)targets.w, (int)targets.h};
    if (rs[D3DRS_SCISSORTESTENABLE])
    {
        int x0 = std::max(0, std::min((int)targets.w, st.scissor.left)), y0 = std::max(0, std::min((int)targets.h, st.scissor.top));
        int x1 = std::max(x0, std::min((int)targets.w, st.scissor.right)), y1 = std::max(y0, std::min((int)targets.h, st.scissor.bottom));
        sc[0] = x0;
        sc[1] = y0;
        sc[2] = x1 - x0;
        sc[3] = y1 - y0;
    }
    if (memcmp(ps.scissor, sc, sizeof(sc)))
    {
        bo1gpu_set_scissor(sc[0], sc[1], sc[2], sc[3]);
        PROF(scissors);
        memcpy(ps.scissor, sc, sizeof(sc));
    }
    if (d.stencilMasks && ps.stencilRef != (int)(rs[D3DRS_STENCILREF] & 0xFF))
    {
        ps.stencilRef = (int)(rs[D3DRS_STENCILREF] & 0xFF);
        bo1gpu_set_stencil_ref(ps.stencilRef);
        PROF(other);
    }
    if (d.blend && (BlendFactorUsesConstant(d.blend >> 1 & 31) || BlendFactorUsesConstant(d.blend >> 6 & 31) ||
                       BlendFactorUsesConstant(d.blend >> 16 & 31) || BlendFactorUsesConstant(d.blend >> 21 & 31)))
    {
        DWORD bf = rs[D3DRS_BLENDFACTOR];
        float c[4] = {((bf >> 16) & 0xFF) / 255.0f, ((bf >> 8) & 0xFF) / 255.0f, (bf & 0xFF) / 255.0f, (bf >> 24) / 255.0f};
        if (memcmp(ps.blend, c, sizeof(c)))
        {
            bo1gpu_set_blend_constant(c[0], c[1], c[2], c[3]);
            PROF(other);
            memcpy(ps.blend, c, sizeof(c));
        }
    }

    // what this encoder reads
    for (int i = 0; i < nbound; ++i)
        if (bound[i].tex)
            bound[i].tex->used = s_serial;
    for (int s = 0; s < l.slots; ++s)
    {
        if (!vb[s])
            continue;
        int64_t stride = l.stride[s] ? l.stride[s] : 16;
        int64_t lo = voff[s], hi = lo + (int64_t)*instances * stride;
        if (!l.instance[s])
        {
            lo += std::max<int64_t>(0, first) * stride;
            hi = lo + count * stride;
        }
        UseBuffer(*vb[s], (uint64_t)lo, (uint64_t)hi);
        // what is bound holds what the draw may read of the buffer: past a binding, Direct3D 12 reads zeros (SwiftShader,
        // which the tests draw with, reads on to the end of the buffer)
        uint64_t start = vb[s]->base + voff[s], end = vb[s]->base + std::min<uint64_t>((uint64_t)std::max<int64_t>(hi, 0), vb[s]->alloc);
        if (start < ps.voff[s] || end > (uint64_t)ps.voff[s] + ps.vsize[s])
            BO1_GPU_ERROR("stream %d reads bytes %llu..%llu of its GPU buffer, %u..%u bound", l.stream[s], (unsigned long long)start,
                (unsigned long long)end, ps.voff[s], ps.voff[s] + ps.vsize[s]);
    }
    return true;
}

static int Topology(uint32_t type, uint32_t primCount, uint32_t *count)
{
    switch (type)
    {
    case D3DPT_POINTLIST:
        *count = primCount;
        return 0;
    case D3DPT_LINELIST:
        *count = primCount * 2;
        return 1;
    case D3DPT_LINESTRIP:
        *count = primCount + 1;
        return 2;
    case D3DPT_TRIANGLESTRIP:
        *count = primCount + 2;
        return 4;
    case D3DPT_TRIANGLEFAN:
        *count = primCount * 3; // as a list (FanIndices)
        return 3;
    default:
        *count = primCount * 3;
        return 3;
    }
}

// a fan's triangles as a list of indices, in the arena
static uint32_t FanIndices(uint32_t primCount)
{
    uint8_t *p;
    uint32_t off = ArenaAlloc(primCount * 3 * 4, 4, &p);
    if (s_prof.on)
        s_prof.upBytes += primCount * 3 * 4;
    uint32_t *idx = (uint32_t *)p;
    for (uint32_t i = 0; i < primCount; ++i)
    {
        idx[i * 3] = 0;
        idx[i * 3 + 1] = i + 1;
        idx[i * 3 + 2] = i + 2;
    }
    return off;
}

// the first instance of the draw PrepareDraw set up: where its vertex shader's block is in the arena, in blocks of its size
static uint32_t FirstInstance() { return s_byInstance ? s_vsUpload / s_vsBlock : 0; }

// a draw's line of the capture (&gpu=4). firstIndex (-1: none), count: an indexed draw's indices as WebGPU gets them,
// logged with its first vertex and first instance: draws that go as one have the same indices and first vertex, and
// first instances that follow each other (library_gpu.js: bo1gpu_draw_indexed)
static void CaptureDraw(const char *kind, uint32_t primType, uint32_t primCount, int baseVertex, bool drawn, int64_t firstIndex = -1,
    uint32_t count = 0, int firstVertex = 0)
{
    if (!s_capturing)
        return;
    char where[64] = "";
    if (firstIndex >= 0 && drawn)
        snprintf(where, sizeof(where), " idx%lld+%u v%d inst%u", (long long)firstIndex, count, firstVertex, FirstInstance());
    const DWORD *rs = st.rs;
    char texs[512];
    int n = 0;
    texs[0] = 0;
    auto psIt = s_shaders.find(st.ps);
    if (psIt != s_shaders.end())
        for (const ShaderInfo::Sampler &smp : psIt->second.prog->info.samplers)
        {
            Tex *t = BoundTexture(smp.stage);
            if (t)
                n += snprintf(texs + n, sizeof(texs) - n, " s%d=%u(%ux%u f%u l%u)", smp.stage, st.tex[smp.stage], t->w, t->h, t->fmt, t->levels);
            else
                n += snprintf(texs + n, sizeof(texs) - n, " s%d=%u(missing)", smp.stage, st.tex[smp.stage]);
            if (n >= (int)sizeof(texs) - 64)
                break;
        }
    auto dIt = s_decls.find(st.decl);
    if (dIt != s_decls.end())
        for (const D3DVERTEXELEMENT9 &e : dIt->second.el)
        {
            if (e.Stream == 0xFF)
                break;
            n += snprintf(texs + n, sizeof(texs) - n, " e%u.%u@%u:%u/u%u.%u", e.Stream, e.Type, e.Offset, e.Method, e.Usage, e.UsageIndex);
            if (n >= (int)sizeof(texs) - 64)
                break;
        }
    auto vsIt = s_shaders.find(st.vs);
    uint32_t vsProg = vsIt != s_shaders.end() && vsIt->second.prog ? vsIt->second.prog->id : 0;
    uint32_t psProg = psIt != s_shaders.end() && psIt->second.prog ? psIt->second.prog->id : 0;
    Bo1_Log("cap %s%s vs%u ps%u (modules vs%u ps%u) prim%u n%u bv%d rt%u:%u ds%u:%u vp%d,%d,%ux%u,%g-%g z%u/%u/%u bias%g/%g cull%u blend%u(%u,%u) cw%x at%u st%u "
            "decl%u%s%s (%s)",
        drawn ? (s_capStandIn ? "STAND-IN " : "") : "SKIP ", kind, st.vs, st.ps, vsProg, psProg, primType, primCount, baseVertex, st.rt[0].kind, st.rt[0].id, st.ds.kind, st.ds.id, st.vp.x,
        st.vp.y, st.vp.width, st.vp.height, st.vp.minZ, st.vp.maxZ, rs[D3DRS_ZENABLE], rs[D3DRS_ZWRITEENABLE], rs[D3DRS_ZFUNC],
        AsFloat(rs[D3DRS_DEPTHBIAS]), AsFloat(rs[D3DRS_SLOPESCALEDEPTHBIAS]), rs[D3DRS_CULLMODE], rs[D3DRS_ALPHABLENDENABLE],
        rs[D3DRS_SRCBLEND], rs[D3DRS_DESTBLEND], rs[D3DRS_COLORWRITEENABLE], rs[D3DRS_ALPHATESTENABLE] ? rs[D3DRS_ALPHAFUNC] : 0,
        rs[D3DRS_STENCILENABLE], st.decl, texs, where, DrawName().c_str());
}

static const uint32_t DRAW_RESERVE = 2 * (UNIFORM_BINDING + 256);

static void Draw(const CmdDraw &c)
{
    uint32_t count, instances, shift;
    int topology = Topology(c.primType, c.primCount, &count);
    bool fan = c.primType == D3DPT_TRIANGLEFAN;
    ArenaReserve(DRAW_RESERVE + (fan ? count * 4 : 0));
    if (!count || !PrepareDraw(topology, fan, true, 0, 0, 0, c.startVertex, fan ? c.primCount + 2 : count, &instances, &shift))
    {
        ++s_frameSkips;
        CaptureDraw("draw", c.primType, c.primCount, 0, false);
        return;
    }
    CaptureDraw("draw", c.primType, c.primCount, 0, true);
    OcclusionDraw();
    if (fan)
    {
        uint32_t off = FanIndices(c.primCount);
        bo1gpu_set_index_buffer(s_arenaGpu, 1, off, count * 4);
        PROF(indexBuffers);
        ps.ibuf = 0;
        bo1gpu_draw_indexed(count, instances, 0, c.startVertex + shift, FirstInstance(), 0);
    }
    else
        bo1gpu_draw(count, instances, c.startVertex + shift, FirstInstance());
    ++s_frameDraws;
    PROF(draws);
}

static void DrawIndexed(const CmdDrawIndexed &c)
{
    Buf *ib = IndexBuffer();
    if ((s_debugFlags & DBG_CHECK) && ib != FindBuffer(st.indices))
        Differs("its index buffer");
    if (!ib)
        return;
    Buf &b = *ib;
    uint32_t count, instances, shift;
    int topology = Topology(c.primType, c.primCount, &count);
    ArenaReserve(DRAW_RESERVE);
    if (c.primType == D3DPT_TRIANGLEFAN)
    {
        BO1_GPU_ERROR("indexed triangle fans are not supported (%u)", c.primCount);
        return;
    }
    uint32_t isz = b.index32 ? 4 : 2;
    if (!count || (uint64_t)(c.startIndex + count) * isz > b.size ||
        !PrepareDraw(topology, true, b.index32, 0, 0, 0, (int64_t)c.baseVertex + c.minIndex, c.numVertices, &instances, &shift))
    {
        ++s_frameSkips;
        CaptureDraw("indexed", c.primType, c.primCount, c.baseVertex, false);
        return;
    }
    CaptureDraw("indexed", c.primType, c.primCount, c.baseVertex, true, c.startIndex + b.base / isz, count, c.baseVertex + (int32_t)shift);
    OcclusionDraw();
    if (ps.ibuf != b.gpu || ps.index32 != b.index32)
    {
        bo1gpu_set_index_buffer(b.gpu, b.index32, 0, b.gpuSize);
        PROF(indexBuffers);
        ps.ibuf = b.gpu;
        ps.index32 = b.index32;
    }
    UseBuffer(b, (uint64_t)c.startIndex * isz, (uint64_t)(c.startIndex + count) * isz);
    bo1gpu_draw_indexed(count, instances, c.startIndex + b.base / isz, c.baseVertex + (int32_t)shift, FirstInstance(), topology == 3);
    ++s_frameDraws;
    PROF(draws);
}

static void DrawUp(const CmdDrawUp &c)
{
    uint32_t count, instances, shift;
    int topology = Topology(c.primType, c.primCount, &count);
    bool fan = c.primType == D3DPT_TRIANGLEFAN;
    ArenaReserve(DRAW_RESERVE + c.vertexBytes + 4 + (fan ? count * 4 : 0));
    uint8_t *p;
    uint32_t off = ArenaAlloc(c.vertexBytes, 4, &p);
    memcpy(p, c.vertices, c.vertexBytes);
    free(c.vertices);
    if (s_prof.on)
        s_prof.upBytes += c.vertexBytes;
    if (!count || !c.stride || (c.stride & 3) || !PrepareDraw(topology, fan, true, c.stride, off, c.vertexBytes, 0, 0, &instances, &shift))
    {
        ++s_frameSkips;
        CaptureDraw("up", c.primType, c.primCount, 0, false);
        return;
    }
    CaptureDraw("up", c.primType, c.primCount, 0, true);
    OcclusionDraw();
    if (fan)
    {
        uint32_t ioff = FanIndices(c.primCount);
        bo1gpu_set_index_buffer(s_arenaGpu, 1, ioff, count * 4);
        PROF(indexBuffers);
        ps.ibuf = 0;
        bo1gpu_draw_indexed(count, 1, 0, 0, FirstInstance(), 0);
    }
    else
        bo1gpu_draw(count, 1, 0, FirstInstance());
    ++s_frameDraws;
    PROF(draws);
}

static void DrawIndexedUp(const CmdDrawIndexedUp &c)
{
    uint32_t count, instances, shift;
    int topology = Topology(c.primType, c.primCount, &count);
    ArenaReserve(DRAW_RESERVE + c.vertexBytes + c.indexBytes + 8);
    uint8_t *p;
    uint32_t voff = ArenaAlloc(c.vertexBytes, 4, &p);
    memcpy(p, c.vertices, c.vertexBytes);
    uint32_t ioff = ArenaAlloc((c.indexBytes + 3) & ~3u, 4, &p);
    memcpy(p, c.indices, c.indexBytes);
    free(c.vertices);
    free(c.indices);
    if (s_prof.on)
        s_prof.upBytes += c.vertexBytes + c.indexBytes;
    if (!count || c.primType == D3DPT_TRIANGLEFAN || !c.stride || (c.stride & 3) ||
        !PrepareDraw(topology, true, c.index32 != 0, c.stride, voff, c.vertexBytes, 0, 0, &instances, &shift))
    {
        ++s_frameSkips;
        CaptureDraw("indexed up", c.primType, c.primCount, 0, false);
        return;
    }
    CaptureDraw("indexed up", c.primType, c.primCount, 0, true);
    OcclusionDraw();
    bo1gpu_set_index_buffer(s_arenaGpu, c.index32, ioff, (c.indexBytes + 3) & ~3u);
    PROF(indexBuffers);
    ps.ibuf = 0;
    bo1gpu_draw_indexed(count, 1, 0, 0, FirstInstance(), 0);
    ++s_frameDraws;
    PROF(draws);
}

// ============================================================================================================
// preloading: pipelines from the list's recipes and the zone's passes
// ============================================================================================================

static std::vector<Recipe> s_recipes;                                 // the map's list (web/pipelines/<map>.bin), then the zone's passes' recipes
static std::unordered_multimap<uint64_t, uint32_t> s_recipesByShader; // Program::hash -> the recipes that need it
// The list's pipelines are made in tiers: tier 0, the spawn's recipes; tier 1, the stand-ins of every recipe of the list
// (stand-ins), so that a draw whose pipeline is not ready has one; and, once the player went in (the preload fence),
// the zone's passes' stand-ins (tier 3, EnumPass). The game waits for tiers 0 and 1 before the player goes in
// (Bo1_PreloadPending). For tier 1 as well: where pipelines compile slowly, a game that went in on tier 0 alone drew
// what the spawn doesn't show with nothing until its stand-in or its pipeline was compiled. A player's first load on a
// PC compiled 3.7 pipelines a second (each 8.5 s in the making, 32 at a time): 53 s for tier 0, and then zombies and
// furniture black for 20 s, and their hands and gun black under a light the spawn has none of. While the map loads,
// tier 0 goes first, and tier 1 once it is compiled, as a device that compiles them as fast as it loads has both by
// the fence. From the fence on, tier 1 goes first: the game goes in without the rest after two minutes
// (CL_PreloadPipelines), and what it lacks then is drawn with stand-ins. The rest of the list (tier 2) is not compiled
// ahead: its 555 pipelines of Kino's took Chrome's GPU process 570 MB, and a draw of one is drawn with its stand-in
// while its pipeline compiles, as one the list lacks. Tier 0 compiles 32 at a time, and so does tier 1 from the fence
// on; before it, and tier 3, 8 (tier 3 32 too where it is waited for, with no list): compiling is latency more than
// work, but Chrome's compiles take the processor from the game's loading, and 32 at a time through all the list's
// tiers held the load up (Kino's player went in at 9.7 s against 7.5); a pipeline a draw needs starts at once whatever
// is compiling.
static std::vector<uint32_t> s_requiredQueue; // tier 0's recipes whose shaders are made, to make pipelines of
static std::vector<uint32_t> s_requiredFirstQueue; // those of them with no stand-in (the scene's readers, the depth as colour, the sky), first
static std::vector<uint32_t> s_standInQueue;  // tier 1's: the list's recipes whose stand-ins are to be made, one of each
static std::unordered_set<std::string> s_standInKeys; // the stand-ins queued, the list's and the zone's (StandInKey)
static std::vector<uint32_t> s_enumQueue;     // tier 3's: the zone's passes' recipes (EnumPass), for their stand-ins
static std::vector<uint32_t> s_enumFirstQueue; // tier 3's passes with no stand-in (the scene's readers), for their pipelines, first of the tier
static int s_recipesState;               // 0: not asked for, 1: on the way, 2: here (or none)
static uint32_t s_compiling[4];          // pipelines of each tier still compiling
static uint32_t s_listCount;             // the list's recipes
static bool s_noList;                    // the map has no list (or one of another build): the zone's stand-ins are waited for
static std::atomic<int> s_listState;     // Bo1_PipelineListState's: 0 on the way, 1 here, 2 none
static double s_noticeSince, s_noticeAt; // the page's notice of the wait: since when there is one, when last given
static uint32_t s_noticeLeft;            // what it last said was left
static double s_wentInAt;                // when the game went in (the preload fence), for the log of the list's end
static double s_waitEndedAt;             // when what the player waited for after the fence was compiled (0: not yet)
// for what the page says of a long wait (SayWaited): what was left to compile at the fence and how many of those were
// the list's stand-ins, the list's tier 0, and the page's turns during the wait, its animation frames and a hidden
// page's wakes (Replay)
static uint32_t s_waitLeftAtFence, s_waitStandInsAtFence, s_listRequired, s_waitFrames, s_waitWakes;
static bool s_waitSaid;
static bool s_listDone;
static const uint32_t PRELOAD_AT_ONCE = 32; // tier 0's pipelines compiling at a time, and tier 3's where they are waited for (no list)
// the other tiers': fewer, as 32 of them at a time held the load up (Chrome's compiles take the processor as much as the
// game's loading: Kino's player went in at 9.7 s against 7.5, and spawned 1.5 s later than the movie's skip against 0.3;
// tier 1 at 32 put Ascension's zone in 1.4 s later)
static const uint32_t LATER_AT_ONCE = 8;

// what the game waits for before the player goes in (Bo1_PreloadPending): tier 0's and tier 1's recipes to make
// pipelines of, those still compiling, the list itself while it is on the way, and with no list the zone's (tier 3)
static void NotePending()
{
    g_bo1PreloadPending.store((uint32_t)(s_requiredFirstQueue.size() + s_requiredQueue.size() + s_standInQueue.size()) + s_compiling[0] +
                                  s_compiling[1] +
                                  (s_recipesState == 1 ? 1u : 0u) +
                                  (s_noList ? (uint32_t)(s_enumFirstQueue.size() + s_enumQueue.size()) + s_compiling[3] : 0u),
        std::memory_order_release);
}

static bool Preloadable(const Recipe &r) { return s_programsByHash.count(r.vs) && s_programsByHash.count(r.ps); }

// a list's recipe's stand-in (tier 1), queued unless its pass has none, or a recipe before it queued the same
// (StandInKey); its shaders are made (Preloadable). Whether its pass has one.
static bool QueueStandIn(uint32_t at)
{
    const Recipe &r = s_recipes[at];
    const Program &vs = *s_programsByHash.find(r.vs)->second, &psh = *s_programsByHash.find(r.ps)->second;
    if (!vs.info.error.empty() || !psh.info.error.empty() || vs.info.pixel || !psh.info.pixel)
        return false;
    StandInRouting rt;
    if (StandInRoute(r, vs, psh, rt))
        return false;
    if (s_standInKeys.insert(StandInKey(r, vs, psh, rt)).second)
        s_standInQueue.push_back(at);
    return true;
}

static void QueueRecipe(uint32_t at)
{
    uint8_t tier = s_recipes[at].tier;
    if (tier == 3)
    {
        s_enumQueue.push_back(at);
        return;
    }
    // tier 2's stand-in too, whose pipeline is made as draws need it; a tier 0 pipeline with none goes before the
    // stand-ins while the player waits (Preload): nothing draws in its place
    bool standIn = QueueStandIn(at);
    if (tier == 0)
        (standIn ? s_requiredQueue : s_requiredFirstQueue).push_back(at);
}

// a program was made: the recipes it completes
static void QueueRecipes(const Program &prog)
{
    auto range = s_recipesByShader.equal_range(prog.hash);
    for (auto it = range.first; it != range.second; ++it)
        if (Preloadable(s_recipes[it->second]))
            QueueRecipe(it->second);
    NotePending();
}

// the list: a header (magic, version, record size), then the records. A record that can't be a draw's is left out.
static void TakeRecipes(const uint8_t *data, size_t len)
{
    uint32_t h[3];
    if (len < sizeof(h))
        return;
    memcpy(h, data, sizeof(h));
    bool v2 = h[1] == 2 && h[2] == RECIPE_V2_SIZE; // a list made before the hints grew: its stand-ins know no model lighting
    if (h[0] != RECIPE_MAGIC || !(v2 || (h[1] == RECIPE_VERSION && h[2] == sizeof(Recipe))))
    {
        Bo1_Log("webgpu: the pipeline list is of another build (version %u, %u bytes a recipe): "
                "zombies-hub's docker/pipelines.sh makes it", h[1], h[2]);
        return;
    }
    size_t size = v2 ? RECIPE_V2_SIZE : sizeof(Recipe);
    size_t n = (len - sizeof(h)) / size, required = 0, sceneReaders = 0, others = 0;
    s_listCount = (uint32_t)n;
    // the spawn draws with other pipelines where shadow maps are off (other lit techniques, and no shadow map's passes):
    // of the spawn's recipes, those of the player's setting are compiled before going in, and the other setting's are
    // of the rest (a list made before the two were told apart has one kind, drawn with shadow maps on)
    const bool shadowMaps = g_bo1ShadowMaps.load(std::memory_order_acquire) != 0;
    for (size_t i = 0; i < n; ++i)
    {
        Recipe r;
        memset(&r, 255, sizeof(r));
        memcpy(&r, data + sizeof(h) + i * size, size);
        const VertexLayout &l = r.layout;
        bool ok = l.slots <= 9 && l.attrs <= 16 && r.topology <= 4 && r.strip <= 2;
        for (int s = 0; ok && s < l.slots; ++s)
            ok = l.stream[s] >= -1 && l.stream[s] < 16;
        for (int a = 0; ok && a < l.attrs; ++a)
            ok = l.attr[a].format <= VF_FLOAT16X4 && l.attr[a].slot < l.slots && l.attr[a].location < 16;
        if (!ok)
            continue;
        others += r.tier == (shadowMaps ? TIER_NO_SHADOWS : TIER_SHADOWS);
        // the list's: 0 the spawn's, at the player's shadow maps setting; else the rest (2; lists made before the
        // stand-ins' tier say 1)
        r.tier = !r.tier || r.tier == (shadowMaps ? TIER_SHADOWS : TIER_NO_SHADOWS) ? 0 : 2;
        r.multisampled = r.multisampled ? 1 : 0;
        if (r.tier)
        {
            Recipe listed = r;
            listed.multisampled = 0;
            s_listed.insert(std::string((const char *)&listed, sizeof(listed)));
        }
        // a pass that reads the scene's colour copy (distortion, post effects) has no stand-in (stand-ins), so its draws
        // would be left out until its pipeline is ready: compiled before going in
        if (r.hints[HINT_FLAGS] & HINT_READS_SCENE)
        {
            sceneReaders += r.tier != 0;
            r.tier = 0;
        }
        required += !r.tier;
        uint32_t at = (uint32_t)s_recipes.size();
        s_recipes.push_back(r);
        s_recipesByShader.emplace(r.vs, at);
        if (r.ps != r.vs)
            s_recipesByShader.emplace(r.ps, at);
        if (Preloadable(r))
            QueueRecipe(at);
    }
    s_listRequired = (uint32_t)required;
    Bo1_Log("webgpu: %zu pipeline recipes, %zu of them compiled before going in (tier 0, the spawn's with shadow maps %s; %zu of those read the "
            "scene's colour copy, moved up from tier 2; %zu more are the spawn's with shadow maps %s), with the stand-ins of all of them (tier 1)",
        s_recipes.size(), required, shadowMaps ? "on" : "off", sceneReaders, others, shadowMaps ? "off" : "on");
}

static void Notice(const char *text)
{
    EM_ASM({
        const f = Module['bo1Status'];
        if (f)
            f('status', UTF8ToString($0));
    }, text);
}

// a recipe's pipeline, if its shaders can make one; true: it started compiling. One made already (by a draw, or from
// another recipe), ready or compiling, takes no place among those compiling at a time (Preload).
static bool PreloadRecipe(uint32_t at)
{
    const Recipe &r = s_recipes[at];
    auto v = s_programsByHash.find(r.vs), p = s_programsByHash.find(r.ps);
    if (v == s_programsByHash.end() || p == s_programsByHash.end())
        return false;
    Program &vs = *v->second, &psh = *p->second;
    if (!vs.info.error.empty() || !psh.info.error.empty() || vs.info.pixel || !psh.info.pixel)
        return false;
    size_t had = s_pipelines.size();
    Derived d;
    return MakePipeline(r, vs, psh, false, d) && s_pipelines.size() > had && d.pipe->status == 0;
}

// a recipe's stand-in pipeline (stand-ins), or for a zone's pass that has no stand-in (tier 3: one that reads the scene's
// colour copy, a depth pass that writes its depth as colour, the sky's cube map) its pipeline; true: it started compiling
// (as PreloadRecipe)
static bool PreloadStandIn(uint32_t at)
{
    const Recipe &r = s_recipes[at];
    auto v = s_programsByHash.find(r.vs), p = s_programsByHash.find(r.ps);
    if (v == s_programsByHash.end() || p == s_programsByHash.end())
        return false;
    Program &vs = *v->second, &psh = *p->second;
    if (!vs.info.error.empty() || !psh.info.error.empty() || vs.info.pixel || !psh.info.pixel)
        return false;
    size_t had = s_pipelines.size();
    if (Pipe *s = MakeStandIn(r, vs, psh, false))
        return s_pipelines.size() > had && s->status == 0;
    if (r.tier != 3)
        return false;
    Derived d;
    return MakePipeline(r, vs, psh, false, d) && s_pipelines.size() > had && d.pipe->status == 0;
}

// A pass of the zone's materials, set up on the device as a draw of it would be (R_Bo1EnumeratePasses: as the map loads,
// before the preload fence, where the map has no list; at the first frame after the player went in where it has one):
// its recipe is a tier 3 recipe, for its stand-in, or for its pipeline where the pass has no
// stand-in (one that reads the scene's colour copy, which no stand-in draws for; a depth pass that writes its depth as
// colour; the sky's cube map), which go first of the tier. So every stand-in a draw can need is made from the zone
// rather than the list. With a list, whose spawn tier and stand-ins cover the spawn, the zone's compile right after the
// player went in, 8 at a time: waiting for Kino's 1,100 and Ascension's 1,200 took the load 1.2 to 1.7 s,
// and the zone's 54 to 204 readers alone 0.3 s. With no list (&pipelines=0, or a map with none) they are waited for, 32
// at a time, so that nothing is left out. A recipe the enumeration gave already, or one whose stand-in the list or another
// pass queued (StandInKey), costs a lookup.
static uint32_t s_enumPasses, s_enumStandIns, s_enumReaders, s_enumNoStandIn;
static uint32_t s_enumQueued; // recipes queued: a stand-in no recipe before had (StandInKey), or a pipeline
static bool s_enumDone;
static double s_enumStart; // when the first pass came, for the log
static std::unordered_set<std::string> s_enumSeen;

static void EnumPass(bool end)
{
    if (!s_enumStart)
        s_enumStart = emscripten_get_now();
    if (end)
    {
        s_enumDone = true;
        s_enumSeen.clear();
        Bo1_Log("webgpu: %u passes of the zone's materials enumerated in %.0f ms: %u recipes for stand-ins (%u of them of passes with no stand-in, "
                "whose pipelines compile instead), and %u of passes that read the scene's colour copy, whose pipelines compile first%s: "
                "%u to compile, the rest's stand-ins another recipe's",
            s_enumPasses, emscripten_get_now() - s_enumStart, s_enumStandIns, s_enumNoStandIn, s_enumReaders,
            s_noList ? ", before going in (no list)" : ", after going in", s_enumQueued);
        s_enumStart = 0.0;
        NotePending();
        return;
    }
    ++s_enumPasses;
    DrawSetup w;
    if (!SetupFull(w, 3, false, false, 0)) // a triangle list (Topology's 3), as the draws are; the streams' strides as the engine bound them
        return;
    Recipe r = w.recipe;
    // a pixel sampler the engine binds outside the pass's arguments (the lightmaps) has nothing bound here: a filterable
    // texture, as it will
    size_t nvs = std::min<size_t>(w.vs->info.samplers.size(), MAX_DRAW_SAMPLERS);
    for (size_t i = nvs; i < (size_t)w.nbound && i < MAX_DRAW_SAMPLERS; ++i)
        if (!r.sampler[i] && w.bound[i].dim == (int)DIM_2D)
            r.sampler[i] = D3DFMT_A8R8G8B8;
    bool reader = (r.hints[HINT_FLAGS] & HINT_READS_SCENE) != 0;
    r.tier = 3;
    if (!s_enumSeen.insert(std::string((const char *)&r, sizeof(r))).second)
        return;
    bool first = reader; // its pipeline rather than a stand-in, first of the tier
    if (reader)
        ++s_enumReaders;
    else
    {
        ++s_enumStandIns;
        StandInRouting rt;
        first = StandInRoute(r, *w.vs, *w.ps, rt) != nullptr;
        s_enumNoStandIn += first;
        // a stand-in queued already, the list's or another pass': nothing to make
        if (!first && !s_standInKeys.insert(StandInKey(r, *w.vs, *w.ps, rt)).second)
            return;
    }
    ++s_enumQueued;
    uint32_t at = (uint32_t)s_recipes.size();
    s_recipes.push_back(r);
    s_recipesByShader.emplace(r.vs, at);
    if (r.ps != r.vs)
        s_recipesByShader.emplace(r.ps, at);
    if (Preloadable(r)) // always, the zone's shaders made by now; else QueueRecipe's when they are
        (first ? s_enumFirstQueue : s_enumQueue).push_back(at);
}

// A wait of seconds is said to the page, which tells the lobby, for its log (zombies-hub's web/src/main.ts: onWaited;
// zombies-hub's server/rooms.go: waited): first loads have compiled 10 to 20 pipelines a second on machines that
// compile hundreds, and what made them slow was not found by trying. So a wait that long says what it had to do with:
// how the compiles went, whether the page had its animation frames or was hidden, the list and the player's settings;
// the page adds the GPU and the browser. Once a game.
static const double WAIT_SAID_MS = 5000.0;

static double s_waitStandInEach; // the stand-ins among those CompiledInWait counted: how long each took, and how many
static uint32_t s_waitStandIns;

static void SayWaited(const char *how, double now, uint32_t n, double each, double longest)
{
    if (s_waitSaid)
        return;
    s_waitSaid = true;
    char list[64] = "no list";
    if (!s_noList)
        snprintf(list, sizeof(list), "%u of the list's %u recipes the spawn's", s_listRequired, s_listCount);
    char text[512];
    snprintf(text, sizeof(text),
        "%s %.1f s once the map had loaded, for %u pipelines left to compile then, %u of them stand-ins: %u compiled meanwhile, %.0f ms each "
        "on average and %.0f at most (%u stand-ins, %.0f ms each); the page had %u animation frames and %u wakes while hidden meanwhile; %s, "
        "shadow maps %s, %s",
        how, (now - s_wentInAt) / 1000.0, s_waitLeftAtFence, s_waitStandInsAtFence, n, each, longest, s_waitStandIns, s_waitStandInEach,
        s_waitFrames, s_waitWakes, list,
        g_bo1ShadowMaps.load(std::memory_order_acquire) != 0 ? "on" : "off", s_back.samples > 1 ? "4x anti-aliasing" : "no anti-aliasing");
    EM_ASM({
        const f = Module['bo1Status'];
        if (f)
            f('waited', UTF8ToString($0));
    }, text);
}

// the pipelines that compiled since the fence: how many, and how long each took from asking to ready
static uint32_t CompiledInWait(double *each, double *longest)
{
    uint32_t n = 0;
    double sum = 0.0, standInSum = 0.0;
    *longest = 0.0;
    s_waitStandIns = 0;
    for (const auto &p : s_pipelines)
    {
        const Pipe &x = *p.second;
        if (x.status != 1 || x.compileStart + x.compileMs < s_wentInAt)
            continue;
        ++n;
        sum += x.compileMs;
        *longest = std::max(*longest, (double)x.compileMs);
        if (x.tier == 1)
        {
            ++s_waitStandIns;
            standInSum += x.compileMs;
        }
    }
    *each = n ? sum / n : 0.0;
    s_waitStandInEach = s_waitStandIns ? standInSum / s_waitStandIns : 0.0;
    return n;
}

// What the player waited for once the map had loaded (the preload fence) is compiled, and they go in: how long that was,
// and how the pipelines that compiled meanwhile went (how long each took from asking to ready, in the browser's GPU
// process), so that a long wait says whether each compile was slow, or few went at a time
static void WaitEnded(double now)
{
    s_waitEndedAt = now;
    double each, longest;
    uint32_t n = CompiledInWait(&each, &longest);
    Bo1_Log("webgpu: the player waited %.1f s for pipelines once the map had loaded: %u compiled meanwhile, %.0f ms each on average, the "
            "longest %.0f ms",
        (now - s_wentInAt) / 1000.0, n, each, longest);
    if (now - s_wentInAt >= WAIT_SAID_MS)
        SayWaited("waited", now, n, each, longest);
}

// once an animation frame: the list once it is here, more pipelines from it, and the page's notice
static void Preload()
{
    // the list is taken once the engine has said the player's shadow maps setting, which its spawn tier goes by
    // (Bo1_PipelineSettings: as its renderer starts, before any shader comes)
    if (s_recipesState == 1 && g_bo1ShadowMaps.load(std::memory_order_acquire) >= 0)
    {
        int len = -1;
        uint8_t *data = (uint8_t *)bo1gpu_take_recipes(&len);
        if (len >= 0)
        {
            if (data)
                TakeRecipes(data, (size_t)len);
            free(data);
            s_recipesState = 2;
            s_noList = s_listCount == 0;
            s_listState.store(s_noList ? 2 : 1, std::memory_order_release);
        }
    }
    uint32_t compiling[4] = {0, 0, 0, 0};
    bool queued = !s_requiredFirstQueue.empty() || !s_requiredQueue.empty() || !s_standInQueue.empty() || !s_enumFirstQueue.empty() ||
                  !s_enumQueue.empty();
    if (s_compiling[0] || s_compiling[1] || s_compiling[2] || s_compiling[3] || queued)
        for (const auto &p : s_pipelines)
            if (p.second->tier >= 0 && p.second->status == 0)
                ++compiling[p.second->tier];
    // the map has loaded, and the player waits for tiers 0 and 1 (with no list, 3) alone: from the preload fence until
    // they are compiled
    bool waiting = s_wentInAt && !s_waitEndedAt;
    // a few milliseconds of starting them a frame: tier 0, those with no stand-in first, then tier 1 once it is started
    // and compiled; while the player waits, tier 0's with no stand-in, then tier 1, 32 at a time, and no more of tier 0
    // until tier 1 is started
    double start = emscripten_get_now();
    while (compiling[0] < PRELOAD_AT_ONCE && emscripten_get_now() - start < 4.0)
    {
        std::vector<uint32_t> &q = s_requiredFirstQueue.empty() ? s_requiredQueue : s_requiredFirstQueue;
        if (q.empty() || (&q == &s_requiredQueue && waiting && !s_standInQueue.empty()))
            break;
        uint32_t at = q.back();
        q.pop_back();
        compiling[0] += PreloadRecipe(at);
    }
    bool requiredQueued = !s_requiredFirstQueue.empty() || !s_requiredQueue.empty();
    if ((waiting && s_requiredFirstQueue.empty()) || (!requiredQueued && !compiling[0]))
    {
        uint32_t most = waiting ? PRELOAD_AT_ONCE : LATER_AT_ONCE;
        while (!s_standInQueue.empty() && compiling[1] < most && emscripten_get_now() - start < 4.0)
        {
            uint32_t at = s_standInQueue.back();
            s_standInQueue.pop_back();
            compiling[1] += PreloadStandIn(at);
        }
        // the queue fills again as shaders load: said once in a while
        static double said;
        static uint32_t saidMade;
        if (s_standInQueue.empty() && s_standInsMade != saidMade && start - said > 5000.0)
        {
            Bo1_Log("webgpu: %u stand-in pipelines for the list's recipes, %u of them still compiling", s_standInsMade, compiling[1]);
            said = start;
            saidMade = s_standInsMade;
        }
    }
    // the zone's (tier 3) after the list's stand-ins: once the player went in, or at once with no list, when they are
    // waited for and go 32 at a time; the passes with no stand-in first
    if ((s_wentInAt || s_noList) && !requiredQueued && !compiling[0] && s_standInQueue.empty() && !compiling[1])
        while (!(s_enumFirstQueue.empty() && s_enumQueue.empty()) && compiling[3] < (s_noList ? PRELOAD_AT_ONCE : LATER_AT_ONCE) &&
               emscripten_get_now() - start < 4.0)
        {
            std::vector<uint32_t> &q = s_enumFirstQueue.empty() ? s_enumQueue : s_enumFirstQueue;
            uint32_t at = q.back();
            q.pop_back();
            compiling[3] += PreloadStandIn(at);
        }
    memcpy(s_compiling, compiling, sizeof(s_compiling));
    NotePending();

    double now = emscripten_get_now();
    // the list's end: once the game went in (its shaders all made by then) and nothing of the list is queued or compiling
    if (!s_listDone && s_wentInAt && s_enumDone && s_recipesState == 2 && !s_recipes.empty() && !queued && !compiling[0] && !compiling[1] &&
        !compiling[2] && !compiling[3])
    {
        s_listDone = true;
        uint32_t made = 0;
        for (const auto &p : s_pipelines)
            made += p.second->tier == 0;
        Bo1_Log("webgpu: the map's list is compiled, %.1f s after going in: %u pipelines and %u stand-ins (%u recipes for stand-ins and %u pipelines from "
                "the zone's passes)",
            (now - s_wentInAt) / 1000.0, made, s_standInsMade, s_enumStandIns, s_enumReaders);
    }
    // The page's notice says what is left while the player waits for it, from half a second into the wait, and never more
    // than it said before. Not while the map loads, the loading bar saying how that goes: its shaders come as it does,
    // and what is left with them, which the notice said as it went up, went away and came back. What is left is
    // pipelines.
    uint32_t left = (uint32_t)(s_requiredFirstQueue.size() + s_requiredQueue.size() + s_standInQueue.size()) + compiling[0] + compiling[1] +
                    (s_noList ? (uint32_t)(s_enumFirstQueue.size() + s_enumQueue.size()) + compiling[3] : 0u);
    if (waiting && !left && s_recipesState == 2)
        WaitEnded(now);
    // the game goes in without them after two minutes (CL_PreloadPipelines): said then, should they never all compile
    else if (waiting && !s_waitSaid && now - s_wentInAt >= 125000.0)
    {
        double each, longest;
        uint32_t n = CompiledInWait(&each, &longest);
        SayWaited("was still waiting after", now, n, each, longest);
    }
    if (waiting && left)
    {
        if (!s_noticeSince)
            s_noticeSince = now;
        if (now - s_noticeSince >= 500.0 && now - s_noticeAt >= 1000.0)
        {
            s_noticeLeft = s_noticeAt ? std::min(left, s_noticeLeft) : left;
            char text[64];
            snprintf(text, sizeof(text), "Compiling shaders: %u left", s_noticeLeft);
            Notice(text);
            s_noticeAt = now;
        }
    }
    else
    {
        if (s_noticeAt)
            Notice("");
        s_noticeSince = s_noticeAt = 0.0;
    }
}

// ============================================================================================================
// clears, copies, read backs
// ============================================================================================================

// clear part of the open pass' targets with a quad
static void ClearQuad(const PassTargets &t, int x, int y, int w, int h, bool color, bool depth, bool stencil, const float rgba[4], float z,
    uint32_t s)
{
    if (!color && !depth && !stencil)
        return;
    uint8_t *p;
    uint32_t off = ArenaAlloc(16, 16, &p);
    memcpy(p, rgba, 16);
    bo1gpu_set_pipeline(ClearPipeline(t, color, depth, stencil));
    bo1gpu_set_vertex_buffer(0, s_arenaGpu, off, 16);
    bo1gpu_set_viewport((float)x, (float)y, (float)w, (float)h, z, z);
    bo1gpu_set_scissor(x, y, w, h);
    if (stencil)
        bo1gpu_set_stencil_ref((int)s);
    bo1gpu_draw(3, 1, 0, 0);
    memset(&ps, 0, sizeof(ps));
    ps.viewport[2] = -1.0f;
    ps.scissor[2] = -1;
    ps.stencilRef = -1;
    ps.blend[0] = -1.0f;
}

static void ClearRect(const PassKey &key, int x0, int y0, int x1, int y1, bool color, bool depth, bool stencil, const float rgba[4], float z,
    uint32_t s)
{
    PassTargets t;
    if (!ResolvePass(key, t))
        return;
    bool anyColor = false;
    for (int i = 0; i < 4; ++i)
        anyColor = anyColor || t.colorView[i];
    color = color && anyColor;
    depth = depth && t.depthView;
    stencil = stencil && t.stencil;
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, (int)t.w);
    y1 = std::min(y1, (int)t.h);
    if (x1 <= x0 || y1 <= y0 || (!color && !depth && !stencil))
        return;
    if (x0 == 0 && y0 == 0 && x1 == (int)t.w && y1 == (int)t.h)
    {
        // the whole of the targets: the next pass' load operations (a pass over others ends: what it draws next
        // may read these)
        if (s_passOpen)
            ClosePass();
        if (s_clear.any && !(s_clear.key == key))
            FlushClears();
        if (!s_clear.any)
        {
            s_clear = PendingClear();
            s_clear.any = true;
            s_clear.key = key;
        }
        if (color)
        {
            s_clear.color = true;
            memcpy(s_clear.rgba, rgba, 16);
        }
        if (depth)
        {
            s_clear.depth = true;
            s_clear.z = z;
        }
        if (stencil)
        {
            s_clear.stencil = true;
            s_clear.s = s;
        }
        return;
    }
    ArenaReserve(512);
    if (EnsurePass(key))
        ClearQuad(s_pass, x0, y0, x1 - x0, y1 - y0, color, depth, stencil, rgba, z, s);
}

static void ColorOf(uint32_t c, float rgba[4])
{
    rgba[0] = ((c >> 16) & 0xFF) / 255.0f;
    rgba[1] = ((c >> 8) & 0xFF) / 255.0f;
    rgba[2] = (c & 0xFF) / 255.0f;
    rgba[3] = (c >> 24) / 255.0f;
}

static void Clear(const CmdClear &c)
{
    if (s_capturing)
        Bo1_Log("cap clear flags%u color%08x z%g rt%u:%u ds%u:%u vp%d,%d,%ux%u", c.flags, c.color, c.z, st.rt[0].kind, st.rt[0].id, st.ds.kind,
            st.ds.id, st.vp.x, st.vp.y, st.vp.width, st.vp.height);
    // D3D9 clears the viewport (and the scissor rectangle when the test is on, and the rectangle passed)
    int x0 = st.vp.x, y0 = st.vp.y, x1 = st.vp.x + st.vp.width, y1 = st.vp.y + st.vp.height;
    if (st.rs[D3DRS_SCISSORTESTENABLE])
    {
        x0 = std::max(x0, (int)st.scissor.left);
        y0 = std::max(y0, (int)st.scissor.top);
        x1 = std::min(x1, (int)st.scissor.right);
        y1 = std::min(y1, (int)st.scissor.bottom);
    }
    if (c.rectCount)
    {
        x0 = std::max(x0, (int)c.rect.left);
        y0 = std::max(y0, (int)c.rect.top);
        x1 = std::min(x1, (int)c.rect.right);
        y1 = std::min(y1, (int)c.rect.bottom);
    }
    float rgba[4];
    ColorOf(c.color, rgba);
    ClearRect(DrawPassKey(), x0, y0, x1, y1, (c.flags & D3DCLEAR_TARGET) != 0, (c.flags & D3DCLEAR_ZBUFFER) != 0, (c.flags & D3DCLEAR_STENCIL) != 0,
        rgba, c.z, c.stencil & 0xFF);
}

static void ColorFill(const CmdColorFill &c)
{
    PassKey key;
    memset(&key, 0, sizeof(key));
    key.color[0] = c.surf;
    Target t;
    if (!Resolve(c.surf, t))
        return;
    CmdRect r = c.hasRect ? c.rect : CmdRect{0, 0, (int32_t)t.w, (int32_t)t.h};
    float rgba[4];
    ColorOf(c.color, rgba);
    ClearRect(key, r.left, r.top, r.right, r.bottom, true, false, false, rgba, 0.0f, 0);
}

static uint32_t s_ownPassIds = 1;

// a pass over one level and layer of a texture (the executor's own: blits)
static void BeginOwnPass(uint32_t view, GpuFormat format, Tex *tex, uint32_t w, uint32_t h)
{
    EndPass();
    PassKey key;
    memset(&key, 0, sizeof(key));
    key.special = s_ownPassIds++;
    PassTargets t;
    t.colorView[0] = view;
    t.colorFormat[0] = format;
    t.colorTex[0] = tex;
    t.w = w;
    t.h = h;
    t.samples = tex ? tex->samples : 1;
    BeginPassWith(key, t, nullptr);
}

// A multisampled surface's pixels, each the mean of its samples, in its copy of one sample a pixel (Tex::resolved), for
// what copies or reads it; a surface of one sample is itself. Resolved each time: the engine draws on between.
static Tex &Resolved(Tex &t)
{
    if (t.samples == 1 || t.tf.depth)
        return t;
    if (!t.resolved)
    {
        t.resolved = new Tex;
        t.resolved->w = t.w;
        t.resolved->h = t.h;
        t.resolved->fmt = t.fmt;
        t.resolved->tf = t.tf;
        MakeTexture(*t.resolved, D3DUSAGE_RENDERTARGET);
    }
    EndPass();
    bo1gpu_resolve(LevelView(t, 0, 0), LevelView(*t.resolved, 0, 0));
    t.used = t.resolved->used = s_serial;
    return *t.resolved;
}

static void StretchRect(const CmdStretchRect &c)
{
    if (s_capturing)
        Bo1_Log("cap stretch %u:%u -> %u:%u", c.src.kind, c.src.id, c.dst.kind, c.dst.id);
    Target src, dst;
    if (!Resolve(c.src, src) || !Resolve(c.dst, dst))
        return;
    CmdRect s = c.hasSrcRect ? c.srcRect : CmdRect{0, 0, (int32_t)src.w, (int32_t)src.h};
    CmdRect d = c.hasDstRect ? c.dstRect : CmdRect{0, 0, (int32_t)dst.w, (int32_t)dst.h};
    if (s.right <= s.left || s.bottom <= s.top || d.right <= d.left || d.bottom <= d.top)
        return;
    if (src.tex->tf.depth || dst.tex->tf.depth)
    {
        BO1_GPU_ERROR("StretchRect of depth buffers is not supported (%u:%u -> %u:%u)", c.src.kind, c.src.id, c.dst.kind, c.dst.id);
        return;
    }
    if (src.tex == dst.tex && src.level == dst.level && src.layer == dst.layer)
    {
        BO1_GPU_ERROR("StretchRect within a surface is not supported (%u:%u)", c.src.kind, c.src.id);
        return;
    }
    ArenaReserve(512);
    EndPass();
    src.tex->used = dst.tex->used = s_serial;
    bool sameFormat = src.tex->tf.gpu == dst.tex->tf.gpu;
    bool sameSize = s.right - s.left == d.right - d.left && s.bottom - s.top == d.bottom - d.top;
    // a multisampled surface of 32-bit floats, all of it, into a surface of its size: each pixel's first sample
    // (FirstSampleInto: the float-z's with anti-aliasing)
    if (src.tex->samples > 1 && src.tex->tf.float32)
    {
        if (dst.tex->samples != 1 || !(dst.tex->usage & TU_RENDER) || src.w != dst.w || src.h != dst.h || s.left || s.top ||
            s.right != (int32_t)src.w || s.bottom != (int32_t)src.h || d.left || d.top || d.right != (int32_t)dst.w || d.bottom != (int32_t)dst.h)
        {
            BO1_GPU_ERROR("StretchRect of part of a multisampled surface of 32-bit floats, or to another size, is not supported (%u:%u -> %u:%u)",
                c.src.kind, c.src.id, c.dst.kind, c.dst.id);
            return;
        }
        BeginOwnPass(LevelView(*dst.tex, dst.level, dst.layer), dst.tex->tf.gpu, dst.tex, dst.w, dst.h);
        bo1gpu_set_viewport(0.0f, 0.0f, (float)dst.w, (float)dst.h, 0.0f, 1.0f);
        FirstSampleInto(*src.tex, dst.tex->tf.gpu);
        EndPass();
        return;
    }
    // a multisampled surface, all of it: resolved into the destination where it is a surface of its size and format,
    // as Direct3D does; else resolved into its copy, which the rest copies from
    if (src.tex->samples > 1 && dst.tex->samples == 1 && sameFormat && (dst.tex->usage & TU_RENDER) && src.w == dst.w && src.h == dst.h &&
        s.left == 0 && s.top == 0 && s.right == (int32_t)src.w && s.bottom == (int32_t)src.h && d.left == 0 && d.top == 0 &&
        d.right == (int32_t)dst.w && d.bottom == (int32_t)dst.h)
    {
        bo1gpu_resolve(LevelView(*src.tex, 0, 0), LevelView(*dst.tex, dst.level, dst.layer));
        return;
    }
    Tex &from = Resolved(*src.tex);
    if (sameFormat && sameSize && dst.tex->samples == 1)
    {
        bo1gpu_copy_texture(from.tex, src.level, src.layer, s.left, s.top, dst.tex->tex, dst.level, dst.layer, d.left, d.top, s.right - s.left,
            s.bottom - s.top);
        return;
    }
    if (!(dst.tex->usage & TU_RENDER))
    {
        BO1_GPU_ERROR("StretchRect to a surface that can't be drawn to (%u:%u, format %u)", c.dst.kind, c.dst.id, dst.tex->fmt);
        return;
    }
    BeginOwnPass(LevelView(*dst.tex, dst.level, dst.layer), dst.tex->tf.gpu, dst.tex, dst.w, dst.h);
    bo1gpu_set_viewport((float)d.left, (float)d.top, (float)(d.right - d.left), (float)(d.bottom - d.top), 0.0f, 1.0f);
    BlitInto(from, src.level, src.layer, s, dst.tex->tf.gpu, c.filter != D3DTEXF_POINT && c.filter != D3DTEXF_NONE);
    EndPass();
}

static void GenerateMips(uint32_t id)
{
    auto it = s_textures.find(id);
    if (it == s_textures.end() || !it->second.tex || it->second.levels < 2)
        return;
    Tex &t = it->second;
    if (!(t.usage & TU_RENDER) || t.type == TEX_3D)
    {
        BO1_GPU_ERROR("texture %u: mipmaps of format %u can't be generated", id, t.fmt);
        return;
    }
    ArenaReserve(t.levels * 6 * 64);
    for (uint32_t layer = 0; layer < (t.type == TEX_CUBE ? 6u : 1u); ++layer)
        for (uint32_t level = 1; level < t.levels; ++level)
        {
            uint32_t w = Bo1_MipSize(t.w, level), h = Bo1_MipSize(t.h, level);
            BeginOwnPass(LevelView(t, level, layer), t.tf.gpu, &t, w, h);
            CmdRect r = {0, 0, (int32_t)Bo1_MipSize(t.w, level - 1), (int32_t)Bo1_MipSize(t.h, level - 1)};
            BlitInto(t, level - 1, layer, r, t.tf.gpu, true);
        }
    EndPass();
}

static uint16_t ToUnorm16(float f) { return (uint16_t)(f <= 0.0f ? 0 : f >= 1.0f ? 65535 : f * 65535.0f + 0.5f); }

struct PendingReadback
{
    CmdReadback cmd;
    GpuFormat format;
    uint32_t buffer, rowBytes;
    uint8_t *data;
    int32_t *flag;
};
static std::vector<PendingReadback> s_readbacks;

static void Readback(const CmdReadback &c)
{
    Target src;
    uint32_t bpp = 0;
    if (Resolve(c.src, src))
        bpp = GpuBpp(src.tex->tf.gpu);
    if (!bpp)
    {
        BO1_GPU_ERROR("read back of %u:%u (format %u) is not supported", c.src.kind, c.src.id, src.tex ? src.tex->fmt : 0);
        memset(c.dst, 0, (size_t)c.pitch * c.height);
        __atomic_store_n(c.done, 1, __ATOMIC_RELEASE);
        emscripten_futex_wake((void *)c.done, 1);
        return;
    }
    uint32_t w = std::min(c.width, src.w), h = std::min(c.height, src.h);
    uint32_t rowBytes = (w * bpp + 255) & ~255u;
    PendingReadback r = {c, src.tex->tf.gpu, bo1gpu_buffer(rowBytes * h, BU_MAP_READ | BU_COPY_DST), rowBytes, (uint8_t *)malloc((size_t)rowBytes * h),
        new int32_t(0)};
    r.cmd.width = w;
    r.cmd.height = h;
    EndPass();
    Tex &from = Resolved(*src.tex);
    bo1gpu_copy_to_buffer(from.tex, src.level, src.layer, w, h, r.buffer, rowBytes);
    src.tex->used = from.used = s_serial;
    Flush();
    bo1gpu_map_read(r.buffer, rowBytes * h, r.data, r.flag);
    s_readbacks.push_back(r);
}

// read backs the GPU has done: their texels, in the format the engine asked for
static void FinishReadbacks()
{
    for (size_t i = 0; i < s_readbacks.size();)
    {
        PendingReadback &r = s_readbacks[i];
        int32_t flag = __atomic_load_n(r.flag, __ATOMIC_ACQUIRE);
        if (!flag)
        {
            ++i;
            continue;
        }
        const CmdReadback &c = r.cmd;
        uint8_t *dst = (uint8_t *)c.dst;
        for (uint32_t y = 0; flag > 0 && y < c.height; ++y)
        {
            const uint8_t *s = r.data + (size_t)y * r.rowBytes;
            uint8_t *row = dst + (size_t)y * c.pitch;
            for (uint32_t x = 0; x < c.width; ++x)
            {
                float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                uint8_t rgba8[4] = {0, 0, 0, 255};
                bool eight = false;
                switch (r.format)
                {
                case GF_BGRA8:
                case GF_BGRA8_SRGB:
                    rgba8[0] = s[x * 4 + 2];
                    rgba8[1] = s[x * 4 + 1];
                    rgba8[2] = s[x * 4];
                    rgba8[3] = s[x * 4 + 3];
                    eight = true;
                    break;
                case GF_RGBA8:
                case GF_RGBA8_SRGB:
                    memcpy(rgba8, s + x * 4, 4);
                    eight = true;
                    break;
                case GF_R16F:
                    v[0] = HalfToFloat(((const uint16_t *)s)[x]);
                    break;
                case GF_RG16F:
                    for (int k = 0; k < 2; ++k)
                        v[k] = HalfToFloat(((const uint16_t *)s)[x * 2 + k]);
                    break;
                case GF_RGBA16F:
                    for (int k = 0; k < 4; ++k)
                        v[k] = HalfToFloat(((const uint16_t *)s)[x * 4 + k]);
                    break;
                case GF_R32F:
                    memcpy(v, s + x * 4, 4);
                    break;
                case GF_RG32F:
                    memcpy(v, s + x * 8, 8);
                    break;
                case GF_RGBA32F:
                    memcpy(v, s + x * 16, 16);
                    break;
                default:
                    break;
                }
                if (eight)
                    for (int k = 0; k < 4; ++k)
                        v[k] = rgba8[k] / 255.0f;
                else
                    for (int k = 0; k < 4; ++k)
                        rgba8[k] = (uint8_t)(v[k] <= 0.0f ? 0 : v[k] >= 1.0f ? 255 : v[k] * 255.0f + 0.5f);
                switch (c.format)
                {
                case D3DFMT_A8R8G8B8:
                case D3DFMT_X8R8G8B8:
                    row[x * 4] = rgba8[2];
                    row[x * 4 + 1] = rgba8[1];
                    row[x * 4 + 2] = rgba8[0];
                    row[x * 4 + 3] = rgba8[3];
                    break;
                case D3DFMT_A8B8G8R8:
                case D3DFMT_X8B8G8R8:
                    memcpy(row + x * 4, rgba8, 4);
                    break;
                case D3DFMT_R5G6B5:
                    ((uint16_t *)row)[x] = (uint16_t)((rgba8[0] >> 3) << 11 | (rgba8[1] >> 2) << 5 | rgba8[2] >> 3);
                    break;
                case D3DFMT_R32F:
                    memcpy(row + x * 4, v, 4);
                    break;
                case D3DFMT_G32R32F:
                    memcpy(row + x * 8, v, 8);
                    break;
                case D3DFMT_A32B32G32R32F:
                    memcpy(row + x * 16, v, 16);
                    break;
                case D3DFMT_R16F:
                    ((uint16_t *)row)[x] = FloatToHalf(v[0]);
                    break;
                case D3DFMT_G16R16F:
                    for (int k = 0; k < 2; ++k)
                        ((uint16_t *)row)[x * 2 + k] = FloatToHalf(v[k]);
                    break;
                case D3DFMT_A16B16G16R16F:
                    for (int k = 0; k < 4; ++k)
                        ((uint16_t *)row)[x * 4 + k] = FloatToHalf(v[k]);
                    break;
                case D3DFMT_L16:
                    ((uint16_t *)row)[x] = ToUnorm16(v[0]);
                    break;
                case D3DFMT_G16R16:
                    for (int k = 0; k < 2; ++k)
                        ((uint16_t *)row)[x * 2 + k] = ToUnorm16(v[k]);
                    break;
                case D3DFMT_A16B16G16R16:
                    for (int k = 0; k < 4; ++k)
                        ((uint16_t *)row)[x * 4 + k] = ToUnorm16(v[k]);
                    break;
                default:
                    break;
                }
            }
        }
        __atomic_store_n(c.done, 1, __ATOMIC_RELEASE);
        emscripten_futex_wake((void *)c.done, 1);
        bo1gpu_destroy(r.buffer);
        free(r.data);
        delete r.flag;
        s_readbacks.erase(s_readbacks.begin() + i);
    }
}

// ============================================================================================================
// the command loop
// ============================================================================================================

static void Destroy(const CmdDestroy &c)
{
    switch (c.kind)
    {
    case OBJ_TEXTURE: {
        auto it = s_textures.find(c.id);
        if (it == s_textures.end())
            return;
        for (int i = 0; i < 4; ++i)
            if (s_passOpen && (s_pass.colorTex[i] == &it->second || s_pass.depthTex == &it->second))
                EndPass();
        ReleaseTexture(it->second);
        s_textures.erase(it);
        break;
    }
    case OBJ_SURFACE: {
        auto it = s_surfaces.find(c.id);
        if (it == s_surfaces.end())
            return;
        for (int i = 0; i < 4; ++i)
            if (s_passOpen && (s_pass.colorTex[i] == &it->second || s_pass.depthTex == &it->second))
                EndPass();
        ReleaseTexture(it->second);
        s_surfaces.erase(it);
        break;
    }
    case OBJ_BUFFER: {
        auto it = s_buffers.find(c.id);
        if (it == s_buffers.end())
            return;
        WritePending(it->second); // for the draws recorded
        if (it->second.pool >= 0)
            s_freedParts.push_back({it->second.pool, it->second.base, it->second.alloc});
        else
            s_retiredBuffers.push_back({it->second.gpu, it->second.alloc});
        s_buffers.erase(it);
        break;
    }
    case OBJ_VDECL:
        s_decls.erase(c.id);
        break;
    case OBJ_SHADER: {
        auto it = s_shaders.find(c.id);
        if (it == s_shaders.end())
            return;
        Program *prog = it->second.prog;
        s_shaders.erase(it);
        if (--prog->refs)
            break;
        s_programsByHash.erase(prog->hash);
        ForgetDerived();
        // its pipelines go too (one still compiling once it is done: it writes its status)
        for (auto p = s_pipelines.begin(); p != s_pipelines.end();)
        {
            bool uses = false;
            for (auto &m : prog->modules)
                uses = uses || p->first.vsModule == m.second.id || p->first.psModule == m.second.id;
            if (!uses)
            {
                ++p;
                continue;
            }
            if (p->second->handle && ps.pipeline == p->second->handle)
                ps.pipeline = 0;
            if (p->second->status != 0)
            {
                bo1gpu_release(p->second->handle);
                delete p->second;
            }
            else
                s_orphans.push_back(p->second);
            p = s_pipelines.erase(p);
        }
        for (auto &p : s_pipelines)
        {
            p.second->stand = nullptr; // one may have gone
            p.second->standTried = false;
        }
        for (auto &m : prog->modules)
            if (m.second.handle)
                bo1gpu_release(m.second.handle);
        for (size_t i = 0; i < s_liveModules.size();)
            if (s_liveModules[i].first == prog)
            {
                s_liveModules[i] = s_liveModules.back();
                s_liveModules.pop_back();
            }
            else
                ++i;
        if (s_lastVs == &prog->info)
            s_lastVs = nullptr;
        if (s_lastPs == &prog->info)
            s_lastPs = nullptr;
        for (auto i = s_programs.begin(); i != s_programs.end(); ++i)
            if (i->second == prog)
            {
                s_programs.erase(i);
                break;
            }
        delete prog;
        break;
    }
    }
}

static void Reset(const CmdReset &c)
{
    EndPass();
    if (s_back.tex)
        ReleaseTexture(s_back);
    s_back.w = c.width;
    s_back.h = c.height;
    s_back.fmt = D3DFMT_A8R8G8B8;
    s_back.tf = Format(D3DFMT_A8R8G8B8);
    s_back.samples = SamplesOf(c.multisample);
    MakeTexture(s_back, D3DUSAGE_RENDERTARGET);
}

// The frame rate overlay (Module.bo1Perf: zombies-hub's web/src/perf.ts): what the frames the page showed in the last
// half second took
static struct
{
    double start, lastShown, worstMs, renderMs, lagMs, pageMs;
    uint32_t frames, lagFrames, animationFrames, gpuWaits;
    uint32_t aheadMost; // AheadMost's answers at those animation frames, added up
    uint64_t gameUs;
    uint32_t gameFrames;
} s_perf;

// The game's brightness: its gamma ramp (SetGammaRamp), which Present's blit applies from the next frame on; none at all
// while the ramp is the identity, as at brightness 1. The queue writes it before that frame's commands, submitted after.
static void Gamma(const CmdGamma &c)
{
    bool identity = true;
    for (int i = 0; i < 256 && identity; ++i)
        identity = c.rgba[i * 4] == i && c.rgba[i * 4 + 1] == i && c.rgba[i * 4 + 2] == i;
    s_gammaOn = !identity;
    if (identity)
        return;
    if (!s_gammaTex)
    {
        s_gammaTex = bo1gpu_texture(0, GF_RGBA8, 256, 1, 1, 1, TU_TEXTURE | TU_COPY_DST, 0, 1);
        s_gammaView = bo1gpu_view(s_gammaTex, 0, 0, 0, 0, 1, 0, 1);
    }
    bo1gpu_write_texture(s_gammaTex, 0, 0, 0, 0, c.rgba, sizeof(c.rgba), 1, 256, 1, 1);
}

// The frame, onto the canvas; its alpha too while the page's loading movie shows under the canvas (bink_web.cpp), where
// the loading screen leaves it transparent (src/client/cl_cin.cpp: CL_DrawLoadingMovie)
static void Present(const CmdPresent &c)
{
    ArenaReserve(512);
    EndPass();
    Tex &frame = Resolved(s_back);
    int cw, ch;
    bo1gpu_canvas_size(&cw, &ch);
    bo1gpu_canvas_see_through(Bo1_MovieUnder());
    uint32_t view = bo1gpu_canvas_view();
    BeginOwnPass(view, s_canvasFormat, nullptr, cw, ch);
    bo1gpu_set_viewport(0.0f, 0.0f, (float)cw, (float)ch, 0.0f, 1.0f);
    CmdRect all = {0, 0, (int32_t)s_back.w, (int32_t)s_back.h};
    BlitInto(frame, 0, 0, all, s_canvasFormat, s_back.w != (uint32_t)cw || s_back.h != (uint32_t)ch, true);
    Submit(true);
    bo1gpu_release(view);
    double now = emscripten_get_now();
    if (s_perf.lastShown > 0.0)
        s_perf.worstMs = std::max(s_perf.worstMs, now - s_perf.lastShown);
    s_perf.lastShown = now;
    ++s_perf.frames;
    s_perf.renderMs += c.renderMs;
    // the mouse lag: from the movement the frame was built with to the GPU having the frame
    if (c.inputTime > 0.0)
    {
        s_perf.lagMs += now - c.inputTime;
        ++s_perf.lagFrames;
    }
}

static void SetConsts(float *bank, int maxRegs, const CmdConst &c, uint32_t &gen)
{
    if (c.start + c.count > (uint32_t)maxRegs)
        return;
    memcpy(bank + c.start * 4, &c + 1, c.count * 16);
    ++gen;
}

static void SetConstsI(int *bank, int maxRegs, int width, const CmdConst &c, uint32_t &gen)
{
    if (c.start + c.count > (uint32_t)maxRegs)
        return;
    memcpy(bank + c.start * width, &c + 1, c.count * width * 4);
    ++gen;
}

// Each command, and what it changes of what draws keep (PrepareDraw: s_changed): a state set to what it was changes
// nothing.
static void Execute(const CmdHeader *h)
{
    const void *p = h + 1;
    if (s_prof.on && h->op < OP_COUNT)
        ++s_prof.ops[h->op];
    switch (h->op)
    {
    case OP_CREATE_TEXTURE:
        CreateTexture(*(const CmdCreateTexture *)p);
        ObjectsChanged();
        break;
    case OP_CREATE_SURFACE:
        CreateSurface(*(const CmdCreateSurface *)p);
        ObjectsChanged();
        break;
    case OP_CREATE_BUFFER:
        CreateBuffer(*(const CmdCreateBuffer *)p);
        ObjectsChanged();
        break;
    case OP_CREATE_VDECL: {
        const CmdCreateVDecl &c = *(const CmdCreateVDecl *)p;
        Decl &d = s_decls[c.id];
        d.el.assign((D3DVERTEXELEMENT9 *)c.elements, (D3DVERTEXELEMENT9 *)c.elements + c.count + 1);
        free(c.elements);
        ObjectsChanged();
        break;
    }
    case OP_CREATE_SHADER: {
        const CmdCreateShader &c = *(const CmdCreateShader *)p;
        Program *&prog = s_programs[std::string((const char *)c.code, c.size)];
        if (!prog)
        {
            prog = new Program;
            prog->id = c.id;
            ShaderParse((const uint32_t *)c.code, c.size / 4, prog->info);
            if (!prog->info.error.empty())
                BO1_GPU_ERROR("shader %u (%s): %s", c.id, c.pixel ? "ps" : "vs", prog->info.error.c_str());
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t i = 0; i < c.size; ++i)
                h = (h ^ ((const uint8_t *)c.code)[i]) * 0x100000001B3ull;
            prog->hash = h;
            s_programsByHash[h] = prog;
            QueueRecipes(*prog);
        }
        ++prog->refs;
        s_shaders[c.id].prog = prog;
        free(c.code);
        ObjectsChanged();
        break;
    }
    case OP_DESTROY:
        Destroy(*(const CmdDestroy *)p);
        ObjectsChanged();
        break;
    case OP_UPLOAD_TEXTURE:
        UploadTexture(*(const CmdUploadTexture *)p);
        break;
    case OP_UPLOAD_BUFFER:
        UploadBuffer(*(const CmdUploadBuffer *)p);
        break;
    case OP_GENERATE_MIPS:
        GenerateMips(((const CmdId *)p)->id);
        break;
    case OP_RENDER_STATE: {
        const CmdState &c = *(const CmdState *)p;
        if (c.state < 256 && st.rs[c.state] != c.value)
        {
            st.rs[c.state] = c.value;
            if (s_pipeState[c.state])
                s_changed |= CH_STATES;
        }
        break;
    }
    case OP_SAMPLER_STATE: {
        const CmdSamplerState &c = *(const CmdSamplerState *)p;
        if (c.sampler < (uint32_t)MAX_SAMPLERS && c.type <= D3DSAMP_DMAPOFFSET && st.ss[c.sampler][c.type] != c.value)
        {
            st.ss[c.sampler][c.type] = c.value;
            s_changed |= CH_SAMPLERS;
            s_samplerKept[c.sampler].known = false;
        }
        break;
    }
    case OP_TEXTURE: {
        const CmdSetTexture &c = *(const CmdSetTexture *)p;
        if (c.stage < (uint32_t)MAX_SAMPLERS && st.tex[c.stage] != c.id)
        {
            st.tex[c.stage] = c.id;
            s_changed |= CH_TEXTURES;
        }
        break;
    }
    case OP_STREAM: {
        const CmdStream &c = *(const CmdStream *)p;
        if (c.stream < 16)
        {
            StreamState &s = st.stream[c.stream];
            if (s.stride != c.stride)
                s_changed |= CH_DECL;
            if (s.id != c.id)
                s_streamKnown &= ~(1u << c.stream);
            s = {c.id, c.offset, c.stride};
        }
        break;
    }
    case OP_STREAM_FREQ: {
        const CmdState &c = *(const CmdState *)p;
        if (c.state < 16 && st.freq[c.state] != c.value)
        {
            st.freq[c.state] = c.value;
            s_changed |= CH_DECL;
        }
        break;
    }
    case OP_INDICES:
        if (st.indices != ((const CmdId *)p)->id)
        {
            st.indices = ((const CmdId *)p)->id;
            s_indexKnown = false;
        }
        break;
    case OP_VDECL:
        if (st.decl != ((const CmdId *)p)->id)
        {
            st.decl = ((const CmdId *)p)->id;
            s_changed |= CH_DECL;
        }
        break;
    case OP_VS:
        if (st.vs != ((const CmdId *)p)->id)
        {
            st.vs = ((const CmdId *)p)->id;
            s_changed |= CH_SHADERS;
        }
        break;
    case OP_PS:
        if (st.ps != ((const CmdId *)p)->id)
        {
            st.ps = ((const CmdId *)p)->id;
            s_changed |= CH_SHADERS;
        }
        break;
    case OP_VS_CONST_F:
        SetConsts(st.vsF, 256, *(const CmdConst *)p, st.vsGen);
        break;
    case OP_PS_CONST_F:
        SetConsts(st.psF, 224, *(const CmdConst *)p, st.psGen);
        break;
    case OP_VS_CONST_I:
        SetConstsI(st.vsI, 16, 4, *(const CmdConst *)p, st.vsGen);
        break;
    case OP_PS_CONST_I:
        SetConstsI(st.psI, 16, 4, *(const CmdConst *)p, st.psGen);
        break;
    case OP_VS_CONST_B:
        SetConstsI(st.vsB, 16, 1, *(const CmdConst *)p, st.vsGen);
        break;
    case OP_PS_CONST_B:
        SetConstsI(st.psB, 16, 1, *(const CmdConst *)p, st.psGen);
        break;
    case OP_VIEWPORT:
        st.vp = *(const CmdViewport *)p;
        break;
    case OP_SCISSOR:
        st.scissor = *(const CmdRect *)p;
        break;
    case OP_RENDER_TARGET: {
        const CmdRenderTarget &c = *(const CmdRenderTarget *)p;
        if (c.index < 4 && memcmp(&st.rt[c.index], &c.surf, sizeof(SurfRef)))
        {
            st.rt[c.index] = c.surf;
            s_changed |= CH_TARGETS;
        }
        break;
    }
    case OP_DEPTH_STENCIL:
        if (memcmp(&st.ds, &((const CmdRenderTarget *)p)->surf, sizeof(SurfRef)))
        {
            st.ds = ((const CmdRenderTarget *)p)->surf;
            s_changed |= CH_TARGETS;
        }
        break;
    case OP_CLEAR:
        Clear(*(const CmdClear *)p);
        break;
    case OP_DRAW:
        Draw(*(const CmdDraw *)p);
        break;
    case OP_DRAW_INDEXED:
        DrawIndexed(*(const CmdDrawIndexed *)p);
        break;
    case OP_DRAW_UP:
        DrawUp(*(const CmdDrawUp *)p);
        break;
    case OP_DRAW_INDEXED_UP:
        DrawIndexedUp(*(const CmdDrawIndexedUp *)p);
        break;
    case OP_STRETCH_RECT:
        StretchRect(*(const CmdStretchRect *)p);
        break;
    case OP_READBACK:
        Readback(*(const CmdReadback *)p);
        break;
    case OP_QUERY_BEGIN:
        OcclusionBegin(((const CmdQuery *)p)->answer);
        break;
    case OP_QUERY_END: {
        const CmdQuery &c = *(const CmdQuery *)p;
        if (c.answer)
        {
            OcclusionEnd(c.answer);
            break;
        }
        if (c.id == ~0u) // the preload fence (d3d9_device.cpp)
        {
            s_wentInAt = emscripten_get_now();
            s_waitLeftAtFence = g_bo1PreloadPending.load();
            s_waitStandInsAtFence = (uint32_t)s_standInQueue.size() + s_compiling[1];
            Bo1_Log("webgpu: going in once %u pipelines compile, %u of them the list's stand-ins", s_waitLeftAtFence, s_waitStandInsAtFence);
        }
        __atomic_store_n(c.done, 1, __ATOMIC_RELEASE);
        if (!c.id)
            free((void *)c.done);
        break;
    }
    case OP_PRESENT:
        Present(*(const CmdPresent *)p);
        break;
    case OP_COLOR_FILL:
        ColorFill(*(const CmdColorFill *)p);
        break;
    case OP_RESET:
        Reset(*(const CmdReset *)p);
        ObjectsChanged();
        // zombies-hub's e2e/game/player.ts reads it
        Bo1_Log("webgpu: the back buffer is %ux%u, %s", s_back.w, s_back.h, s_back.samples > 1 ? "4x anti-aliasing" : "no anti-aliasing");
        break;
    case OP_GAMMA:
        Gamma(*(const CmdGamma *)p);
        break;
    case OP_ENUM_PASS:
        EnumPass(((const CmdEnumPass *)p)->end != 0);
        break;
    case OP_PASS_HINTS: {
        const CmdPassHints &c = *(const CmdPassHints *)p;
        if (memcmp(st.hints, c.hints, HINT_COUNT))
        {
            memcpy(st.hints, c.hints, HINT_COUNT);
            s_changed |= CH_HINTS;
        }
        st.passMaterial = c.material; // the names are no part of a recipe: nothing to work out again
        st.passTechnique = c.technique;
        st.passSurfType = c.surfType;
        break;
    }
    default:
        break;
    }
}

static double s_lastReport, s_reportPageMs;
static uint32_t s_reportFrames;
static double s_lastAnimationFrame, s_refreshMs;
static uint32_t s_sinceTaken; // the frame rate limit: the display's refreshes since the last frame taken (Frame)
// The display's rate as the game plays, in hertz (0: not yet), which the frame rate limit and the game's menu go by
// (DisplayRate), as an adaptive display's (a phone's, that goes from 120 Hz down to 60 or less while nothing moves) may
// be faster than the page's own measure before the game (zombies-hub's web/src/settings.ts: measureRefresh) found it.
static std::atomic<int> s_displayHz;
static double s_intervals[120]; // the last animation frames' intervals, in ms
static uint32_t s_intervalCount;
static uint32_t s_framesShown; // PRESENT commands replayed
static uint32_t s_onTime;       // animation frames in a row that had a frame to take
static double s_gpuBehindSince; // the page as far ahead of the GPU as it goes since then (-1: reported)

// How far ahead of the GPU the page goes: how many frames not done an animation frame takes no more at (Replay).
// Two, where the GPU is behind: each frame more in its queue is a frame's time more between what the player does and
// what they see, and not a frame more a second. Three, where the GPU keeps pace and the word that a frame is done
// takes long to come: there a frame is as old either way, and an animation frame that takes none is a frame the
// player doesn't get. A Galaxy S23+ at 60 Hz heard so 21 to 30 ms after it sent a frame that its GPU drew in 11 to 14
// (Chrome's GPU process carries out the page's calls first, and the word comes back when that process next looks),
// which with the page's own 7 to 9 ms is over two refreshes: held to two, it took no frame at every third animation
// frame of Kino's heaviest view, 40 frames a second where it had the time for 58, and with a third less to do a
// second the phone clocked down, which kept it there.
// Which of the two a device is, its queue says: a GPU that keeps pace has three frames not done only after one that
// took it long (a pipeline compiled, textures came). So the page goes three ahead, until animation frames find three
// not done for the third time in a second, with frames taken in between: that GPU is behind, and the page is held
// to two for 2 s, then tries the third again. Each time the third lasted under 5 s it is held twice as long, up to 16
// s: a GPU that stays behind has a frame more in its queue for the few frames it takes to tell, every 16 s.
// Not on mobile (zombies-hub's web/public/player.js: Module.bo1Mobile), where the page goes three ahead whatever its
// queue says. A phone's queue can't tell the two apart: with less to do it clocks down, and then its frames take long
// enough to fill the queue, as a GPU that is behind fills it. That Galaxy, with the rule above, was held to two as it
// came to Kino's heaviest view (the frames that took long there), which had it at 41 frames a second, and each try of
// the third frame from there, at the clocks 41 had left it, was soon over: it was held to two for nine tenths of the 6
// s it was measured over. Where the third frame was left to it, it had its display's 60 at each view and waited at none
// of its animation frames. A phone is played with a gamepad, and what a frame more in a queue costs is a mouse's to
// feel.
static double s_twoAheadUntil;   // the GPU is behind: the page is held to two frames ahead until then
static double s_twoAheadFor;     // for how long it was held, the last time
static double s_threeNotDone[2]; // when animation frames began to find three not done, the last two times
static bool s_threeNotDoneNow;   // the last animation frame found three not done

// How many frames not done an animation frame takes no more at: pending are not done at this one.
static int AheadMost(double now, int pending, uint32_t flags)
{
    static int s_mobile = -1;
    if (s_mobile < 0)
        s_mobile = EM_ASM_INT({ return Module['bo1Mobile'] ? 1 : 0; });
    if (s_mobile && !(flags & DBG_TWO_AHEAD))
        return 3;
    if ((flags & DBG_TWO_AHEAD) || now < s_twoAheadUntil)
    {
        s_threeNotDoneNow = false;
        return 2;
    }
    if (pending < 3)
    {
        s_threeNotDoneNow = false;
        return 3;
    }
    if (s_threeNotDoneNow)
        return 3;
    if (s_threeNotDone[1] > 0.0 && now - s_threeNotDone[1] < 1000.0)
    {
        bool soon = s_twoAheadUntil > 0.0 && now - s_twoAheadUntil < 5000.0;
        s_twoAheadFor = !soon ? 2000.0 : s_twoAheadFor < 8000.0 ? s_twoAheadFor * 2.0 : 16000.0;
        s_twoAheadUntil = now + s_twoAheadFor;
        s_threeNotDone[0] = s_threeNotDone[1] = 0.0;
        return 2;
    }
    s_threeNotDoneNow = true;
    s_threeNotDone[1] = s_threeNotDone[0];
    s_threeNotDone[0] = now;
    return 3;
}

// every half second, to the overlay: frames shown a second, the time between them (on average and the longest), the
// mouse lag (-1: the mouse did not move), what the game thread, the render thread and the page took a frame, the share
// of animation frames the page left to the GPU to catch up, the pacing (d3d9_device.cpp: Present), the back buffer
// the engine draws to (its size, and its samples a pixel, 4 with anti-aliasing), what a frame took the GPU (the GPU's
// time, above; -1: the device has no clock to say) with each kind of pass' count and time a frame, how long after
// the page sent a frame it heard that the GPU was done with it (-1: none done), which the waiting goes by (Replay),
// the page's animation frames a second, and how many frames ahead of the GPU the page went at most (AheadMost: 2 to 3,
// over those animation frames)
static void ReportPerf(double now)
{
    double elapsed = now - s_perf.start;
    if (elapsed < 500.0)
        return;
    uint64_t gameUs = g_bo1GameUs;
    uint32_t gameFrames = g_bo1GameFrames;
    double doneMs = 0.0, doneFrames = 0.0;
    bo1gpu_take_done(&doneMs, &doneFrames);
    if (s_perf.start > 0.0)
    {
        uint32_t f = s_perf.frames, games = gameFrames - s_perf.gameFrames;
        double timed = s_gpuTimes.frames ? s_gpuTimes.frames : 1.0;
        // by its address: EM_ASM takes 16 values at most
        const double v[] = {f * 1000.0 / elapsed, f ? elapsed / f : 0.0, s_perf.worstMs, s_perf.lagFrames ? s_perf.lagMs / s_perf.lagFrames : -1.0,
            games ? (gameUs - s_perf.gameUs) / 1000.0 / games : 0.0, f ? s_perf.renderMs / f : 0.0, f ? s_perf.pageMs / f : 0.0,
            s_perf.animationFrames ? (double)s_perf.gpuWaits / s_perf.animationFrames : 0.0, (double)g_ring.pacing, (double)s_back.w,
            (double)s_back.h, (double)s_back.samples, s_gpuTimes.frames ? s_gpuTimes.ms / timed : -1.0, s_gpuTimes.kindMs[TIME_SCENE] / timed,
            s_gpuTimes.passes[TIME_SCENE] / timed, s_gpuTimes.kindMs[TIME_FULL] / timed, s_gpuTimes.passes[TIME_FULL] / timed,
            s_gpuTimes.kindMs[TIME_OTHER] / timed, s_gpuTimes.passes[TIME_OTHER] / timed, doneFrames > 0.0 ? doneMs / doneFrames : -1.0,
            s_perf.animationFrames * 1000.0 / elapsed,
            s_perf.animationFrames ? (double)s_perf.aheadMost / s_perf.animationFrames : 3.0};
        EM_ASM({
            var report = Module['bo1Perf'];
            if (!report)
                return;
            var v = HEAPF64.subarray($0 >>> 3, ($0 >>> 3) + 22);
            report({fps : v[0], frameMs : v[1], worstMs : v[2], lagMs : v[3], gameMs : v[4], renderMs : v[5], pageMs : v[6], gpu : v[7],
                pacing : v[8], width : v[9], height : v[10], samples : v[11], gpuMs : v[12], gpuSceneMs : v[13], gpuScenePasses : v[14],
                gpuFullMs : v[15], gpuFullPasses : v[16], gpuOtherMs : v[17], gpuOtherPasses : v[18], gpuDoneMs : v[19],
                animationFrames : v[20], ahead : v[21]});
        },
            v);
    }
    s_gpuTimes = {};
    s_perf = {};
    s_perf.start = now;
    s_perf.gameUs = gameUs;
    s_perf.gameFrames = gameFrames;
}

// the frame's profile (&gpu=16), logged: a line for the frame, one for each pass, and one for each of the buffers written
// most
static void ProfileLog()
{
    static const char *formats[] = {"-", "bgra8", "bgra8-srgb", "rgba8", "rgba8-srgb", "rgba8snorm", "rg8snorm", "rgb10a2", "r16f", "rg16f",
        "rgba16f", "r32f", "rg32f", "rgba32f", "bc1", "bc1-srgb", "bc2", "bc2-srgb", "bc3", "bc3-srgb", "bc4", "bc5", "d16", "d24", "d24s8",
        "d32f"};
    auto format = [](uint32_t f) { return f < sizeof(formats) / sizeof(formats[0]) ? formats[f] : "?"; };
    const Profile &p = s_prof;
    ProfPass all = {};
    for (const ProfPass &q : p.passes)
    {
        all.draws += q.draws;
        all.pipelines += q.pipelines;
        all.constants += q.constants;
        all.textures += q.textures;
        all.vertexBuffers += q.vertexBuffers;
        all.indexBuffers += q.indexBuffers;
        all.viewports += q.viewports;
        all.scissors += q.scissors;
        all.other += q.other;
        all.vbDraws += q.vbDraws;
        all.vbNewBuffer += q.vbNewBuffer;
        all.vbShift += q.vbShift;
        all.instanced += q.instanced;
    }
    uint32_t sets = all.pipelines + all.constants + all.textures + all.vertexBuffers + all.indexBuffers + all.viewports + all.scissors + all.other;
    Bo1_Log("webgpu profile: %u draws in %zu passes, %u calls to set state for them (pipelines %u, constants %u, textures %u, vertex "
            "buffers %u, index buffers %u, viewports %u, scissors %u, other %u), %u submits, %u snapshots of targets drawn to, %u draws "
            "whose pipelines were compiling (%u of them drawn with a stand-in)",
        all.draws, p.passes.size(), sets, all.pipelines, all.constants, all.textures, all.vertexBuffers, all.indexBuffers, all.viewports,
        all.scissors, all.other, p.submits, p.snapshots, p.waits, p.standIns);
    // the engine's commands the page replayed for it: each is one to read and carry out, most of them a state the
    // engine set for a draw (the device sends none that sets a state to what it is: d3d9_device.cpp)
    uint32_t commands = 0;
    for (uint32_t op = 0; op < OP_COUNT; ++op)
        commands += p.ops[op];
    Bo1_Log("webgpu profile: %u commands replayed: render states %u, sampler states %u, textures %u, streams %u, index buffers %u, vertex "
            "declarations %u, shaders %u, constants %u (vertex shaders' %u), viewports and scissors %u, targets %u, clears %u, draws %u, "
            "uploads %u, other %u",
        commands, p.ops[OP_RENDER_STATE], p.ops[OP_SAMPLER_STATE], p.ops[OP_TEXTURE], p.ops[OP_STREAM] + p.ops[OP_STREAM_FREQ], p.ops[OP_INDICES],
        p.ops[OP_VDECL], p.ops[OP_VS] + p.ops[OP_PS],
        p.ops[OP_VS_CONST_F] + p.ops[OP_PS_CONST_F] + p.ops[OP_VS_CONST_I] + p.ops[OP_PS_CONST_I] + p.ops[OP_VS_CONST_B] + p.ops[OP_PS_CONST_B],
        p.ops[OP_VS_CONST_F] + p.ops[OP_VS_CONST_I] + p.ops[OP_VS_CONST_B], p.ops[OP_VIEWPORT] + p.ops[OP_SCISSOR],
        p.ops[OP_RENDER_TARGET] + p.ops[OP_DEPTH_STENCIL], p.ops[OP_CLEAR],
        p.ops[OP_DRAW] + p.ops[OP_DRAW_INDEXED] + p.ops[OP_DRAW_UP] + p.ops[OP_DRAW_INDEXED_UP], p.ops[OP_UPLOAD_BUFFER] + p.ops[OP_UPLOAD_TEXTURE],
        commands - p.ops[OP_RENDER_STATE] - p.ops[OP_SAMPLER_STATE] - p.ops[OP_TEXTURE] - p.ops[OP_STREAM] - p.ops[OP_STREAM_FREQ] - p.ops[OP_INDICES] -
            p.ops[OP_VDECL] - p.ops[OP_VS] - p.ops[OP_PS] - p.ops[OP_VS_CONST_F] - p.ops[OP_PS_CONST_F] - p.ops[OP_VS_CONST_I] - p.ops[OP_PS_CONST_I] -
            p.ops[OP_VS_CONST_B] - p.ops[OP_PS_CONST_B] - p.ops[OP_VIEWPORT] - p.ops[OP_SCISSOR] - p.ops[OP_RENDER_TARGET] - p.ops[OP_DEPTH_STENCIL] -
            p.ops[OP_CLEAR] - p.ops[OP_DRAW] - p.ops[OP_DRAW_INDEXED] - p.ops[OP_DRAW_UP] - p.ops[OP_DRAW_INDEXED_UP] - p.ops[OP_UPLOAD_BUFFER] -
            p.ops[OP_UPLOAD_TEXTURE]);
    uint32_t drawn[3];
    bo1gpu_take_drawn(drawn);
    Bo1_Log("webgpu profile: %u calls to draw them: %u draws went as instances of the draw before, %u as more indices of it", drawn[0], drawn[1],
        drawn[2]);
    Bo1_Log("webgpu profile: draws that set vertex buffers %u: to another buffer %u, the same further on by whole vertices %u; draws of "
            "several instances %u",
        all.vbDraws, all.vbNewBuffer, all.vbShift, all.instanced);
    Bo1_Log("webgpu profile: draws that worked out again what the draws before had (the rest kept): their shaders %u, what they draw to %u, "
            "their pipelines' recipes %u (vertex layouts %u), pipelines %u (made %u), texture bind groups %u",
        p.workedShaders, p.workedTargets, p.workedRecipes, p.workedLayouts, p.workedPipelines, p.madePipelines, p.workedGroups);
    Bo1_Log("webgpu profile: written: constants %u KB (%u vertex shader blocks, %u pixel shader blocks), DrawPrimitiveUP data %u KB, "
            "static vertex buffers %u KB in %u writes, dynamic %u KB in %u, static index buffers %u KB in %u, dynamic %u KB in %u, "
            "textures %u KB in %u; %u calls to the queue's writes",
        p.constantBytes / 1024, p.vsUploads, p.psUploads, p.upBytes / 1024, p.bytes[PROF_STATIC_VB] / 1024, p.writes[PROF_STATIC_VB],
        p.bytes[PROF_DYNAMIC_VB] / 1024, p.writes[PROF_DYNAMIC_VB], p.bytes[PROF_STATIC_IB] / 1024, p.writes[PROF_STATIC_IB],
        p.bytes[PROF_DYNAMIC_IB] / 1024, p.writes[PROF_DYNAMIC_IB], p.bytes[PROF_TEXTURES] / 1024, p.writes[PROF_TEXTURES], p.queueWrites);
    for (size_t i = 0; i < p.passes.size(); ++i)
    {
        const ProfPass &q = p.passes[i];
        Bo1_Log("webgpu profile: pass %zu %ux%u %s %s: %u draws, pipelines %u, constants %u, textures %u, vertex buffers %u, index "
                "buffers %u, viewports %u, scissors %u, other %u; vertex buffers: draws %u, another buffer %u, further on %u",
            i, q.w, q.h, format(q.color), format(q.depth), q.draws, q.pipelines, q.constants, q.textures, q.vertexBuffers, q.indexBuffers,
            q.viewports, q.scissors, q.other, q.vbDraws, q.vbNewBuffer, q.vbShift);
    }
    std::vector<std::pair<uint32_t, ProfBuffer>> buffers(p.buffers.begin(), p.buffers.end());
    std::sort(buffers.begin(), buffers.end(), [](const auto &a, const auto &b) { return a.second.bytes > b.second.bytes; });
    for (size_t i = 0; i < buffers.size() && i < 12; ++i)
    {
        auto b = s_buffers.find(buffers[i].first);
        if (b == s_buffers.end())
            continue;
        const ProfBuffer &pb = buffers[i].second;
        Bo1_Log("webgpu profile: buffer %u (%s %s, %u KB): %u KB in %u writes, %u discarding", buffers[i].first,
            b->second.dynamic ? "dynamic" : "static", b->second.index ? "index" : "vertex", b->second.size / 1024, pb.bytes / 1024, pb.writes,
            pb.discards);
    }
    std::vector<std::pair<uint32_t, ProfBuffer>> textures(p.textures.begin(), p.textures.end());
    std::sort(textures.begin(), textures.end(), [](const auto &a, const auto &b) { return a.second.bytes > b.second.bytes; });
    for (size_t i = 0; i < textures.size() && i < 8; ++i)
    {
        auto t = s_textures.find(textures[i].first);
        if (t == s_textures.end())
            continue;
        const Tex &x = t->second;
        Bo1_Log("webgpu profile: texture %u (%ux%ux%u, %u levels, D3D format %u): %u KB in %u writes", textures[i].first, x.w, x.h,
            x.type == TEX_CUBE ? 6 : x.d, x.levels, x.fmt, textures[i].second.bytes / 1024, textures[i].second.writes);
    }
}

// The engine quits (web_main.cpp: Bo1_Exit), and its exit destroys what the replay uses, while the page may stay,
// saying why: before it does, the page stops replaying (ExecutorStop), and its animation frames end.
static bool s_stopped;

// What the engine wrote is replayed, up to a frame's PRESENT, on each animation frame (Frame); and in a hidden page,
// which has none, on the page's wakes (library_gpu.js: bo1gpu_wake_hidden), so that the engine goes on, if slower, and
// its game keeps hearing from it (a lobby's game lets a player's character go after 5 minutes without a word:
// zombies-hub's server/rooms.go, keepCharacter). Only an animation frame with no frame to take is a miss (Pace). An
// animation frame too soon for the player's frame rate limit (Frame: take false) takes none and replays up to the next
// PRESENT, not past it, so the engine waits for a later one. Whether it took a frame.
static bool Replay(bool animationFrame, bool take = true)
{
    if (s_stopped)
        return false;
    double start = emscripten_get_now();
    double startNow = emscripten_performance_now(); // the same clock less its origin: quicker to read
    ReportPerf(start);
    ++s_perf.animationFrames;
    if (s_wentInAt && !s_waitEndedAt)
        ++(animationFrame ? s_waitFrames : s_waitWakes);
    FinishReadbacks();
    FinishOcclusion();
    FinishTimes();
    for (size_t i = 0; i < s_orphans.size();)
        if (s_orphans[i]->status != 0)
        {
            bo1gpu_release(s_orphans[i]->handle);
            delete s_orphans[i];
            s_orphans[i] = s_orphans.back();
            s_orphans.pop_back();
        }
        else
            ++i;
    uint32_t flags = (uint32_t)EM_ASM_INT({ return Module['bo1GpuFlags'] | 0; });
    if ((flags ^ s_debugFlags) & (DBG_NO_CULL | DBG_FLIP_CULL))
        ForgetDerived(); // pipelines take the culling
    SetByInstance(!(flags & DBG_BOUND_CONSTANTS));
    if (flags & DBG_DROP)
    {
        DropPipelines();
        EM_ASM({ Module['bo1GpuFlags'] &= ~256; });
        flags &= ~DBG_DROP;
    }
    s_debugFlags = flags;
    int pending = bo1gpu_frames_pending();
    __atomic_store_n(&g_ring.replays, g_ring.replays + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&g_ring.pagePending, pending, __ATOMIC_RELAXED);
    // no more than two or three frames ahead of the GPU (AheadMost): the engine waits for the ring instead
    int most = AheadMost(start, pending, flags);
    s_perf.aheadMost += most;
    if (pending >= most)
    {
        ++s_perf.gpuWaits;
        if (!s_gpuBehindSince)
            s_gpuBehindSince = start;
        else if (start - s_gpuBehindSince > 10000.0 && s_gpuBehindSince > 0.0)
        {
            Bo1_LogStall("the page waiting for the GPU");
            s_gpuBehindSince = -1.0; // once
        }
        return false;
    }
    if (s_gpuBehindSince > 0.0)
        s_gpuBehindSince = 0.0;
    // A frame written whole is replayed now, up to its PRESENT: the engine may go on to the next meanwhile
    // (d3d9_device.cpp: Present). None, after a second of frames on time, is a miss, which paces the engine further ahead
    // (Pace); while loading, frames are seldom on time.
    // one the limit holds back is neither a miss nor on time
    bool took = false;
    if (!take)
        ;
    else if (__atomic_load_n(&g_ring.framesSubmitted, __ATOMIC_ACQUIRE) == s_framesShown)
    {
        if (animationFrame && s_onTime >= 60)
            __atomic_add_fetch(&g_ring.misses, 1, __ATOMIC_RELEASE);
        s_onTime = 0;
    }
    else
    {
        s_onTime += animationFrame;
        if (g_ring.framesTaken == s_framesShown)
        {
            __atomic_store_n(&g_ring.framesTaken, s_framesShown + 1, __ATOMIC_RELEASE);
            emscripten_futex_wake((void *)&g_ring.framesTaken, 0x7FFFFFFF);
            took = true;
        }
    }
    bool work = false, shown = false, replayed = false;
    uint32_t read = g_ring.readPos;
    for (uint32_t n = 1;; ++n)
    {
        uint32_t write = __atomic_load_n(&g_ring.writePos, __ATOMIC_ACQUIRE);
        if (read == write)
            break;
        const CmdHeader *h = (const CmdHeader *)(g_ring.data + read % BO1CMD_RING_SIZE);
        uint16_t op = h->op;
        if (op == OP_PRESENT && !take)
            break;
        work = replayed = true;
        if (op != OP_WRAP)
            Execute(h);
        read += h->size;
        if (op == OP_PRESENT)
        {
            __atomic_store_n(&g_ring.readPos, read, __ATOMIC_RELEASE);
            work = false;
            shown = true;
            if (s_capturing)
            {
                s_capturing = false;
                Bo1_Log("cap end");
                EM_ASM({ Module['bo1GpuFlags'] &= ~4; });
                s_debugFlags &= ~DBG_CAPTURE;
            }
            else if (s_debugFlags & DBG_CAPTURE)
            {
                s_capturing = true;
                Bo1_Log("cap begin");
            }
            if (s_prof.on)
            {
                ProfileLog();
                s_prof = Profile();
                EM_ASM({ Module['bo1GpuFlags'] &= ~16; });
                s_debugFlags &= ~DBG_PROFILE;
            }
            else if (s_debugFlags & DBG_PROFILE)
            {
                s_prof.on = true;
                uint32_t drawn[3];
                bo1gpu_take_drawn(drawn); // counted from here
            }
            // one finished as it was replayed is taken now
            if (g_ring.framesTaken != ++s_framesShown)
            {
                __atomic_store_n(&g_ring.framesTaken, s_framesShown, __ATOMIC_RELEASE);
                emscripten_futex_wake((void *)&g_ring.framesTaken, 0x7FFFFFFF);
            }
            __atomic_store_n(&g_ring.pageShown, s_framesShown, __ATOMIC_RELAXED);
            ++s_reportFrames;
            break;
        }
        // Now and then (a store the device sees is a fence, and the clock and waking are calls out of WebAssembly: too
        // slow for every command): the room in the ring, for the device, woken if it may be waiting for some (it waits
        // only once the ring is nearly full: Bo1_WaitRingSpace), and the page back while loading floods the ring
        // without presenting. The clock less often still: every 64 commands it was read some 1,500 times a frame of
        // Kino's heaviest view, 0.1 ms of the page's 4.8.
        if (!(n & 63))
        {
            __atomic_store_n(&g_ring.readPos, read, __ATOMIC_RELEASE);
            if (write - read > BO1CMD_RING_SIZE / 2)
                emscripten_futex_wake((void *)&g_ring.readPos, 0x7FFFFFFF);
            if (!(n & 1023) && emscripten_performance_now() - startNow > 40.0)
                break;
        }
    }
    __atomic_store_n(&g_ring.readPos, read, __ATOMIC_RELEASE);
    emscripten_futex_wake((void *)&g_ring.readPos, 0x7FFFFFFF);
    // what was recorded of an unfinished frame goes now (a frame's own goes at its Present)
    if (work)
        Submit(false);
    Preload();
    double now = emscripten_get_now();
    ReleaseIdleModules(now);
    g_bo1PageUs += (uint64_t)((now - start) * 1000.0);
    s_reportPageMs += now - start;
    // the overlay's: every replay that ran commands, as under a frame rate limit the animation frame before the one
    // that takes the frame replays all of it but its PRESENT
    if (replayed)
        s_perf.pageMs += now - start;
    if (now - s_lastReport > 10000.0)
    {
        double calls = 0.0, bytes = 0.0, frames = s_reportFrames ? s_reportFrames : 1;
        if (flags & 8)
            bo1gpu_take_counts(&calls, &bytes);
        if (flags & 8)
            Bo1_Log("webgpu: %u frames in %.1f s (the page drew for %.1f ms a frame), last frame %u draws (%u left out, %u waiting for "
                    "pipelines, %u of those drawn with a stand-in), %u submits for writes, %zu pipelines (%u stand-ins), %zu shader "
                    "modules, %u occlusion queries answered (%u visible); a frame %.0f calls to WebGPU's passes and queue, %.0f KB written",
                s_reportFrames, (now - s_lastReport) / 1000.0, s_reportPageMs / (s_reportFrames ? s_reportFrames : 1), s_frameDraws, s_frameSkips,
                s_frameWaits, s_frameStandIns, s_frameFlushes, s_pipelines.size(), s_standInsMade, s_liveModules.size(), s_occAnswered,
                s_occVisible, calls / frames, bytes / 1024.0 / frames);
        double compiles = 0.0, compileMs = 0.0, compileMax = 0.0;
        bo1gpu_take_compiles(&compiles, &compileMs, &compileMax);
        if ((flags & 8) && (compiles > 0.0 || s_waitsEnded))
            Bo1_Log("webgpu: %.0f pipelines compiled in %.1f s, %.0f ms each on average, the longest %.0f ms; %u waits for one ended, %.0f ms "
                    "on average, the longest %.0f ms, %u of them with draws drawn with a stand-in meanwhile, %u left out (%u for want "
                    "of a stand-in, %u while the stand-in compiled too, %u whose stand-in failed, %u whose stand-in didn't fit the draw's textures); "
                    "draw-frames left out: %u for want of a stand-in, %u while it compiled too, %u whose failed, %u whose didn't fit",
                compiles, (now - s_lastReport) / 1000.0, compiles > 0.0 ? compileMs / compiles : 0.0, compileMax, s_waitsEnded,
                s_waitsEnded ? s_waitMs / s_waitsEnded : 0.0, s_waitMaxMs, s_waitsStoodIn, s_waitsEnded - s_waitsStoodIn,
                s_waitsLeftOutWhy[1], s_waitsLeftOutWhy[2], s_waitsLeftOutWhy[3], s_waitsLeftOutWhy[4], s_leftOutWhy[1], s_leftOutWhy[2], s_leftOutWhy[3],
                s_leftOutWhy[4]);
        s_waitsEnded = s_waitsStoodIn = 0;
        memset(s_waitsLeftOutWhy, 0, sizeof(s_waitsLeftOutWhy));
        memset(s_leftOutWhy, 0, sizeof(s_leftOutWhy));
        s_waitMs = s_waitMaxMs = 0.0;
        if (flags & DBG_CHECK)
            Bo1_Log("webgpu: %u draws worked out both ways in %.1f s, %u of them differently", s_checked, (now - s_lastReport) / 1000.0, s_differed);
        s_checked = s_differed = s_differedAt = 0;
        s_occAnswered = s_occVisible = 0;
        s_lastReport = now;
        s_reportFrames = 0;
        s_reportPageMs = 0.0;
    }
    s_frameDraws = 0;
    s_frameSkips = 0;
    s_frameWaits = 0;
    s_frameStandIns = 0;
    s_frameFlushes = 0;
    return took || shown;
}

// The display's rate as the game plays (s_displayHz): every second or so, of the last 120 animation frames' intervals,
// the median of those the page didn't miss a refresh in (up to 1.5 times the shortest tenth's), rounded: the
// browser's frames come on the display's refreshes, so it is the display's rate as the page has it (59.94 Hz: 60),
// whatever a frame the page took long over did (a test's software renderer draws 6 a second of a display's 60).
// Taken where it is 24-500 Hz, and logged as it changes.
static void DisplayRate(double interval)
{
    s_intervals[s_intervalCount++ % 120] = interval;
    if (s_intervalCount < 120 || s_intervalCount % 60)
        return;
    double sorted[120];
    memcpy(sorted, s_intervals, sizeof(sorted));
    std::sort(sorted, sorted + 120);
    int kept = 12;
    while (kept < 120 && sorted[kept] <= sorted[12] * 1.5)
        ++kept;
    const double period = sorted[kept / 2];
    const int hz = period > 0.0 ? (int)lround(1000.0 / period) : 0;
    if (hz < 24 || hz > 500 || hz == s_displayHz.load(std::memory_order_relaxed))
        return;
    s_displayHz.store(hz, std::memory_order_relaxed);
    Bo1_Log("display: %d Hz, as the game measures it", hz);
}

static EM_BOOL Frame(double time, void *)
{
    if (s_stopped)
        return EM_FALSE;
    // the display's refresh, for how the page paces the engine (d3d9_device.cpp: Present): the time between
    // animation frames, less those the page missed
    double interval = time - s_lastAnimationFrame;
    bool timed = s_lastAnimationFrame > 0.0 && interval > 0.0;
    if (timed)
        s_refreshMs = !s_refreshMs ? interval : interval < 1.5 * s_refreshMs ? s_refreshMs * 0.95 + interval * 0.05 : s_refreshMs * 1.01;
    s_lastAnimationFrame = time;
    g_ring.refreshUs = (uint32_t)(s_refreshMs * 1000.0);
    // The player's frame rate limit (bo1_maxfps, the engine's: Ring::maxFps): each frame on the same number of
    // refreshes, its step at the display's rate as the game measures it (Com_FrameLimitStep: 30 on a 60 Hz display is
    // every second, 60 on a 144 Hz one every third, 48 a second); an animation frame sooner takes none (Replay). The
    // refreshes are counted by the time gone by, not by the animation frames: a page that took long over one had none
    // at the next refresh, and by its own frames a limit of 60 at 120 Hz would then wait three refreshes, 40 a second,
    // where the frame after is two on. It also keeps to the limit on a display whose rate changed since the game last
    // measured it (an adaptive one gone from 120 Hz to 60: each animation frame is then two of the refreshes the step
    // is in). A frame the engine had yet to finish goes on the first animation frame after it, and the count starts
    // there. The engine paces itself against the step, the time it has for a frame (Pace).
    if (timed)
        DisplayRate(interval);
    const int measured = s_displayHz.load(std::memory_order_relaxed);
    const int hz = measured ? measured : Bo1_DisplayHz();
    const int step = ::Com_FrameLimitStep(hz, (int)__atomic_load_n(&g_ring.maxFps, __ATOMIC_RELAXED));
    if (step > 1)
    {
        g_ring.refreshUs = (uint32_t)(s_refreshMs * 1000.0 * step);
        const double passed = timed ? floor(interval * hz / 1000.0 + 0.5) : 1.0;
        const uint32_t since = s_sinceTaken + (passed >= step ? (uint32_t)step : passed > 1.0 ? (uint32_t)passed : 1);
        s_sinceTaken = since < (uint32_t)step ? since : (uint32_t)step; // no further, while the engine has none ready
        if (Replay(true, s_sinceTaken >= (uint32_t)step))
            s_sinceTaken = 0;
        return EM_TRUE;
    }
    s_sinceTaken = 0;
    Replay(true);
    return EM_TRUE;
}

static void ExecutorInit(int width, int height)
{
    s_caps = bo1gpu_init();
    if (!s_caps)
        return;
    s_canvasFormat = (GpuFormat)bo1gpu_canvas_format();
    s_arena = (uint8_t *)malloc(ARENA_SIZE);
    s_arenaGpu = bo1gpu_buffer(ARENA_SIZE + UNIFORM_BINDING, BU_VERTEX | BU_INDEX | BU_UNIFORM | BU_STORAGE | BU_COPY_DST);
    // group 0, the vertex shader's constants: a uniform block at an offset, as the pixel shader's, or the arena whole
    for (uint32_t byInstance = 0; byInstance < 2; ++byInstance)
    {
        uint32_t layout[10] = {0, VIS_VERTEX, byInstance ? 3u : 0u, 0, 0, 1, VIS_FRAGMENT, 0, 0, 0};
        s_arenaLayouts[byInstance] = bo1gpu_bind_group_layout(layout, 2);
        uint32_t group[10] = {0, 0, s_arenaGpu, 0, byInstance ? ARENA_SIZE + UNIFORM_BINDING : UNIFORM_BINDING, 1, 0, s_arenaGpu, 0, UNIFORM_BINDING};
        s_arenaGroups[byInstance] = bo1gpu_bind_group(s_arenaLayouts[byInstance], group, 2);
    }
    s_arenaGroup = s_arenaGroups[0];
    s_arenaLayout = s_arenaLayouts[0];
    SetByInstance(!(EM_ASM_INT({ return Module['bo1GpuFlags'] | 0; }) & DBG_BOUND_CONSTANTS));
    s_emptyPipelineLayout = bo1gpu_pipeline_layout(nullptr, 0);
    for (int f = 0; f < 2; ++f)
    {
        uint32_t blit[10] = {0, VIS_FRAGMENT, 1, (uint32_t)f, 0, 1, VIS_FRAGMENT, 2, (uint32_t)f, 0};
        s_blitLayout[f] = bo1gpu_bind_group_layout(blit, 2);
        s_blitPipelineLayout[f] = bo1gpu_pipeline_layout(&s_blitLayout[f], 1);
    }
    uint32_t firstSample[5] = {0, VIS_FRAGMENT, 1, 1, 3};
    s_firstSampleLayout = bo1gpu_bind_group_layout(firstSample, 1);
    s_firstSamplePipelineLayout = bo1gpu_pipeline_layout(&s_firstSampleLayout, 1);
    uint32_t gamma[15] = {0, VIS_FRAGMENT, 1, 0, 0, 1, VIS_FRAGMENT, 2, 0, 0, 2, VIS_FRAGMENT, 1, 0, 0};
    s_gammaLayout = bo1gpu_bind_group_layout(gamma, 3);
    s_gammaPipelineLayout = bo1gpu_pipeline_layout(&s_gammaLayout, 1);
    s_quadVs = OwnShader(kQuadVs, "quad");
    float zero[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    s_zeroBuffer = bo1gpu_buffer(16, BU_VERTEX | BU_COPY_DST);
    bo1gpu_write_buffer(s_zeroBuffer, 0, zero, 16);
    // the samplers' stand-ins: black, opaque
    static const uint8_t black[6 * 4] = {0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255};
    for (int d = 0; d < 3; ++d)
    {
        uint32_t t = bo1gpu_texture(d == 2, GF_RGBA8, 1, 1, d == 1 ? 6 : 1, 1, TU_TEXTURE | TU_COPY_DST, 0, 1);
        bo1gpu_write_texture(t, 0, 0, 0, 0, black, 4, 1, 1, 1, d == 1 ? 6 : 1);
        s_dummy[d] = bo1gpu_view(t, 0, d, 0, 0, 1, 0, d == 1 ? 6 : 1);
    }
    CmdReset reset = {(uint32_t)width, (uint32_t)height, D3DMULTISAMPLE_NONE};
    Reset(reset);
    for (int i = 0; i < 16; ++i)
        st.freq[i] = 1;
    memset(st.hints, 255, HINT_COUNT); // no pass set up yet: nothing known
    st.hints[HINT_FLAGS] = 0;
    for (D3DRENDERSTATETYPE state : kPipeStates)
        s_pipeState[state] = 1;
    s_lastReport = emscripten_get_now();
    uint32_t header[3] = {RECIPE_MAGIC, RECIPE_VERSION, (uint32_t)sizeof(Recipe)};
    s_usedRecipes.assign((const uint8_t *)header, (const uint8_t *)(header + 3));
    s_recipesState = 1;
    bo1gpu_fetch_recipes();
    NotePending();
    Bo1_Log("WebGPU: canvas %dx%d (%s%s%s)", width, height, s_caps & CAP_BC ? "BC textures" : "no BC textures: decompressed",
        s_caps & CAP_FLOAT32_FILTER ? ", float32-filterable" : "", s_caps & CAP_FLOAT32_BLEND ? ", float32-blendable" : "");
    g_ring.executorAlive = 1;
    emscripten_request_animation_frame_loop(Frame, nullptr);
    bo1gpu_wake_hidden(100, [] { Replay(false); });
}

void ExecutorStart(int width, int height)
{
    emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_VII, (void *)ExecutorInit, width, height);
}

// on the page's thread, between two replays
static void ExecutorStopNow() { s_stopped = true; }

void ExecutorStop()
{
    if (g_ring.executorAlive)
        emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_V, (void *)ExecutorStopNow);
}
} // namespace bo1gpu

// the page ended the engine (library_bo1.js: bo1Stop), on this thread, between two replays: they end, and with them its
// animation frames, as its threads are gone
extern "C" EMSCRIPTEN_KEEPALIVE void Bo1_StopReplay() { bo1gpu::ExecutorStopNow(); }

// the engine's: whether the map has a list (src/gfx_d3d/rb_draw3d.cpp: the zone's passes are enumerated before the preload
// fence with none, at the first frame after the player went in with one)
int Bo1_PipelineListState() { return bo1gpu::s_listState.load(std::memory_order_acquire); }

// the display's rate as the game measures it (bo1gpu::DisplayRate), 0 until it has: the game's menu's
// (src/ui/ui_main.cpp: UI_FrameLimit_Update)
int Bo1_LiveDisplayHz() { return bo1gpu::s_displayHz.load(std::memory_order_relaxed); }
