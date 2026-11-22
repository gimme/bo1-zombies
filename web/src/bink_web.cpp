// Bink for the WebAssembly build: the movies' decoder, on FFmpeg's Bink decoders (LGPL: libavcodec's binkvideo,
// binkaudio_dct and binkaudio_rdft, libavformat's bink demuxer), built for Emscripten in the web image with nothing
// else of FFmpeg (docker/Dockerfile: the web target, FFMPEG_DIR; web/CMakeLists.txt: BO1_WEB_BINK; without them
// web_stubs.cpp's stubs play nothing). On Windows the engine plays its movies with RAD's closed library (binkw32.dll).
//
// The engine's cinematics (src/gfx_d3d/r_cinematic.cpp) call the Bink API as that library offers it (src/binklib/bink.h);
// this is the part of it they use:
//  - BinkOpen of a file (main/video/<name>.bik, read through the install's file system: the browser's store), or of a
//    zone's rawfile in memory (BINKFROMMEMORY, whose size the Bink header says); BinkClose; BinkGetError (always "": the
//    engine asserts on an error, and what went wrong is logged).
//  - BinkGetFrameBuffersInfo, BinkRegisterFrameBuffers: the engine owns the planes (BINKNOFRAMEBUFFERS), two sets of Y,
//    Cr, Cb (and A when the movie has alpha), and names them again before each frame, then copies the set FrameNum
//    names into its textures (Unlock_Bink_textures2), which its cinematic material draws.
//  - BinkDoFrame decodes the frame to be displayed (the BINK's FrameNum, 1-based) into the other set, and the sound
//    that comes with it; BinkNextFrame moves on, and around past the last frame (the engine stops a movie that doesn't
//    loop before that).
//  - BinkWait and BinkShouldSkip: whether the frame's time is yet to come, or past, on the clock since the movie
//    started or was unpaused (BinkPause); the engine times its fullscreen movies by its own clock (playbackFlags 0x40)
//    and asks these only for the rest. BinkGetRealtime: the frame, the rate, and a read buffer always full (the engine
//    pauses a movie on an underrun; the store has the whole file).
//  - The sound: BinkSetSoundTrack names the tracks to play before an open, BinkSetVolume each one's volume (0..32768),
//    as the engine sets them (the first two at the movie's volume, the rest silent: R_Cinematic_InitBinkVolumes). The
//    tracks played are decoded as their frame is, summed, and queued with the sound stand-in (xaudio2_web.cpp:
//    bo1_cinematic_audio_*), which plays them after the game's sounds, as Bink's own device did on Windows, in step
//    with the frame the engine decodes to show (bo1_cinematic_audio_sync). The engine
//    gives Bink a DirectSound device there (BinkSetSoundSystem); here there is none to give, and the sound plays all the
//    same.
//  - BinkSetMemory, BinkSetIOSize, BinkControlBackgroundIO, BinkSetSoundSystem: taken and ignored (malloc; the file is
//    read as the frames need it, on the cinematic worker, which is the one thread that calls any of this).
//
// The maps' loading movies don't play through the engine's cinematics: the page shows them (below: the loading movie).
#ifdef BO1_WEB_BINK

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
}

#include <windows.h>
#include <binklib/bink.h>

#include <emscripten.h>
#include <emscripten/threading.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include "web_platform.h"

extern "C" {
void bo1_cinematic_audio_open(int rate);
void bo1_cinematic_audio_close();
void bo1_cinematic_audio_queue(const float *stereo, int frames);
void bo1_cinematic_audio_sync(double seconds);
void bo1_cinematic_audio_pause(int paused);
}

struct Bo1Movie;

namespace
{
// a sound track of the movie, as the demuxer has it: played when the engine named its id (BinkSetSoundTrack)
struct Track
{
    int stream;
    int id;
    AVCodecContext *ctx = nullptr;
    int channels = 1;
    int rate = 48000;
    bool played = false;
    float volume = 0.0f;
    std::vector<float> fifo; // decoded, stereo, waiting for the other tracks' share of the frame (QueueSound)
};

struct WebBink
{
    BINK bink; // first: the handle the engine reads (Width, Height, Frames, FrameNum, FrameRate, FrameRateDiv)

    // the file (read at a position of our own, as the install's streams are: crt_compat.cpp), or the memory
    int fd = -1;
    int64_t fileSize = 0, filePos = 0;
    const uint8_t *memory = nullptr;
    int64_t memorySize = 0, memoryPos = 0;

    AVIOContext *io = nullptr;
    AVFormatContext *fmt = nullptr;
    int videoStream = -1;
    AVCodecContext *video = nullptr;
    std::vector<Track> tracks;
    AVPacket *pkt = nullptr;
    AVFrame *frame = nullptr;      // the picture last decoded, kept: a frame asked for again is copied again
    AVFrame *decoded = nullptr;    // what the decoder hands out (avcodec_receive_frame clears it on each call)
    AVFrame *audioFrame = nullptr;
    uint32_t decodedFrame = 0;     // the frame `frame` holds (1-based); 0 none
    uint32_t nextPacketFrame = 1;  // the frame the demuxer's next video packet belongs to
    bool hasAlpha = false;
    bool soundOpen = false;
    int soundRate = 0;
    std::vector<float> mix;
    Bo1Movie *page = nullptr; // the page's loading movie: its sound goes there, not to the engine's (QueueSound)

    BINKFRAMEBUFFERS *buffers = nullptr; // the engine's (BinkRegisterFrameBuffers)
    int bufferIndex = 0;                 // the set the next frame goes into

    // the clock: frame startFrame was due at startTime (ms), each next one a frame's time later
    double startTime = 0.0;
    uint32_t startFrame = 1;
    bool paused = false;
    double pausedAt = 0.0;
};

// what the engine sets before an open, as Bink keeps them
U32 g_trackCount;
U32 g_trackIds[16];
bool g_logHooked;

void PageSound(Bo1Movie *m, const float *stereo, size_t frames); // the loading movie's sound (below)

double Now() { return emscripten_get_now(); }

double FrameTime(const WebBink *b, uint32_t frame)
{
    double rate = b->bink.FrameRate ? (double)b->bink.FrameRate / (double)(b->bink.FrameRateDiv ? b->bink.FrameRateDiv : 1) : 30.0;
    return b->startTime + ((double)frame - (double)b->startFrame) * 1000.0 / rate;
}

// FFmpeg's warnings and errors, in the engine's log
void LogCallback(void *, int level, const char *fmt, va_list args)
{
    if (level > AV_LOG_WARNING)
        return;
    char text[512];
    vsnprintf(text, sizeof(text), fmt, args);
    size_t n = strlen(text);
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r'))
        text[--n] = 0;
    if (n)
        Bo1_Log("bink: ffmpeg: %s", text);
}

const char *ErrorText(int err)
{
    static char text[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(err, text, sizeof(text));
    return text;
}

// ---- reading the file for the demuxer ----

int ReadPacket(void *opaque, uint8_t *buf, int size)
{
    WebBink *b = (WebBink *)opaque;
    if (b->memory)
    {
        int64_t left = b->memorySize - b->memoryPos;
        if (left <= 0)
            return AVERROR_EOF;
        int n = (int)std::min<int64_t>(size, left);
        memcpy(buf, b->memory + b->memoryPos, (size_t)n);
        b->memoryPos += n;
        return n;
    }
    if (b->filePos >= b->fileSize)
        return AVERROR_EOF;
    ssize_t n = pread(b->fd, buf, (size_t)std::min<int64_t>(size, b->fileSize - b->filePos), (off_t)b->filePos);
    if (n < 0)
    {
        Bo1_Log("bink: reading at %lld: %s", (long long)b->filePos, strerror(errno));
        return AVERROR(errno);
    }
    if (n == 0)
        return AVERROR_EOF;
    b->filePos += n;
    return (int)n;
}

int64_t SeekPacket(void *opaque, int64_t offset, int whence)
{
    WebBink *b = (WebBink *)opaque;
    int64_t size = b->memory ? b->memorySize : b->fileSize;
    int64_t *pos = b->memory ? &b->memoryPos : &b->filePos;
    if (whence == AVSEEK_SIZE)
        return size;
    whence &= ~AVSEEK_FORCE;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? *pos : size;
    int64_t to = base + offset;
    if (to < 0 || to > size)
        return AVERROR(EINVAL);
    *pos = to;
    return to;
}

// ---- opening ----

AVCodecContext *OpenDecoder(AVStream *st)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec)
    {
        Bo1_Log("bink: no decoder for codec %d (stream %d)", (int)st->codecpar->codec_id, st->index);
        return nullptr;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return nullptr;
    if (avcodec_parameters_to_context(ctx, st->codecpar) < 0)
    {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    ctx->pkt_timebase = st->time_base;
    int err = avcodec_open2(ctx, codec, nullptr);
    if (err < 0)
    {
        Bo1_Log("bink: opening the %s decoder: %s", codec->name, ErrorText(err));
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

void CloseAll(WebBink *b)
{
    if (b->soundOpen && !b->page)
        bo1_cinematic_audio_close();
    b->soundOpen = false;
    for (Track &t : b->tracks)
        if (t.ctx)
            avcodec_free_context(&t.ctx);
    b->tracks.clear();
    if (b->video)
        avcodec_free_context(&b->video);
    if (b->frame)
        av_frame_free(&b->frame);
    if (b->decoded)
        av_frame_free(&b->decoded);
    if (b->audioFrame)
        av_frame_free(&b->audioFrame);
    if (b->pkt)
        av_packet_free(&b->pkt);
    if (b->fmt)
        avformat_close_input(&b->fmt);
    if (b->io)
    {
        av_freep(&b->io->buffer);
        avio_context_free(&b->io);
    }
    if (b->fd >= 0)
        close(b->fd);
    b->fd = -1;
}

// opens the file at the path (b->fd, b->fileSize); false (and why logged) when it can't
bool OpenFile(WebBink *b, const char *name, const char *path)
{
    b->fd = open(path, O_RDONLY);
    struct stat st;
    if (b->fd < 0 || fstat(b->fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
    {
        Bo1_Log("bink: %s: can't open %s", name, path);
        if (b->fd >= 0)
            close(b->fd);
        b->fd = -1;
        return false;
    }
    b->fileSize = (int64_t)st.st_size;
    return true;
}

// opens the demuxer on the file or the memory, the decoders of the video and of the tracks named (the engine's, by
// BinkSetSoundTrack), and fills in the handle; false (and the reason logged) when it can't
bool Open(WebBink *b, const char *what, const U32 *trackIds, U32 trackCount)
{
    if (!g_logHooked)
    {
        g_logHooked = true;
        av_log_set_callback(LogCallback);
    }
    const size_t bufferSize = 256 << 10;
    unsigned char *buffer = (unsigned char *)av_malloc(bufferSize);
    b->io = avio_alloc_context(buffer, (int)bufferSize, 0, b, ReadPacket, nullptr, SeekPacket);
    if (!b->io)
    {
        av_free(buffer);
        return false;
    }
    b->fmt = avformat_alloc_context();
    if (!b->fmt)
        return false;
    b->fmt->pb = b->io;
    b->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    const AVInputFormat *in = av_find_input_format("bink");
    int err = avformat_open_input(&b->fmt, what, in, nullptr);
    if (err < 0)
    {
        Bo1_Log("bink: %s: not a Bink movie: %s", what, ErrorText(err));
        return false;
    }
    for (unsigned int i = 0; i < b->fmt->nb_streams; ++i)
    {
        AVStream *st = b->fmt->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && b->videoStream < 0)
        {
            b->video = OpenDecoder(st);
            if (!b->video)
                return false;
            b->videoStream = (int)i;
            b->bink.Width = (U32)st->codecpar->width;
            b->bink.Height = (U32)st->codecpar->height;
            b->bink.Frames = st->duration > 0 ? (U32)st->duration : st->nb_frames > 0 ? (U32)st->nb_frames : 1;
            // the time base is a frame: the rate's divisor over the rate
            b->bink.FrameRate = (U32)st->time_base.den;
            b->bink.FrameRateDiv = (U32)(st->time_base.num ? st->time_base.num : 1);
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            Track t;
            t.stream = (int)i;
            t.id = st->id;
            t.channels = st->codecpar->ch_layout.nb_channels;
            t.rate = st->codecpar->sample_rate;
            for (U32 k = 0; k < trackCount; ++k)
                if ((int)trackIds[k] == t.id)
                    t.played = true;
            if (t.played)
            {
                t.ctx = OpenDecoder(st);
                if (!t.ctx)
                    t.played = false;
            }
            b->tracks.push_back(t);
        }
    }
    if (b->videoStream < 0 || !b->bink.Width || !b->bink.Height || !b->bink.FrameRate)
    {
        Bo1_Log("bink: %s: no video in it", what);
        return false;
    }
    b->pkt = av_packet_alloc();
    b->frame = av_frame_alloc();
    b->decoded = av_frame_alloc();
    b->audioFrame = av_frame_alloc();
    if (!b->pkt || !b->frame || !b->decoded || !b->audioFrame)
        return false;
    b->bink.FrameNum = 1;
    b->bink.LastFrameNum = 0;
    b->bink.NumTracks = (S32)b->tracks.size();
    b->hasAlpha = b->video->pix_fmt == AV_PIX_FMT_YUVA420P;
    for (Track &t : b->tracks)
    {
        if (t.played && !b->soundOpen)
        {
            if (!b->page)
                bo1_cinematic_audio_open(t.rate);
            b->soundOpen = true;
            b->soundRate = t.rate;
        }
    }
    b->startTime = Now();
    b->startFrame = 1;
    int played = 0;
    for (const Track &t : b->tracks)
        played += t.played;
    Bo1_Log("bink: %s: %ux%u, %u frames at %u/%u fps, %d sound tracks (%d played)%s", what, b->bink.Width, b->bink.Height,
            b->bink.Frames, b->bink.FrameRate, b->bink.FrameRateDiv, (int)b->tracks.size(), played, b->hasAlpha ? ", alpha" : "");
    return true;
}

// ---- decoding ----

// a track's decoded samples, stereo, into its fifo
void DecodeAudio(WebBink *b, Track &t, AVPacket *pkt)
{
    int err = avcodec_send_packet(t.ctx, pkt);
    if (err < 0)
        return;
    while (avcodec_receive_frame(t.ctx, b->audioFrame) == 0)
    {
        AVFrame *f = b->audioFrame;
        int n = f->nb_samples;
        int ch = f->ch_layout.nb_channels;
        size_t at = t.fifo.size();
        t.fifo.resize(at + (size_t)n * 2);
        float *out = &t.fifo[at];
        switch (f->format)
        {
        case AV_SAMPLE_FMT_FLTP:
        {
            const float *l = (const float *)f->data[0], *r = ch > 1 ? (const float *)f->data[1] : l;
            for (int i = 0; i < n; ++i)
            {
                out[i * 2] = l[i];
                out[i * 2 + 1] = r[i];
            }
            break;
        }
        case AV_SAMPLE_FMT_FLT:
        {
            const float *s = (const float *)f->data[0];
            for (int i = 0; i < n; ++i)
            {
                out[i * 2] = s[i * ch];
                out[i * 2 + 1] = s[i * ch + (ch > 1 ? 1 : 0)];
            }
            break;
        }
        case AV_SAMPLE_FMT_S16P:
        {
            const int16_t *l = (const int16_t *)f->data[0], *r = ch > 1 ? (const int16_t *)f->data[1] : l;
            for (int i = 0; i < n; ++i)
            {
                out[i * 2] = l[i] / 32768.0f;
                out[i * 2 + 1] = r[i] / 32768.0f;
            }
            break;
        }
        case AV_SAMPLE_FMT_S16:
        {
            const int16_t *s = (const int16_t *)f->data[0];
            for (int i = 0; i < n; ++i)
            {
                out[i * 2] = s[i * ch] / 32768.0f;
                out[i * 2 + 1] = s[i * ch + (ch > 1 ? 1 : 0)] / 32768.0f;
            }
            break;
        }
        default:
            memset(out, 0, sizeof(float) * (size_t)n * 2);
            break;
        }
        av_frame_unref(f);
    }
}

// the tracks' sound as far as every one played has it, summed at their volumes, to the queue
void QueueSound(WebBink *b)
{
    if (!b->soundOpen)
        return;
    size_t frames = SIZE_MAX;
    for (const Track &t : b->tracks)
        if (t.played)
            frames = std::min<size_t>(frames, t.fifo.size() / 2); // (min, bare, is a Windows macro here)
    if (frames == SIZE_MAX || frames == 0)
        return;
    b->mix.assign(frames * 2, 0.0f);
    for (Track &t : b->tracks)
    {
        if (!t.played)
            continue;
        if (t.volume > 0.0f)
            for (size_t i = 0; i < frames * 2; ++i)
                b->mix[i] += t.fifo[i] * t.volume;
        t.fifo.erase(t.fifo.begin(), t.fifo.begin() + (ptrdiff_t)(frames * 2));
    }
    if (b->page)
        PageSound(b->page, b->mix.data(), frames);
    else
        bo1_cinematic_audio_queue(b->mix.data(), (int)frames);
}

void Rewind(WebBink *b)
{
    int err = av_seek_frame(b->fmt, b->videoStream, 0, AVSEEK_FLAG_BACKWARD);
    if (err < 0)
        Bo1_Log("bink: seeking to the start: %s", ErrorText(err));
    avcodec_flush_buffers(b->video);
    for (Track &t : b->tracks)
    {
        if (t.ctx)
            avcodec_flush_buffers(t.ctx);
        t.fifo.clear();
    }
    // the sound starts again with the first picture
    if (b->soundOpen && !b->page)
        bo1_cinematic_audio_open(b->soundRate);
    b->decodedFrame = 0;
    b->nextPacketFrame = 1;
}

// decodes up to the video frame wanted (1-based), the sound of each frame on the way: false at the end, or on an error
bool DecodeTo(WebBink *b, uint32_t want)
{
    if (b->decodedFrame > want)
        Rewind(b);
    while (b->decodedFrame < want)
    {
        int err = av_read_frame(b->fmt, b->pkt);
        if (err < 0)
        {
            // the end, short of the frame asked for: a word when it comes before the movie's last frame
            if (err != AVERROR_EOF || want <= b->bink.Frames)
                Bo1_Log("bink: reading frame %u of %u: %s", b->nextPacketFrame, b->bink.Frames, ErrorText(err));
            return false;
        }
        if (b->pkt->stream_index == b->videoStream)
        {
            err = avcodec_send_packet(b->video, b->pkt);
            av_packet_unref(b->pkt);
            if (err < 0)
            {
                Bo1_Log("bink: decoding frame %u: %s", b->nextPacketFrame, ErrorText(err));
                return false;
            }
            // a frame a packet; the call that finds none left clears what it is given, so the picture is kept apart
            while (avcodec_receive_frame(b->video, b->decoded) == 0)
            {
                av_frame_unref(b->frame);
                av_frame_move_ref(b->frame, b->decoded);
                b->decodedFrame = b->nextPacketFrame;
            }
            ++b->nextPacketFrame;
            QueueSound(b);
        }
        else
        {
            for (Track &t : b->tracks)
                if (t.played && t.stream == b->pkt->stream_index)
                    DecodeAudio(b, t, b->pkt);
            av_packet_unref(b->pkt);
        }
    }
    return true;
}

void CopyPlane(const BINKPLANE &dst, const uint8_t *src, int srcPitch, uint32_t width, uint32_t height)
{
    if (!dst.Buffer || !src)
        return;
    uint8_t *out = (uint8_t *)dst.Buffer;
    for (uint32_t y = 0; y < height; ++y)
        memcpy(out + (size_t)y * dst.BufferPitch, src + (ptrdiff_t)y * srcPitch, width);
}

// the picture decoded, into the set of planes the next frame goes to
void CopyPlanes(WebBink *b)
{
    BINKFRAMEBUFFERS *fb = b->buffers;
    BINKFRAMEPLANESET &set = fb->Frames[b->bufferIndex];
    AVFrame *f = b->frame;
    uint32_t w = std::min<uint32_t>(fb->YABufferWidth, (uint32_t)f->width);
    uint32_t h = std::min<uint32_t>(fb->YABufferHeight, (uint32_t)f->height);
    uint32_t cw = std::min<uint32_t>(fb->cRcBBufferWidth, (uint32_t)(f->width + 1) / 2);
    uint32_t chh = std::min<uint32_t>(fb->cRcBBufferHeight, (uint32_t)(f->height + 1) / 2);
    CopyPlane(set.YPlane, f->data[0], f->linesize[0], w, h);
    CopyPlane(set.cRPlane, f->data[2], f->linesize[2], cw, chh); // Cr is V
    CopyPlane(set.cBPlane, f->data[1], f->linesize[1], cw, chh); // Cb is U
    if (set.APlane.Allocate && f->data[3])
        CopyPlane(set.APlane, f->data[3], f->linesize[3], w, h);
    fb->FrameNum = (U32)b->bufferIndex;
}
} // namespace

// ---- the API ----

extern "C" {

RADEXPFUNC HBINK RADEXPLINK BinkOpen(const char *name, U32 flags)
{
    WebBink *b = new WebBink();
    memset(&b->bink, 0, sizeof(b->bink));
    b->bink.OpenFlags = flags;
    std::string what;
    if (flags & BINKFROMMEMORY)
    {
        // the Bink header: its tag (BIKb, BIKf, BIKg, BIKh, BIKi), then the file's size less 8
        const uint8_t *data = (const uint8_t *)name;
        if (!data || memcmp(data, "BIK", 3))
        {
            Bo1_Log("bink: the memory handed in is not a Bink movie");
            delete b;
            return nullptr;
        }
        b->memory = data;
        b->memorySize = (int64_t)(data[4] | data[5] << 8 | data[6] << 16 | (uint32_t)data[7] << 24) + 8;
        what = "a movie in memory";
    }
    else
    {
        if (!OpenFile(b, name, Bo1_ResolvePath(name, false).c_str()))
        {
            delete b;
            return nullptr;
        }
        what = name;
    }
    if (!Open(b, what.c_str(), g_trackIds, g_trackCount))
    {
        CloseAll(b);
        delete b;
        return nullptr;
    }
    return &b->bink;
}

RADEXPFUNC void RADEXPLINK BinkClose(HBINK h)
{
    if (!h)
        return;
    WebBink *b = (WebBink *)h;
    CloseAll(b);
    delete b;
}

RADEXPFUNC char *RADEXPLINK BinkGetError(void) { return (char *)""; }

RADEXPFUNC void RADEXPLINK BinkGetFrameBuffersInfo(HBINK h, BINKFRAMEBUFFERS *fbset)
{
    memset(fbset, 0, sizeof(*fbset));
    if (!h)
        return;
    WebBink *b = (WebBink *)h;
    fbset->TotalFrames = BINKMAXFRAMEBUFFERS;
    fbset->YABufferWidth = (b->bink.Width + 1) & ~1u;
    fbset->YABufferHeight = (b->bink.Height + 1) & ~1u;
    fbset->cRcBBufferWidth = fbset->YABufferWidth / 2;
    fbset->cRcBBufferHeight = fbset->YABufferHeight / 2;
    fbset->FrameNum = 0;
    for (int i = 0; i < BINKMAXFRAMEBUFFERS; ++i)
    {
        fbset->Frames[i].YPlane.Allocate = 1;
        fbset->Frames[i].cRPlane.Allocate = 1;
        fbset->Frames[i].cBPlane.Allocate = 1;
        fbset->Frames[i].APlane.Allocate = b->hasAlpha;
    }
}

RADEXPFUNC void RADEXPLINK BinkRegisterFrameBuffers(HBINK h, BINKFRAMEBUFFERS *fbset)
{
    if (!h)
        return;
    WebBink *b = (WebBink *)h;
    b->buffers = fbset;
    b->bink.FrameBuffers = fbset;
}

RADEXPFUNC S32 RADEXPLINK BinkDoFrame(HBINK h)
{
    if (!h)
        return 0;
    WebBink *b = (WebBink *)h;
    if (!b->buffers)
    {
        Bo1_Log("bink: a frame asked for with no frame buffers registered");
        return 0;
    }
    uint32_t want = b->bink.FrameNum;
    if (b->decodedFrame != want && !DecodeTo(b, want))
    {
        b->bink.ReadError = 1;
        return 1;
    }
    CopyPlanes(b);
    b->bink.LastFrameNum = want;
    // the engine shows the frame next: its sound goes with it (xaudio2_web.cpp)
    if (b->soundOpen)
        bo1_cinematic_audio_sync((double)(want - 1) * (b->bink.FrameRateDiv ? b->bink.FrameRateDiv : 1) / b->bink.FrameRate);
    return 0;
}

RADEXPFUNC void RADEXPLINK BinkNextFrame(HBINK h)
{
    if (!h)
        return;
    WebBink *b = (WebBink *)h;
    b->bufferIndex ^= 1;
    if (b->bink.FrameNum >= b->bink.Frames)
    {
        b->bink.FrameNum = 1;
        Rewind(b);
        b->startTime = Now();
        b->startFrame = 1;
    }
    else
    {
        ++b->bink.FrameNum;
    }
}

// whether the frame to be displayed is still to come
RADEXPFUNC S32 RADEXPLINK BinkWait(HBINK h)
{
    if (!h)
        return 0;
    WebBink *b = (WebBink *)h;
    if (b->paused)
        return 1;
    return Now() < FrameTime(b, b->bink.FrameNum) ? 1 : 0;
}

// whether the frame to be displayed is already late by a frame: the engine then decodes on to catch up
RADEXPFUNC S32 RADEXPLINK BinkShouldSkip(HBINK h)
{
    if (!h)
        return 0;
    WebBink *b = (WebBink *)h;
    if (b->paused)
        return 0;
    return Now() >= FrameTime(b, b->bink.FrameNum + 1) ? 1 : 0;
}

RADEXPFUNC S32 RADEXPLINK BinkPause(HBINK h, S32 pause)
{
    if (!h)
        return 0;
    WebBink *b = (WebBink *)h;
    S32 was = b->paused;
    if (pause && !b->paused)
    {
        b->paused = true;
        b->pausedAt = Now();
    }
    else if (!pause && b->paused)
    {
        b->paused = false;
        b->startTime += Now() - b->pausedAt;
    }
    b->bink.Paused = b->paused;
    if (b->soundOpen)
        bo1_cinematic_audio_pause(b->paused);
    return was;
}

RADEXPFUNC void RADEXPLINK BinkGetRealtime(HBINK h, BINKREALTIME *run, U32)
{
    memset(run, 0, sizeof(*run));
    if (!h)
        return;
    WebBink *b = (WebBink *)h;
    run->FrameNum = b->bink.FrameNum;
    run->FrameRate = b->bink.FrameRate;
    run->FrameRateDiv = b->bink.FrameRateDiv;
    run->Frames = 1;
    run->ReadBufferSize = 1;
    run->ReadBufferUsed = 1; // full: the store has the whole file
}

RADEXPFUNC void RADEXPLINK BinkSetSoundTrack(U32 total_tracks, U32 *tracks)
{
    g_trackCount = std::min<U32>(total_tracks, 16);
    for (U32 i = 0; i < g_trackCount; ++i)
        g_trackIds[i] = tracks[i];
}

// the volume of the track at that place in the list BinkSetSoundTrack gave: 0 to 32768
RADEXPFUNC void RADEXPLINK BinkSetVolume(HBINK h, U32 trackindex, S32 volume)
{
    if (!h || trackindex >= g_trackCount)
        return;
    WebBink *b = (WebBink *)h;
    float v = volume <= 0 ? 0.0f : volume >= 32768 ? 1.0f : (float)volume / 32768.0f;
    for (Track &t : b->tracks)
        if (t.id == (int)g_trackIds[trackindex])
            t.volume = v;
}

RADEXPFUNC S32 RADEXPLINK BinkControlBackgroundIO(HBINK, U32) { return 0; }
RADEXPFUNC BINKSNDOPEN RADEXPLINK BinkOpenDirectSound(UINTa) { return 0; }
RADEXPFUNC void RADEXPLINK BinkSetIOSize(U32) {}
RADEXPFUNC void RADEXPLINK BinkSetMemory(BINKMEMALLOC, BINKMEMFREE) {}
RADEXPFUNC S32 RADEXPLINK BinkSetSoundSystem(BINKSNDSYSOPEN, UINTa) { return 0; }

} // extern "C"

// ---- the loading movie, which the page shows ----
// A map's loading movie plays from the engine's start, the player's click on Play, rather than from the client's load
// of the map, ten seconds and more on, as the engine's cinematics would play it: the page gives the map
// (web_main.cpp), and the client plays on with it once it loads the map (src/client/cl_cin.cpp, through
// Sys_LoadingMovie*). Nor does anything the engine does as it starts and loads hold it up: a thread of its own decodes
// it ahead into a ring of pictures in the shared memory, and its sound into a ring of its own, and the page shows the
// pictures on a canvas of its own under the game's, on a worker's animation frames, and plays the sound with an
// AudioWorklet (library_bo1.js: bo1_movie_show). The engine's loading screen draws nothing where it shows, only the
// menus over it, and the game's canvas lets it through there (d3d9_webgpu.cpp: Present). One a page: an engine plays
// one game, and the page reloads after it, so what the movie took is the page's for good.

// The page reads it as int32s, in this order (library_bo1.js: bo1_movie_show's fields); the addresses are the heap's.
struct Bo1Movie
{
    // set before the page is told of it
    int32_t width, height;    // the picture
    int32_t frames;           // how many,
    int32_t rate, rateDiv;    // rate / rateDiv a second
    int32_t slots;            // the pictures the ring holds,
    int32_t slotBytes;        // each its Y (width x height), then its U and V (half as wide and high, rounded up),
    int32_t pictures;         // from this address
    int32_t soundRate;        // the sound's frames a second (0: none),
    int32_t soundFrames;      // the stereo frames its ring holds (floats),
    int32_t sound;            // from this address
    // the thread
    volatile int32_t decoded; // the latest picture in the ring (1-based; 0 none yet)
    volatile int32_t soundIn; // the sound's frames decoded so far: the ring holds the last soundFrames of them
    volatile int32_t ended;   // it decoded its last picture, or stopped
    // the page
    volatile int32_t shown;   // the picture on the canvas (0 none yet)
    volatile int32_t due;     // the picture its clock is at: the thread puts none before it in the ring
    volatile int32_t showing; // the canvas shows the movie
    volatile int32_t over;    // its last picture's time has passed, or the engine stopped it
    volatile int32_t soundAt; // the worklet's place in the sound
    volatile int32_t soundTicks; // its process() calls: it plays while this moves
    volatile int32_t soundSeek;  // where the page's clock is, for the worklet to go to (-1: nowhere)
    // the engine
    volatile int32_t stop;    // it is done with the movie (Bo1_MovieStop)
};

extern "C" void bo1_movie_show(Bo1Movie *movie); // library_bo1.js, on the page's thread

namespace
{
// A quarter of a second of pictures at 30 a second; the sound comes ahead of them (the first picture has the first 0.76 s
// of it), and the worklet plays the ring's.
const int32_t kMovieSlots = 8;
const int32_t kMovieSoundSeconds = 4;

struct PageMovie
{
    char name[80];
    float volume;              // the player's (Bo1_MovieStart)
    Bo1Movie *m;               // the page's view of it, zero but for stop and soundSeek until it opens
    std::atomic<bool> failed;  // it could not be opened, nor the engine's default
};
std::atomic<PageMovie *> g_pageMovie;

void PageSound(Bo1Movie *m, const float *stereo, size_t frames)
{
    float *ring = (float *)(uintptr_t)(uint32_t)m->sound;
    uint32_t in = (uint32_t)m->soundIn;
    for (size_t i = 0; i < frames; ++i)
    {
        size_t at = (size_t)((in + (uint32_t)i) % (uint32_t)m->soundFrames) * 2;
        ring[at] = stereo[i * 2];
        ring[at + 1] = stereo[i * 2 + 1];
    }
    __atomic_store_n(&m->soundIn, (int32_t)(in + (uint32_t)frames), __ATOMIC_RELEASE);
}

// the picture decoded, into its slot: Y, U, V
void CopyPicture(const AVFrame *f, uint8_t *to, uint32_t w, uint32_t h)
{
    uint32_t cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (uint32_t y = 0; y < h; ++y)
        memcpy(to + (size_t)y * w, f->data[0] + (ptrdiff_t)y * f->linesize[0], w);
    to += (size_t)w * h;
    for (int plane = 1; plane <= 2; ++plane, to += (size_t)cw * ch)
        for (uint32_t y = 0; y < ch; ++y)
            memcpy(to + (size_t)y * cw, f->data[plane] + (ptrdiff_t)y * f->linesize[plane], cw);
}

// main/video/<name>.bik open to decode for the page, its sound to the movie's ring; null when it can't be
WebBink *OpenPageMovie(const char *name, Bo1Movie *m, float volume)
{
    // the tracks the engine names, the first two at the loading movie's volume, the rest silent (r_cinematic.cpp:
    // R_Cinematic_StartPlayback_Now, R_Cinematic_InitBinkVolumes: 0.707 of its 1, times the player's volume)
    static const U32 tracks[] = {0, 1, 2, 3, 4};
    std::string path = std::string(BO1_WEB_GAME_ROOT "/main/video/") + name + ".bik";
    WebBink *b = new WebBink();
    memset(&b->bink, 0, sizeof(b->bink));
    b->page = m;
    if (!OpenFile(b, name, path.c_str()) || !Open(b, path.c_str(), tracks, 5))
    {
        CloseAll(b);
        delete b;
        return nullptr;
    }
    for (Track &t : b->tracks)
        t.volume = t.id == 0 || t.id == 1 ? 0.707f * volume : 0.0f;
    return b;
}

// The movie's thread: opens it (else the engine's default movie, as its cinematics do), tells the page, and decodes it
// ahead of the page's clock, to its end or until the engine stops it.
void *PageMovieThread(void *arg)
{
    PageMovie *pm = (PageMovie *)arg;
    Bo1Movie *m = pm->m;
    WebBink *b = OpenPageMovie(pm->name, m, pm->volume);
    if (!b)
        b = OpenPageMovie("default", m, pm->volume);
    if (!b)
    {
        pm->failed = true;
        __atomic_store_n(&m->ended, 1, __ATOMIC_RELEASE);
        return nullptr;
    }
    uint32_t w = b->bink.Width, h = b->bink.Height, cw = (w + 1) / 2, ch = (h + 1) / 2;
    m->width = (int32_t)w;
    m->height = (int32_t)h;
    m->frames = (int32_t)b->bink.Frames;
    m->rate = (int32_t)b->bink.FrameRate;
    m->rateDiv = (int32_t)(b->bink.FrameRateDiv ? b->bink.FrameRateDiv : 1);
    m->slots = kMovieSlots;
    m->slotBytes = (int32_t)(((size_t)w * h + (size_t)cw * ch * 2 + 15) & ~(size_t)15);
    m->pictures = (int32_t)(uintptr_t)aligned_alloc(16, (size_t)m->slots * (size_t)m->slotBytes);
    if (b->soundOpen)
    {
        m->soundRate = b->soundRate;
        m->soundFrames = b->soundRate * kMovieSoundSeconds;
        m->sound = (int32_t)(uintptr_t)calloc((size_t)m->soundFrames * 2, sizeof(float));
    }
    Bo1_Log("bink: the loading movie %s plays on the page, at %.3f of its volume", pm->name, pm->volume);
    if (!__atomic_load_n(&m->stop, __ATOMIC_ACQUIRE))
        bo1_movie_show(m);
    for (uint32_t n = 1; n <= b->bink.Frames && !__atomic_load_n(&m->stop, __ATOMIC_ACQUIRE); ++n)
    {
        if (!DecodeTo(b, n))
            break;
        // a picture the page's clock is past is left out (it shows the next one on time instead), the last aside
        if ((int32_t)n < __atomic_load_n(&m->due, __ATOMIC_ACQUIRE) && n < b->bink.Frames)
            continue;
        // no further ahead of the page than the ring holds: the slot taken is that of a picture it has shown
        for (;;)
        {
            int32_t shown = __atomic_load_n(&m->shown, __ATOMIC_ACQUIRE);
            if ((int32_t)n - shown < m->slots || __atomic_load_n(&m->stop, __ATOMIC_ACQUIRE))
                break;
            emscripten_futex_wait((void *)&m->shown, (uint32_t)shown, 100.0);
        }
        if (__atomic_load_n(&m->stop, __ATOMIC_ACQUIRE))
            break;
        uint8_t *slot = (uint8_t *)(uintptr_t)(uint32_t)m->pictures + (size_t)(n % (uint32_t)m->slots) * (size_t)m->slotBytes;
        CopyPicture(b->frame, slot, w, h);
        __atomic_store_n(&m->decoded, (int32_t)n, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&m->ended, 1, __ATOMIC_RELEASE);
    CloseAll(b);
    delete b;
    return nullptr;
}
} // namespace

// The page plays main/video/<name>.bik, the first time it's asked to, its sound at volume times the movie's own; true
// when it plays it, or plays on with it (it may be opening still), at the volume it started at. Asked for another, it
// stops the one it plays, and the engine plays that one itself.
bool Bo1_MovieStart(const char *name, float volume)
{
    PageMovie *pm = g_pageMovie.load();
    if (pm)
    {
        if (!strcmp(pm->name, name))
            return !pm->failed && !__atomic_load_n(&pm->m->stop, __ATOMIC_ACQUIRE);
        Bo1_MovieStop();
        return false;
    }
    pm = new PageMovie();
    I_strncpyz_web(pm->name, name, sizeof(pm->name));
    pm->volume = volume < 0.0f ? 0.0f : volume > 1.0f ? 1.0f : volume;
    pm->m = (Bo1Movie *)calloc(1, sizeof(Bo1Movie));
    pm->m->soundSeek = -1;
    PageMovie *none = nullptr;
    if (!g_pageMovie.compare_exchange_strong(none, pm))
    {
        free(pm->m);
        delete pm;
        return Bo1_MovieStart(name, volume);
    }
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int err = pthread_create(&thread, &attr, PageMovieThread, pm);
    pthread_attr_destroy(&attr);
    if (err)
    {
        Bo1_Log("bink: no thread for the loading movie (%d)", err);
        pm->failed = true;
        return false;
    }
    return true;
}

int Bo1_MovieState()
{
    PageMovie *pm = g_pageMovie.load();
    if (!pm)
        return BO1_MOVIE_NONE;
    Bo1Movie *m = pm->m;
    if (pm->failed || __atomic_load_n(&m->over, __ATOMIC_ACQUIRE) || __atomic_load_n(&m->stop, __ATOMIC_ACQUIRE))
        return BO1_MOVIE_OVER;
    return __atomic_load_n(&m->showing, __ATOMIC_ACQUIRE) ? BO1_MOVIE_SHOWS : BO1_MOVIE_PENDING;
}

void Bo1_MovieStop()
{
    PageMovie *pm = g_pageMovie.load();
    if (!pm || __atomic_exchange_n(&pm->m->stop, 1, __ATOMIC_ACQ_REL))
        return;
    emscripten_futex_wake((void *)&pm->m->shown, 0x7FFFFFFF);
}

bool Bo1_MovieUnder()
{
    PageMovie *pm = g_pageMovie.load();
    return pm && __atomic_load_n(&pm->m->showing, __ATOMIC_ACQUIRE) && !__atomic_load_n(&pm->m->stop, __ATOMIC_ACQUIRE);
}

#endif // BO1_WEB_BINK
