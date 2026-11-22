// xWMA for the WebAssembly build's XAudio2 (xaudio2_web.cpp): the WMA version 2 packets of the engine's loaded sounds, to
// 16-bit PCM. Ported from FFmpeg's decoder (n7.1: libavcodec/wmadec.c and wma.c, LGPL 2.1 or later), the parts xWMA
// uses. xWMA carries no codec data: the flags are those FFmpeg's xWMA demuxer gives (exponents in VLCs, a bit reservoir
// across packets, variable block lengths), and the output leaves out the decoder's first two frames, as XAudio2's does:
// a sound decodes to the length its table of decoded bytes ends at.
#pragma once

#include <stdint.h>
#include <vector>

class XwmaDecoder
{
  public:
    XwmaDecoder();
    ~XwmaDecoder();
    XwmaDecoder(const XwmaDecoder &) = delete;
    XwmaDecoder &operator=(const XwmaDecoder &) = delete;

    // false for what WMA 2 can't be (1 or 2 channels, up to 50 kHz)
    bool Init(uint32_t channels, uint32_t rate, uint32_t avgBytesPerSec, uint32_t blockAlign);
    // the next packet (blockAlign bytes), its samples (interleaved) added to `out`; false where it is broken: its samples
    // are left out, and the next packet starts afresh
    bool Decode(const uint8_t *packet, std::vector<int16_t> &out);
    // after the last packet: what the decoder still holds
    void Finish(std::vector<int16_t> &out);

  private:
    struct State;
    State *s;
};
