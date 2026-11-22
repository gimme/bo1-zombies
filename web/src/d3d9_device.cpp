// Direct3D 9 for the WebAssembly build: the device the engine creates (IDirect3D9 / IDirect3DDevice9 and their
// resources), recording every call into the command ring (d3d9_cmd.h) that d3d9_webgpu.cpp replays into WebGPU on the
// page's thread.
//
// The device keeps what the engine can read back (states, bindings, descriptions) and the CPU side of locks: a lock
// returns a staging block, the unlock sends it to the executor, which converts and uploads it. Calls that need the
// GPU's answer (GetRenderTargetData, a read lock of a render target, queries) wait for the executor. Every call takes
// the device lock, as D3DCREATE_MULTITHREADED does.
#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include <emscripten.h>
#include <emscripten/threading.h>

#include "d3d9_cmd.h"
#include "d3d9_formats.h"
#include "web_platform.h"

using namespace bo1cmd;

namespace bo1cmd
{
Ring g_ring;
}

static std::recursive_mutex s_dev;
static std::atomic<uint32_t> s_nextId{1};
static uint32_t s_cmdSize;
static class Bo1Device *s_device;

static uint32_t Bo1_NewId() { return s_nextId++; }

// Released objects are deleted a few seconds later, not at once: the engine releases resources on one thread while
// another may still hold the pointer for a moment, and a late Lock or Issue must not write into freed memory.
static std::deque<std::pair<double, std::function<void()>>> s_retired;

static void Bo1_Retire(std::function<void()> deleter)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    s_retired.emplace_back(emscripten_get_now(), std::move(deleter));
}

static void Bo1_ReapRetired()
{
    double now = emscripten_get_now();
    while (!s_retired.empty() && now - s_retired.front().first > 5000.0)
    {
        std::function<void()> deleter = std::move(s_retired.front().second);
        s_retired.pop_front();
        deleter();
    }
}

static void Bo1_LateCall(const char *what)
{
    static int reports;
    if (reports++ < 20)
        Bo1_Log("d3d9: %s on a released object", what);
}

// ============================================================================================================
// the ring (device side)
// ============================================================================================================

static void Bo1_WaitRingSpace(uint32_t bytes)
{
    // the executor drains the ring on the page's animation frames
    double start = 0.0, began = 0.0;
    while (g_ring.writePos - __atomic_load_n(&g_ring.readPos, __ATOMIC_ACQUIRE) + bytes > BO1CMD_RING_SIZE)
    {
        if (began == 0.0)
            began = emscripten_get_now();
        emscripten_futex_wait((void *)&g_ring.readPos, g_ring.readPos, 2.0);
        if (start == 0.0)
            start = emscripten_get_now();
        else if (start > 0.0 && emscripten_get_now() - start > 20000.0)
        {
            Bo1_LogStall("waiting for room in the command ring");
            start = -1.0;
        }
    }
    if (began != 0.0)
        g_bo1WaitPageUs += (uint64_t)((emscripten_get_now() - began) * 1000.0);
}

// BeginCmd..EndCmd hold the device lock: engine threads (the render thread, the database thread loading textures)
// call into the device at the same time, and a command must reach the ring whole
void *bo1cmd::BeginCmd(Op op, uint32_t payloadBytes)
{
    s_dev.lock();
    uint32_t size = (uint32_t)((sizeof(CmdHeader) + payloadBytes + 7) & ~7u);
    uint32_t off = g_ring.writePos % BO1CMD_RING_SIZE;
    if (off + size > BO1CMD_RING_SIZE)
    {
        // wrap: mark the tail unused
        uint32_t tail = BO1CMD_RING_SIZE - off;
        Bo1_WaitRingSpace(tail);
        CmdHeader *h = (CmdHeader *)(g_ring.data + off);
        h->op = OP_WRAP;
        h->size = tail;
        __atomic_store_n(&g_ring.writePos, g_ring.writePos + tail, __ATOMIC_RELEASE);
        off = 0;
    }
    Bo1_WaitRingSpace(size);
    CmdHeader *h = (CmdHeader *)(g_ring.data + off);
    h->op = op;
    h->pad = 0;
    h->size = size;
    s_cmdSize = size;
    return h + 1;
}

void bo1cmd::EndCmd()
{
    __atomic_store_n(&g_ring.writePos, g_ring.writePos + s_cmdSize, __ATOMIC_RELEASE);
    s_dev.unlock();
}

template <class T> static T *Cmd(Op op, uint32_t extra = 0) { return (T *)BeginCmd(op, sizeof(T) + extra); }

static void Bo1_CmdId(Op op, uint32_t id)
{
    Cmd<CmdId>(op)->id = id;
    EndCmd();
}

static void Bo1_CmdDestroy(ObjKind kind, uint32_t id)
{
    CmdDestroy *c = Cmd<CmdDestroy>(OP_DESTROY);
    c->kind = kind;
    c->id = id;
    EndCmd();
}

// the engine's word on the material pass it set up (src/gfx_d3d/r_shade.cpp: R_SetupPass), for the executor's stand-in
// pipelines (d3d9_cmd.h: CmdPassHints)
void Bo1_PassHints(const unsigned char *hints, const char *material, const char *technique, unsigned char surfType)
{
    CmdPassHints *c = Cmd<CmdPassHints>(OP_PASS_HINTS);
    memcpy(c->hints, hints, HINT_COUNT);
    c->material = material;
    c->technique = technique;
    c->surfType = surfType;
    EndCmd();
}

// a pass of the zone's materials set up for its stand-in (src/gfx_d3d/rb_draw3d.cpp: R_Bo1EnumeratePasses; d3d9_cmd.h:
// CmdEnumPass)
void Bo1_EnumPass(int end)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    CmdEnumPass *c = Cmd<CmdEnumPass>(OP_ENUM_PASS);
    c->end = (uint8_t)end;
    EndCmd();
}

// wait for a flag the executor sets (read backs, queries)
static void Bo1_WaitFlag(volatile int32_t *flag)
{
    double began = emscripten_get_now(), start = began;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE))
    {
        emscripten_futex_wait((void *)flag, 0, 5.0);
        if (start > 0.0 && emscripten_get_now() - start > 20000.0)
        {
            Bo1_LogStall("waiting for the page's executor");
            start = -1.0;
        }
    }
    g_bo1WaitPageUs += (uint64_t)((emscripten_get_now() - began) * 1000.0);
}

// ============================================================================================================
// objects
// ============================================================================================================

#define BO1_IUNKNOWN                                                                                                  \
    ULONG m_refs = 1;                                                                                                 \
    bool m_released = false;                                                                                          \
    STDMETHOD(QueryInterface)(REFIID, void **ppvObj) override                                                        \
    {                                                                                                                 \
        *ppvObj = this;                                                                                               \
        AddRef();                                                                                                     \
        return S_OK;                                                                                                  \
    }                                                                                                                 \
    STDMETHOD_(ULONG, AddRef)() override { return __atomic_add_fetch(&m_refs, 1, __ATOMIC_SEQ_CST); }               \
    STDMETHOD_(ULONG, Release)() override                                                                             \
    {                                                                                                                 \
        if (m_released)                                                                                               \
        {                                                                                                             \
            Bo1_LateCall("Release");                                                                                  \
            return 0;                                                                                                 \
        }                                                                                                             \
        ULONG r = __atomic_sub_fetch(&m_refs, 1, __ATOMIC_SEQ_CST);                                                   \
        if (!r)                                                                                                       \
        {                                                                                                             \
            m_released = true;                                                                                        \
            Destroy();                                                                                                \
        }                                                                                                             \
        return r;                                                                                                     \
    }

#define BO1_RESOURCE(rtype)                                                                                           \
    STDMETHOD(GetDevice)(IDirect3DDevice9 * *ppDevice) override;                                                     \
    STDMETHOD(SetPrivateData)(REFGUID, CONST void *, DWORD, DWORD) override { return S_OK; }                         \
    STDMETHOD(GetPrivateData)(REFGUID, void *, DWORD *) override { return D3DERR_NOTFOUND; }                         \
    STDMETHOD(FreePrivateData)(REFGUID) override { return S_OK; }                                                    \
    STDMETHOD_(DWORD, SetPriority)(DWORD p) override                                                                 \
    {                                                                                                                 \
        DWORD old = m_priority;                                                                                       \
        m_priority = p;                                                                                               \
        return old;                                                                                                   \
    }                                                                                                                 \
    STDMETHOD_(DWORD, GetPriority)() override { return m_priority; }                                                 \
    STDMETHOD_(void, PreLoad)() override {}                                                                           \
    STDMETHOD_(D3DRESOURCETYPE, GetType)() override { return rtype; }                                                \
    DWORD m_priority = 0;

class Bo1Surface;
class Bo1Volume;

// ----- textures -----

struct Bo1Lock
{
    bool active = false;
    void *data = nullptr;
    uint32_t x = 0, y = 0, z = 0, w = 0, h = 0, d = 1;
    uint32_t pitch = 0, slicePitch = 0;
    DWORD flags = 0;
    uint32_t lockCount = 0;
    // a level locked more than once keeps its texels here, as D3D9 does (the engine patches parts of a texture,
    // e.g. the model lighting atlas every frame, and expects the rest to stay)
    void *shadow = nullptr;
};

class Bo1TextureBase
{
public:
    uint32_t m_id;
    TexType m_type;
    uint32_t m_width, m_height, m_depth;
    uint32_t m_levels;
    D3DFORMAT m_format;
    DWORD m_usage;
    D3DPOOL m_pool;
    // system memory textures keep their texels (UpdateTexture uploads them); one block per face and level
    std::vector<void *> m_sysmem;
    std::vector<Bo1Lock> m_locks;      // [face * levels + level]
    std::vector<Bo1Surface *> m_surfs; // [face * levels + level], created on demand, weak

    Bo1TextureBase(TexType type, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, D3DFORMAT format, DWORD usage, D3DPOOL pool);
    ~Bo1TextureBase();
    uint32_t Faces() const { return m_type == TEX_CUBE ? 6 : 1; }
    uint32_t LevelW(uint32_t l) const { return Bo1_MipSize(m_width, l); }
    uint32_t LevelH(uint32_t l) const { return Bo1_MipSize(m_height, l); }
    uint32_t LevelD(uint32_t l) const { return m_type == TEX_3D ? Bo1_MipSize(m_depth, l) : 1; }
    uint32_t LevelBytes(uint32_t l) const { return Bo1Fmt_LevelBytes(m_format, LevelW(l), LevelH(l)) * LevelD(l); }
    bool IsSysmem() const { return m_pool == D3DPOOL_SYSTEMMEM || m_pool == D3DPOOL_SCRATCH; }
    void *SysmemLevel(uint32_t face, uint32_t level);
    HRESULT Lock(uint32_t face, uint32_t level, const RECT *rect, const D3DBOX *box, DWORD flags, void **bits, INT *pitch, INT *slicePitch);
    HRESULT Unlock(uint32_t face, uint32_t level);
    void UploadLevel(uint32_t face, uint32_t level, const void *src);
    Bo1Surface *Surface(uint32_t face, uint32_t level);
    void Unbind();
    void DestroyBase();
};

#define BO1_BASETEXTURE                                                                                               \
    STDMETHOD_(DWORD, SetLOD)(DWORD) override { return 0; }                                                          \
    STDMETHOD_(DWORD, GetLOD)() override { return 0; }                                                               \
    STDMETHOD_(DWORD, GetLevelCount)() override { return m_levels; }                                                 \
    STDMETHOD(SetAutoGenFilterType)(D3DTEXTUREFILTERTYPE) override { return S_OK; }                                  \
    STDMETHOD_(D3DTEXTUREFILTERTYPE, GetAutoGenFilterType)() override { return D3DTEXF_LINEAR; }                     \
    STDMETHOD_(void, GenerateMipSubLevels)() override;

class Bo1Texture : public IDirect3DTexture9, public Bo1TextureBase
{
public:
    Bo1Texture(uint32_t w, uint32_t h, uint32_t levels, DWORD usage, D3DFORMAT f, D3DPOOL pool)
        : Bo1TextureBase(TEX_2D, w, h, 1, levels, f, usage, pool)
    {
    }
    void Destroy()
    {
        DestroyBase();
        Bo1_Retire([this] { delete this; });
    }
    BO1_IUNKNOWN
    BO1_RESOURCE(D3DRTYPE_TEXTURE)
    BO1_BASETEXTURE
    STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC *pDesc) override;
    STDMETHOD(GetSurfaceLevel)(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) override;
    STDMETHOD(LockRect)(UINT Level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override
    {
        return Lock(0, Level, pRect, nullptr, Flags, &pLockedRect->pBits, &pLockedRect->Pitch, nullptr);
    }
    STDMETHOD(UnlockRect)(UINT Level) override { return Unlock(0, Level); }
    STDMETHOD(AddDirtyRect)(CONST RECT *) override { return S_OK; }
};

class Bo1CubeTexture : public IDirect3DCubeTexture9, public Bo1TextureBase
{
public:
    Bo1CubeTexture(uint32_t edge, uint32_t levels, DWORD usage, D3DFORMAT f, D3DPOOL pool) : Bo1TextureBase(TEX_CUBE, edge, edge, 1, levels, f, usage, pool) {}
    void Destroy()
    {
        DestroyBase();
        Bo1_Retire([this] { delete this; });
    }
    BO1_IUNKNOWN
    BO1_RESOURCE(D3DRTYPE_CUBETEXTURE)
    BO1_BASETEXTURE
    STDMETHOD(GetLevelDesc)(UINT Level, D3DSURFACE_DESC *pDesc) override;
    STDMETHOD(GetCubeMapSurface)(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) override;
    STDMETHOD(LockRect)(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override
    {
        return Lock((uint32_t)FaceType, Level, pRect, nullptr, Flags, &pLockedRect->pBits, &pLockedRect->Pitch, nullptr);
    }
    STDMETHOD(UnlockRect)(D3DCUBEMAP_FACES FaceType, UINT Level) override { return Unlock((uint32_t)FaceType, Level); }
    STDMETHOD(AddDirtyRect)(D3DCUBEMAP_FACES, CONST RECT *) override { return S_OK; }
};

class Bo1VolumeTexture : public IDirect3DVolumeTexture9, public Bo1TextureBase
{
public:
    Bo1VolumeTexture(uint32_t w, uint32_t h, uint32_t d, uint32_t levels, DWORD usage, D3DFORMAT f, D3DPOOL pool)
        : Bo1TextureBase(TEX_3D, w, h, d, levels, f, usage, pool)
    {
    }
    void Destroy()
    {
        DestroyBase();
        Bo1_Retire([this] { delete this; });
    }
    BO1_IUNKNOWN
    BO1_RESOURCE(D3DRTYPE_VOLUMETEXTURE)
    BO1_BASETEXTURE
    STDMETHOD(GetLevelDesc)(UINT Level, D3DVOLUME_DESC *pDesc) override
    {
        if (Level >= m_levels)
            return D3DERR_INVALIDCALL;
        pDesc->Format = m_format;
        pDesc->Type = D3DRTYPE_VOLUME;
        pDesc->Usage = m_usage;
        pDesc->Pool = m_pool;
        pDesc->Width = LevelW(Level);
        pDesc->Height = LevelH(Level);
        pDesc->Depth = LevelD(Level);
        return S_OK;
    }
    STDMETHOD(GetVolumeLevel)(UINT, IDirect3DVolume9 **ppVolumeLevel) override
    {
        *ppVolumeLevel = nullptr;
        return D3DERR_INVALIDCALL;
    }
    STDMETHOD(LockBox)(UINT Level, D3DLOCKED_BOX *pLockedVolume, CONST D3DBOX *pBox, DWORD Flags) override
    {
        return Lock(0, Level, nullptr, pBox, Flags, &pLockedVolume->pBits, &pLockedVolume->RowPitch, &pLockedVolume->SlicePitch);
    }
    STDMETHOD(UnlockBox)(UINT Level) override { return Unlock(0, Level); }
    STDMETHOD(AddDirtyBox)(CONST D3DBOX *) override { return S_OK; }
};

static IDirect3DBaseTexture9 *Bo1_AsInterface(Bo1TextureBase *t);
static Bo1TextureBase *Bo1_AsTexture(IDirect3DBaseTexture9 *t)
{
    if (!t)
        return nullptr;
    switch (t->GetType())
    {
    case D3DRTYPE_TEXTURE:
        return static_cast<Bo1Texture *>((IDirect3DTexture9 *)t);
    case D3DRTYPE_CUBETEXTURE:
        return static_cast<Bo1CubeTexture *>((IDirect3DCubeTexture9 *)t);
    case D3DRTYPE_VOLUMETEXTURE:
        return static_cast<Bo1VolumeTexture *>((IDirect3DVolumeTexture9 *)t);
    default:
        return nullptr;
    }
}

static IDirect3DBaseTexture9 *Bo1_AsInterface(Bo1TextureBase *t)
{
    switch (t->m_type)
    {
    case TEX_2D:
        return static_cast<IDirect3DTexture9 *>(static_cast<Bo1Texture *>(t));
    case TEX_CUBE:
        return static_cast<IDirect3DCubeTexture9 *>(static_cast<Bo1CubeTexture *>(t));
    default:
        return static_cast<IDirect3DVolumeTexture9 *>(static_cast<Bo1VolumeTexture *>(t));
    }
}

// ----- surfaces -----

class Bo1Surface : public IDirect3DSurface9
{
public:
    SurfKind m_kind;
    uint32_t m_id = 0;                   // standalone surfaces
    Bo1TextureBase *m_texture = nullptr; // texture levels (holds a reference)
    uint32_t m_face = 0, m_level = 0;
    uint32_t m_width, m_height;
    D3DFORMAT m_format;
    D3DPOOL m_pool;
    DWORD m_usage;
    D3DMULTISAMPLE_TYPE m_ms = D3DMULTISAMPLE_NONE;
    void *m_sysmem = nullptr; // offscreen plain surfaces in system memory, and read back render targets
    bool m_hasGpu;            // the executor holds a GPU object for it (render target / depth)
    Bo1Lock m_lock;

    Bo1Surface(SurfKind kind, uint32_t w, uint32_t h, D3DFORMAT f, D3DPOOL pool, DWORD usage)
        : m_kind(kind), m_width(w), m_height(h), m_format(f), m_pool(pool), m_usage(usage), m_hasGpu(false)
    {
    }
    void Destroy();
    SurfRef Ref() const
    {
        SurfRef r;
        r.kind = m_kind;
        r.id = m_kind == SURF_TEXTURE ? m_texture->m_id : m_id;
        r.level = m_level;
        r.face = m_face;
        return r;
    }
    BO1_IUNKNOWN
    BO1_RESOURCE(D3DRTYPE_SURFACE)
    STDMETHOD(GetContainer)(REFIID, void **ppContainer) override
    {
        if (m_kind == SURF_TEXTURE)
        {
            IDirect3DBaseTexture9 *t = Bo1_AsInterface(m_texture);
            t->AddRef();
            *ppContainer = t;
            return S_OK;
        }
        *ppContainer = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD(GetDesc)(D3DSURFACE_DESC *pDesc) override
    {
        pDesc->Format = m_format;
        pDesc->Type = D3DRTYPE_SURFACE;
        pDesc->Usage = m_usage;
        pDesc->Pool = m_pool;
        pDesc->MultiSampleType = m_ms;
        pDesc->MultiSampleQuality = 0;
        pDesc->Width = m_width;
        pDesc->Height = m_height;
        return S_OK;
    }
    STDMETHOD(LockRect)(D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override;
    STDMETHOD(UnlockRect)() override;
    STDMETHOD(GetDC)(HDC *phdc) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(ReleaseDC)(HDC) override { return D3DERR_INVALIDCALL; }
};

// ----- buffers -----

class Bo1Buffer
{
public:
    uint32_t m_id;
    uint32_t m_size;
    DWORD m_usage;
    D3DPOOL m_pool;
    D3DFORMAT m_format; // index buffers
    bool m_index;
    uint8_t *m_shadow = nullptr; // dynamic buffers keep a CPU copy (partial NOOVERWRITE locks)
    uint8_t *m_lockData = nullptr;
    uint32_t m_lockOffset = 0, m_lockSize = 0;
    DWORD m_lockFlags = 0;
    int m_lockCount = 0;

    Bo1Buffer(bool index, uint32_t size, DWORD usage, D3DPOOL pool, D3DFORMAT format)
        : m_id(Bo1_NewId()), m_size(size), m_usage(usage), m_pool(pool), m_format(format), m_index(index)
    {
        if (usage & D3DUSAGE_DYNAMIC)
            m_shadow = (uint8_t *)calloc(1, size ? size : 1);
        CmdCreateBuffer *c = Cmd<CmdCreateBuffer>(OP_CREATE_BUFFER);
        c->id = m_id;
        c->index = index;
        c->size = size;
        c->usage = usage;
        c->format = format;
        EndCmd();
    }

    HRESULT DoLock(UINT offset, UINT size, void **ppbData, DWORD flags)
    {
        if (!size || offset + size > m_size)
            size = m_size - (offset < m_size ? offset : m_size);
        if (m_lockCount++)
        {
            // nested lock of a dynamic buffer (the engine does not; keep the first range)
            *ppbData = m_shadow ? m_shadow + offset : m_lockData + (offset - m_lockOffset);
            return S_OK;
        }
        m_lockOffset = offset;
        m_lockSize = size;
        m_lockFlags = flags;
        if (m_shadow)
            *ppbData = m_shadow + offset;
        else
        {
            m_lockData = (uint8_t *)malloc(size ? size : 1);
            if (flags & D3DLOCK_READONLY)
                memset(m_lockData, 0, size); // no CPU copy of a static buffer to read back
            *ppbData = m_lockData;
        }
        return S_OK;
    }

    HRESULT DoUnlock()
    {
        if (!m_lockCount || --m_lockCount)
            return m_lockCount >= 0 ? S_OK : D3DERR_INVALIDCALL;
        if (m_lockFlags & D3DLOCK_READONLY)
        {
            free(m_lockData);
            m_lockData = nullptr;
            return S_OK;
        }
        CmdUploadBuffer *c = Cmd<CmdUploadBuffer>(OP_UPLOAD_BUFFER);
        c->id = m_id;
        c->offset = m_lockOffset;
        c->size = m_lockSize;
        c->discard = (m_lockFlags & D3DLOCK_DISCARD) != 0;
        if (m_shadow)
        {
            // the engine writes the next range while the executor has not uploaded this one: send a copy
            c->data = malloc(m_lockSize ? m_lockSize : 1);
            memcpy(c->data, m_shadow + m_lockOffset, m_lockSize);
        }
        else
        {
            c->data = m_lockData;
            m_lockData = nullptr;
        }
        EndCmd();
        return S_OK;
    }

    void DestroyBuffer() { Bo1_CmdDestroy(OBJ_BUFFER, m_id); }
    ~Bo1Buffer()
    {
        free(m_shadow);
        free(m_lockData);
    }
};

class Bo1VertexBuffer : public IDirect3DVertexBuffer9, public Bo1Buffer
{
public:
    Bo1VertexBuffer(uint32_t size, DWORD usage, D3DPOOL pool) : Bo1Buffer(false, size, usage, pool, D3DFMT_VERTEXDATA) {}
    void Destroy();
    BO1_IUNKNOWN
    BO1_RESOURCE(D3DRTYPE_VERTEXBUFFER)
    STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) override
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        return DoLock(OffsetToLock, SizeToLock, ppbData, Flags);
    }
    STDMETHOD(Unlock)() override
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        return DoUnlock();
    }
    STDMETHOD(GetDesc)(D3DVERTEXBUFFER_DESC *pDesc) override
    {
        pDesc->Format = D3DFMT_VERTEXDATA;
        pDesc->Type = D3DRTYPE_VERTEXBUFFER;
        pDesc->Usage = m_usage;
        pDesc->Pool = m_pool;
        pDesc->Size = m_size;
        pDesc->FVF = 0;
        return S_OK;
    }
};

class Bo1IndexBuffer : public IDirect3DIndexBuffer9, public Bo1Buffer
{
public:
    Bo1IndexBuffer(uint32_t size, DWORD usage, D3DFORMAT format, D3DPOOL pool) : Bo1Buffer(true, size, usage, pool, format) {}
    void Destroy();
    BO1_IUNKNOWN
    BO1_RESOURCE(D3DRTYPE_INDEXBUFFER)
    STDMETHOD(Lock)(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) override
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        return DoLock(OffsetToLock, SizeToLock, ppbData, Flags);
    }
    STDMETHOD(Unlock)() override
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        return DoUnlock();
    }
    STDMETHOD(GetDesc)(D3DINDEXBUFFER_DESC *pDesc) override
    {
        pDesc->Format = m_format;
        pDesc->Type = D3DRTYPE_INDEXBUFFER;
        pDesc->Usage = m_usage;
        pDesc->Pool = m_pool;
        pDesc->Size = m_size;
        return S_OK;
    }
};

// ----- shaders, declarations, queries -----

class Bo1VertexDecl : public IDirect3DVertexDeclaration9
{
public:
    uint32_t m_id;
    std::vector<D3DVERTEXELEMENT9> m_elements; // D3DDECL_END included
    void Destroy();
    BO1_IUNKNOWN
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) override;
    STDMETHOD(GetDeclaration)(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) override
    {
        if (pElement)
            memcpy(pElement, m_elements.data(), m_elements.size() * sizeof(D3DVERTEXELEMENT9));
        *pNumElements = (UINT)m_elements.size();
        return S_OK;
    }
};

class Bo1Shader
{
public:
    uint32_t m_id;
    std::vector<DWORD> m_code;
};

class Bo1VertexShader : public IDirect3DVertexShader9, public Bo1Shader
{
public:
    void Destroy();
    BO1_IUNKNOWN
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) override;
    STDMETHOD(GetFunction)(void *pData, UINT *pSizeOfData) override
    {
        if (pData)
            memcpy(pData, m_code.data(), m_code.size() * 4);
        *pSizeOfData = (UINT)(m_code.size() * 4);
        return S_OK;
    }
};

class Bo1PixelShader : public IDirect3DPixelShader9, public Bo1Shader
{
public:
    void Destroy();
    BO1_IUNKNOWN
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) override;
    STDMETHOD(GetFunction)(void *pData, UINT *pSizeOfData) override
    {
        if (pData)
            memcpy(pData, m_code.data(), m_code.size() * 4);
        *pSizeOfData = (UINT)(m_code.size() * 4);
        return S_OK;
    }
};

class Bo1Query : public IDirect3DQuery9
{
public:
    D3DQUERYTYPE m_type;
    uint32_t m_id;
    volatile int32_t *m_done;  // separate allocations: the executor may set them after the query is released
    OcclusionAnswer *m_answer; // an occlusion query's, "visible" until the first
    bool m_issued = false;
    explicit Bo1Query(D3DQUERYTYPE type) : m_type(type), m_id(Bo1_NewId())
    {
        m_done = (volatile int32_t *)calloc(1, 64);
        *m_done = 1;
        m_answer = (OcclusionAnswer *)calloc(1, 64);
        m_answer->samples = BO1_OCCLUSION_VISIBLE;
    }
    void Destroy()
    {
        // the flag may still be pending in the ring: freed with the object, seconds later
        Bo1_Retire([this] {
            free((void *)m_done);
            free(m_answer);
            delete this;
        });
    }
    BO1_IUNKNOWN
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) override;
    STDMETHOD_(D3DQUERYTYPE, GetType)() override { return m_type; }
    STDMETHOD_(DWORD, GetDataSize)() override
    {
        switch (m_type)
        {
        case D3DQUERYTYPE_EVENT:
            return sizeof(BOOL);
        case D3DQUERYTYPE_OCCLUSION:
            return sizeof(DWORD);
        case D3DQUERYTYPE_TIMESTAMP:
        case D3DQUERYTYPE_TIMESTAMPFREQ:
            return sizeof(UINT64);
        case D3DQUERYTYPE_TIMESTAMPDISJOINT:
            return sizeof(BOOL);
        default:
            return 0;
        }
    }
    STDMETHOD(Issue)(DWORD dwIssueFlags) override
    {
        if (m_released)
        {
            Bo1_LateCall("IDirect3DQuery9::Issue");
            return S_OK;
        }
        if (m_type == D3DQUERYTYPE_OCCLUSION)
        {
            // begun and ended among the draws; the GPU answers frames later (d3d9_webgpu.cpp: occlusion queries)
            std::lock_guard<std::recursive_mutex> guard(s_dev);
            CmdQuery *c = Cmd<CmdQuery>(dwIssueFlags & D3DISSUE_END ? OP_QUERY_END : OP_QUERY_BEGIN);
            c->id = m_id;
            c->done = nullptr;
            c->answer = m_answer;
            EndCmd();
            m_issued = true;
            return S_OK;
        }
        if (!(dwIssueFlags & D3DISSUE_END))
            return S_OK;
        // a timestamp is the time it is read (GetData): only an event waits for the page to replay what came before it
        if (m_type != D3DQUERYTYPE_EVENT)
        {
            m_issued = true;
            return S_OK;
        }
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        __atomic_store_n(m_done, 0, __ATOMIC_RELEASE);
        CmdQuery *c = Cmd<CmdQuery>(OP_QUERY_END);
        c->id = m_id;
        c->done = m_done;
        c->answer = nullptr;
        EndCmd();
        m_issued = true;
        return S_OK;
    }
    STDMETHOD(GetData)(void *pData, DWORD dwSize, DWORD) override
    {
        if (m_released)
        {
            Bo1_LateCall("IDirect3DQuery9::GetData");
            return S_OK;
        }
        if (!__atomic_load_n(m_done, __ATOMIC_ACQUIRE))
            return S_FALSE;
        if (pData && dwSize)
        {
            switch (m_type)
            {
            case D3DQUERYTYPE_EVENT:
                *(BOOL *)pData = TRUE;
                break;
            case D3DQUERYTYPE_OCCLUSION:
                // The newest answer, at once: coronas and the sun read their queries two frames on (rb_corona.cpp,
                // rb_sky.cpp) and wait for the answer, which would hold the render thread until the GPU had drawn that
                // frame. They fade towards it, so one that comes a frame or two later is not seen.
                *(DWORD *)pData = __atomic_load_n(&m_answer->samples, __ATOMIC_ACQUIRE);
                break;
            case D3DQUERYTYPE_TIMESTAMP:
                *(UINT64 *)pData = (UINT64)(Bo1_Milliseconds() * 1000.0);
                break;
            case D3DQUERYTYPE_TIMESTAMPFREQ:
                *(UINT64 *)pData = 1000000;
                break;
            default:
                memset(pData, 0, dwSize);
                break;
            }
        }
        return S_OK;
    }
};

class Bo1SwapChain : public IDirect3DSwapChain9
{
public:
    void Destroy() {}
    BO1_IUNKNOWN
    STDMETHOD(Present)(CONST RECT *, CONST RECT *, HWND, CONST RGNDATA *, DWORD) override;
    STDMETHOD(GetFrontBufferData)(IDirect3DSurface9 *pDestSurface) override;
    STDMETHOD(GetBackBuffer)(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **ppBackBuffer) override;
    STDMETHOD(GetRasterStatus)(D3DRASTER_STATUS *pRasterStatus) override
    {
        memset(pRasterStatus, 0, sizeof(*pRasterStatus));
        return S_OK;
    }
    STDMETHOD(GetDisplayMode)(D3DDISPLAYMODE *pMode) override;
    STDMETHOD(GetDevice)(IDirect3DDevice9 **ppDevice) override;
    STDMETHOD(GetPresentParameters)(D3DPRESENT_PARAMETERS *pp) override;
};

// ============================================================================================================
// the device
// ============================================================================================================

static const int BO1_MAX_SAMPLERS = 261; // 0..15 pixel, 257..260 vertex (D3DVERTEXTEXTURESAMPLER0)

class Bo1Device : public IDirect3DDevice9
{
public:
    D3DPRESENT_PARAMETERS m_pp;
    HWND m_window;
    DWORD m_behavior;
    Bo1Surface *m_backBuffer = nullptr;
    Bo1SwapChain m_swapChain;
    uint32_t m_frame = 0;
    double m_frameBegan = 0.0; // BeginScene of the frame the render thread draws
    double m_frameInput = 0.0; // when the mouse had last moved as the game thread read it for that frame
    D3DGAMMARAMP m_gamma;      // SetGammaRamp's, the identity until then
    struct
    {
        uint32_t level = 2;       // g_ring.pacing
        double holdUntil = 0.0;   // no lower before then
        double hold = 2000.0;     // what a missed refresh holds it up for
        double lowered = 0.0;     // when it last went lower
        uint32_t misses = 0;      // g_ring.misses, seen
        double second = 0.0;      // the slowest frames: this second's from then, and the four seconds before
        float chainUs[5] = {}, renderUs[5] = {};
    } m_pacing;                   // Present
    void Pace(double now, float renderMs);

    DWORD m_rs[256];
    DWORD m_ss[BO1_MAX_SAMPLERS][D3DSAMP_DMAPOFFSET + 1];
    IDirect3DBaseTexture9 *m_textures[BO1_MAX_SAMPLERS] = {};
    IDirect3DVertexBuffer9 *m_streams[16] = {};
    UINT m_streamOffset[16] = {};
    UINT m_streamStride[16] = {};
    UINT m_streamFreq[16];
    IDirect3DIndexBuffer9 *m_indices = nullptr;
    IDirect3DVertexDeclaration9 *m_decl = nullptr;
    IDirect3DVertexShader9 *m_vs = nullptr;
    IDirect3DPixelShader9 *m_ps = nullptr;
    IDirect3DSurface9 *m_rt[4] = {};
    IDirect3DSurface9 *m_ds = nullptr;
    D3DVIEWPORT9 m_viewport;
    RECT m_scissor;
    float m_vsConstF[256 * 4] = {};
    float m_psConstF[224 * 4] = {};
    int m_vsConstI[16 * 4] = {};
    int m_psConstI[16 * 4] = {};
    BOOL m_vsConstB[16] = {};
    BOOL m_psConstB[16] = {};
    // fixed-function state, kept only for the Get calls: the engine draws with shaders and vertex declarations (only
    // Bink, which the web build leaves out, sets these)
    DWORD m_tss[8][33] = {};
    float m_clipPlanes[6][4] = {};
    D3DMATRIX m_transforms[512];
    DWORD m_fvf = 0;

    Bo1Device(HWND window, DWORD behavior, const D3DPRESENT_PARAMETERS *pp);
    void Destroy() {}
    void ResetState();
    void CreateBackBuffer();

    BO1_IUNKNOWN

    STDMETHOD(TestCooperativeLevel)() override { return D3D_OK; }
    STDMETHOD_(UINT, GetAvailableTextureMem)() override { return 2048u << 20; }
    STDMETHOD(EvictManagedResources)() override { return S_OK; }
    STDMETHOD(GetDirect3D)(IDirect3D9 **ppD3D9) override;
    STDMETHOD(GetDeviceCaps)(D3DCAPS9 *pCaps) override;
    STDMETHOD(GetDisplayMode)(UINT, D3DDISPLAYMODE *pMode) override { return m_swapChain.GetDisplayMode(pMode); }
    STDMETHOD(GetCreationParameters)(D3DDEVICE_CREATION_PARAMETERS *p) override
    {
        p->AdapterOrdinal = 0;
        p->DeviceType = D3DDEVTYPE_HAL;
        p->hFocusWindow = m_window;
        p->BehaviorFlags = m_behavior;
        return S_OK;
    }
    STDMETHOD(SetCursorProperties)(UINT, UINT, IDirect3DSurface9 *) override { return S_OK; }
    STDMETHOD_(void, SetCursorPosition)(int, int, DWORD) override {}
    STDMETHOD_(BOOL, ShowCursor)(BOOL) override { return FALSE; }
    STDMETHOD(CreateAdditionalSwapChain)(D3DPRESENT_PARAMETERS *, IDirect3DSwapChain9 **) override { return D3DERR_NOTAVAILABLE; }
    STDMETHOD(GetSwapChain)(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain) override
    {
        if (iSwapChain)
            return D3DERR_INVALIDCALL;
        m_swapChain.AddRef();
        *pSwapChain = &m_swapChain;
        return S_OK;
    }
    STDMETHOD_(UINT, GetNumberOfSwapChains)() override { return 1; }
    STDMETHOD(Reset)(D3DPRESENT_PARAMETERS *pPresentationParameters) override;
    STDMETHOD(Present)(CONST RECT *, CONST RECT *, HWND, CONST RGNDATA *) override;
    STDMETHOD(GetBackBuffer)(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **ppBackBuffer) override
    {
        m_backBuffer->AddRef();
        *ppBackBuffer = m_backBuffer;
        return S_OK;
    }
    STDMETHOD(GetRasterStatus)(UINT, D3DRASTER_STATUS *pRasterStatus) override { return m_swapChain.GetRasterStatus(pRasterStatus); }
    STDMETHOD(SetDialogBoxMode)(BOOL) override { return S_OK; }
    // The game's brightness (r_gamma: R_SetColorMappings), as the ramp it gives the display, which a browser can't set:
    // the executor applies it as the frame goes onto the canvas (d3d9_webgpu.cpp: Present), as the display would.
    STDMETHOD_(void, SetGammaRamp)(UINT, DWORD, CONST D3DGAMMARAMP *pRamp) override
    {
        if (!pRamp)
            return;
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        m_gamma = *pRamp;
        CmdGamma *c = Cmd<CmdGamma>(OP_GAMMA);
        for (int i = 0; i < 256; ++i)
        {
            c->rgba[i * 4] = (uint8_t)((pRamp->red[i] + 128u) / 257u);
            c->rgba[i * 4 + 1] = (uint8_t)((pRamp->green[i] + 128u) / 257u);
            c->rgba[i * 4 + 2] = (uint8_t)((pRamp->blue[i] + 128u) / 257u);
            c->rgba[i * 4 + 3] = 255;
        }
        EndCmd();
    }
    STDMETHOD_(void, GetGammaRamp)(UINT, D3DGAMMARAMP *pRamp) override { *pRamp = m_gamma; }
    STDMETHOD(CreateTexture)(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture,
        HANDLE *) override;
    STDMETHOD(CreateVolumeTexture)(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
        IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *) override;
    STDMETHOD(CreateCubeTexture)(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture,
        HANDLE *) override;
    STDMETHOD(CreateVertexBuffer)(UINT Length, DWORD Usage, DWORD, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *) override
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        *ppVertexBuffer = new Bo1VertexBuffer(Length, Usage, Pool);
        return S_OK;
    }
    STDMETHOD(CreateIndexBuffer)(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *) override
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        *ppIndexBuffer = new Bo1IndexBuffer(Length, Usage, Format, Pool);
        return S_OK;
    }
    STDMETHOD(CreateRenderTarget)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD, BOOL,
        IDirect3DSurface9 **ppSurface, HANDLE *) override;
    STDMETHOD(CreateDepthStencilSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD, BOOL,
        IDirect3DSurface9 **ppSurface, HANDLE *) override;
    STDMETHOD(UpdateSurface)(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface,
        CONST POINT *pDestPoint) override;
    STDMETHOD(UpdateTexture)(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) override;
    STDMETHOD(GetRenderTargetData)(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) override;
    STDMETHOD(GetFrontBufferData)(UINT, IDirect3DSurface9 *pDestSurface) override { return GetRenderTargetData(m_backBuffer, pDestSurface); }
    STDMETHOD(StretchRect)(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, CONST RECT *pDestRect,
        D3DTEXTUREFILTERTYPE Filter) override;
    STDMETHOD(ColorFill)(IDirect3DSurface9 *pSurface, CONST RECT *pRect, D3DCOLOR color) override;
    STDMETHOD(CreateOffscreenPlainSurface)(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *) override;
    STDMETHOD(SetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) override;
    STDMETHOD(GetRenderTarget)(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) override
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        if (RenderTargetIndex >= 4)
            return D3DERR_INVALIDCALL;
        *ppRenderTarget = m_rt[RenderTargetIndex];
        if (!*ppRenderTarget)
            return D3DERR_NOTFOUND;
        m_rt[RenderTargetIndex]->AddRef();
        return S_OK;
    }
    STDMETHOD(SetDepthStencilSurface)(IDirect3DSurface9 *pNewZStencil) override;
    STDMETHOD(GetDepthStencilSurface)(IDirect3DSurface9 **ppZStencilSurface) override
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        *ppZStencilSurface = m_ds;
        if (!m_ds)
            return D3DERR_NOTFOUND;
        m_ds->AddRef();
        return S_OK;
    }
    STDMETHOD(BeginScene)() override
    {
        // the render thread starts drawing a frame (RB_BeginFrame): how long it takes, and when the mouse had moved
        if (m_frameBegan == 0.0)
            m_frameBegan = emscripten_get_now();
        if (double input = Bo1_FrameTakeInput())
            m_frameInput = input;
        return S_OK;
    }
    STDMETHOD(EndScene)() override { return S_OK; }
    STDMETHOD(Clear)(DWORD Count, CONST D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override;
    STDMETHOD(SetTransform)(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix) override
    {
        if ((unsigned)State < 512)
            m_transforms[State] = *pMatrix;
        return S_OK;
    }
    STDMETHOD(GetTransform)(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) override
    {
        if ((unsigned)State < 512)
            *pMatrix = m_transforms[State];
        return S_OK;
    }
    STDMETHOD(MultiplyTransform)(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX *) override { return S_OK; }
    STDMETHOD(SetViewport)(CONST D3DVIEWPORT9 *pViewport) override;
    STDMETHOD(GetViewport)(D3DVIEWPORT9 *pViewport) override
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        *pViewport = m_viewport;
        return S_OK;
    }
    STDMETHOD(SetMaterial)(CONST D3DMATERIAL9 *) override { return S_OK; }
    STDMETHOD(GetMaterial)(D3DMATERIAL9 *p) override
    {
        memset(p, 0, sizeof(*p));
        return S_OK;
    }
    STDMETHOD(SetLight)(DWORD, CONST D3DLIGHT9 *) override { return S_OK; }
    STDMETHOD(GetLight)(DWORD, D3DLIGHT9 *) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(LightEnable)(DWORD, BOOL) override { return S_OK; }
    STDMETHOD(GetLightEnable)(DWORD, BOOL *pEnable) override
    {
        *pEnable = FALSE;
        return S_OK;
    }
    STDMETHOD(SetClipPlane)(DWORD Index, CONST float *pPlane) override;
    STDMETHOD(GetClipPlane)(DWORD Index, float *pPlane) override
    {
        if (Index >= 6)
            return D3DERR_INVALIDCALL;
        memcpy(pPlane, m_clipPlanes[Index], 16);
        return S_OK;
    }
    STDMETHOD(SetRenderState)(D3DRENDERSTATETYPE State, DWORD Value) override;
    STDMETHOD(GetRenderState)(D3DRENDERSTATETYPE State, DWORD *pValue) override
    {
        if ((unsigned)State >= 256)
            return D3DERR_INVALIDCALL;
        *pValue = m_rs[State];
        return S_OK;
    }
    STDMETHOD(CreateStateBlock)(D3DSTATEBLOCKTYPE, IDirect3DStateBlock9 **ppSB) override
    {
        *ppSB = nullptr;
        return D3DERR_NOTAVAILABLE;
    }
    STDMETHOD(BeginStateBlock)() override { return D3DERR_NOTAVAILABLE; }
    STDMETHOD(EndStateBlock)(IDirect3DStateBlock9 **ppSB) override
    {
        *ppSB = nullptr;
        return D3DERR_NOTAVAILABLE;
    }
    STDMETHOD(SetClipStatus)(CONST D3DCLIPSTATUS9 *) override { return S_OK; }
    STDMETHOD(GetClipStatus)(D3DCLIPSTATUS9 *p) override
    {
        memset(p, 0, sizeof(*p));
        return S_OK;
    }
    STDMETHOD(GetTexture)(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) override
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        if (Stage >= (DWORD)BO1_MAX_SAMPLERS)
            return D3DERR_INVALIDCALL;
        *ppTexture = m_textures[Stage];
        if (*ppTexture)
            (*ppTexture)->AddRef();
        return S_OK;
    }
    STDMETHOD(SetTexture)(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override;
    STDMETHOD(GetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override
    {
        if (Stage >= 8 || (unsigned)Type > 32)
            return D3DERR_INVALIDCALL;
        *pValue = m_tss[Stage][Type];
        return S_OK;
    }
    STDMETHOD(SetTextureStageState)(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
    STDMETHOD(GetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) override
    {
        if (Sampler >= (DWORD)BO1_MAX_SAMPLERS || (unsigned)Type > D3DSAMP_DMAPOFFSET)
            return D3DERR_INVALIDCALL;
        *pValue = m_ss[Sampler][Type];
        return S_OK;
    }
    STDMETHOD(SetSamplerState)(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override;
    STDMETHOD(ValidateDevice)(DWORD *pNumPasses) override
    {
        *pNumPasses = 1;
        return S_OK;
    }
    STDMETHOD(SetPaletteEntries)(UINT, CONST PALETTEENTRY *) override { return S_OK; }
    STDMETHOD(GetPaletteEntries)(UINT, PALETTEENTRY *) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(SetCurrentTexturePalette)(UINT) override { return S_OK; }
    STDMETHOD(GetCurrentTexturePalette)(UINT *p) override
    {
        *p = 0;
        return S_OK;
    }
    STDMETHOD(SetScissorRect)(CONST RECT *pRect) override;
    STDMETHOD(GetScissorRect)(RECT *pRect) override
    {
        *pRect = m_scissor;
        return S_OK;
    }
    STDMETHOD(SetSoftwareVertexProcessing)(BOOL) override { return S_OK; }
    STDMETHOD_(BOOL, GetSoftwareVertexProcessing)() override { return FALSE; }
    STDMETHOD(SetNPatchMode)(float) override { return S_OK; }
    STDMETHOD_(float, GetNPatchMode)() override { return 0.0f; }
    STDMETHOD(DrawPrimitive)(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override;
    STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex,
        UINT primCount) override;
    STDMETHOD(DrawPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
    STDMETHOD(DrawIndexedPrimitiveUP)(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount,
        CONST void *pIndexData, D3DFORMAT IndexDataFormat, CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
    STDMETHOD(ProcessVertices)(UINT, UINT, UINT, IDirect3DVertexBuffer9 *, IDirect3DVertexDeclaration9 *, DWORD) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(CreateVertexDeclaration)(CONST D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) override;
    STDMETHOD(SetVertexDeclaration)(IDirect3DVertexDeclaration9 *pDecl) override;
    STDMETHOD(GetVertexDeclaration)(IDirect3DVertexDeclaration9 **ppDecl) override
    {
        *ppDecl = m_decl;
        if (m_decl)
            m_decl->AddRef();
        return S_OK;
    }
    STDMETHOD(SetFVF)(DWORD FVF) override;
    STDMETHOD(GetFVF)(DWORD *pFVF) override
    {
        *pFVF = m_fvf;
        return S_OK;
    }
    STDMETHOD(CreateVertexShader)(CONST DWORD *pFunction, IDirect3DVertexShader9 **ppShader) override;
    STDMETHOD(SetVertexShader)(IDirect3DVertexShader9 *pShader) override;
    STDMETHOD(GetVertexShader)(IDirect3DVertexShader9 **ppShader) override
    {
        *ppShader = m_vs;
        if (m_vs)
            m_vs->AddRef();
        return S_OK;
    }
    STDMETHOD(SetVertexShaderConstantF)(UINT StartRegister, CONST float *pConstantData, UINT Vector4fCount) override;
    STDMETHOD(GetVertexShaderConstantF)(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override
    {
        if (StartRegister + Vector4fCount > 256)
            return D3DERR_INVALIDCALL;
        memcpy(pConstantData, &m_vsConstF[StartRegister * 4], Vector4fCount * 16);
        return S_OK;
    }
    STDMETHOD(SetVertexShaderConstantI)(UINT StartRegister, CONST int *pConstantData, UINT Vector4iCount) override;
    STDMETHOD(GetVertexShaderConstantI)(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override
    {
        if (StartRegister + Vector4iCount > 16)
            return D3DERR_INVALIDCALL;
        memcpy(pConstantData, &m_vsConstI[StartRegister * 4], Vector4iCount * 16);
        return S_OK;
    }
    STDMETHOD(SetVertexShaderConstantB)(UINT StartRegister, CONST BOOL *pConstantData, UINT BoolCount) override;
    STDMETHOD(GetVertexShaderConstantB)(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override
    {
        if (StartRegister + BoolCount > 16)
            return D3DERR_INVALIDCALL;
        memcpy(pConstantData, &m_vsConstB[StartRegister], BoolCount * 4);
        return S_OK;
    }
    STDMETHOD(SetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) override;
    STDMETHOD(GetStreamSource)(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride) override
    {
        if (StreamNumber >= 16)
            return D3DERR_INVALIDCALL;
        *ppStreamData = m_streams[StreamNumber];
        if (*ppStreamData)
            (*ppStreamData)->AddRef();
        *pOffsetInBytes = m_streamOffset[StreamNumber];
        *pStride = m_streamStride[StreamNumber];
        return S_OK;
    }
    STDMETHOD(SetStreamSourceFreq)(UINT StreamNumber, UINT Setting) override;
    STDMETHOD(GetStreamSourceFreq)(UINT StreamNumber, UINT *pSetting) override
    {
        if (StreamNumber >= 16)
            return D3DERR_INVALIDCALL;
        *pSetting = m_streamFreq[StreamNumber];
        return S_OK;
    }
    STDMETHOD(SetIndices)(IDirect3DIndexBuffer9 *pIndexData) override;
    STDMETHOD(GetIndices)(IDirect3DIndexBuffer9 **ppIndexData) override
    {
        *ppIndexData = m_indices;
        if (m_indices)
            m_indices->AddRef();
        return S_OK;
    }
    STDMETHOD(CreatePixelShader)(CONST DWORD *pFunction, IDirect3DPixelShader9 **ppShader) override;
    STDMETHOD(SetPixelShader)(IDirect3DPixelShader9 *pShader) override;
    STDMETHOD(GetPixelShader)(IDirect3DPixelShader9 **ppShader) override
    {
        *ppShader = m_ps;
        if (m_ps)
            m_ps->AddRef();
        return S_OK;
    }
    STDMETHOD(SetPixelShaderConstantF)(UINT StartRegister, CONST float *pConstantData, UINT Vector4fCount) override;
    STDMETHOD(GetPixelShaderConstantF)(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override
    {
        if (StartRegister + Vector4fCount > 224)
            return D3DERR_INVALIDCALL;
        memcpy(pConstantData, &m_psConstF[StartRegister * 4], Vector4fCount * 16);
        return S_OK;
    }
    STDMETHOD(SetPixelShaderConstantI)(UINT StartRegister, CONST int *pConstantData, UINT Vector4iCount) override;
    STDMETHOD(GetPixelShaderConstantI)(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override
    {
        if (StartRegister + Vector4iCount > 16)
            return D3DERR_INVALIDCALL;
        memcpy(pConstantData, &m_psConstI[StartRegister * 4], Vector4iCount * 16);
        return S_OK;
    }
    STDMETHOD(SetPixelShaderConstantB)(UINT StartRegister, CONST BOOL *pConstantData, UINT BoolCount) override;
    STDMETHOD(GetPixelShaderConstantB)(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override
    {
        if (StartRegister + BoolCount > 16)
            return D3DERR_INVALIDCALL;
        memcpy(pConstantData, &m_psConstB[StartRegister], BoolCount * 4);
        return S_OK;
    }
    STDMETHOD(DrawRectPatch)(UINT, CONST float *, CONST D3DRECTPATCH_INFO *) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(DrawTriPatch)(UINT, CONST float *, CONST D3DTRIPATCH_INFO *) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(DeletePatch)(UINT) override { return D3DERR_INVALIDCALL; }
    STDMETHOD(CreateQuery)(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) override;

    // bindings the engine never cleared before releasing the object
    void UnbindTexture(IDirect3DBaseTexture9 *t)
    {
        for (int i = 0; i < BO1_MAX_SAMPLERS; ++i)
            if (m_textures[i] == t)
                m_textures[i] = nullptr;
    }
    void UnbindSurface(IDirect3DSurface9 *s)
    {
        for (int i = 0; i < 4; ++i)
            if (m_rt[i] == s)
                m_rt[i] = nullptr;
        if (m_ds == s)
            m_ds = nullptr;
    }
};

// ----- resource helpers that need the device -----

#define BO1_GETDEVICE(cls)                                                                                            \
    HRESULT STDMETHODCALLTYPE cls::GetDevice(IDirect3DDevice9 **ppDevice)                                            \
    {                                                                                                                 \
        s_device->AddRef();                                                                                           \
        *ppDevice = s_device;                                                                                         \
        return S_OK;                                                                                                  \
    }
BO1_GETDEVICE(Bo1Texture)
BO1_GETDEVICE(Bo1CubeTexture)
BO1_GETDEVICE(Bo1VolumeTexture)
BO1_GETDEVICE(Bo1Surface)
BO1_GETDEVICE(Bo1VertexBuffer)
BO1_GETDEVICE(Bo1IndexBuffer)
BO1_GETDEVICE(Bo1VertexDecl)
BO1_GETDEVICE(Bo1VertexShader)
BO1_GETDEVICE(Bo1PixelShader)
BO1_GETDEVICE(Bo1Query)
BO1_GETDEVICE(Bo1SwapChain)

void Bo1VertexBuffer::Destroy()
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    for (int i = 0; i < 16; ++i)
        if (s_device && s_device->m_streams[i] == this)
            s_device->m_streams[i] = nullptr;
    DestroyBuffer();
    Bo1_Retire([this] { delete this; });
}

void Bo1IndexBuffer::Destroy()
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (s_device && s_device->m_indices == this)
        s_device->m_indices = nullptr;
    DestroyBuffer();
    Bo1_Retire([this] { delete this; });
}

void Bo1VertexDecl::Destroy()
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (s_device && s_device->m_decl == this)
        s_device->m_decl = nullptr;
    Bo1_CmdDestroy(OBJ_VDECL, m_id);
    Bo1_Retire([this] { delete this; });
}

void Bo1VertexShader::Destroy()
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (s_device && s_device->m_vs == this)
        s_device->m_vs = nullptr;
    Bo1_CmdDestroy(OBJ_SHADER, m_id);
    Bo1_Retire([this] { delete this; });
}

void Bo1PixelShader::Destroy()
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (s_device && s_device->m_ps == this)
        s_device->m_ps = nullptr;
    Bo1_CmdDestroy(OBJ_SHADER, m_id);
    Bo1_Retire([this] { delete this; });
}

// ----- textures -----

Bo1TextureBase::Bo1TextureBase(TexType type, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, D3DFORMAT format, DWORD usage, D3DPOOL pool)
    : m_id(Bo1_NewId()), m_type(type), m_width(w), m_height(h), m_depth(d), m_format(format), m_usage(usage), m_pool(pool)
{
    uint32_t full = Bo1_FullMipCount(w, h, type == TEX_3D ? d : 1);
    m_levels = (levels == 0 || levels > full) ? full : levels;
    if (usage & D3DUSAGE_AUTOGENMIPMAP)
        m_levels = full;
    m_locks.resize(Faces() * m_levels);
    m_surfs.resize(Faces() * m_levels, nullptr);
    if (IsSysmem())
    {
        m_sysmem.resize(Faces() * m_levels, nullptr);
        return;
    }
    CmdCreateTexture *c = Cmd<CmdCreateTexture>(OP_CREATE_TEXTURE);
    c->id = m_id;
    c->type = type;
    c->width = w;
    c->height = h;
    c->depth = d;
    c->levels = (usage & D3DUSAGE_AUTOGENMIPMAP) ? full : m_levels;
    c->format = format;
    c->usage = usage;
    c->pool = pool;
    EndCmd();
}

Bo1TextureBase::~Bo1TextureBase()
{
    for (void *p : m_sysmem)
        free(p);
    for (Bo1Lock &lk : m_locks)
        free(lk.shadow);
}

void Bo1TextureBase::DestroyBase()
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (s_device)
        s_device->UnbindTexture(Bo1_AsInterface(this));
    if (!IsSysmem())
        Bo1_CmdDestroy(OBJ_TEXTURE, m_id);
}

void *Bo1TextureBase::SysmemLevel(uint32_t face, uint32_t level)
{
    void *&p = m_sysmem[face * m_levels + level];
    if (!p)
        p = calloc(1, LevelBytes(level) ? LevelBytes(level) : 1);
    return p;
}

HRESULT Bo1TextureBase::Lock(uint32_t face, uint32_t level, const RECT *rect, const D3DBOX *box, DWORD flags, void **bits, INT *pitch,
    INT *slicePitch)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (level >= m_levels || face >= Faces())
        return D3DERR_INVALIDCALL;
    Bo1Lock &lk = m_locks[face * m_levels + level];
    if (lk.active)
        return D3DERR_INVALIDCALL;
    uint32_t lw = LevelW(level), lh = LevelH(level), ld = LevelD(level);
    uint32_t x = 0, y = 0, z = 0, w = lw, h = lh, d = ld;
    if (rect)
    {
        x = rect->left;
        y = rect->top;
        w = rect->right - rect->left;
        h = rect->bottom - rect->top;
    }
    if (box)
    {
        x = box->Left;
        y = box->Top;
        z = box->Front;
        w = box->Right - box->Left;
        h = box->Bottom - box->Top;
        d = box->Back - box->Front;
    }
    uint32_t levelPitch = Bo1Fmt_Pitch(m_format, lw);
    uint32_t levelSlice = levelPitch * Bo1Fmt_Rows(m_format, lh);
    lk.active = true;
    lk.flags = flags;
    lk.x = x;
    lk.y = y;
    lk.z = z;
    lk.w = w;
    lk.h = h;
    lk.d = d;
    if (IsSysmem())
    {
        // the texels live here: point into the level
        uint8_t *base = (uint8_t *)SysmemLevel(face, level);
        uint32_t bx = Bo1Fmt_IsBlock(m_format) ? x / 4 * Bo1Fmt_BlockBytes(m_format) : x * Bo1Fmt_Bpp(m_format);
        uint32_t by = Bo1Fmt_IsBlock(m_format) ? y / 4 : y;
        lk.data = nullptr;
        *bits = base + z * levelSlice + by * levelPitch + bx;
        *pitch = (INT)levelPitch;
        if (slicePitch)
            *slicePitch = (INT)levelSlice;
        return S_OK;
    }
    if (!lk.shadow && lk.lockCount > 0 && !(flags & D3DLOCK_DISCARD))
    {
        // locked again: from now on keep the level's texels, starting from what the GPU has
        lk.shadow = calloc(1, (size_t)levelSlice * ld + 16);
        bool readable = m_format == D3DFMT_A8R8G8B8 || m_format == D3DFMT_X8R8G8B8 || m_format == D3DFMT_A8B8G8R8;
        for (uint32_t slice = 0; readable && slice < ld; ++slice)
        {
            volatile int32_t done = 0;
            CmdReadback *c = Cmd<CmdReadback>(OP_READBACK);
            c->src = SurfRef{SURF_TEXTURE, m_id, level, m_type == TEX_3D ? slice : face};
            c->width = lw;
            c->height = lh;
            c->format = m_format;
            c->pitch = levelPitch;
            c->dst = (uint8_t *)lk.shadow + (size_t)slice * levelSlice;
            c->done = &done;
            EndCmd();
            Bo1_WaitFlag(&done);
        }
    }
    ++lk.lockCount;
    if (lk.shadow)
    {
        uint32_t bx = Bo1Fmt_IsBlock(m_format) ? x / 4 * Bo1Fmt_BlockBytes(m_format) : x * Bo1Fmt_Bpp(m_format);
        uint32_t by = Bo1Fmt_IsBlock(m_format) ? y / 4 : y;
        lk.data = nullptr;
        *bits = (uint8_t *)lk.shadow + (size_t)z * levelSlice + by * levelPitch + bx;
        *pitch = (INT)levelPitch;
        if (slicePitch)
            *slicePitch = (INT)levelSlice;
        return S_OK;
    }
    // a staging block for the locked region, sent to the executor at unlock
    lk.pitch = Bo1Fmt_Pitch(m_format, w);
    lk.slicePitch = lk.pitch * Bo1Fmt_Rows(m_format, h);
    lk.data = calloc(1, (size_t)lk.slicePitch * d + 16);
    *bits = lk.data;
    *pitch = (INT)lk.pitch;
    if (slicePitch)
        *slicePitch = (INT)lk.slicePitch;
    return S_OK;
}

HRESULT Bo1TextureBase::Unlock(uint32_t face, uint32_t level)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (level >= m_levels || face >= Faces())
        return D3DERR_INVALIDCALL;
    Bo1Lock &lk = m_locks[face * m_levels + level];
    if (!lk.active)
        return D3DERR_INVALIDCALL;
    lk.active = false;
    if (IsSysmem())
        return S_OK;
    if (lk.flags & D3DLOCK_READONLY)
    {
        free(lk.data);
        lk.data = nullptr;
        return S_OK;
    }
    if (lk.shadow)
    {
        // the locked region out of the kept level
        uint32_t lw = LevelW(level), lh = LevelH(level);
        uint32_t levelPitch = Bo1Fmt_Pitch(m_format, lw), levelSlice = levelPitch * Bo1Fmt_Rows(m_format, lh);
        uint32_t bx = Bo1Fmt_IsBlock(m_format) ? lk.x / 4 * Bo1Fmt_BlockBytes(m_format) : lk.x * Bo1Fmt_Bpp(m_format);
        uint32_t by = Bo1Fmt_IsBlock(m_format) ? lk.y / 4 : lk.y;
        lk.pitch = Bo1Fmt_Pitch(m_format, lk.w);
        uint32_t rows = Bo1Fmt_Rows(m_format, lk.h);
        lk.slicePitch = lk.pitch * rows;
        uint8_t *staging = (uint8_t *)malloc((size_t)lk.slicePitch * lk.d + 16);
        for (uint32_t slice = 0; slice < lk.d; ++slice)
            for (uint32_t row = 0; row < rows; ++row)
                memcpy(staging + (size_t)slice * lk.slicePitch + (size_t)row * lk.pitch,
                    (uint8_t *)lk.shadow + (size_t)(lk.z + slice) * levelSlice + (size_t)(by + row) * levelPitch + bx, lk.pitch);
        lk.data = staging;
    }
    CmdUploadTexture *c = Cmd<CmdUploadTexture>(OP_UPLOAD_TEXTURE);
    c->id = m_id;
    c->face = face;
    c->level = level;
    c->x = lk.x;
    c->y = lk.y;
    c->z = lk.z;
    c->width = lk.w;
    c->height = lk.h;
    c->depth = lk.d;
    c->pitch = lk.pitch;
    c->slicePitch = lk.slicePitch;
    c->data = lk.data;
    EndCmd();
    lk.data = nullptr;
    return S_OK;
}

// a whole level of a system memory texture to a GPU texture (UpdateTexture)
void Bo1TextureBase::UploadLevel(uint32_t face, uint32_t level, const void *src)
{
    uint32_t bytes = LevelBytes(level);
    CmdUploadTexture *c = Cmd<CmdUploadTexture>(OP_UPLOAD_TEXTURE);
    c->id = m_id;
    c->face = face;
    c->level = level;
    c->x = c->y = c->z = 0;
    c->width = LevelW(level);
    c->height = LevelH(level);
    c->depth = LevelD(level);
    c->pitch = Bo1Fmt_Pitch(m_format, c->width);
    c->slicePitch = c->pitch * Bo1Fmt_Rows(m_format, c->height);
    c->data = malloc(bytes ? bytes : 1);
    memcpy(c->data, src, bytes);
    EndCmd();
}

Bo1Surface *Bo1TextureBase::Surface(uint32_t face, uint32_t level)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    Bo1Surface *&s = m_surfs[face * m_levels + level];
    if (s)
    {
        s->AddRef();
        return s;
    }
    s = new Bo1Surface(SURF_TEXTURE, LevelW(level), LevelH(level), m_format, m_pool, m_usage);
    s->m_texture = this;
    s->m_face = face;
    s->m_level = level;
    s->m_hasGpu = !IsSysmem();
    Bo1_AsInterface(this)->AddRef(); // a surface keeps its texture alive
    return s;
}

void STDMETHODCALLTYPE Bo1Texture::GenerateMipSubLevels() { Bo1_CmdId(OP_GENERATE_MIPS, m_id); }
void STDMETHODCALLTYPE Bo1CubeTexture::GenerateMipSubLevels() { Bo1_CmdId(OP_GENERATE_MIPS, m_id); }
void STDMETHODCALLTYPE Bo1VolumeTexture::GenerateMipSubLevels() { Bo1_CmdId(OP_GENERATE_MIPS, m_id); }

HRESULT STDMETHODCALLTYPE Bo1Texture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc)
{
    if (Level >= m_levels)
        return D3DERR_INVALIDCALL;
    pDesc->Format = m_format;
    pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = m_usage;
    pDesc->Pool = m_pool;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width = LevelW(Level);
    pDesc->Height = LevelH(Level);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1CubeTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc)
{
    if (Level >= m_levels)
        return D3DERR_INVALIDCALL;
    pDesc->Format = m_format;
    pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = m_usage;
    pDesc->Pool = m_pool;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width = LevelW(Level);
    pDesc->Height = LevelH(Level);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Texture::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel)
{
    if (Level >= m_levels)
        return D3DERR_INVALIDCALL;
    *ppSurfaceLevel = Surface(0, Level);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface)
{
    if (Level >= m_levels || (unsigned)FaceType >= 6)
        return D3DERR_INVALIDCALL;
    *ppCubeMapSurface = Surface((uint32_t)FaceType, Level);
    return S_OK;
}

// ----- surfaces -----

void Bo1Surface::Destroy()
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (s_device)
        s_device->UnbindSurface(this);
    if (m_kind == SURF_TEXTURE)
    {
        m_texture->m_surfs[m_face * m_texture->m_levels + m_level] = nullptr;
        Bo1_AsInterface(m_texture)->Release();
    }
    else if (m_kind == SURF_STANDALONE && m_hasGpu)
        Bo1_CmdDestroy(OBJ_SURFACE, m_id);
    Bo1_Retire([this] {
        free(m_sysmem);
        free(m_lock.data);
        delete this;
    });
}

HRESULT STDMETHODCALLTYPE Bo1Surface::LockRect(D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags)
{
    if (m_kind == SURF_TEXTURE)
        return m_texture->Lock(m_face, m_level, pRect, nullptr, Flags, &pLockedRect->pBits, &pLockedRect->Pitch, nullptr);
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (m_lock.active)
        return D3DERR_INVALIDCALL;
    uint32_t pitch = Bo1Fmt_Pitch(m_format, m_width);
    if (!m_sysmem)
    {
        m_sysmem = calloc(1, (size_t)pitch * Bo1Fmt_Rows(m_format, m_height) + 16);
        if (m_hasGpu)
        {
            // a lockable render target read back (the engine only reads them)
            volatile int32_t done = 0;
            CmdReadback *c = Cmd<CmdReadback>(OP_READBACK);
            c->src = Ref();
            c->width = m_width;
            c->height = m_height;
            c->format = m_format;
            c->pitch = pitch;
            c->dst = m_sysmem;
            c->done = &done;
            EndCmd();
            Bo1_WaitFlag(&done);
        }
    }
    uint32_t x = pRect ? pRect->left : 0, y = pRect ? pRect->top : 0;
    uint32_t bx = Bo1Fmt_IsBlock(m_format) ? x / 4 * Bo1Fmt_BlockBytes(m_format) : x * Bo1Fmt_Bpp(m_format);
    uint32_t by = Bo1Fmt_IsBlock(m_format) ? y / 4 : y;
    m_lock.active = true;
    m_lock.flags = Flags;
    pLockedRect->pBits = (uint8_t *)m_sysmem + by * pitch + bx;
    pLockedRect->Pitch = (INT)pitch;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Surface::UnlockRect()
{
    if (m_kind == SURF_TEXTURE)
        return m_texture->Unlock(m_face, m_level);
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (!m_lock.active)
        return D3DERR_INVALIDCALL;
    m_lock.active = false;
    if (m_hasGpu)
    {
        // the next read lock reads the GPU's contents again
        free(m_sysmem);
        m_sysmem = nullptr;
    }
    return S_OK;
}

// ----- swap chain -----

HRESULT STDMETHODCALLTYPE Bo1SwapChain::Present(CONST RECT *a, CONST RECT *b, HWND c, CONST RGNDATA *d, DWORD) { return s_device->Present(a, b, c, d); }
HRESULT STDMETHODCALLTYPE Bo1SwapChain::GetFrontBufferData(IDirect3DSurface9 *pDestSurface) { return s_device->GetFrontBufferData(0, pDestSurface); }
HRESULT STDMETHODCALLTYPE Bo1SwapChain::GetBackBuffer(UINT a, D3DBACKBUFFER_TYPE b, IDirect3DSurface9 **c) { return s_device->GetBackBuffer(0, a, b, c); }

HRESULT STDMETHODCALLTYPE Bo1SwapChain::GetDisplayMode(D3DDISPLAYMODE *pMode)
{
    pMode->Width = g_bo1Input.width;
    pMode->Height = g_bo1Input.height;
    pMode->RefreshRate = Bo1_DisplayHz();
    pMode->Format = D3DFMT_X8R8G8B8;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS *pp)
{
    *pp = s_device->m_pp;
    return S_OK;
}

// ============================================================================================================
// device methods
// ============================================================================================================

Bo1Device::Bo1Device(HWND window, DWORD behavior, const D3DPRESENT_PARAMETERS *pp) : m_pp(*pp), m_window(window), m_behavior(behavior)
{
    m_refs = 1;
    s_device = this;
    for (int i = 0; i < 256; ++i)
        m_gamma.red[i] = m_gamma.green[i] = m_gamma.blue[i] = (WORD)(i * 257);
    if (!m_pp.BackBufferWidth)
        m_pp.BackBufferWidth = g_bo1Input.width;
    if (!m_pp.BackBufferHeight)
        m_pp.BackBufferHeight = g_bo1Input.height;
    CreateBackBuffer();
    ResetState();
}

void Bo1Device::CreateBackBuffer()
{
    m_backBuffer = new Bo1Surface(SURF_BACKBUFFER, m_pp.BackBufferWidth, m_pp.BackBufferHeight, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        D3DUSAGE_RENDERTARGET);
    m_backBuffer->m_hasGpu = true;
    m_backBuffer->m_refs = 1000000; // the device owns it
    m_backBuffer->m_ms = m_pp.MultiSampleType;
    CmdReset *c = Cmd<CmdReset>(OP_RESET);
    c->width = m_pp.BackBufferWidth;
    c->height = m_pp.BackBufferHeight;
    c->multisample = m_pp.MultiSampleType;
    EndCmd();
}

void Bo1Device::ResetState()
{
    // D3D9 defaults (D3DRENDERSTATETYPE / D3DSAMPLERSTATETYPE documentation), sent as the executor's states start at 0
    memset(m_rs, 0, sizeof(m_rs));
    m_rs[D3DRS_ZENABLE] = D3DZB_TRUE;
    m_rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    m_rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    m_rs[D3DRS_ZWRITEENABLE] = TRUE;
    m_rs[D3DRS_LASTPIXEL] = TRUE;
    m_rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    m_rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    m_rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    m_rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    m_rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_DITHERENABLE] = FALSE;
    m_rs[D3DRS_FOGCOLOR] = 0;
    m_rs[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_STENCILMASK] = 0xFFFFFFFF;
    m_rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    m_rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
    m_rs[D3DRS_CLIPPING] = TRUE;
    m_rs[D3DRS_LIGHTING] = TRUE;
    m_rs[D3DRS_COLORVERTEX] = TRUE;
    m_rs[D3DRS_LOCALVIEWER] = TRUE;
    m_rs[D3DRS_POINTSIZE] = 0x3F800000;
    m_rs[D3DRS_POINTSIZE_MIN] = 0x3F800000;
    m_rs[D3DRS_POINTSCALE_A] = 0x3F800000;
    m_rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
    m_rs[D3DRS_MULTISAMPLEMASK] = 0xFFFFFFFF;
    m_rs[D3DRS_POINTSIZE_MAX] = 0x47000000;
    m_rs[D3DRS_COLORWRITEENABLE] = 0xF;
    m_rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    m_rs[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_COLORWRITEENABLE1] = 0xF;
    m_rs[D3DRS_COLORWRITEENABLE2] = 0xF;
    m_rs[D3DRS_COLORWRITEENABLE3] = 0xF;
    m_rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
    m_rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    m_rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    m_rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    for (int i = 0; i < 256; ++i)
    {
        CmdState *c = Cmd<CmdState>(OP_RENDER_STATE);
        c->state = i;
        c->value = m_rs[i];
        EndCmd();
    }
    for (int s = 0; s < BO1_MAX_SAMPLERS; ++s)
    {
        DWORD *ss = m_ss[s];
        memset(ss, 0, sizeof(m_ss[s]));
        ss[D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        ss[D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
        ss[D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        ss[D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        ss[D3DSAMP_MAXANISOTROPY] = 1;
        if (s < 16 || s >= D3DVERTEXTEXTURESAMPLER0)
        {
            for (int t = D3DSAMP_ADDRESSU; t <= D3DSAMP_DMAPOFFSET; ++t)
            {
                CmdSamplerState *c = Cmd<CmdSamplerState>(OP_SAMPLER_STATE);
                c->sampler = s;
                c->type = t;
                c->value = ss[t];
                EndCmd();
            }
        }
    }
    for (int i = 0; i < 16; ++i)
        m_streamFreq[i] = 1;
    m_viewport.X = 0;
    m_viewport.Y = 0;
    m_viewport.Width = m_pp.BackBufferWidth;
    m_viewport.Height = m_pp.BackBufferHeight;
    m_viewport.MinZ = 0.0f;
    m_viewport.MaxZ = 1.0f;
    m_scissor.left = 0;
    m_scissor.top = 0;
    m_scissor.right = m_pp.BackBufferWidth;
    m_scissor.bottom = m_pp.BackBufferHeight;
    SetRenderTarget(0, m_backBuffer);
    SetViewport(&m_viewport);
    SetScissorRect(&m_scissor);
}

static bool Bo1_FormatSupported(D3DFORMAT f, DWORD usage)
{
    switch ((uint32_t)f)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_R5G6B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_A8L8:
    case D3DFMT_L16:
    case D3DFMT_R16F:
    case D3DFMT_G16R16F:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_R32F:
    case D3DFMT_G32R32F:
    case D3DFMT_A32B32G32R32F:
    case D3DFMT_G16R16:
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_V8U8:
    case D3DFMT_Q8W8V8U8:
        return true;
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        return !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
    case D3DFMT_D16:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24FS8:
    case D3DFMT_D32:
    case D3DFMT_D15S1:
    case D3DFMT_D16_LOCKABLE:
        // depth surfaces, and depth textures: the engine's hardware shadow maps are D24S8 (R_SetShadowmapFormats_DX)
        return true;
    default:
        return (uint32_t)f == BO1FMT_ATI1 || (uint32_t)f == BO1FMT_ATI2;
    }
}

HRESULT STDMETHODCALLTYPE Bo1Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DTexture9 **ppTexture, HANDLE *)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (!Width || !Height || !Bo1_FormatSupported(Format, Usage))
    {
        Bo1_Log("CreateTexture %ux%u format %u usage 0x%x: not supported", Width, Height, (unsigned)Format, (unsigned)Usage);
        *ppTexture = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *ppTexture = new Bo1Texture(Width, Height, Levels, Usage, Format, Pool);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format,
    D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (!Width || !Height || !Depth || !Bo1_FormatSupported(Format, Usage))
    {
        *ppVolumeTexture = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *ppVolumeTexture = new Bo1VolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (!EdgeLength || !Bo1_FormatSupported(Format, Usage))
    {
        *ppCubeTexture = nullptr;
        return D3DERR_INVALIDCALL;
    }
    *ppCubeTexture = new Bo1CubeTexture(EdgeLength, Levels, Usage, Format, Pool);
    return S_OK;
}

static Bo1Surface *Bo1_NewGpuSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE ms, bool depth)
{
    Bo1Surface *s = new Bo1Surface(SURF_STANDALONE, Width, Height, Format, D3DPOOL_DEFAULT, depth ? D3DUSAGE_DEPTHSTENCIL : D3DUSAGE_RENDERTARGET);
    s->m_id = Bo1_NewId();
    s->m_ms = ms;
    s->m_hasGpu = true;
    CmdCreateSurface *c = Cmd<CmdCreateSurface>(OP_CREATE_SURFACE);
    c->id = s->m_id;
    c->width = Width;
    c->height = Height;
    c->format = Format;
    c->multisample = ms;
    c->depth = depth;
    EndCmd();
    return s;
}

HRESULT STDMETHODCALLTYPE Bo1Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD, BOOL,
    IDirect3DSurface9 **ppSurface, HANDLE *)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    *ppSurface = Bo1_NewGpuSurface(Width, Height, Format, MultiSample, false);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD,
    BOOL, IDirect3DSurface9 **ppSurface, HANDLE *)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    *ppSurface = Bo1_NewGpuSurface(Width, Height, Format, MultiSample, true);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface,
    HANDLE *)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    Bo1Surface *s = new Bo1Surface(SURF_STANDALONE, Width, Height, Format, Pool, 0);
    s->m_id = Bo1_NewId();
    s->m_hasGpu = false; // system memory: lives here
    *ppSurface = s;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::UpdateSurface(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface,
    CONST POINT *pDestPoint)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    Bo1Surface *src = static_cast<Bo1Surface *>(pSourceSurface);
    Bo1Surface *dst = static_cast<Bo1Surface *>(pDestinationSurface);
    if (dst->m_kind != SURF_TEXTURE)
        return D3DERR_INVALIDCALL;
    D3DLOCKED_RECT s;
    if (FAILED(src->LockRect(&s, pSourceRect, D3DLOCK_READONLY)))
        return D3DERR_INVALIDCALL;
    RECT r = pSourceRect ? *pSourceRect : RECT{0, 0, (LONG)src->m_width, (LONG)src->m_height};
    RECT d = {pDestPoint ? pDestPoint->x : 0, pDestPoint ? pDestPoint->y : 0, 0, 0};
    d.right = d.left + (r.right - r.left);
    d.bottom = d.top + (r.bottom - r.top);
    D3DLOCKED_RECT t;
    if (SUCCEEDED(dst->LockRect(&t, &d, 0)))
    {
        uint32_t rows = Bo1Fmt_Rows(dst->m_format, r.bottom - r.top);
        uint32_t bytes = Bo1Fmt_Pitch(dst->m_format, r.right - r.left);
        for (uint32_t y = 0; y < rows; ++y)
            memcpy((uint8_t *)t.pBits + y * t.Pitch, (uint8_t *)s.pBits + y * s.Pitch, bytes);
        dst->UnlockRect();
    }
    src->UnlockRect();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    Bo1TextureBase *src = Bo1_AsTexture(pSourceTexture);
    Bo1TextureBase *dst = Bo1_AsTexture(pDestinationTexture);
    if (!src || !dst || !src->IsSysmem() || dst->IsSysmem())
        return D3DERR_INVALIDCALL;
    // the destination's levels match the source's smallest ones when it has fewer
    uint32_t skip = src->m_levels > dst->m_levels ? src->m_levels - dst->m_levels : 0;
    for (uint32_t f = 0; f < dst->Faces(); ++f)
    {
        for (uint32_t l = 0; l < dst->m_levels && l + skip < src->m_levels; ++l)
            dst->UploadLevel(f, l, src->SysmemLevel(f, l + skip));
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    Bo1Surface *src = static_cast<Bo1Surface *>(pRenderTarget);
    Bo1Surface *dst = static_cast<Bo1Surface *>(pDestSurface);
    uint32_t pitch = Bo1Fmt_Pitch(dst->m_format, dst->m_width);
    if (!dst->m_sysmem)
        dst->m_sysmem = calloc(1, (size_t)pitch * dst->m_height + 16);
    volatile int32_t done = 0;
    CmdReadback *c = Cmd<CmdReadback>(OP_READBACK);
    c->src = src->Ref();
    c->width = dst->m_width;
    c->height = dst->m_height;
    c->format = dst->m_format;
    c->pitch = pitch;
    c->dst = dst->m_sysmem;
    c->done = &done;
    EndCmd();
    Bo1_WaitFlag(&done);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::StretchRect(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect, IDirect3DSurface9 *pDestSurface,
    CONST RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (!pSourceSurface || !pDestSurface)
        return D3DERR_INVALIDCALL;
    CmdStretchRect *c = Cmd<CmdStretchRect>(OP_STRETCH_RECT);
    c->src = static_cast<Bo1Surface *>(pSourceSurface)->Ref();
    c->dst = static_cast<Bo1Surface *>(pDestSurface)->Ref();
    c->hasSrcRect = pSourceRect != nullptr;
    c->hasDstRect = pDestRect != nullptr;
    if (pSourceRect)
        c->srcRect = {pSourceRect->left, pSourceRect->top, pSourceRect->right, pSourceRect->bottom};
    if (pDestRect)
        c->dstRect = {pDestRect->left, pDestRect->top, pDestRect->right, pDestRect->bottom};
    c->filter = Filter;
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::ColorFill(IDirect3DSurface9 *pSurface, CONST RECT *pRect, D3DCOLOR color)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    CmdColorFill *c = Cmd<CmdColorFill>(OP_COLOR_FILL);
    c->surf = static_cast<Bo1Surface *>(pSurface)->Ref();
    c->hasRect = pRect != nullptr;
    if (pRect)
        c->rect = {pRect->left, pRect->top, pRect->right, pRect->bottom};
    c->color = color;
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (RenderTargetIndex >= 4 || (RenderTargetIndex == 0 && !pRenderTarget))
        return D3DERR_INVALIDCALL;
    m_rt[RenderTargetIndex] = pRenderTarget;
    CmdRenderTarget *c = Cmd<CmdRenderTarget>(OP_RENDER_TARGET);
    c->index = RenderTargetIndex;
    if (pRenderTarget)
        c->surf = static_cast<Bo1Surface *>(pRenderTarget)->Ref();
    else
        c->surf = SurfRef{SURF_NONE, 0, 0, 0};
    EndCmd();
    if (RenderTargetIndex == 0)
    {
        // D3D9 resets the viewport and scissor to the new target
        Bo1Surface *s = static_cast<Bo1Surface *>(pRenderTarget);
        D3DVIEWPORT9 vp = {0, 0, s->m_width, s->m_height, 0.0f, 1.0f};
        SetViewport(&vp);
        RECT sc = {0, 0, (LONG)s->m_width, (LONG)s->m_height};
        SetScissorRect(&sc);
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    m_ds = pNewZStencil;
    CmdRenderTarget *c = Cmd<CmdRenderTarget>(OP_DEPTH_STENCIL);
    c->index = 0;
    if (pNewZStencil)
        c->surf = static_cast<Bo1Surface *>(pNewZStencil)->Ref();
    else
        c->surf = SurfRef{SURF_NONE, 0, 0, 0};
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::Clear(DWORD Count, CONST D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    CmdClear *c = Cmd<CmdClear>(OP_CLEAR);
    c->flags = Flags;
    c->color = Color;
    c->z = Z;
    c->stencil = Stencil;
    c->rectCount = pRects ? Count : 0;
    if (pRects && Count)
        c->rect = {pRects[0].x1, pRects[0].y1, pRects[0].x2, pRects[0].y2};
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetViewport(CONST D3DVIEWPORT9 *pViewport)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    m_viewport = *pViewport;
    CmdViewport *c = Cmd<CmdViewport>(OP_VIEWPORT);
    c->x = pViewport->X;
    c->y = pViewport->Y;
    c->width = pViewport->Width;
    c->height = pViewport->Height;
    c->minZ = pViewport->MinZ;
    c->maxZ = pViewport->MaxZ;
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetScissorRect(CONST RECT *pRect)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    m_scissor = *pRect;
    CmdRect *c = Cmd<CmdRect>(OP_SCISSOR);
    *c = {pRect->left, pRect->top, pRect->right, pRect->bottom};
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetClipPlane(DWORD Index, CONST float *pPlane)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (Index >= 6)
        return D3DERR_INVALIDCALL;
    memcpy(m_clipPlanes[Index], pPlane, 16);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
    if ((unsigned)State >= 256)
        return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (m_rs[State] == Value)
        return S_OK;
    m_rs[State] = Value;
    CmdState *c = Cmd<CmdState>(OP_RENDER_STATE);
    c->state = State;
    c->value = Value;
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (Stage >= (DWORD)BO1_MAX_SAMPLERS)
        return D3DERR_INVALIDCALL;
    if (m_textures[Stage] == pTexture)
        return S_OK;
    m_textures[Stage] = pTexture;
    Bo1TextureBase *t = Bo1_AsTexture(pTexture);
    CmdSetTexture *c = Cmd<CmdSetTexture>(OP_TEXTURE);
    c->stage = Stage;
    c->id = t ? t->m_id : 0;
    c->type = t ? t->m_type : TEX_2D;
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (Stage >= 8 || (unsigned)Type > 32)
        return D3DERR_INVALIDCALL;
    m_tss[Stage][Type] = Value;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
{
    if (Sampler >= (DWORD)BO1_MAX_SAMPLERS || (unsigned)Type > D3DSAMP_DMAPOFFSET)
        return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (m_ss[Sampler][Type] == Value)
        return S_OK;
    m_ss[Sampler][Type] = Value;
    CmdSamplerState *c = Cmd<CmdSamplerState>(OP_SAMPLER_STATE);
    c->sampler = Sampler;
    c->type = Type;
    c->value = Value;
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    CmdDraw *c = Cmd<CmdDraw>(OP_DRAW);
    c->primType = PrimitiveType;
    c->startVertex = StartVertex;
    c->primCount = PrimitiveCount;
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices,
    UINT startIndex, UINT primCount)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    CmdDrawIndexed *c = Cmd<CmdDrawIndexed>(OP_DRAW_INDEXED);
    c->primType = type;
    c->baseVertex = BaseVertexIndex;
    c->minIndex = MinVertexIndex;
    c->numVertices = NumVertices;
    c->startIndex = startIndex;
    c->primCount = primCount;
    EndCmd();
    return S_OK;
}

static uint32_t Bo1_VertexCount(D3DPRIMITIVETYPE type, UINT primCount)
{
    switch (type)
    {
    case D3DPT_POINTLIST:
        return primCount;
    case D3DPT_LINELIST:
        return primCount * 2;
    case D3DPT_LINESTRIP:
        return primCount + 1;
    case D3DPT_TRIANGLELIST:
        return primCount * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:
        return primCount + 2;
    default:
        return 0;
    }
}

HRESULT STDMETHODCALLTYPE Bo1Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void *pVertexStreamZeroData,
    UINT VertexStreamZeroStride)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    uint32_t bytes = Bo1_VertexCount(PrimitiveType, PrimitiveCount) * VertexStreamZeroStride;
    CmdDrawUp *c = Cmd<CmdDrawUp>(OP_DRAW_UP);
    c->primType = PrimitiveType;
    c->primCount = PrimitiveCount;
    c->stride = VertexStreamZeroStride;
    c->vertexBytes = bytes;
    c->vertices = malloc(bytes ? bytes : 1);
    memcpy(c->vertices, pVertexStreamZeroData, bytes);
    EndCmd();
    // D3D9: stream 0 is unset after an UP draw
    m_streams[0] = nullptr;
    m_streamOffset[0] = 0;
    m_streamStride[0] = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount,
    CONST void *pIndexData, D3DFORMAT IndexDataFormat, CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    uint32_t indexSize = IndexDataFormat == D3DFMT_INDEX32 ? 4 : 2;
    uint32_t ibytes = Bo1_VertexCount(PrimitiveType, PrimitiveCount) * indexSize;
    uint32_t vbytes = (MinVertexIndex + NumVertices) * VertexStreamZeroStride;
    CmdDrawIndexedUp *c = Cmd<CmdDrawIndexedUp>(OP_DRAW_INDEXED_UP);
    c->primType = PrimitiveType;
    c->minIndex = MinVertexIndex;
    c->numVertices = NumVertices;
    c->primCount = PrimitiveCount;
    c->index32 = indexSize == 4;
    c->stride = VertexStreamZeroStride;
    c->vertexBytes = vbytes;
    c->indexBytes = ibytes;
    c->vertices = malloc(vbytes ? vbytes : 1);
    memcpy(c->vertices, pVertexStreamZeroData, vbytes);
    c->indices = malloc(ibytes ? ibytes : 1);
    memcpy(c->indices, pIndexData, ibytes);
    EndCmd();
    m_streams[0] = nullptr;
    m_streamOffset[0] = 0;
    m_streamStride[0] = 0;
    m_indices = nullptr;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    Bo1VertexDecl *d = new Bo1VertexDecl;
    d->m_id = Bo1_NewId();
    uint32_t count = 0;
    while (pVertexElements[count].Stream != 0xFF)
        ++count;
    d->m_elements.assign(pVertexElements, pVertexElements + count + 1);
    CmdCreateVDecl *c = Cmd<CmdCreateVDecl>(OP_CREATE_VDECL);
    c->id = d->m_id;
    c->count = count;
    c->elements = malloc(sizeof(D3DVERTEXELEMENT9) * (count + 1));
    memcpy(c->elements, pVertexElements, sizeof(D3DVERTEXELEMENT9) * (count + 1));
    EndCmd();
    *ppDecl = d;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (m_decl == pDecl)
        return S_OK;
    m_decl = pDecl;
    Bo1_CmdId(OP_VDECL, pDecl ? static_cast<Bo1VertexDecl *>(pDecl)->m_id : 0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetFVF(DWORD FVF)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    m_fvf = FVF;
    m_decl = nullptr;
    return S_OK;
}

static uint32_t Bo1_ShaderLength(const DWORD *code)
{
    // tokens up to and including D3DSIO_END (0x0000FFFF); comments carry their length
    uint32_t n = 1;
    while (code[n] != 0x0000FFFF)
    {
        DWORD t = code[n];
        if ((t & 0xFFFF) == 0xFFFE) // comment
            n += 1 + (t >> 16);
        else
            ++n;
        if (n > (1u << 20))
            break;
    }
    return n + 1;
}

HRESULT STDMETHODCALLTYPE Bo1Device::CreateVertexShader(CONST DWORD *pFunction, IDirect3DVertexShader9 **ppShader)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (!pFunction)
        return D3DERR_INVALIDCALL;
    Bo1VertexShader *s = new Bo1VertexShader;
    s->m_id = Bo1_NewId();
    uint32_t len = Bo1_ShaderLength(pFunction);
    s->m_code.assign(pFunction, pFunction + len);
    CmdCreateShader *c = Cmd<CmdCreateShader>(OP_CREATE_SHADER);
    c->id = s->m_id;
    c->pixel = 0;
    c->size = len * 4;
    c->code = malloc(len * 4);
    memcpy(c->code, pFunction, len * 4);
    EndCmd();
    *ppShader = s;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::CreatePixelShader(CONST DWORD *pFunction, IDirect3DPixelShader9 **ppShader)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (!pFunction)
        return D3DERR_INVALIDCALL;
    Bo1PixelShader *s = new Bo1PixelShader;
    s->m_id = Bo1_NewId();
    uint32_t len = Bo1_ShaderLength(pFunction);
    s->m_code.assign(pFunction, pFunction + len);
    CmdCreateShader *c = Cmd<CmdCreateShader>(OP_CREATE_SHADER);
    c->id = s->m_id;
    c->pixel = 1;
    c->size = len * 4;
    c->code = malloc(len * 4);
    memcpy(c->code, pFunction, len * 4);
    EndCmd();
    *ppShader = s;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetVertexShader(IDirect3DVertexShader9 *pShader)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (m_vs == pShader)
        return S_OK;
    m_vs = pShader;
    Bo1_CmdId(OP_VS, pShader ? static_cast<Bo1VertexShader *>(pShader)->m_id : 0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetPixelShader(IDirect3DPixelShader9 *pShader)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (m_ps == pShader)
        return S_OK;
    m_ps = pShader;
    Bo1_CmdId(OP_PS, pShader ? static_cast<Bo1PixelShader *>(pShader)->m_id : 0);
    return S_OK;
}

static void Bo1_CmdConst(Op op, UINT start, const void *data, UINT count, UINT bytesPerItem)
{
    CmdConst *c = (CmdConst *)BeginCmd(op, sizeof(CmdConst) + count * bytesPerItem);
    c->start = start;
    c->count = count;
    memcpy(c + 1, data, count * bytesPerItem);
    EndCmd();
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetVertexShaderConstantF(UINT StartRegister, CONST float *pConstantData, UINT Vector4fCount)
{
    if (StartRegister + Vector4fCount > 256)
        return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    memcpy(&m_vsConstF[StartRegister * 4], pConstantData, Vector4fCount * 16);
    Bo1_CmdConst(OP_VS_CONST_F, StartRegister, pConstantData, Vector4fCount, 16);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetPixelShaderConstantF(UINT StartRegister, CONST float *pConstantData, UINT Vector4fCount)
{
    if (StartRegister + Vector4fCount > 224)
        return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    memcpy(&m_psConstF[StartRegister * 4], pConstantData, Vector4fCount * 16);
    Bo1_CmdConst(OP_PS_CONST_F, StartRegister, pConstantData, Vector4fCount, 16);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetVertexShaderConstantI(UINT StartRegister, CONST int *pConstantData, UINT Vector4iCount)
{
    if (StartRegister + Vector4iCount > 16)
        return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    memcpy(&m_vsConstI[StartRegister * 4], pConstantData, Vector4iCount * 16);
    Bo1_CmdConst(OP_VS_CONST_I, StartRegister, pConstantData, Vector4iCount, 16);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetPixelShaderConstantI(UINT StartRegister, CONST int *pConstantData, UINT Vector4iCount)
{
    if (StartRegister + Vector4iCount > 16)
        return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    memcpy(&m_psConstI[StartRegister * 4], pConstantData, Vector4iCount * 16);
    Bo1_CmdConst(OP_PS_CONST_I, StartRegister, pConstantData, Vector4iCount, 16);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetVertexShaderConstantB(UINT StartRegister, CONST BOOL *pConstantData, UINT BoolCount)
{
    if (StartRegister + BoolCount > 16)
        return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    memcpy(&m_vsConstB[StartRegister], pConstantData, BoolCount * 4);
    Bo1_CmdConst(OP_VS_CONST_B, StartRegister, pConstantData, BoolCount, 4);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetPixelShaderConstantB(UINT StartRegister, CONST BOOL *pConstantData, UINT BoolCount)
{
    if (StartRegister + BoolCount > 16)
        return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    memcpy(&m_psConstB[StartRegister], pConstantData, BoolCount * 4);
    Bo1_CmdConst(OP_PS_CONST_B, StartRegister, pConstantData, BoolCount, 4);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride)
{
    if (StreamNumber >= 16)
        return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (m_streams[StreamNumber] == pStreamData && m_streamOffset[StreamNumber] == OffsetInBytes && m_streamStride[StreamNumber] == Stride)
        return S_OK;
    m_streams[StreamNumber] = pStreamData;
    m_streamOffset[StreamNumber] = OffsetInBytes;
    m_streamStride[StreamNumber] = Stride;
    CmdStream *c = Cmd<CmdStream>(OP_STREAM);
    c->stream = StreamNumber;
    c->id = pStreamData ? static_cast<Bo1VertexBuffer *>(pStreamData)->m_id : 0;
    c->offset = OffsetInBytes;
    c->stride = Stride;
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetStreamSourceFreq(UINT StreamNumber, UINT Setting)
{
    if (StreamNumber >= 16)
        return D3DERR_INVALIDCALL;
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    m_streamFreq[StreamNumber] = Setting;
    CmdState *c = Cmd<CmdState>(OP_STREAM_FREQ);
    c->state = StreamNumber;
    c->value = Setting;
    EndCmd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::SetIndices(IDirect3DIndexBuffer9 *pIndexData)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    if (m_indices == pIndexData)
        return S_OK;
    m_indices = pIndexData;
    Bo1_CmdId(OP_INDICES, pIndexData ? static_cast<Bo1IndexBuffer *>(pIndexData)->m_id : 0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery)
{
    switch (Type)
    {
    case D3DQUERYTYPE_EVENT:
    case D3DQUERYTYPE_OCCLUSION:
    case D3DQUERYTYPE_TIMESTAMP:
    case D3DQUERYTYPE_TIMESTAMPFREQ:
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
        break;
    default:
        return D3DERR_NOTAVAILABLE;
    }
    if (!ppQuery)
        return S_OK; // the "is this query type supported" probe
    *ppQuery = new Bo1Query(Type);
    return S_OK;
}

bool Bo1_SerialPacing() { return g_ring.executorAlive && g_ring.pacing == 0; }

// The loading screen: a remote screen update draws it on the render thread (rb_backend.cpp: RB_RenderThread) with the
// database's lock held, which the database thread loading the map waits for. Its frame doesn't wait for the page to
// take it (Present), and the next is drawn only once the page has: frames waiting with the lock held for a page busy
// with the loading movie kept the lock for 70% of a map's load, and two browsers loading Call of the Dead together took
// 90 s rather than 27. True when the page has taken the last frame, and the next may be drawn; Bo1_EndLoadingScreenFrame
// after it.
static bool s_loadingScreenFrame;
bool Bo1_BeginLoadingScreenFrame()
{
    if (__atomic_load_n(&g_ring.framesTaken, __ATOMIC_ACQUIRE) != g_ring.framesSubmitted)
        return false;
    s_loadingScreenFrame = true;
    return true;
}
void Bo1_EndLoadingScreenFrame() { s_loadingScreenFrame = false; }

// ----- mouse lag: the mouse movement each frame read -----
// The game thread builds a frame into one of the renderer's two GfxBackEndData while the render thread draws the frame
// before from the other (r_rendercmds.cpp: frontEndDataOut, rb_backend.cpp: backEndData). When the mouse had last moved
// as a frame read it is kept with its frame data until the render thread starts drawing it.
struct GfxBackEndData;
extern GfxBackEndData *frontEndDataOut;
extern GfxBackEndData *backEndData;
bool Sys_IsMainThread();

static std::mutex s_frameInputLock;
static struct
{
    GfxBackEndData *frame;
    double time;
} s_frameInput[2];
static double s_inputRead; // the last movement the game thread read

void Bo1_FrameReadInput()
{
    if (!Sys_IsMainThread())
        return;
    double time = g_bo1Input.inputTime;
    GfxBackEndData *frame = frontEndDataOut;
    if (time == s_inputRead || !frame)
        return;
    s_inputRead = time;
    std::lock_guard<std::mutex> guard(s_frameInputLock);
    int i = s_frameInput[0].frame == frame || !s_frameInput[0].frame ? 0 : 1;
    s_frameInput[i].frame = frame;
    s_frameInput[i].time = time;
}

double Bo1_FrameTakeInput()
{
    GfxBackEndData *frame = backEndData;
    std::lock_guard<std::mutex> guard(s_frameInputLock);
    for (auto &f : s_frameInput)
        if (frame && f.frame == frame)
        {
            double time = f.time;
            f.time = 0.0;
            return time;
        }
    return 0.0;
}

// How the page paces the engine. Present waits for the page to take the frame to replay it, on an animation frame; then
// the render thread completes the frame, and the game thread hands over its next and starts on the one after, reading
// the mouse for it (rb_backend.cpp: RB_RenderCommandFrame). What the player sees is a refresh older for each frame in
// between:
//   serial (0): the game thread starts a frame only once the page has taken the last (web_main.cpp: main), so it builds
//     the frame and the render thread draws it within the refresh, and the page takes it at its next animation frame.
//     Both threads, and a millisecond for handing the frame over, have to fit in a refresh. In the browser the render
//     thread waits for the frame on its event, and the game thread for its worker commands by yielding, not with 1 ms
//     sleeps, which last up to 16 here (rb_backend.cpp: RB_RenderThread, r_rendercmds.cpp: R_ToggleSmpFrameCmd).
//   pipelined (1): the game thread builds a frame while the render thread draws the one before: a refresh more. Each
//     has a refresh of its own.
//   queued (2): Present lets one frame wait for the page, so the render thread has more than a refresh when the page or
//     the GPU holds it up: a refresh more again.
// The pacing goes lower once the slowest frame of the last five seconds would have fitted in 85% of the refresh, and up
// again when one of the last second or two would not fit at all, or when a frame missed its animation frame after a
// second on time (the page counts those: g_ring.misses). Each change shows once, as a frame a refresh newer or older than
// the one before: the margin keeps the pacing from going lower on a run of quick frames only to go up at the next slow
// one. It stays up for two seconds, longer when it missed soon after going lower, so that it does not go down and up
// over and over.
void Bo1Device::Pace(double now, float renderMs)
{
    auto &p = m_pacing;
    float refreshUs = (float)g_ring.refreshUs, renderUs = renderMs * 1000.0f + 1000.0f, chainUs = renderUs + g_bo1GameFrameUs;
    if (now - p.second >= 1000.0)
    {
        p.second = now;
        for (int i = 4; i > 0; --i)
        {
            p.chainUs[i] = p.chainUs[i - 1];
            p.renderUs[i] = p.renderUs[i - 1];
        }
        p.chainUs[0] = p.renderUs[0] = 0.0f;
    }
    if (chainUs > p.chainUs[0])
        p.chainUs[0] = chainUs;
    if (renderUs > p.renderUs[0])
        p.renderUs[0] = renderUs;
    // the slowest frame of this second and the seconds before, against the refresh: would it fit at this pacing, in this
    // share of the refresh?
    auto fits = [&](uint32_t level, float share, int seconds) {
        const float *us = level == 0 ? p.chainUs : p.renderUs;
        float worst = 0.0f;
        for (int i = 0; i < seconds; ++i)
            worst = us[i] > worst ? us[i] : worst;
        return level == 2 || (refreshUs && worst <= share * refreshUs);
    };
    uint32_t misses = __atomic_load_n(&g_ring.misses, __ATOMIC_ACQUIRE);
    bool missed = misses != p.misses;
    p.misses = misses;
    if (p.level < 2 && (missed || !fits(p.level, 1.0f, 2)))
    {
        ++p.level;
        p.hold = now - p.lowered >= 5000.0 ? 2000.0 : p.hold < 16000.0 ? p.hold * 2.0 : 32000.0;
        p.holdUntil = now + p.hold;
    }
    else if (p.level > 0 && now >= p.holdUntil && fits(p.level - 1, 0.85f, 5))
    {
        --p.level;
        p.lowered = now;
    }
    g_ring.pacing = p.level;
}

int Com_PlayerMaxFps(); // src/qcommon/common.cpp: bo1_maxfps, the player's frame rate limit

HRESULT STDMETHODCALLTYPE Bo1Device::Present(CONST RECT *, CONST RECT *, HWND, CONST RGNDATA *)
{
    double now = emscripten_get_now();
    float renderMs = m_frameBegan > 0.0 ? (float)(now - m_frameBegan) : 0.0f;
    __atomic_store_n(&g_ring.maxFps, (uint32_t)Com_PlayerMaxFps(), __ATOMIC_RELAXED);
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        CmdPresent *c = Cmd<CmdPresent>(OP_PRESENT);
        c->frame = ++m_frame;
        c->renderMs = renderMs;
        c->inputTime = m_frameInput;
        // Counted before the page can replay it (EndCmd puts it in the ring), never after. A frame the page replayed
        // to its PRESENT before it was counted looked, at the page's next animation frame, like one more written: the
        // page took it (d3d9_webgpu.cpp: Replay), framesTaken went one past framesSubmitted, and this thread waited
        // below for good, the page with nothing more to take. A busy machine held this thread up for that long now and
        // then (Bo1_ReapRetired is in between too), and the player never spawned.
        __atomic_add_fetch(&g_ring.framesSubmitted, 1, __ATOMIC_RELEASE);
        EndCmd();
        Bo1_ReapRetired();
    }
    m_frameBegan = 0.0;
    m_frameInput = 0.0;
    Pace(now, renderMs);
    uint32_t ahead = m_pacing.level == 2 || s_loadingScreenFrame ? 1 : 0;
    double began = emscripten_get_now();
    uint32_t replays = __atomic_load_n(&g_ring.replays, __ATOMIC_RELAXED);
    bool stalled = false;
    for (;;)
    {
        uint32_t taken = __atomic_load_n(&g_ring.framesTaken, __ATOMIC_ACQUIRE);
        if (g_ring.framesSubmitted - taken <= ahead)
            break;
        emscripten_futex_wait((void *)&g_ring.framesTaken, taken, 50.0);
        if (!stalled && emscripten_get_now() - began > 20000.0)
        {
            stalled = true;
            // what the page did meanwhile: whether it still replays (on its animation frames), how far it got, and
            // whether the GPU holds it up (two or three frames unfinished: d3d9_webgpu.cpp, AheadMost)
            char what[256];
            snprintf(what, sizeof(what),
                "waiting for the page to take a frame (%u written, %u taken, %u may wait; in 20 s the page replayed %u times, it "
                "has replayed %u frames to their end, %d of them unfinished on the GPU)",
                g_ring.framesSubmitted, taken, ahead, __atomic_load_n(&g_ring.replays, __ATOMIC_RELAXED) - replays,
                __atomic_load_n(&g_ring.pageShown, __ATOMIC_RELAXED), __atomic_load_n(&g_ring.pagePending, __ATOMIC_RELAXED));
            Bo1_LogStall(what);
        }
    }
    g_bo1WaitPageUs += (uint64_t)((emscripten_get_now() - began) * 1000.0);
    Bo1_FrameEnd();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Bo1Device::Reset(D3DPRESENT_PARAMETERS *pp)
{
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    m_pp = *pp;
    if (!m_pp.BackBufferWidth)
        m_pp.BackBufferWidth = g_bo1Input.width;
    if (!m_pp.BackBufferHeight)
        m_pp.BackBufferHeight = g_bo1Input.height;
    m_backBuffer->m_width = m_pp.BackBufferWidth;
    m_backBuffer->m_height = m_pp.BackBufferHeight;
    m_backBuffer->m_ms = m_pp.MultiSampleType;
    CmdReset *c = Cmd<CmdReset>(OP_RESET);
    c->width = m_pp.BackBufferWidth;
    c->height = m_pp.BackBufferHeight;
    c->multisample = m_pp.MultiSampleType;
    EndCmd();
    memset(m_textures, 0, sizeof(m_textures));
    memset(m_streams, 0, sizeof(m_streams));
    m_indices = nullptr;
    m_decl = nullptr;
    m_vs = nullptr;
    m_ps = nullptr;
    memset(m_rt, 0, sizeof(m_rt));
    m_ds = nullptr;
    ResetState();
    return S_OK;
}

// ----- capabilities -----

static void Bo1_FillCaps(D3DCAPS9 *c)
{
    memset(c, 0, sizeof(*c));
    c->DeviceType = D3DDEVTYPE_HAL;
    c->AdapterOrdinal = 0;
    c->Caps = 0;
    c->Caps2 = D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_CANAUTOGENMIPMAP | D3DCAPS2_FULLSCREENGAMMA;
    c->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
    c->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
    c->CursorCaps = D3DCURSORCAPS_COLOR;
    c->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                 D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY | D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP |
                 D3DDEVCAPS_TEXTURENONLOCALVIDMEM | D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                 D3DDEVCAPS_CANBLTSYSTONONLOCAL | D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_PUREDEVICE;
    c->PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW | D3DPMISCCAPS_COLORWRITEENABLE |
                           D3DPMISCCAPS_CLIPPLANESCALEDPOINTS | D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_BLENDOP | D3DPMISCCAPS_INDEPENDENTWRITEMASKS |
                           D3DPMISCCAPS_FOGANDSPECULARALPHA | D3DPMISCCAPS_SEPARATEALPHABLEND | D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS |
                           D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING | D3DPMISCCAPS_FOGVERTEXCLAMPED;
    c->RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
                    D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_ZFOG | D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG |
                    D3DPRASTERCAPS_COLORPERSPECTIVE | D3DPRASTERCAPS_SCISSORTEST | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS |
                    D3DPRASTERCAPS_MULTISAMPLE_TOGGLE;
    c->ZCmpCaps = 0xFF;
    c->SrcBlendCaps = 0x1FFF;
    c->DestBlendCaps = 0x1FFF;
    c->AlphaCmpCaps = 0xFF;
    c->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB | D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;
    c->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_CUBEMAP |
                     D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_MIPVOLUMEMAP | D3DPTEXTURECAPS_MIPCUBEMAP;
    DWORD filters = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MIPFPOINT |
                    D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MAGFANISOTROPIC;
    c->TextureFilterCaps = filters;
    c->CubeTextureFilterCaps = filters;
    c->VolumeTextureFilterCaps = filters;
    c->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP | D3DPTADDRESSCAPS_BORDER |
                            D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
    c->VolumeTextureAddressCaps = c->TextureAddressCaps;
    c->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;
    c->MaxTextureWidth = 8192;
    c->MaxTextureHeight = 8192;
    c->MaxVolumeExtent = 2048;
    c->MaxTextureRepeat = 8192;
    c->MaxTextureAspectRatio = 8192;
    c->MaxAnisotropy = 16;
    c->MaxVertexW = 1e10f;
    c->GuardBandLeft = -1e8f;
    c->GuardBandTop = -1e8f;
    c->GuardBandRight = 1e8f;
    c->GuardBandBottom = 1e8f;
    c->StencilCaps = 0x1FF | D3DSTENCILCAPS_TWOSIDED;
    c->FVFCaps = 8;
    c->TextureOpCaps = 0x3FFFFFF;
    c->MaxTextureBlendStages = 8;
    c->MaxSimultaneousTextures = 8;
    c->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_POSITIONALLIGHTS |
                              D3DVTXPCAPS_LOCALVIEWER;
    c->MaxActiveLights = 8;
    c->MaxUserClipPlanes = 6;
    c->MaxVertexBlendMatrices = 4;
    c->MaxVertexBlendMatrixIndex = 0;
    c->MaxPointSize = 256.0f;
    c->MaxPrimitiveCount = 0xFFFFF;
    c->MaxVertexIndex = 0xFFFFFF;
    c->MaxStreams = 16;
    c->MaxStreamStride = 255;
    c->VertexShaderVersion = D3DVS_VERSION(3, 0);
    c->MaxVertexShaderConst = 256;
    c->PixelShaderVersion = D3DPS_VERSION(3, 0);
    c->PixelShader1xMaxValue = 65504.0f;
    c->DevCaps2 = D3DDEVCAPS2_STREAMOFFSET | D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET | D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES;
    c->MaxNpatchTessellationLevel = 0.0f;
    c->MasterAdapterOrdinal = 0;
    c->AdapterOrdinalInGroup = 0;
    c->NumberOfAdaptersInGroup = 1;
    c->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N | D3DDTCAPS_USHORT2N | D3DDTCAPS_USHORT4N |
                   D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;
    c->NumSimultaneousRTs = 4;
    c->StretchRectFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;
    c->VS20Caps.Caps = D3DVS20CAPS_PREDICATION;
    c->VS20Caps.DynamicFlowControlDepth = 24;
    c->VS20Caps.NumTemps = 32;
    c->VS20Caps.StaticFlowControlDepth = 4;
    c->PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE | D3DPS20CAPS_GRADIENTINSTRUCTIONS | D3DPS20CAPS_PREDICATION |
                       D3DPS20CAPS_NODEPENDENTREADLIMIT | D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
    c->PS20Caps.DynamicFlowControlDepth = 24;
    c->PS20Caps.NumTemps = 32;
    c->PS20Caps.StaticFlowControlDepth = 4;
    c->PS20Caps.NumInstructionSlots = 512;
    c->VertexTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MAGFPOINT;
    c->MaxVShaderInstructionsExecuted = 65535;
    c->MaxPShaderInstructionsExecuted = 65535;
    c->MaxVertexShader30InstructionSlots = 32768;
    c->MaxPixelShader30InstructionSlots = 32768;
}

HRESULT STDMETHODCALLTYPE Bo1Device::GetDeviceCaps(D3DCAPS9 *pCaps)
{
    Bo1_FillCaps(pCaps);
    return S_OK;
}

// ============================================================================================================
// IDirect3D9
// ============================================================================================================

class Bo1D3D9 : public IDirect3D9
{
public:
    void Destroy() { delete this; }
    BO1_IUNKNOWN
    STDMETHOD(RegisterSoftwareDevice)(void *) override { return D3DERR_NOTAVAILABLE; }
    STDMETHOD_(UINT, GetAdapterCount)() override { return 1; }
    STDMETHOD(GetAdapterIdentifier)(UINT Adapter, DWORD, D3DADAPTER_IDENTIFIER9 *pIdentifier) override
    {
        if (Adapter)
            return D3DERR_INVALIDCALL;
        memset(pIdentifier, 0, sizeof(*pIdentifier));
        strcpy(pIdentifier->Driver, "webgpu");
        strcpy(pIdentifier->Description, "WebGPU");
        strcpy(pIdentifier->DeviceName, "\\\\.\\DISPLAY1");
        pIdentifier->VendorId = 0x10DE; // the engine's GPU tables look NVIDIA up first; any value works
        pIdentifier->DeviceId = 0x0400;
        pIdentifier->WHQLLevel = 1;
        return S_OK;
    }
    STDMETHOD_(UINT, GetAdapterModeCount)(UINT, D3DFORMAT) override { return 1; }
    STDMETHOD(EnumAdapterModes)(UINT, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) override
    {
        if (Mode)
            return D3DERR_INVALIDCALL;
        pMode->Width = g_bo1Input.width;
        pMode->Height = g_bo1Input.height;
        pMode->RefreshRate = Bo1_DisplayHz();
        pMode->Format = Format;
        return S_OK;
    }
    STDMETHOD(GetAdapterDisplayMode)(UINT, D3DDISPLAYMODE *pMode) override
    {
        pMode->Width = g_bo1Input.width;
        pMode->Height = g_bo1Input.height;
        pMode->RefreshRate = Bo1_DisplayHz();
        pMode->Format = D3DFMT_X8R8G8B8;
        return S_OK;
    }
    STDMETHOD(CheckDeviceType)(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) override { return S_OK; }
    STDMETHOD(CheckDeviceFormat)(UINT, D3DDEVTYPE, D3DFORMAT, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override
    {
        uint32_t f = (uint32_t)CheckFormat;
        // depth textures are hardware shadow maps (the executor samples them with depth comparisons, as D3D9 drivers
        // do), 2D only; no NULL / ATOC / SSAA driver hacks, nor INTZ and the like (raw depth reads)
        if (Bo1Fmt_IsDepth(f) && RType == D3DRTYPE_CUBETEXTURE)
            return D3DERR_NOTAVAILABLE;
        if (f == BO1FMT_NULL || f == BO1FMT_INTZ || f == BO1FMT_RAWZ || f == BO1FMT_DF24 || f == BO1FMT_DF16)
            return D3DERR_NOTAVAILABLE;
        if (f == BO1_FOURCC('A', 'T', 'O', 'C') || f == BO1_FOURCC('S', 'S', 'A', 'A'))
            return D3DERR_NOTAVAILABLE;
        if (Usage & D3DUSAGE_QUERY_VERTEXTEXTURE)
            return (f == D3DFMT_R32F || f == D3DFMT_A32B32G32R32F || f == D3DFMT_A16B16G16R16F) ? S_OK : D3DERR_NOTAVAILABLE;
        return Bo1_FormatSupported(CheckFormat, Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) ? S_OK : D3DERR_NOTAVAILABLE;
    }
    STDMETHOD(CheckDeviceMultiSampleType)(UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQualityLevels) override
    {
        // WebGPU draws with 4 samples a pixel, or 1: the engine asks from r_aaSamples down, so 4 and more have 4, 2
        // and 3 none (src/gfx_d3d/r_init.cpp: R_SetupAntiAliasing)
        if (pQualityLevels)
            *pQualityLevels = 1;
        return MultiSampleType == D3DMULTISAMPLE_NONE || MultiSampleType == D3DMULTISAMPLE_4_SAMPLES ? S_OK : D3DERR_NOTAVAILABLE;
    }
    STDMETHOD(CheckDepthStencilMatch)(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT DepthStencilFormat) override
    {
        return Bo1Fmt_IsDepth(DepthStencilFormat) ? S_OK : D3DERR_NOTAVAILABLE;
    }
    STDMETHOD(CheckDeviceFormatConversion)(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT) override { return S_OK; }
    STDMETHOD(GetDeviceCaps)(UINT, D3DDEVTYPE, D3DCAPS9 *pCaps) override
    {
        Bo1_FillCaps(pCaps);
        return S_OK;
    }
    STDMETHOD_(HMONITOR, GetAdapterMonitor)(UINT) override { return (HMONITOR)0x20000; }
    STDMETHOD(CreateDevice)(UINT, D3DDEVTYPE, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pp,
        IDirect3DDevice9 **ppReturnedDeviceInterface) override
    {
        std::lock_guard<std::recursive_mutex> guard(s_dev);
        if (s_device)
        {
            // one device per page; a vid_restart recreates it: reuse with the new parameters
            s_device->Reset(pp);
            s_device->AddRef();
            *ppReturnedDeviceInterface = s_device;
            return S_OK;
        }
        Bo1_Log("CreateDevice %ux%u", pp->BackBufferWidth, pp->BackBufferHeight);
        *ppReturnedDeviceInterface = new Bo1Device(hFocusWindow, BehaviorFlags, pp);
        return S_OK;
    }
};

HRESULT STDMETHODCALLTYPE Bo1Device::GetDirect3D(IDirect3D9 **ppD3D9)
{
    *ppD3D9 = new Bo1D3D9;
    return S_OK;
}

// ============================================================================================================
// entry points
// ============================================================================================================

// Before the player goes in (CL_InitCGame), the pipelines the list has first (tier 0) for the shaders made so far compile
// (d3d9_webgpu.cpp: preloading). Bo1_PreloadFence marks the point in the ring after those shaders; Bo1_PreloadPending
// says whether the executor has yet to reach it, or is still compiling them.
static volatile int32_t s_preloadFence = 1;

// The engine says whether the player's shadow maps are on as its renderer starts (R_Init), before any shader comes: the
// executor takes the map's list then, and of its spawn tier the recipes the spawn draws with at that setting
// (d3d9_webgpu.cpp: TakeRecipes).
void Bo1_PipelineSettings(int shadowMaps)
{
    g_bo1ShadowMaps.store(shadowMaps ? 1 : 0, std::memory_order_release);
}

void Bo1_PreloadFence()
{
    if (!g_ring.executorAlive)
        return; // no WebGPU device: nothing would pass the fence
    std::lock_guard<std::recursive_mutex> guard(s_dev);
    __atomic_store_n(&s_preloadFence, 0, __ATOMIC_RELEASE);
    CmdQuery *c = Cmd<CmdQuery>(OP_QUERY_END);
    c->id = ~0u; // no query's: the executor leaves the flag be
    c->done = &s_preloadFence;
    c->answer = nullptr;
    EndCmd();
}

bool Bo1_PreloadPending()
{
    return !__atomic_load_n(&s_preloadFence, __ATOMIC_ACQUIRE) || g_bo1PreloadPending.load(std::memory_order_acquire) != 0;
}

void Bo1_InitGraphics()
{
    g_ring.data = (uint8_t *)aligned_alloc(64, BO1CMD_RING_SIZE);
    g_ring.writePos = g_ring.readPos = 0;
    g_ring.framesSubmitted = g_ring.framesTaken = 0;
    bo1gpu::ExecutorStart(g_bo1Input.width, g_bo1Input.height);
}

IDirect3D9 *WINAPI Direct3DCreate9(UINT)
{
    return new Bo1D3D9;
}

HRESULT WINAPI Direct3DCreate9Ex(UINT, IDirect3D9Ex **ppD3D)
{
    *ppD3D = nullptr;
    return D3DERR_NOTAVAILABLE;
}
