// XAudio2 2.7 for the WebAssembly build. The engine's own driver (src/sound/snd_driver_xaudio2.cpp) and DSP effects
// (snd_driver_xaudio2_dsp.cpp) run unchanged on this software implementation: it mixes in 10 ms passes at 48 kHz on a
// thread of its own and hands the result to an AudioWorklet through a ring in the shared memory (library_bo1.js:
// bo1_audio_start).
//
// Source formats: 16-bit and 8-bit PCM, IEEE float, MS-ADPCM, xWMA (decoded as it plays, xwma.h). Filters
// (SetFilterParameters) are kept but not applied; the engine filters in its own effects. A movie's sound (bink_web.cpp)
// comes in a queue of its own (bo1_cinematic_audio_*), and goes out after the game's sounds.
#include <windows.h>
#include <XAudio2.h>
#include <XAPOBase.h>

#include <emscripten.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "web_platform.h"
#include "xwma.h"

EXTERN_C const GUID IID_XAudio2 = {0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
EXTERN_C const GUID IID_XAudio2_Debug = {0xdb05ea35, 0x0329, 0x4d4b, {0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52}};
EXTERN_C const GUID IID_IXAudio2 = {0x8bcf1f58, 0x9fe7, 0x4583, {0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb}};
EXTERN_C const GUID IID_IXAPO = {0xA90BC001, 0xE897, 0xE897, {0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x00}};
EXTERN_C const GUID IID_IXAPOParameters = {0xA90BC001, 0xE897, 0xE897, {0x55, 0xE4, 0x9E, 0x47, 0x00, 0x00, 0x00, 0x01}};

// the ring the AudioWorklet reads (library_bo1.js knows this layout)
struct Bo1AudioRing
{
    volatile int32_t writePos; // frames written by the mixer
    volatile int32_t readPos;  // frames read by the worklet
    volatile int32_t ticks;    // the worklet's process() calls: the audio context is running while this moves
    int32_t channels;
    int32_t capacity;  // frames
    int32_t underruns; // process() calls the ring could not fill
    int32_t pad[2];
    float data[1];
};

extern "C" int bo1_audio_start(Bo1AudioRing *ring, int capacity, int channels, int rate); // library_bo1.js

namespace
{
const uint32_t kRate = 48000;
const uint32_t kQuantum = 480; // frames per pass (XAudio2 on Windows: 10 ms)
const uint32_t kOutChannels = 2;
const int32_t kRingFrames = 8192;
const int32_t kLeadFrames = 2400; // what the mixer keeps queued ahead of the worklet (50 ms)
const size_t kWmaKeepBytes = 64 << 20; // decoded xWMA kept for sounds to play again (those playing stay, whatever it comes to)

const GUID kFloatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::recursive_mutex g_lock;
Bo1AudioRing *g_ring;
bool g_engineRunning;
pthread_t g_mixer;
bool g_mixerStarted;

enum Kind
{
    KIND_SOURCE,
    KIND_SUBMIX,
    KIND_MASTER,
};

struct Core;

struct Send
{
    Core *dst;
    std::vector<float> matrix; // [dst channel * source channels + source channel]
};

struct Effect
{
    IXAPO *xapo;
    IXAPOParameters *params;
    bool enabled;
    uint32_t outChannels;
};

// an xWMA sound: decoded as far as a voice has played it, for every voice that plays it
struct WmaSound
{
    const uint8_t *data;
    uint32_t bytes;
    const UINT32 *table; // XAUDIO2_BUFFER_WMA: the decoded bytes after each packet
    uint32_t packets;
    uint32_t channels, rate, blockAlign;
    uint8_t head[16]; // the data's first bytes: the same address could hold another sound after a zone unloads
    uint32_t frames;  // its decoded length (the table's last entry)
    uint32_t nextPacket = 0;
    std::unique_ptr<XwmaDecoder> decoder; // until the last packet is in
    std::vector<int16_t> pcm;             // interleaved
    double lastUse = 0.0;
};

std::vector<std::shared_ptr<WmaSound>> g_wmaSounds;
size_t g_wmaBytes; // their decoded samples

struct QueuedBuffer
{
    XAUDIO2_BUFFER b;
    uint32_t playBegin, playEnd;
    uint32_t loopBegin, loopEnd;
    uint32_t loopsLeft;
    bool started;
    std::shared_ptr<WmaSound> wma;
};

struct Core
{
    Kind kind = KIND_SOURCE;
    IXAudio2Voice *iface = nullptr;
    uint32_t inChannels = 0;
    uint32_t inRate = 0;
    uint32_t stage = 0;
    float volume = 1.0f;
    std::vector<float> channelVolumes;
    std::vector<Send> sends;
    std::vector<Effect> effects;
    std::vector<float> mix;  // submix and mastering voices: this pass's input
    std::vector<float> work; // the pass's samples on their way through the voice
    XAUDIO2_FILTER_PARAMETERS filter = {LowPassFilter, 1.0f, 1.0f};
    bool dead = false;

    // source voices
    uint16_t formatTag = 0;
    uint16_t bitsPerSample = 0;
    uint32_t blockAlign = 0;
    uint32_t avgBytesPerSec = 0;  // xWMA
    uint32_t samplesPerBlock = 0; // MS-ADPCM
    int16_t coef[7][2] = {};
    IXAudio2VoiceCallback *cb = nullptr;
    std::deque<QueuedBuffer> queue;
    bool running = false;
    float ratio = 1.0f, maxRatio = 2.0f;
    double pos = 0.0;    // frame position in the front buffer
    double played = 0.0; // source frames processed since the start or the last end of stream
    const uint8_t *cacheData = nullptr;
    uint32_t cacheBlock = 0;
    std::vector<float> cache; // a decoded MS-ADPCM block

    virtual ~Core() {}
};

std::vector<Core *> g_voices;
std::map<IXAudio2Voice *, Core *> g_byIface;
Core *g_master;
bool g_inPass;
// destroyed voices are kept a while before they are deleted: a late call on one is caught instead of writing freed memory
struct Grave
{
    Core *voice;
    double when;
};
std::deque<Grave> g_graveyard;

void DeadCall(Core *v, const char *what)
{
    static int reports;
    if (reports++ < 20)
        Bo1_Log("XAudio2: %s on a destroyed voice %p", what, (void *)v);
}

Core *CoreOf(IXAudio2Voice *v)
{
    auto it = g_byIface.find(v);
    return it == g_byIface.end() ? nullptr : it->second;
}

uint32_t OutputChannels(const Core *v)
{
    uint32_t ch = v->inChannels;
    for (const Effect &e : v->effects)
        ch = e.outChannels ? e.outChannels : ch;
    return ch;
}

// the matrix XAudio2 uses until SetOutputMatrix
void DefaultMatrix(Send &s, uint32_t srcCh, uint32_t dstCh)
{
    s.matrix.assign((size_t)srcCh * dstCh, 0.0f);
    for (uint32_t d = 0; d < dstCh; ++d)
        for (uint32_t c = 0; c < srcCh; ++c)
            if (srcCh == 1 || c == d)
                s.matrix[d * srcCh + c] = 1.0f;
}

HRESULT SetSends(Core *v, const XAUDIO2_VOICE_SENDS *list)
{
    v->sends.clear();
    uint32_t ch = OutputChannels(v);
    if (!list)
    {
        if (v->kind != KIND_MASTER && g_master)
        {
            Send s{g_master, {}};
            DefaultMatrix(s, ch, g_master->inChannels);
            v->sends.push_back(s);
        }
        return S_OK;
    }
    for (UINT32 i = 0; i < list->SendCount; ++i)
    {
        Core *dst = CoreOf(list->pSends[i].pOutputVoice);
        if (!dst)
            return XAUDIO2_E_INVALID_CALL;
        Send s{dst, {}};
        DefaultMatrix(s, ch, dst->inChannels);
        v->sends.push_back(s);
    }
    return S_OK;
}

void ReleaseEffects(Core *v)
{
    for (Effect &e : v->effects)
    {
        e.xapo->UnlockForProcess();
        e.xapo->Release();
    }
    v->effects.clear();
}

HRESULT SetEffects(Core *v, const XAUDIO2_EFFECT_CHAIN *chain)
{
    ReleaseEffects(v);
    if (!chain)
        return S_OK;
    uint32_t ch = v->inChannels;
    for (UINT32 i = 0; i < chain->EffectCount; ++i)
    {
        const XAUDIO2_EFFECT_DESCRIPTOR &d = chain->pEffectDescriptors[i];
        Effect e = {};
        if (FAILED(d.pEffect->QueryInterface(IID_IXAPO, (void **)&e.xapo)))
            return XAUDIO2_E_INVALID_CALL;
        // the parameters interface lives as long as the effect: no reference of its own
        if (SUCCEEDED(d.pEffect->QueryInterface(IID_IXAPOParameters, (void **)&e.params)))
            e.params->Release();
        e.enabled = d.InitialState != 0;
        e.outChannels = d.OutputChannels ? d.OutputChannels : ch;
        WAVEFORMATEXTENSIBLE in = {}, out;
        in.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        in.Format.nChannels = (WORD)ch;
        in.Format.nSamplesPerSec = kRate;
        in.Format.wBitsPerSample = 32;
        in.Format.nBlockAlign = (WORD)(4 * ch);
        in.Format.nAvgBytesPerSec = kRate * 4 * ch;
        in.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        in.Samples.wValidBitsPerSample = 32;
        in.SubFormat = kFloatSubtype;
        out = in;
        out.Format.nChannels = (WORD)e.outChannels;
        out.Format.nBlockAlign = (WORD)(4 * e.outChannels);
        out.Format.nAvgBytesPerSec = kRate * 4 * e.outChannels;
        XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS lin = {&in.Format, kQuantum}, lout = {&out.Format, kQuantum};
        e.xapo->LockForProcess(1, &lin, 1, &lout);
        v->effects.push_back(e);
        ch = e.outChannels;
    }
    return S_OK;
}

void DestroyCore(Core *v)
{
    ReleaseEffects(v);
    g_voices.erase(std::remove(g_voices.begin(), g_voices.end(), v), g_voices.end());
    g_byIface.erase(v->iface);
    for (Core *o : g_voices)
        o->sends.erase(std::remove_if(o->sends.begin(), o->sends.end(), [v](const Send &s) { return s.dst == v; }), o->sends.end());
    if (g_master == v)
        g_master = nullptr;
    v->dead = true;
    v->queue.clear();
    v->cb = nullptr;
    g_graveyard.push_back(Grave{v, emscripten_get_now()});
}

// ----- source decoding -----

// the xWMA sound a buffer holds: one decoded before, or a new one
std::shared_ptr<WmaSound> FindWmaSound(Core *v, const XAUDIO2_BUFFER &b, const XAUDIO2_BUFFER_WMA &wma)
{
    double now = emscripten_get_now();
    uint8_t head[16] = {};
    memcpy(head, b.pAudioData, (std::min<size_t>)(b.AudioBytes, sizeof(head)));
    for (const std::shared_ptr<WmaSound> &w : g_wmaSounds)
        if (w->data == b.pAudioData && w->bytes == b.AudioBytes && w->table == wma.pDecodedPacketCumulativeBytes &&
            w->packets == wma.PacketCount && w->channels == v->inChannels && w->rate == v->inRate && w->blockAlign == v->blockAlign &&
            !memcmp(w->head, head, sizeof(head)) && w->frames == wma.pDecodedPacketCumulativeBytes[wma.PacketCount - 1] / (2 * v->inChannels))
        {
            w->lastUse = now;
            return w;
        }

    // sounds no voice plays any more go if they were cut off (their decoder is large), and past what is kept the rest
    // go too, the least recently played first
    for (auto it = g_wmaSounds.begin(); it != g_wmaSounds.end();)
    {
        if (it->use_count() == 1 && (*it)->decoder)
        {
            g_wmaBytes -= (*it)->pcm.capacity() * sizeof(int16_t);
            it = g_wmaSounds.erase(it);
        }
        else
            ++it;
    }
    if (g_wmaBytes > kWmaKeepBytes)
    {
        std::stable_sort(g_wmaSounds.begin(), g_wmaSounds.end(),
            [](const std::shared_ptr<WmaSound> &a, const std::shared_ptr<WmaSound> &b) { return a->lastUse < b->lastUse; });
        for (auto it = g_wmaSounds.begin(); it != g_wmaSounds.end() && g_wmaBytes > kWmaKeepBytes;)
        {
            if (it->use_count() == 1)
            {
                g_wmaBytes -= (*it)->pcm.capacity() * sizeof(int16_t);
                it = g_wmaSounds.erase(it);
            }
            else
                ++it;
        }
    }

    std::shared_ptr<WmaSound> w = std::make_shared<WmaSound>();
    w->data = b.pAudioData;
    w->bytes = b.AudioBytes;
    w->table = wma.pDecodedPacketCumulativeBytes;
    w->packets = wma.PacketCount;
    w->channels = v->inChannels;
    w->rate = v->inRate;
    w->blockAlign = v->blockAlign;
    memcpy(w->head, head, sizeof(head));
    w->frames = wma.pDecodedPacketCumulativeBytes[wma.PacketCount - 1] / (2 * v->inChannels);
    w->lastUse = now;
    w->decoder.reset(new XwmaDecoder);
    if (!w->decoder->Init(v->inChannels, v->inRate, v->avgBytesPerSec, v->blockAlign))
    {
        static int reports;
        if (reports++ < 5)
            Bo1_Log("sound: xWMA at %u Hz, %u channels, %u bytes a second does not decode (it plays silence)", v->inRate, v->inChannels,
                v->avgBytesPerSec);
        w->decoder.reset();
    }
    w->pcm.reserve((size_t)w->frames * w->channels);
    g_wmaBytes += w->pcm.capacity() * sizeof(int16_t);
    g_wmaSounds.push_back(w);
    return w;
}

// decodes packets until `frame` is in, or the sound is done
void DecodeWma(WmaSound *w, uint32_t frame)
{
    size_t need = ((size_t)frame + 1) * w->channels, capacity = w->pcm.capacity();
    while (w->decoder && w->pcm.size() < need)
    {
        if (w->nextPacket < w->packets && (size_t)(w->nextPacket + 1) * w->blockAlign <= w->bytes)
        {
            w->decoder->Decode(w->data + (size_t)w->nextPacket * w->blockAlign, w->pcm);
            ++w->nextPacket;
            continue;
        }
        // the last packet is in: what the decoder still holds, to the sound's length
        w->decoder->Finish(w->pcm);
        w->decoder.reset();
        w->pcm.resize((size_t)w->frames * w->channels);
    }
    g_wmaBytes += (w->pcm.capacity() - capacity) * sizeof(int16_t);
}

void DecodeAdpcmBlock(Core *v, const uint8_t *block, uint32_t bytes)
{
    static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
    uint32_t ch = v->inChannels;
    v->cache.assign((size_t)v->samplesPerBlock * ch, 0.0f);
    if (bytes < 7 * ch)
        return;
    int pred[2], delta[2], s1[2], s2[2];
    const uint8_t *p = block;
    for (uint32_t c = 0; c < ch; ++c)
        pred[c] = (std::min<int>)(*p++, 6);
    for (uint32_t c = 0; c < ch; ++c, p += 2)
        delta[c] = (int16_t)(p[0] | (p[1] << 8));
    for (uint32_t c = 0; c < ch; ++c, p += 2)
        s1[c] = (int16_t)(p[0] | (p[1] << 8));
    for (uint32_t c = 0; c < ch; ++c, p += 2)
        s2[c] = (int16_t)(p[0] | (p[1] << 8));
    float *out = v->cache.data();
    for (uint32_t c = 0; c < ch; ++c)
    {
        out[c] = s2[c] / 32768.0f;
        out[ch + c] = s1[c] / 32768.0f;
    }
    uint32_t frames = 2 + (bytes - 7 * ch) * 2 / ch;
    if (frames > v->samplesPerBlock)
        frames = v->samplesPerBlock;
    const uint8_t *end = block + bytes;
    uint32_t n = 0; // nibbles decoded past the header
    for (uint32_t f = 2; f < frames; ++f)
    {
        for (uint32_t c = 0; c < ch; ++c, ++n)
        {
            if (p >= end)
                return;
            int nib = (n & 1) ? (*p++ & 0xF) : (*p >> 4);
            int signedNib = nib >= 8 ? nib - 16 : nib;
            int predicted = (s1[c] * v->coef[pred[c]][0] + s2[c] * v->coef[pred[c]][1]) / 256;
            predicted += signedNib * delta[c];
            predicted = predicted < -32768 ? -32768 : predicted > 32767 ? 32767 : predicted;
            s2[c] = s1[c];
            s1[c] = predicted;
            delta[c] = (adapt[nib] * delta[c]) / 256;
            if (delta[c] < 16)
                delta[c] = 16;
            out[f * ch + c] = predicted / 32768.0f;
        }
    }
}

uint32_t BufferFrames(Core *v, const XAUDIO2_BUFFER &b, const XAUDIO2_BUFFER_WMA *wma)
{
    uint32_t ch = v->inChannels;
    switch (v->formatTag)
    {
    case WAVE_FORMAT_PCM:
        return b.AudioBytes / (ch * (v->bitsPerSample == 8 ? 1 : 2));
    case WAVE_FORMAT_IEEE_FLOAT:
        return b.AudioBytes / (ch * 4);
    case WAVE_FORMAT_ADPCM: {
        uint32_t blocks = b.AudioBytes / v->blockAlign, rest = b.AudioBytes % v->blockAlign;
        uint32_t frames = blocks * v->samplesPerBlock;
        if (rest >= 7 * ch)
            frames += 2 + (rest - 7 * ch) * 2 / ch;
        return frames;
    }
    case 0x0161: // xWMA: the decoded length
        if (wma && wma->PacketCount && wma->pDecodedPacketCumulativeBytes)
            return wma->pDecodedPacketCumulativeBytes[wma->PacketCount - 1] / (2 * ch);
        return 0;
    default:
        return 0;
    }
}

float Sample(Core *v, const QueuedBuffer &q, uint32_t frame, uint32_t c)
{
    uint32_t ch = v->inChannels;
    const uint8_t *data = q.b.pAudioData;
    switch (v->formatTag)
    {
    case WAVE_FORMAT_PCM:
        if (v->bitsPerSample == 8)
            return (data[frame * ch + c] - 128) / 128.0f;
        return ((const int16_t *)data)[frame * ch + c] / 32768.0f;
    case WAVE_FORMAT_IEEE_FLOAT:
        return ((const float *)data)[frame * ch + c];
    case WAVE_FORMAT_ADPCM: {
        uint32_t block = frame / v->samplesPerBlock;
        if (v->cacheData != data || v->cacheBlock != block)
        {
            uint32_t offset = block * v->blockAlign;
            uint32_t bytes = (std::min)(v->blockAlign, q.b.AudioBytes - offset);
            DecodeAdpcmBlock(v, data + offset, bytes);
            v->cacheData = data;
            v->cacheBlock = block;
        }
        return v->cache[(frame % v->samplesPerBlock) * ch + c];
    }
    case 0x0161: {
        WmaSound *w = q.wma.get();
        if (!w)
            return 0.0f;
        size_t at = (size_t)frame * ch + c;
        if (at >= w->pcm.size())
            DecodeWma(w, frame);
        return at < w->pcm.size() ? w->pcm[at] / 32768.0f : 0.0f;
    }
    default:
        return 0.0f;
    }
}

// ----- the pass -----

void RunEffects(Core *v, uint32_t &ch)
{
    for (Effect &e : v->effects)
    {
        if (e.outChannels != ch)
            continue; // the engine's effects keep their channel count; others are not supported
        XAPO_PROCESS_BUFFER_PARAMETERS in = {v->work.data(), XAPO_BUFFER_VALID, kQuantum};
        XAPO_PROCESS_BUFFER_PARAMETERS out = {v->work.data(), XAPO_BUFFER_VALID, kQuantum};
        e.xapo->Process(1, &in, 1, &out, e.enabled);
    }
}

void Deliver(Core *v, uint32_t ch)
{
    float gain[XAUDIO2_MAX_AUDIO_CHANNELS];
    for (uint32_t c = 0; c < ch && c < XAUDIO2_MAX_AUDIO_CHANNELS; ++c)
        gain[c] = v->volume * (c < v->channelVolumes.size() ? v->channelVolumes[c] : 1.0f);
    const float *src = v->work.data();
    for (Send &s : v->sends)
    {
        Core *d = s.dst;
        uint32_t dch = d->inChannels;
        if (s.matrix.size() != (size_t)ch * dch)
            DefaultMatrix(s, ch, dch);
        float *dst = d->mix.data();
        for (uint32_t o = 0; o < dch; ++o)
        {
            float m[XAUDIO2_MAX_AUDIO_CHANNELS];
            bool any = false;
            for (uint32_t c = 0; c < ch; ++c)
            {
                m[c] = s.matrix[o * ch + c] * gain[c];
                any = any || m[c] != 0.0f;
            }
            if (!any)
                continue;
            for (uint32_t f = 0; f < kQuantum; ++f)
            {
                float acc = 0.0f;
                for (uint32_t c = 0; c < ch; ++c)
                    acc += m[c] * src[f * ch + c];
                dst[f * dch + o] += acc;
            }
        }
    }
}

void RenderSource(Core *v)
{
    if (!v->running || v->queue.empty())
        return;
    if (v->cb)
        v->cb->OnVoiceProcessingPassStart(0);
    uint32_t ch = v->inChannels;
    v->work.assign((size_t)kQuantum * ch, 0.0f);
    double step = (double)v->inRate * v->ratio / kRate;
    for (uint32_t f = 0; f < kQuantum && !v->queue.empty(); ++f)
    {
        QueuedBuffer &q = v->queue.front();
        if (!q.started)
        {
            q.started = true;
            if (v->cb)
                v->cb->OnBufferStart(q.b.pContext);
        }
        uint32_t limit = q.loopsLeft ? q.loopEnd : q.playEnd;
        uint32_t i = (uint32_t)v->pos;
        if (i < limit)
        {
            float frac = (float)(v->pos - i);
            for (uint32_t c = 0; c < ch; ++c)
            {
                float a = Sample(v, q, i, c);
                float b = i + 1 < limit ? Sample(v, q, i + 1, c) : a;
                v->work[f * ch + c] = a + (b - a) * frac;
            }
        }
        v->pos += step;
        v->played += step;
        while (!v->queue.empty())
        {
            QueuedBuffer &cur = v->queue.front();
            if (cur.loopsLeft && cur.loopEnd > cur.loopBegin && v->pos >= cur.loopEnd)
            {
                v->pos -= cur.loopEnd - cur.loopBegin;
                if (cur.loopsLeft != XAUDIO2_LOOP_INFINITE)
                    --cur.loopsLeft;
                if (v->cb)
                    v->cb->OnLoopEnd(cur.b.pContext);
                continue;
            }
            if (v->pos < cur.playEnd)
                break;
            double over = v->pos - cur.playEnd;
            void *context = cur.b.pContext;
            bool eos = (cur.b.Flags & XAUDIO2_END_OF_STREAM) != 0;
            v->queue.pop_front();
            v->pos = v->queue.empty() ? 0.0 : v->queue.front().playBegin + over;
            if (v->cb)
                v->cb->OnBufferEnd(context);
            if (eos)
            {
                v->played = 0.0;
                if (v->cb)
                    v->cb->OnStreamEnd();
            }
            if (v->dead)
                return;
        }
    }
    if (v->dead)
        return;
    RunEffects(v, ch);
    Deliver(v, ch);
    if (v->cb)
        v->cb->OnVoiceProcessingPassEnd();
}

void RenderBus(Core *v)
{
    uint32_t ch = v->inChannels;
    v->work = v->mix;
    RunEffects(v, ch);
    if (v->kind == KIND_MASTER)
    {
        for (uint32_t c = 0; c < ch; ++c)
        {
            float g = v->volume * (c < v->channelVolumes.size() ? v->channelVolumes[c] : 1.0f);
            for (uint32_t f = 0; f < kQuantum; ++f)
                v->work[f * ch + c] *= g;
        }
        return;
    }
    Deliver(v, ch);
}

// ---- a cinematic's sound (bink_web.cpp) ----
// On Windows Bink plays a movie's sound through a device of its own, beside the game's, in step with its pictures. Here
// the movie's tracks, decoded and mixed at the volumes the engine set them to as each frame is, come to this queue, and
// go out after the game's sounds: stereo at the movie's rate, resampled to the output's where they differ. A Bink movie
// carries its sound ahead of its pictures: the first frame has the first 0.76 s of it (the loading movies'), each next
// one its own 1/30 s. So the queue keeps what has come, and plays it in step with the pictures, which the engine times
// by its clock: the frame it decodes to show says where the sound should be (bo1_cinematic_audio_sync). The sound
// starts with the first picture, and is brought to the pictures once the clock has settled, a third of a second in
// (the engine shows its second frame first, and the ring holds 50 ms of the game's sound ahead of the movie's). From
// there the mixer keeps the clock itself, which runs on as the sound does: a frame later than it (a stalled engine
// decodes the frame it is past, then the frames up to its clock, which a slow machine takes a while over) leaves it
// be, and one ahead of it moves it on. The sound moves only when it is more than 100 ms off: when it ran out while the
// engine decoded nothing, it skips ahead; when it is ahead of the pictures, it waits. Kept to a quarter of a second at
// most, as the queue was, it dropped the first half second of each movie, and played the rest that far ahead of its
// pictures; brought to the frames themselves as they came, in a test's software browser, it skipped and waited by
// most of a second at each stall.
std::mutex g_cineLock;
std::vector<float> g_cineQueue; // interleaved stereo frames at g_cineRate, from g_cineHead
size_t g_cineHead;
uint32_t g_cineRate = kRate;
bool g_cineOpen, g_cinePaused;
double g_cinePos;    // the read position between frames, for resampling
int64_t g_cineAt;    // the movie's sound frame g_cineHead holds (its first, 0, goes with its first picture)
int64_t g_cineIn;    // the movie's sound frames come in so far: where the next goes
uint32_t g_cineWait; // output frames of silence before the queue plays on: the sound is that far ahead of the pictures
// the last frame the engine decoded to show: its place in the sound, and when (Bo1_Milliseconds); applied once
double g_cineSyncAt, g_cineSyncMs;
bool g_cineSync;
bool g_cineSettled;   // the sound was brought to the pictures once the clock settled, and the mixer keeps the clock:
double g_cineClockAt; // where the pictures were in the sound,
double g_cineClockMs; // and when (Bo1_Milliseconds; moved on while the movie is paused)
double g_cinePausedMs;

// Brings the sound to the pictures, as the mixer is about to play on. What is heard now is what it reached, less what
// waits in the ring for the worklet; a picture decoded now shows about as much later.
void SyncCinematic()
{
    if (!g_cineSync)
        return;
    g_cineSync = false;
    double step = (double)g_cineRate / kRate;
    int32_t ring = g_ring ? g_ring->writePos - g_ring->readPos : 0;
    double now = Bo1_Milliseconds();
    double picture = g_cineSyncAt + (now - g_cineSyncMs) * g_cineRate / 1000.0;
    double heard = (double)g_cineAt + g_cinePos - ((double)g_cineWait + (double)std::max<int32_t>(ring, 0)) * step;
    double slack = 0.1 * g_cineRate;
    if (!g_cineSettled)
    {
        if (picture < g_cineRate / 3.0)
            return;
        g_cineSettled = true;
        slack = 0.005 * g_cineRate;
    }
    else
    {
        double clock = g_cineClockAt + (now - g_cineClockMs) * g_cineRate / 1000.0;
        if (picture < clock)
            picture = clock;
    }
    g_cineClockAt = picture;
    g_cineClockMs = now;
    if (heard > picture + slack)
    {
        g_cineWait += (uint32_t)((heard - picture) / step);
        Bo1_Log("bink: the sound waits %.0f ms for the pictures", (heard - picture) * 1000.0 / g_cineRate);
    }
    else if (heard < picture - slack)
    {
        // what the wait still holds back goes first, then the queue up to the pictures
        double behind = picture - heard;
        double unwait = std::min<double>(behind / step, (double)g_cineWait);
        g_cineWait -= (uint32_t)unwait;
        int64_t skip = (int64_t)(behind - unwait * step);
        if (skip > 0)
        {
            size_t queued = (g_cineQueue.size() - g_cineHead) / 2;
            if ((size_t)skip >= queued)
            {
                g_cineQueue.clear();
                g_cineHead = 0;
            }
            else
            {
                g_cineHead += (size_t)skip * 2;
            }
            g_cineAt += skip;
            g_cinePos = 0.0;
        }
        Bo1_Log("bink: the sound skipped %.0f ms to the pictures", behind * 1000.0 / g_cineRate);
    }
}

// the queue's part of a pass: kQuantum output frames added to out, from what has come in
void MixCinematic(float *out)
{
    std::lock_guard<std::mutex> guard(g_cineLock);
    if (!g_cineOpen || g_cinePaused)
        return;
    SyncCinematic();
    double step = (double)g_cineRate / kRate;
    size_t frames = (g_cineQueue.size() - g_cineHead) / 2;
    uint32_t f = std::min<uint32_t>(g_cineWait, kQuantum);
    g_cineWait -= f;
    for (; f < kQuantum; ++f)
    {
        size_t i = (size_t)g_cinePos;
        if (i + 1 >= frames)
            break;
        float t = (float)(g_cinePos - (double)i);
        const float *a = &g_cineQueue[g_cineHead + i * 2], *b = a + 2;
        out[f * 2] += a[0] + (b[0] - a[0]) * t;
        out[f * 2 + 1] += a[1] + (b[1] - a[1]) * t;
        g_cinePos += step;
    }
    size_t used = (size_t)g_cinePos;
    if (used > frames)
        used = frames;
    g_cineHead += used * 2;
    g_cineAt += (int64_t)used;
    g_cinePos -= (double)used;
    if (g_cineHead > 65536 && g_cineHead * 2 > g_cineQueue.size())
    {
        g_cineQueue.erase(g_cineQueue.begin(), g_cineQueue.begin() + g_cineHead);
        g_cineHead = 0;
    }
}
} // namespace

// a movie opens, or starts again from its first frame: its sound starts with its first picture
extern "C" void bo1_cinematic_audio_open(int rate)
{
    std::lock_guard<std::mutex> guard(g_cineLock);
    g_cineQueue.clear();
    g_cineHead = 0;
    g_cinePos = 0.0;
    g_cineAt = g_cineIn = 0;
    g_cineWait = 0;
    g_cineSync = g_cineSettled = false;
    g_cineRate = rate > 0 ? (uint32_t)rate : kRate;
    g_cinePaused = false;
    g_cineOpen = true;
}

extern "C" void bo1_cinematic_audio_close()
{
    std::lock_guard<std::mutex> guard(g_cineLock);
    g_cineOpen = false;
    g_cineQueue.clear();
    g_cineHead = 0;
}

// stereo frames, interleaved, at the rate the stream opened with: the movie's next, after those come before
extern "C" void bo1_cinematic_audio_queue(const float *stereo, int frames)
{
    std::lock_guard<std::mutex> guard(g_cineLock);
    if (!g_cineOpen || frames <= 0)
        return;
    int64_t from = g_cineIn;
    g_cineIn += frames;
    // what the sound has skipped past (SyncCinematic) is not played
    int64_t skipped = g_cineAt - from;
    if (skipped >= frames)
        return;
    if (skipped > 0)
    {
        stereo += skipped * 2;
        frames -= (int)skipped;
    }
    g_cineQueue.insert(g_cineQueue.end(), stereo, stereo + (size_t)frames * 2);
    // a mixer that has stopped taking it keeps 8 s at most, the newest, rather than the whole movie (in step, it holds
    // under a second)
    size_t queued = (g_cineQueue.size() - g_cineHead) / 2, most = (size_t)g_cineRate * 8;
    if (queued > most)
    {
        g_cineHead += (queued - most) * 2;
        g_cineAt += (int64_t)(queued - most);
        g_cinePos = 0.0;
    }
}

// the engine decoded the picture it shows next, `seconds` into the movie: its sound is to be heard with it (the mixer's
// next pass sees to it: SyncCinematic)
extern "C" void bo1_cinematic_audio_sync(double seconds)
{
    std::lock_guard<std::mutex> guard(g_cineLock);
    g_cineSyncAt = seconds * g_cineRate;
    g_cineSyncMs = Bo1_Milliseconds();
    g_cineSync = true;
}

// the movie pauses, or plays on: its clock stops meanwhile, as the engine's does
extern "C" void bo1_cinematic_audio_pause(int paused)
{
    std::lock_guard<std::mutex> guard(g_cineLock);
    if (paused && !g_cinePaused)
        g_cinePausedMs = Bo1_Milliseconds();
    else if (!paused && g_cinePaused)
    {
        g_cineClockMs += Bo1_Milliseconds() - g_cinePausedMs;
        g_cineSyncMs += Bo1_Milliseconds() - g_cinePausedMs;
    }
    g_cinePaused = paused != 0;
}

namespace
{
// one pass; the mastering voice's output (kQuantum * kOutChannels) in `out`
void Pass(float *out)
{
    std::lock_guard<std::recursive_mutex> guard(g_lock);
    memset(out, 0, sizeof(float) * kQuantum * kOutChannels);
    if (!g_master)
        return;
    g_inPass = true;
    std::vector<Core *> voices = g_voices;
    for (Core *v : voices)
        if (v->kind != KIND_SOURCE)
            v->mix.assign((size_t)kQuantum * v->inChannels, 0.0f);
    for (Core *v : voices)
        if (v->kind == KIND_SOURCE && !v->dead)
            RenderSource(v);
    std::vector<Core *> buses;
    for (Core *v : voices)
        if (v->kind == KIND_SUBMIX && !v->dead)
            buses.push_back(v);
    std::stable_sort(buses.begin(), buses.end(), [](Core *a, Core *b) { return a->stage < b->stage; });
    for (Core *v : buses)
        RenderBus(v);
    if (g_master && !g_master->dead)
    {
        RenderBus(g_master);
        uint32_t ch = g_master->inChannels;
        for (uint32_t f = 0; f < kQuantum; ++f)
            for (uint32_t c = 0; c < kOutChannels; ++c)
                out[f * kOutChannels + c] = c < ch ? g_master->work[f * ch + c] : 0.0f;
    }
    MixCinematic(out);
    g_inPass = false;
    double now = emscripten_get_now();
    while (!g_graveyard.empty() && now - g_graveyard.front().when > 5000.0)
    {
        delete g_graveyard.front().voice;
        g_graveyard.pop_front();
    }
}

void *MixerThread(void *)
{
    std::vector<float> out((size_t)kQuantum * kOutChannels);
    double clock = emscripten_get_now();
    int32_t lastTicks = -1;
    double lastTickChange = clock;
    for (;;)
    {
        double now = emscripten_get_now();
        if (!g_engineRunning)
        {
            usleep(5000);
            clock = now;
            continue;
        }
        int32_t ticks = __atomic_load_n(&g_ring->ticks, __ATOMIC_ACQUIRE);
        if (ticks != lastTicks)
        {
            lastTicks = ticks;
            lastTickChange = now;
        }
        if (now - lastTickChange < 300.0)
        {
            // the worklet is playing: keep it fed
            int32_t queued = g_ring->writePos - __atomic_load_n(&g_ring->readPos, __ATOMIC_ACQUIRE);
            if (queued + (int32_t)kQuantum <= kLeadFrames)
            {
                Pass(out.data());
                uint32_t w = (uint32_t)g_ring->writePos;
                for (uint32_t f = 0; f < kQuantum; ++f)
                {
                    float *dst = g_ring->data + ((w + f) % kRingFrames) * kOutChannels;
                    for (uint32_t c = 0; c < kOutChannels; ++c)
                        dst[c] = out[f * kOutChannels + c];
                }
                __atomic_store_n(&g_ring->writePos, (int32_t)(w + kQuantum), __ATOMIC_RELEASE);
                clock = now;
                continue;
            }
            usleep(2000);
            clock = now;
        }
        else
        {
            // nothing plays (the page has not been clicked yet, or the tab is hidden): sounds still advance in real time
            if (now >= clock)
            {
                Pass(out.data());
                clock += kQuantum * 1000.0 / kRate;
                if (now - clock > 200.0)
                    clock = now;
                continue;
            }
            usleep(2000);
        }
    }
    return nullptr;
}

void StartMixer()
{
    if (g_mixerStarted)
        return;
    g_mixerStarted = true;
    size_t bytes = sizeof(Bo1AudioRing) + sizeof(float) * kRingFrames * kOutChannels;
    g_ring = (Bo1AudioRing *)aligned_alloc(64, (bytes + 63) & ~(size_t)63);
    memset(g_ring, 0, bytes);
    g_ring->channels = kOutChannels;
    g_ring->capacity = kRingFrames;
    if (!bo1_audio_start(g_ring, kRingFrames, kOutChannels, kRate))
        Bo1_Log("sound: Web Audio is not available; mixing without output");
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1 << 20);
    pthread_create(&g_mixer, &attr, MixerThread, nullptr);
    pthread_attr_destroy(&attr);
}

// ============================================================================================================
// the COM objects
// ============================================================================================================

template <class I> struct VoiceImpl : I, Core
{
    STDMETHOD_(void, GetVoiceDetails)(XAUDIO2_VOICE_DETAILS *d) override
    {
        if (dead) { DeadCall(this, "GetVoiceDetails"); return; }
        d->CreationFlags = 0;
        d->InputChannels = inChannels;
        d->InputSampleRate = inRate;
    }
    STDMETHOD(SetOutputVoices)(const XAUDIO2_VOICE_SENDS *list) override
    {
        if (dead) { DeadCall(this, "SetOutputVoices"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        return SetSends(this, list);
    }
    STDMETHOD(SetEffectChain)(const XAUDIO2_EFFECT_CHAIN *chain) override
    {
        if (dead) { DeadCall(this, "SetEffectChain"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        HRESULT hr = SetEffects(this, chain);
        for (Send &s : sends)
            DefaultMatrix(s, OutputChannels(this), s.dst->inChannels);
        return hr;
    }
    STDMETHOD(EnableEffect)(UINT32 i, UINT32) override { return SetEnabled(i, true); }
    STDMETHOD(DisableEffect)(UINT32 i, UINT32) override { return SetEnabled(i, false); }
    HRESULT SetEnabled(UINT32 i, bool on)
    {
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        if (i >= effects.size())
            return XAUDIO2_E_INVALID_CALL;
        effects[i].enabled = on;
        return S_OK;
    }
    STDMETHOD_(void, GetEffectState)(UINT32 i, BOOL *enabled) override
    {
        if (dead) { DeadCall(this, "GetEffectState"); return; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        *enabled = i < effects.size() && effects[i].enabled;
    }
    STDMETHOD(SetEffectParameters)(UINT32 i, const void *p, UINT32 size, UINT32) override
    {
        if (dead) { DeadCall(this, "SetEffectParameters"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        if (i >= effects.size() || !effects[i].params)
            return XAUDIO2_E_INVALID_CALL;
        effects[i].params->SetParameters(p, size);
        return S_OK;
    }
    STDMETHOD(GetEffectParameters)(UINT32 i, void *p, UINT32 size) override
    {
        if (dead) { DeadCall(this, "GetEffectParameters"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        if (i >= effects.size() || !effects[i].params)
            return XAUDIO2_E_INVALID_CALL;
        effects[i].params->GetParameters(p, size);
        return S_OK;
    }
    STDMETHOD(SetFilterParameters)(const XAUDIO2_FILTER_PARAMETERS *p, UINT32) override
    {
        if (dead) { DeadCall(this, "SetFilterParameters"); return XAUDIO2_E_INVALID_CALL; }
        filter = *p;
        return S_OK;
    }
    STDMETHOD_(void, GetFilterParameters)(XAUDIO2_FILTER_PARAMETERS *p) override { *p = filter; }
    STDMETHOD(SetOutputFilterParameters)(IXAudio2Voice *, const XAUDIO2_FILTER_PARAMETERS *, UINT32) override { return S_OK; }
    STDMETHOD_(void, GetOutputFilterParameters)(IXAudio2Voice *, XAUDIO2_FILTER_PARAMETERS *p) override
    {
        if (dead) { DeadCall(this, "GetOutputFilterParameters"); return; }
        *p = {LowPassFilter, 1.0f, 1.0f};
    }
    STDMETHOD(SetVolume)(float v, UINT32) override
    {
        if (dead) { DeadCall(this, "SetVolume"); return XAUDIO2_E_INVALID_CALL; }
        volume = v;
        return S_OK;
    }
    STDMETHOD_(void, GetVolume)(float *v) override { *v = volume; }
    STDMETHOD(SetChannelVolumes)(UINT32 n, const float *v, UINT32) override
    {
        if (dead) { DeadCall(this, "SetChannelVolumes"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        channelVolumes.assign(v, v + n);
        return S_OK;
    }
    STDMETHOD_(void, GetChannelVolumes)(UINT32 n, float *v) override
    {
        if (dead) { DeadCall(this, "GetChannelVolumes"); return; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        for (UINT32 i = 0; i < n; ++i)
            v[i] = i < channelVolumes.size() ? channelVolumes[i] : 1.0f;
    }
    Send *FindSend(IXAudio2Voice *dst)
    {
        if (!dst)
            return sends.size() == 1 ? &sends[0] : nullptr;
        Core *d = CoreOf(dst);
        for (Send &s : sends)
            if (s.dst == d)
                return &s;
        return nullptr;
    }
    STDMETHOD(SetOutputMatrix)(IXAudio2Voice *dst, UINT32 srcCh, UINT32 dstCh, const float *levels, UINT32) override
    {
        if (dead) { DeadCall(this, "SetOutputMatrix"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        Send *s = FindSend(dst);
        if (!s || srcCh != OutputChannels(this) || dstCh != s->dst->inChannels)
            return XAUDIO2_E_INVALID_CALL;
        s->matrix.assign(levels, levels + srcCh * dstCh);
        return S_OK;
    }
    STDMETHOD_(void, GetOutputMatrix)(IXAudio2Voice *dst, UINT32 srcCh, UINT32 dstCh, float *levels) override
    {
        if (dead) { DeadCall(this, "GetOutputMatrix"); return; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        Send *s = FindSend(dst);
        for (UINT32 i = 0; i < srcCh * dstCh; ++i)
            levels[i] = s && i < s->matrix.size() ? s->matrix[i] : 0.0f;
    }
    STDMETHOD_(void, DestroyVoice)() override
    {
        if (dead) { DeadCall(this, "DestroyVoice"); return; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        DestroyCore(this);
    }
};

struct SourceVoice : VoiceImpl<IXAudio2SourceVoice>
{
    STDMETHOD(Start)(UINT32, UINT32) override
    {
        if (dead) { DeadCall(this, "Start"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        running = true;
        return S_OK;
    }
    STDMETHOD(Stop)(UINT32, UINT32) override
    {
        if (dead) { DeadCall(this, "Stop"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        running = false;
        return S_OK;
    }
    STDMETHOD(SubmitSourceBuffer)(const XAUDIO2_BUFFER *b, const XAUDIO2_BUFFER_WMA *wma) override
    {
        if (dead) { DeadCall(this, "SubmitSourceBuffer"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        if (queue.size() >= XAUDIO2_MAX_QUEUED_BUFFERS)
            return XAUDIO2_E_INVALID_CALL;
        QueuedBuffer q = {};
        q.b = *b;
        uint32_t frames = BufferFrames(this, *b, wma);
        if (formatTag == 0x0161 && frames)
            q.wma = FindWmaSound(this, *b, *wma);
        q.playBegin = (std::min)(b->PlayBegin, frames);
        q.playEnd = b->PlayLength ? (std::min)(b->PlayBegin + b->PlayLength, frames) : frames;
        q.loopsLeft = b->LoopCount;
        q.loopBegin = b->LoopBegin;
        q.loopEnd = b->LoopLength ? b->LoopBegin + b->LoopLength : q.playEnd;
        if (q.loopEnd > q.playEnd)
            q.loopEnd = q.playEnd;
        if (queue.empty())
            pos = q.playBegin;
        queue.push_back(q);
        return S_OK;
    }
    STDMETHOD(FlushSourceBuffers)() override
    {
        if (dead) { DeadCall(this, "FlushSourceBuffers"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        size_t keep = running && !queue.empty() && queue.front().started ? 1 : 0;
        while (queue.size() > keep)
        {
            void *context = queue.back().b.pContext;
            queue.pop_back();
            if (cb)
                cb->OnBufferEnd(context);
        }
        if (queue.empty())
            pos = 0.0;
        return S_OK;
    }
    STDMETHOD(Discontinuity)() override
    {
        if (dead) { DeadCall(this, "Discontinuity"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        if (!queue.empty())
            queue.back().b.Flags |= XAUDIO2_END_OF_STREAM;
        return S_OK;
    }
    STDMETHOD(ExitLoop)(UINT32) override
    {
        if (dead) { DeadCall(this, "ExitLoop"); return XAUDIO2_E_INVALID_CALL; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        if (!queue.empty())
            queue.front().loopsLeft = 0;
        return S_OK;
    }
    STDMETHOD_(void, GetState)(XAUDIO2_VOICE_STATE *s) override
    {
        if (dead) { DeadCall(this, "GetState"); s->pCurrentBufferContext = nullptr; s->BuffersQueued = 0; s->SamplesPlayed = 0; return; }
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        s->pCurrentBufferContext = queue.empty() ? nullptr : queue.front().b.pContext;
        s->BuffersQueued = (UINT32)queue.size();
        s->SamplesPlayed = (UINT64)played;
    }
    STDMETHOD(SetFrequencyRatio)(float r, UINT32) override
    {
        if (dead) { DeadCall(this, "SetFrequencyRatio"); return XAUDIO2_E_INVALID_CALL; }
        ratio = (std::max)(XAUDIO2_MIN_FREQ_RATIO, (std::min)(r, maxRatio));
        return S_OK;
    }
    STDMETHOD_(void, GetFrequencyRatio)(float *r) override { *r = ratio; }
    STDMETHOD(SetSourceSampleRate)(UINT32 rate) override
    {
        if (dead) { DeadCall(this, "SetSourceSampleRate"); return XAUDIO2_E_INVALID_CALL; }
        inRate = rate;
        return S_OK;
    }
};

struct SubmixVoice : VoiceImpl<IXAudio2SubmixVoice>
{
};

struct MasterVoice : VoiceImpl<IXAudio2MasteringVoice>
{
};

void Register(Core *v, IXAudio2Voice *iface)
{
    v->iface = iface;
    g_voices.push_back(v);
    g_byIface[iface] = v;
}

const wchar_t kDeviceId[] = L"{B01A0D10-0000-4000-8000-000000000001}";

struct Engine : IXAudio2
{
    LONG refs = 1;

    STDMETHOD(QueryInterface)(REFIID riid, void **ppv) override
    {
        if (riid == IID_IXAudio2 || riid == IID_IUnknown)
        {
            *ppv = static_cast<IXAudio2 *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return (ULONG)InterlockedIncrement(&refs); }
    STDMETHOD_(ULONG, Release)() override
    {
        LONG n = InterlockedDecrement(&refs);
        if (n == 0)
            delete this;
        return (ULONG)n;
    }
    STDMETHOD(GetDeviceCount)(UINT32 *count) override
    {
        *count = 1;
        return S_OK;
    }
    STDMETHOD(GetDeviceDetails)(UINT32 index, XAUDIO2_DEVICE_DETAILS *d) override
    {
        if (index != 0)
            return XAUDIO2_E_INVALID_CALL;
        memset(d, 0, sizeof(*d));
        wcsncpy(d->DeviceID, kDeviceId, 255);
        wcsncpy(d->DisplayName, L"Browser audio", 255);
        d->Role = GlobalDefaultDevice;
        WAVEFORMATEXTENSIBLE &f = d->OutputFormat;
        f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        f.Format.nChannels = kOutChannels;
        f.Format.nSamplesPerSec = kRate;
        f.Format.wBitsPerSample = 32;
        f.Format.nBlockAlign = 4 * kOutChannels;
        f.Format.nAvgBytesPerSec = kRate * 4 * kOutChannels;
        f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        f.Samples.wValidBitsPerSample = 32;
        f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        f.SubFormat = kFloatSubtype;
        return S_OK;
    }
    STDMETHOD(Initialize)(UINT32, XAUDIO2_PROCESSOR) override { return S_OK; }
    STDMETHOD(RegisterForCallbacks)(IXAudio2EngineCallback *) override { return S_OK; }
    STDMETHOD_(void, UnregisterForCallbacks)(IXAudio2EngineCallback *) override {}
    STDMETHOD(CreateSourceVoice)(IXAudio2SourceVoice **out, const WAVEFORMATEX *fmt, UINT32, float maxRatio, IXAudio2VoiceCallback *cb,
        const XAUDIO2_VOICE_SENDS *sends, const XAUDIO2_EFFECT_CHAIN *chain) override
    {
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        *out = nullptr;
        if (!fmt || !fmt->nChannels || fmt->nChannels > 2 || !fmt->nSamplesPerSec)
            return XAUDIO2_E_INVALID_CALL;
        SourceVoice *v = new SourceVoice;
        v->kind = KIND_SOURCE;
        v->inChannels = fmt->nChannels;
        v->inRate = fmt->nSamplesPerSec;
        v->formatTag = fmt->wFormatTag;
        v->bitsPerSample = fmt->wBitsPerSample;
        v->blockAlign = fmt->nBlockAlign;
        v->avgBytesPerSec = fmt->nAvgBytesPerSec;
        v->maxRatio = maxRatio > 0.0f ? (std::min)(maxRatio, XAUDIO2_MAX_FREQ_RATIO) : 2.0f;
        v->cb = cb;
        if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        {
            const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)fmt;
            v->formatTag = x->SubFormat == kFloatSubtype ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        }
        if (fmt->wFormatTag == WAVE_FORMAT_ADPCM)
        {
            const ADPCMWAVEFORMAT *a = (const ADPCMWAVEFORMAT *)fmt;
            v->samplesPerBlock = a->wSamplesPerBlock;
            static const int16_t standard[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};
            for (int i = 0; i < 7; ++i)
            {
                v->coef[i][0] = i < a->wNumCoef ? a->aCoef[i].iCoef1 : standard[i][0];
                v->coef[i][1] = i < a->wNumCoef ? a->aCoef[i].iCoef2 : standard[i][1];
            }
            if (!v->blockAlign || !v->samplesPerBlock)
            {
                delete v;
                return XAUDIO2_E_INVALID_CALL;
            }
        }
        else if (v->formatTag != WAVE_FORMAT_PCM && v->formatTag != WAVE_FORMAT_IEEE_FLOAT && v->formatTag != 0x0161)
        {
            Bo1_Log("sound: source format 0x%x is not supported", v->formatTag);
            delete v;
            return XAUDIO2_E_INVALID_CALL;
        }
        Register(v, static_cast<IXAudio2SourceVoice *>(v));
        SetEffects(v, chain);
        if (FAILED(SetSends(v, sends)))
        {
            DestroyCore(v);
            return XAUDIO2_E_INVALID_CALL;
        }
        *out = v;
        return S_OK;
    }
    STDMETHOD(CreateSubmixVoice)(IXAudio2SubmixVoice **out, UINT32 channels, UINT32 rate, UINT32, UINT32 stage, const XAUDIO2_VOICE_SENDS *sends,
        const XAUDIO2_EFFECT_CHAIN *chain) override
    {
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        *out = nullptr;
        if (!channels || channels > XAUDIO2_MAX_AUDIO_CHANNELS || (rate && rate != kRate))
            return XAUDIO2_E_INVALID_CALL;
        SubmixVoice *v = new SubmixVoice;
        v->kind = KIND_SUBMIX;
        v->inChannels = channels;
        v->inRate = kRate;
        v->stage = stage;
        Register(v, static_cast<IXAudio2SubmixVoice *>(v));
        SetEffects(v, chain);
        if (FAILED(SetSends(v, sends)))
        {
            DestroyCore(v);
            return XAUDIO2_E_INVALID_CALL;
        }
        *out = v;
        return S_OK;
    }
    STDMETHOD(CreateMasteringVoice)(IXAudio2MasteringVoice **out, UINT32 channels, UINT32 rate, UINT32, UINT32 device,
        const XAUDIO2_EFFECT_CHAIN *chain) override
    {
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        *out = nullptr;
        if (device != 0 || g_master || (rate && rate != kRate))
            return XAUDIO2_E_INVALID_CALL;
        MasterVoice *v = new MasterVoice;
        v->kind = KIND_MASTER;
        v->inChannels = channels ? channels : kOutChannels;
        v->inRate = kRate;
        Register(v, static_cast<IXAudio2MasteringVoice *>(v));
        SetEffects(v, chain);
        g_master = v;
        StartMixer();
        g_engineRunning = true;
        *out = v;
        return S_OK;
    }
    STDMETHOD(StartEngine)() override
    {
        g_engineRunning = true;
        return S_OK;
    }
    STDMETHOD_(void, StopEngine)() override { g_engineRunning = false; }
    STDMETHOD(CommitChanges)(UINT32) override { return S_OK; }
    STDMETHOD_(void, GetPerformanceData)(XAUDIO2_PERFORMANCE_DATA *p) override
    {
        std::lock_guard<std::recursive_mutex> guard(g_lock);
        memset(p, 0, sizeof(*p));
        for (Core *v : g_voices)
        {
            if (v->kind == KIND_SOURCE)
            {
                ++p->TotalSourceVoiceCount;
                p->ActiveSourceVoiceCount += v->running ? 1 : 0;
            }
            else if (v->kind == KIND_SUBMIX)
                ++p->ActiveSubmixVoiceCount;
        }
        p->CurrentLatencyInSamples = kLeadFrames;
    }
    STDMETHOD_(void, SetDebugConfiguration)(const XAUDIO2_DEBUG_CONFIGURATION *, void *) override {}
};
} // namespace

HRESULT Bo1_CreateXAudio2(REFCLSID clsid, void **ppv)
{
    if (!(clsid == IID_XAudio2) && !(clsid == IID_XAudio2_Debug))
        return E_NOINTERFACE;
    *ppv = static_cast<IXAudio2 *>(new Engine);
    return S_OK;
}

// ============================================================================================================
// CXAPOBase (xapobase.lib in the DirectX SDK): what the engine's effects inherit
// ============================================================================================================

CXAPOBase::CXAPOBase(const XAPO_REGISTRATION_PROPERTIES *pRegistrationProperties)
    : m_pRegistrationProperties(pRegistrationProperties), m_pfnMatrixMixFunction(nullptr), m_pfl32MatrixCoefficients(nullptr),
      m_nSrcFormatType(0), m_fIsScalarMatrix(FALSE), m_fIsLocked(FALSE), m_lReferenceCount(1)
{
}

CXAPOBase::~CXAPOBase() {}

HRESULT CXAPOBase::ValidateFormatDefault(WAVEFORMATEX *, BOOL) { return S_OK; }

HRESULT CXAPOBase::ValidateFormatPair(const WAVEFORMATEX *, WAVEFORMATEX *, BOOL) { return S_OK; }

void CXAPOBase::ProcessThru(void *pInputBuffer, FLOAT32 *pOutputBuffer, UINT32 FrameCount, WORD InputChannelCount, WORD OutputChannelCount,
    BOOL MixWithOutput)
{
    const FLOAT32 *in = (const FLOAT32 *)pInputBuffer;
    for (UINT32 f = 0; f < FrameCount; ++f)
        for (WORD c = 0; c < OutputChannelCount; ++c)
        {
            FLOAT32 v = c < InputChannelCount ? in[f * InputChannelCount + c] : 0.0f;
            FLOAT32 &o = pOutputBuffer[f * OutputChannelCount + c];
            o = MixWithOutput ? o + v : v;
        }
}

HRESULT CXAPOBase::GetRegistrationProperties(XAPO_REGISTRATION_PROPERTIES **pp)
{
    *pp = (XAPO_REGISTRATION_PROPERTIES *)CoTaskMemAlloc(sizeof(XAPO_REGISTRATION_PROPERTIES));
    memcpy(*pp, m_pRegistrationProperties, sizeof(XAPO_REGISTRATION_PROPERTIES));
    return S_OK;
}

HRESULT CXAPOBase::IsInputFormatSupported(const WAVEFORMATEX *, const WAVEFORMATEX *, WAVEFORMATEX **pp)
{
    if (pp)
        *pp = nullptr;
    return S_OK;
}

HRESULT CXAPOBase::IsOutputFormatSupported(const WAVEFORMATEX *, const WAVEFORMATEX *, WAVEFORMATEX **pp)
{
    if (pp)
        *pp = nullptr;
    return S_OK;
}

HRESULT CXAPOBase::LockForProcess(UINT32, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *, UINT32, const XAPO_LOCKFORPROCESS_BUFFER_PARAMETERS *)
{
    m_fIsLocked = TRUE;
    return S_OK;
}

void CXAPOBase::UnlockForProcess() { m_fIsLocked = FALSE; }
