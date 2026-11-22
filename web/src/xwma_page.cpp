// web/src/xwma_page.cpp - the page's xWMA decoder (build/web/xwma.js, web/CMakeLists.txt): the engine's (xwma.h), on
// its own, for the game's menu sounds, which the server sends as the zone keeps them (zombies-hub's server/sounds.go)
// and the page plays (zombies-hub's web/src/sounds.ts). Not part of the engine.
#include "xwma.h"

#include <emscripten/emscripten.h>
#include <string.h>

#include <algorithm>
#include <vector>

// Decodes a sound as the web client's XAudio2 plays it (xaudio2_web.cpp: DecodeWma): its packets in turn, then what the
// decoder still holds, to its length, `frames` (its table of decoded bytes' last, over 2 a channel), into `out`
// (16-bit, the channels interleaved), silence past what decoded. The frames, or 0 for what WMA 2 can't be.
extern "C" EMSCRIPTEN_KEEPALIVE int bo1_xwma_decode(int channels, int rate, int bytesPerSecond, int blockAlign,
    const uint8_t *data, int packets, int frames, int16_t *out)
{
    XwmaDecoder decoder;
    if (frames <= 0 || !decoder.Init(channels, rate, bytesPerSecond, blockAlign))
        return 0;
    std::vector<int16_t> pcm;
    size_t want = (size_t)frames * channels;
    pcm.reserve(want + 8192);
    for (int i = 0; i < packets && pcm.size() < want; ++i)
        decoder.Decode(data + (size_t)i * blockAlign, pcm);
    if (pcm.size() < want)
        decoder.Finish(pcm);
    pcm.resize(want);
    memcpy(out, pcm.data(), want * sizeof(int16_t));
    return frames;
}
