// xWMA decoding (xwma.h): FFmpeg's WMA version 2 decoder, with what xWMA never uses left out (WMA 1, exponents coded
// as LSPs, fixed block lengths, packets without the bit reservoir). Names follow FFmpeg's, so the two read side by side.
#include "xwma.h"

#include <math.h>
#include <string.h>

#include <algorithm>

#include "xwma_tables.h"

namespace
{
using namespace xwma_tables;

const int BLOCK_MIN_BITS = 7;
const int BLOCK_MAX_BITS = 11;
const int BLOCK_MAX_SIZE = 1 << BLOCK_MAX_BITS;
const int BLOCK_NB_SIZES = BLOCK_MAX_BITS - BLOCK_MIN_BITS + 1;
const int HIGH_BAND_MAX_SIZE = 16;
const int MAX_CODED_SUPERFRAME_SIZE = 32768;
const int MAX_CHANNELS = 2;
const int NOISE_TAB_SIZE = 8192;
const int VLCBITS = 9;
const int EXPVLCBITS = 8;
const int HGAINVLCBITS = 9;
const uint32_t FLAGS2 = 31; // the codec data FFmpeg's xWMA demuxer makes up: exponent VLCs, bit reservoir, variable blocks

// MSB first, as FFmpeg's get_bits; past the end it reads zeros
struct Bits
{
    const uint8_t *buf = nullptr;
    int size = 0; // bits
    int pos = 0;

    void Init(const uint8_t *data, int bits)
    {
        buf = data;
        size = bits;
        pos = 0;
    }
    uint32_t Peek(int n) const // n <= 25
    {
        if (n <= 0)
            return 0;
        int byte = pos >> 3, bytes = (size + 7) >> 3;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v = (v << 8) | (byte + i < bytes ? buf[byte + i] : 0);
        return (v << (pos & 7)) >> (32 - n);
    }
    void Skip(int n) { pos += n; }
    uint32_t Get(int n)
    {
        uint32_t v = Peek(n);
        pos += n;
        return v;
    }
    int Get1() { return (int)Get(1); }
    int Left() const { return size - pos; }
};

// a prefix code: a table read `bits` at a time, longer codes in subtables
struct Vlc
{
    struct Entry
    {
        int32_t sym; // the symbol, or where the subtable starts
        int32_t len; // > 0: the code's length (in this table); < 0: a subtable of -len bits; 0: no code
    };
    struct Code
    {
        uint32_t code;
        int len;
        int sym;
    };
    std::vector<Entry> table;
    int bits = 0;

    void Build(int rootBits, const std::vector<Code> &codes)
    {
        bits = rootBits;
        table.clear();
        Add(rootBits, codes);
    }
    int Add(int nbits, const std::vector<Code> &codes)
    {
        int base = (int)table.size();
        table.resize(base + (1 << nbits), Entry{-1, 0});
        std::vector<std::vector<Code>> longer(1 << nbits);
        for (const Code &c : codes)
        {
            if (c.len <= nbits)
            {
                uint32_t first = c.code << (nbits - c.len), count = 1u << (nbits - c.len);
                for (uint32_t i = 0; i < count; ++i)
                    table[base + first + i] = Entry{c.sym, c.len};
            }
            else
                longer[c.code >> (c.len - nbits)].push_back(Code{c.code & ((1u << (c.len - nbits)) - 1), c.len - nbits, c.sym});
        }
        for (int prefix = 0; prefix < (1 << nbits); ++prefix)
        {
            if (longer[prefix].empty())
                continue;
            int maxLen = 0;
            for (const Code &c : longer[prefix])
                maxLen = (std::max)(maxLen, c.len);
            int subBits = (std::min)(maxLen, nbits);
            int at = Add(subBits, longer[prefix]);
            table[base + prefix] = Entry{at, -subBits};
        }
        return base;
    }
    // the symbol; -1 where no code matches, as FFmpeg's get_vlc2
    int Read(Bits &gb) const
    {
        int base = 0, n = bits;
        for (;;)
        {
            const Entry &e = table[base + gb.Peek(n)];
            if (e.len > 0)
            {
                gb.Skip(e.len);
                return e.sym;
            }
            if (e.len == 0)
                return -1;
            gb.Skip(n);
            base = e.sym;
            n = -e.len;
        }
    }
};

struct CoefCode
{
    Vlc vlc;
    std::vector<uint16_t> run_table;
    std::vector<float> level_table;
};

// init_coef_vlc
void InitCoefVlc(CoefCode &c, const CoefVlcTable &t)
{
    std::vector<Vlc::Code> codes(t.n);
    for (int i = 0; i < t.n; ++i)
        codes[i] = Vlc::Code{t.huffcodes[i], t.huffbits[i], i};
    c.vlc.Build(VLCBITS, codes);
    c.run_table.assign(t.n, 0);
    c.level_table.assign(t.n, 0.0f);
    int i = 2, level = 1, k = 0;
    while (i < t.n)
    {
        int l = t.levels[k++];
        for (int j = 0; j < l && i < t.n; j++)
        {
            c.run_table[i] = (uint16_t)j;
            c.level_table[i] = (float)level;
            i++;
        }
        level++;
    }
}

// what every decoder shares: the codes, the sine windows, pow(10, i / 16) for i in -60..95
struct Shared
{
    Vlc exp_vlc, hgain_vlc;
    CoefCode coefs[6];
    std::vector<float> windows[BLOCK_MAX_BITS + 1]; // by log2 of their length
    float pow_tab[156];

    Shared()
    {
        std::vector<Vlc::Code> codes;
        for (int i = 0; i < 121; ++i)
            codes.push_back(Vlc::Code{ff_aac_scalefactor_code[i], ff_aac_scalefactor_bits[i], i});
        exp_vlc.Build(EXPVLCBITS, codes);
        // ff_vlc_init_from_lengths: the codes follow from the lengths, in the table's order; the symbols are offset by -18
        codes.clear();
        uint64_t code = 0;
        for (int i = 0; i < 37; ++i)
        {
            int len = ff_wma_hgain_hufftab[i][1];
            codes.push_back(Vlc::Code{(uint32_t)(code >> (32 - len)), len, ff_wma_hgain_hufftab[i][0] - 18});
            code += 1ull << (32 - len);
        }
        hgain_vlc.Build(HGAINVLCBITS, codes);
        for (int i = 0; i < 6; ++i)
            InitCoefVlc(coefs[i], coef_vlcs[i]);
        for (int b = BLOCK_MIN_BITS; b <= BLOCK_MAX_BITS; ++b)
        {
            int n = 1 << b;
            windows[b].resize(n);
            for (int i = 0; i < n; i++)
                windows[b][i] = sinf((float)((i + 0.5) * (M_PI / (2.0 * n))));
        }
        for (int i = 0; i < 156; ++i)
            pow_tab[i] = (float)pow(10.0, (i - 60) / 16.0);
    }
};

const Shared &GetShared()
{
    static Shared shared;
    return shared;
}

// the inverse MDCT of FFmpeg's WMA decoder before libavutil/tx (ff_imdct_calc): n / 2 coefficients to n samples, through a
// complex FFT of n / 4
struct Imdct
{
    int n = 0;
    std::vector<float> tcos, tsin;
    std::vector<int> revtab;
    std::vector<float> fft_cos, fft_sin; // e^(2 pi i k / (n / 4)), k < n / 8
    std::vector<float> zr, zi;

    void Init(int nbits, double scale)
    {
        n = 1 << nbits;
        int n4 = n >> 2;
        tcos.resize(n4);
        tsin.resize(n4);
        double theta = 1.0 / 8.0;
        scale = sqrt(fabs(scale));
        for (int i = 0; i < n4; i++)
        {
            double alpha = 2 * M_PI * (i + theta) / n;
            tcos[i] = (float)(-cos(alpha) * scale);
            tsin[i] = (float)(-sin(alpha) * scale);
        }
        int bits = nbits - 2;
        revtab.resize(n4);
        for (int i = 0; i < n4; i++)
        {
            int r = 0;
            for (int b = 0; b < bits; b++)
                r |= ((i >> b) & 1) << (bits - 1 - b);
            revtab[i] = r;
        }
        fft_cos.resize(n4 / 2);
        fft_sin.resize(n4 / 2);
        for (int k = 0; k < n4 / 2; k++)
        {
            fft_cos[k] = (float)cos(2 * M_PI * k / n4);
            fft_sin[k] = (float)sin(2 * M_PI * k / n4);
        }
        zr.resize(n4);
        zi.resize(n4);
    }
    // the inverse complex FFT (e^+i), in place, the input in bit-reversed order
    void Fft()
    {
        int n4 = n >> 2;
        for (int len = 2; len <= n4; len <<= 1)
        {
            int half = len >> 1, step = n4 / len;
            for (int start = 0; start < n4; start += len)
                for (int k = 0; k < half; k++)
                {
                    float wr = fft_cos[k * step], wi = fft_sin[k * step];
                    int a = start + k, b = a + half;
                    float tr = zr[b] * wr - zi[b] * wi;
                    float ti = zr[b] * wi + zi[b] * wr;
                    zr[b] = zr[a] - tr;
                    zi[b] = zi[a] - ti;
                    zr[a] += tr;
                    zi[a] += ti;
                }
        }
    }
    void Calc(float *output, const float *input)
    {
        int n2 = n >> 1, n4 = n >> 2, n8 = n >> 3;
        // ff_imdct_half: pre rotation, FFT, post rotation, into the middle half of the output
        const float *in1 = input, *in2 = input + n2 - 1;
        for (int k = 0; k < n4; k++)
        {
            int j = revtab[k];
            zr[j] = *in2 * tcos[k] - *in1 * tsin[k];
            zi[j] = *in2 * tsin[k] + *in1 * tcos[k];
            in1 += 2;
            in2 -= 2;
        }
        Fft();
        float *z = output + n4; // re, im pairs
        for (int k = 0; k < n8; k++)
        {
            int a = n8 - k - 1, b = n8 + k;
            float r0 = zi[a] * tsin[a] - zr[a] * tcos[a];
            float i1 = zi[a] * tcos[a] + zr[a] * tsin[a];
            float r1 = zi[b] * tsin[b] - zr[b] * tcos[b];
            float i0 = zi[b] * tcos[b] + zr[b] * tsin[b];
            z[2 * a] = r0;
            z[2 * a + 1] = i0;
            z[2 * b] = r1;
            z[2 * b + 1] = i1;
        }
        // the rest, by symmetry
        for (int k = 0; k < n4; k++)
        {
            output[k] = -output[n2 - k - 1];
            output[n - k - 1] = output[n2 + k];
        }
    }
};

int av_log2(unsigned v)
{
    int n = 0;
    while (v >>= 1)
        n++;
    return n;
}
} // namespace

struct XwmaDecoder::State
{
    const Shared *shared;
    int channels;
    int sample_rate;
    Bits gb;
    int use_noise_coding;
    int byte_offset_bits;
    int exponent_sizes[BLOCK_NB_SIZES];
    uint16_t exponent_bands[BLOCK_NB_SIZES][25];
    int high_band_start[BLOCK_NB_SIZES];
    int coefs_end[BLOCK_NB_SIZES];
    int exponent_high_sizes[BLOCK_NB_SIZES];
    int exponent_high_bands[BLOCK_NB_SIZES][HIGH_BAND_MAX_SIZE];
    int high_band_coded[MAX_CHANNELS][HIGH_BAND_MAX_SIZE];
    int high_band_values[MAX_CHANNELS][HIGH_BAND_MAX_SIZE];
    const CoefCode *coef_codes[2];
    int frame_len;
    int frame_len_bits;
    int nb_block_sizes;
    int reset_block_lengths;
    int block_len_bits;
    int next_block_len_bits;
    int prev_block_len_bits;
    int block_len;
    int block_num;
    int block_pos;
    uint8_t ms_stereo;
    uint8_t channel_coded[MAX_CHANNELS];
    int exponents_bsize[MAX_CHANNELS];
    float exponents[MAX_CHANNELS][BLOCK_MAX_SIZE];
    float max_exponent[MAX_CHANNELS];
    float coefs1[MAX_CHANNELS][BLOCK_MAX_SIZE];
    float coefs[MAX_CHANNELS][BLOCK_MAX_SIZE];
    float output[BLOCK_MAX_SIZE * 2];
    Imdct mdct[BLOCK_NB_SIZES];
    const float *windows[BLOCK_NB_SIZES];
    float frame_out[MAX_CHANNELS][BLOCK_MAX_SIZE * 2];
    uint8_t last_superframe[MAX_CODED_SUPERFRAME_SIZE + 64];
    int last_bitoffset;
    int last_superframe_len;
    int exponents_initialized[MAX_CHANNELS];
    float noise_table[NOISE_TAB_SIZE];
    int noise_index;
    float noise_mult;
    int block_align;
    int skip; // samples (per channel) still to leave out of the output: the decoder's first two frames
    std::vector<float> samples[MAX_CHANNELS]; // a packet's

    bool Init(int ch, int rate, int64_t bit_rate, int align);
    int DecodeExpVlc(int ch);
    void Window(float *out);
    int RunLevelDecode(const CoefCode &code, float *ptr, int num_coefs, int coef_nb_bits);
    int DecodeBlock();
    int DecodeFrame();
    int DecodeSuperframe(const uint8_t *buf);
    void Emit(std::vector<int16_t> &out, int from);
};

// wma_decode_init, ff_wma_init
bool XwmaDecoder::State::Init(int ch, int rate, int64_t bit_rate, int align)
{
    if (rate <= 0 || rate > 50000 || ch <= 0 || ch > MAX_CHANNELS || bit_rate <= 0 || align <= 0)
        return false;
    shared = &GetShared();
    channels = ch;
    sample_rate = rate;
    block_align = align;
    for (int i = 0; i < MAX_CHANNELS; i++)
        max_exponent[i] = 1.0;

    // ff_wma_get_frame_len_bits, version 2
    frame_len_bits = rate <= 16000 ? 9 : rate <= 22050 ? 10 : 11;
    next_block_len_bits = frame_len_bits;
    prev_block_len_bits = frame_len_bits;
    block_len_bits = frame_len_bits;
    frame_len = 1 << frame_len_bits;
    {
        int nb_max, nb;
        nb = ((FLAGS2 >> 3) & 3) + 1;
        if ((bit_rate / channels) >= 32000)
            nb += 2;
        nb_max = frame_len_bits - BLOCK_MIN_BITS;
        if (nb > nb_max)
            nb = nb_max;
        nb_block_sizes = nb + 1;
    }

    use_noise_coding = 1;
    float high_freq = rate * 0.5;
    int sample_rate1 = rate;
    if (sample_rate1 >= 44100)
        sample_rate1 = 44100;
    else if (sample_rate1 >= 22050)
        sample_rate1 = 22050;
    else if (sample_rate1 >= 16000)
        sample_rate1 = 16000;
    else if (sample_rate1 >= 11025)
        sample_rate1 = 11025;
    else if (sample_rate1 >= 8000)
        sample_rate1 = 8000;

    float bps = (float)bit_rate / (float)(channels * rate);
    byte_offset_bits = av_log2((int)(bps * frame_len / 8.0 + 0.5)) + 2;
    if (byte_offset_bits + 3 > 25)
        return false;

    float bps1 = bps;
    if (channels == 2)
        bps1 = bps * 1.6;
    if (sample_rate1 == 44100)
    {
        if (bps1 >= 0.61)
            use_noise_coding = 0;
        else
            high_freq = high_freq * 0.4;
    }
    else if (sample_rate1 == 22050)
    {
        if (bps1 >= 1.16)
            use_noise_coding = 0;
        else if (bps1 >= 0.72)
            high_freq = high_freq * 0.7;
        else
            high_freq = high_freq * 0.6;
    }
    else if (sample_rate1 == 16000)
    {
        if (bps > 0.5)
            high_freq = high_freq * 0.5;
        else
            high_freq = high_freq * 0.3;
    }
    else if (sample_rate1 == 11025)
        high_freq = high_freq * 0.7;
    else if (sample_rate1 == 8000)
    {
        if (bps <= 0.625)
            high_freq = high_freq * 0.5;
        else if (bps > 0.75)
            use_noise_coding = 0;
        else
            high_freq = high_freq * 0.65;
    }
    else
    {
        if (bps >= 0.8)
            high_freq = high_freq * 0.75;
        else if (bps >= 0.6)
            high_freq = high_freq * 0.6;
        else
            high_freq = high_freq * 0.5;
    }

    // the scale factor band sizes of each block length
    for (int k = 0; k < nb_block_sizes; k++)
    {
        int block_len = frame_len >> k;
        const uint8_t *table = nullptr;
        int a = frame_len_bits - BLOCK_MIN_BITS - k;
        if (a < 3)
        {
            if (rate >= 44100)
                table = exponent_band_44100[a];
            else if (rate >= 32000)
                table = exponent_band_32000[a];
            else if (rate >= 22050)
                table = exponent_band_22050[a];
        }
        if (table)
        {
            int n = *table++;
            for (int i = 0; i < n; i++)
                exponent_bands[k][i] = table[i];
            exponent_sizes[k] = n;
        }
        else
        {
            int j = 0, lpos = 0;
            for (int i = 0; i < 25; i++)
            {
                int b = rate;
                int pos = ((block_len * 2 * ff_wma_critical_freqs[i]) + (b << 1)) / (4 * b);
                pos <<= 2;
                if (pos > block_len)
                    pos = block_len;
                if (pos > lpos)
                    exponent_bands[k][j++] = pos - lpos;
                if (pos >= block_len)
                    break;
                lpos = pos;
            }
            exponent_sizes[k] = j;
        }

        coefs_end[k] = (frame_len - ((frame_len * 9) / 100)) >> k;
        high_band_start[k] = (int)((block_len * 2 * high_freq) / rate + 0.5);
        int n = exponent_sizes[k], j = 0, pos = 0;
        for (int i = 0; i < n; i++)
        {
            int start = pos;
            pos += exponent_bands[k][i];
            int end = pos;
            if (start < high_band_start[k])
                start = high_band_start[k];
            if (end > coefs_end[k])
                end = coefs_end[k];
            if (end > start && j < HIGH_BAND_MAX_SIZE)
                exponent_high_bands[k][j++] = end - start;
        }
        exponent_high_sizes[k] = j;
    }

    for (int i = 0; i < nb_block_sizes; i++)
    {
        windows[i] = shared->windows[frame_len_bits - i].data();
        mdct[i].Init(frame_len_bits - i + 1, 1.0 / 32768.0);
    }
    reset_block_lengths = 1;

    if (use_noise_coding)
    {
        noise_mult = 0.02; // exponents in VLCs
        unsigned int seed = 1;
        float norm = (1.0 / (float)(1LL << 31)) * sqrt(3) * noise_mult;
        for (int i = 0; i < NOISE_TAB_SIZE; i++)
        {
            seed = seed * 314159 + 1;
            noise_table[i] = (float)((int)seed) * norm;
        }
    }

    int coef_vlc_table = 2;
    if (rate >= 32000)
    {
        if (bps1 < 0.72)
            coef_vlc_table = 0;
        else if (bps1 < 1.16)
            coef_vlc_table = 1;
    }
    coef_codes[0] = &shared->coefs[coef_vlc_table * 2];
    coef_codes[1] = &shared->coefs[coef_vlc_table * 2 + 1];

    skip = frame_len * 2;
    return true;
}

// decode_exp_vlc
int XwmaDecoder::State::DecodeExpVlc(int ch)
{
    const uint16_t *ptr = exponent_bands[frame_len_bits - block_len_bits];
    float *q = exponents[ch], *q_end = q + block_len;
    const float *ptab = shared->pow_tab + 60;
    float max_scale = 0;
    int last_exp = 36;
    while (q < q_end)
    {
        int code = shared->exp_vlc.Read(gb);
        last_exp += code - 60;
        if ((unsigned)last_exp + 60 >= 156)
            return -1;
        float v = ptab[last_exp];
        if (v > max_scale)
            max_scale = v;
        int n = *ptr++;
        if (n > q_end - q)
            n = (int)(q_end - q);
        for (int i = 0; i < n; i++)
            *q++ = v;
        if (!n)
            return -1;
    }
    max_exponent[ch] = max_scale;
    return 0;
}

// wma_window: the block's samples, windowed, added into the frame. Where the windows overlap, their squares add up to 1.
void XwmaDecoder::State::Window(float *out)
{
    const float *in = output;
    int len, bsize, n;

    // left part
    if (block_len_bits <= prev_block_len_bits)
    {
        len = block_len;
        bsize = frame_len_bits - block_len_bits;
        const float *w = windows[bsize];
        for (int i = 0; i < len; i++)
            out[i] += in[i] * w[i];
    }
    else
    {
        len = 1 << prev_block_len_bits;
        n = (block_len - len) / 2;
        bsize = frame_len_bits - prev_block_len_bits;
        const float *w = windows[bsize];
        for (int i = 0; i < len; i++)
            out[n + i] += in[n + i] * w[i];
        memcpy(out + n + len, in + n + len, n * sizeof(float));
    }

    out += block_len;
    in += block_len;

    // right part
    if (block_len_bits <= next_block_len_bits)
    {
        len = block_len;
        bsize = frame_len_bits - block_len_bits;
        const float *w = windows[bsize];
        for (int i = 0; i < len; i++)
            out[i] = in[i] * w[len - 1 - i];
    }
    else
    {
        len = 1 << next_block_len_bits;
        n = (block_len - len) / 2;
        bsize = frame_len_bits - next_block_len_bits;
        const float *w = windows[bsize];
        memcpy(out, in, n * sizeof(float));
        for (int i = 0; i < len; i++)
            out[n + i] = in[n + i] * w[len - 1 - i];
        memset(out + n + len, 0, n * sizeof(float));
    }
}

// ff_wma_run_level_decode, for WMA 1 and 2
int XwmaDecoder::State::RunLevelDecode(const CoefCode &code, float *ptr, int num_coefs, int coef_nb_bits)
{
    const unsigned int coef_mask = block_len - 1;
    int offset = 0;
    for (; offset < num_coefs; offset++)
    {
        int c = code.vlc.Read(gb);
        if (c > 1)
        {
            // normal code
            offset += code.run_table[c];
            float level = code.level_table[c];
            ptr[offset & coef_mask] = gb.Get1() ? level : -level;
        }
        else if (c == 1)
            break; // EOB
        else
        {
            // escape
            int level = (int)gb.Get(coef_nb_bits);
            offset += (int)gb.Get(frame_len_bits);
            ptr[offset & coef_mask] = (float)(gb.Get1() ? level : -level);
        }
    }
    // EOB can be left out
    if (offset > num_coefs)
        return -1;
    return 0;
}

// wma_decode_block: 0 if OK, 1 for the frame's last block, -1 if broken
int XwmaDecoder::State::DecodeBlock()
{
    int n, v, a, ch, bsize;
    int coef_nb_bits, total_gain;
    int nb_coefs[MAX_CHANNELS];
    float mdct_norm;

    // the block's length
    n = av_log2(nb_block_sizes - 1) + 1;
    if (reset_block_lengths)
    {
        reset_block_lengths = 0;
        v = (int)gb.Get(n);
        if (v >= nb_block_sizes)
            return -1;
        prev_block_len_bits = frame_len_bits - v;
        v = (int)gb.Get(n);
        if (v >= nb_block_sizes)
            return -1;
        block_len_bits = frame_len_bits - v;
    }
    else
    {
        prev_block_len_bits = block_len_bits;
        block_len_bits = next_block_len_bits;
    }
    v = (int)gb.Get(n);
    if (v >= nb_block_sizes)
        return -1;
    next_block_len_bits = frame_len_bits - v;

    if (frame_len_bits - block_len_bits >= nb_block_sizes)
        return -1;

    block_len = 1 << block_len_bits;
    if ((block_pos + block_len) > frame_len)
        return -1;

    if (channels == 2)
        ms_stereo = (uint8_t)gb.Get1();
    v = 0;
    for (ch = 0; ch < channels; ch++)
    {
        a = gb.Get1();
        channel_coded[ch] = (uint8_t)a;
        v |= a;
    }

    bsize = frame_len_bits - block_len_bits;

    // no channel coded: nothing more to read
    if (v)
    {
        // the total gain, and from it the bits of escaped coefficients
        total_gain = 1;
        for (;;)
        {
            if (gb.Left() < 7)
                return -1;
            a = (int)gb.Get(7);
            total_gain += a;
            if (a != 127)
                break;
        }
        coef_nb_bits = total_gain < 15 ? 13 : total_gain < 32 ? 12 : total_gain < 40 ? 11 : total_gain < 45 ? 10 : 9;

        n = coefs_end[bsize];
        for (ch = 0; ch < channels; ch++)
            nb_coefs[ch] = n;

        // noise coding
        if (use_noise_coding)
        {
            for (ch = 0; ch < channels; ch++)
            {
                if (channel_coded[ch])
                {
                    int nb = exponent_high_sizes[bsize];
                    for (int i = 0; i < nb; i++)
                    {
                        a = gb.Get1();
                        high_band_coded[ch][i] = a;
                        // noise coded: the coefficients are not there
                        if (a)
                            nb_coefs[ch] -= exponent_high_bands[bsize][i];
                    }
                }
            }
            for (ch = 0; ch < channels; ch++)
            {
                if (channel_coded[ch])
                {
                    int nb = exponent_high_sizes[bsize];
                    int val = (int)0x80000000;
                    for (int i = 0; i < nb; i++)
                    {
                        if (high_band_coded[ch][i])
                        {
                            if (val == (int)0x80000000)
                                val = (int)gb.Get(7) - 19;
                            else
                                val += shared->hgain_vlc.Read(gb);
                            high_band_values[ch][i] = val;
                        }
                    }
                }
            }
        }

        // short blocks can reuse the exponents
        if ((block_len_bits == frame_len_bits) || gb.Get1())
        {
            for (ch = 0; ch < channels; ch++)
            {
                if (channel_coded[ch])
                {
                    if (DecodeExpVlc(ch) < 0)
                        return -1;
                    exponents_bsize[ch] = bsize;
                    exponents_initialized[ch] = 1;
                }
            }
        }

        for (ch = 0; ch < channels; ch++)
            if (channel_coded[ch] && !exponents_initialized[ch])
                return -1;

        // the spectral coefficients, run-length coded
        for (ch = 0; ch < channels; ch++)
        {
            if (channel_coded[ch])
            {
                // mid/side stereo's side has codes of its own: there is less in it
                int tindex = (ch == 1 && ms_stereo);
                float *ptr = coefs1[ch];
                memset(ptr, 0, block_len * sizeof(float));
                if (nb_coefs[ch] < 0 || RunLevelDecode(*coef_codes[tindex], ptr, nb_coefs[ch], coef_nb_bits) < 0)
                    return -1;
            }
        }

        mdct_norm = 1.0 / (float)(block_len / 2);

        // the MDCT coefficients
        for (ch = 0; ch < channels; ch++)
        {
            if (!channel_coded[ch])
                continue;
            float *c1 = coefs1[ch];
            float *exps = exponents[ch];
            int esize = exponents_bsize[ch];
            float mult = pow(10.0, total_gain * 0.05) / max_exponent[ch];
            mult *= mdct_norm;
            float *c = coefs[ch];
            if (use_noise_coding)
            {
                float mult1;
                float exp_power[HIGH_BAND_MAX_SIZE];
                int n1 = exponent_high_sizes[bsize];
                int last_high_band = 0;

                // the power of the high bands
                exps = exponents[ch] + (high_band_start[bsize] << bsize >> esize);
                for (int j = 0; j < n1; j++)
                {
                    int nb = exponent_high_bands[bsize][j];
                    if (high_band_coded[ch][j])
                    {
                        float e2 = 0;
                        for (int i = 0; i < nb; i++)
                        {
                            float e = exps[i << bsize >> esize];
                            e2 += e * e;
                        }
                        exp_power[j] = e2 / nb;
                        last_high_band = j;
                    }
                    exps += nb << bsize >> esize;
                }

                // main and high frequencies
                exps = exponents[ch];
                for (int j = -1; j < n1; j++)
                {
                    int nb = j < 0 ? high_band_start[bsize] : exponent_high_bands[bsize][j];
                    if (j >= 0 && high_band_coded[ch][j])
                    {
                        // noise of the given power
                        mult1 = sqrt(exp_power[j] / exp_power[last_high_band]);
                        mult1 = mult1 * pow(10.0, high_band_values[ch][j] * 0.05);
                        mult1 = mult1 / (max_exponent[ch] * noise_mult);
                        mult1 *= mdct_norm;
                        for (int i = 0; i < nb; i++)
                        {
                            float noise = noise_table[noise_index];
                            noise_index = (noise_index + 1) & (NOISE_TAB_SIZE - 1);
                            *c++ = noise * exps[i << bsize >> esize] * mult1;
                        }
                    }
                    else
                    {
                        // coded values and a little noise
                        for (int i = 0; i < nb; i++)
                        {
                            float noise = noise_table[noise_index];
                            noise_index = (noise_index + 1) & (NOISE_TAB_SIZE - 1);
                            *c++ = ((*c1++) + noise) * exps[i << bsize >> esize] * mult;
                        }
                    }
                    exps += nb << bsize >> esize;
                }

                // the highest frequencies: noise
                int nb = block_len - coefs_end[bsize];
                mult1 = mult * exps[(-(1 << bsize)) >> esize];
                for (int i = 0; i < nb; i++)
                {
                    *c++ = noise_table[noise_index] * mult1;
                    noise_index = (noise_index + 1) & (NOISE_TAB_SIZE - 1);
                }
            }
            else
            {
                int nb = nb_coefs[ch];
                for (int i = 0; i < nb; i++)
                    *c++ = c1[i] * exps[i << bsize >> esize] * mult;
                nb = block_len - coefs_end[bsize];
                for (int i = 0; i < nb; i++)
                    *c++ = 0.0;
            }
        }

        if (ms_stereo && channel_coded[1])
        {
            // mid/side to left/right, before the MDCT
            if (!channel_coded[0])
            {
                memset(coefs[0], 0, sizeof(float) * block_len);
                channel_coded[0] = 1;
            }
            for (int i = 0; i < block_len; i++)
            {
                float t = coefs[0][i] - coefs[1][i];
                coefs[0][i] += coefs[1][i];
                coefs[1][i] = t;
            }
        }
    }

    for (ch = 0; ch < channels; ch++)
    {
        int n4 = block_len / 2;
        if (channel_coded[ch])
            mdct[bsize].Calc(output, coefs[ch]);
        else if (!(ms_stereo && ch == 1))
            memset(output, 0, sizeof(output));

        // windowed, into the frame
        int index = (frame_len / 2) + block_pos - n4;
        Window(&frame_out[ch][index]);
    }

    block_num++;
    block_pos += block_len;
    return block_pos >= frame_len ? 1 : 0;
}

// wma_decode_frame: a frame of frame_len samples, onto `samples`
int XwmaDecoder::State::DecodeFrame()
{
    block_num = 0;
    block_pos = 0;
    for (;;)
    {
        int ret = DecodeBlock();
        if (ret < 0)
            return -1;
        if (ret)
            break;
    }
    for (int ch = 0; ch < channels; ch++)
    {
        samples[ch].insert(samples[ch].end(), frame_out[ch], frame_out[ch] + frame_len);
        memmove(&frame_out[ch][0], &frame_out[ch][frame_len], frame_len * sizeof(float));
    }
    return 0;
}

// wma_decode_superframe: a packet, its frames onto `samples`
int XwmaDecoder::State::DecodeSuperframe(const uint8_t *buf)
{
    int buf_size = block_align;
    int nb_frames, bit_offset, pos, len;
    uint8_t *q;

    gb.Init(buf, buf_size * 8);

    // the superframe's header
    gb.Skip(4); // superframe index
    nb_frames = (int)gb.Get(4) - (last_superframe_len <= 0);
    if (nb_frames <= 0)
    {
        if (nb_frames < 0 || gb.Left() <= 8)
            return -1;
        // no frame ends in this packet: all of it goes to the next
        if ((last_superframe_len + buf_size - 1) > MAX_CODED_SUPERFRAME_SIZE)
            goto fail;
        q = last_superframe + last_superframe_len;
        len = buf_size - 1;
        while (len > 0)
        {
            *q++ = (uint8_t)gb.Get(8);
            len--;
        }
        memset(q, 0, 8);
        last_superframe_len += 8 * buf_size - 8;
        return 0;
    }

    bit_offset = (int)gb.Get(byte_offset_bits + 3);
    if (bit_offset > gb.Left())
        goto fail;

    if (last_superframe_len > 0)
    {
        // the frame the last packet started ends with this one's first bit_offset bits
        if ((last_superframe_len + ((bit_offset + 7) >> 3)) > MAX_CODED_SUPERFRAME_SIZE)
            goto fail;
        q = last_superframe + last_superframe_len;
        len = bit_offset;
        while (len > 7)
        {
            *q++ = (uint8_t)gb.Get(8);
            len -= 8;
        }
        if (len > 0)
            *q++ = (uint8_t)(gb.Get(len) << (8 - len));
        memset(q, 0, 8);

        gb.Init(last_superframe, last_superframe_len * 8 + bit_offset);
        if (last_bitoffset > 0)
            gb.Skip(last_bitoffset);
        if (DecodeFrame() < 0)
            goto fail;
        nb_frames--;
    }

    // the frames from bit_offset on
    pos = bit_offset + 4 + 4 + byte_offset_bits + 3;
    if (pos >= MAX_CODED_SUPERFRAME_SIZE * 8 || pos > buf_size * 8)
        return -1;
    gb.Init(buf + (pos >> 3), (buf_size - (pos >> 3)) * 8);
    len = pos & 7;
    if (len > 0)
        gb.Skip(len);

    reset_block_lengths = 1;
    for (int i = 0; i < nb_frames; i++)
        if (DecodeFrame() < 0)
            goto fail;

    // the rest begins the next packet's first frame
    pos = gb.pos + ((bit_offset + 4 + 4 + byte_offset_bits + 3) & ~7);
    last_bitoffset = pos & 7;
    pos >>= 3;
    len = buf_size - pos;
    if (len > MAX_CODED_SUPERFRAME_SIZE || len < 0)
        goto fail;
    last_superframe_len = len;
    memcpy(last_superframe, buf + pos, len);
    return 0;

fail:
    // a broken packet resets the bit reservoir
    last_superframe_len = 0;
    return -1;
}

// `samples` from `from` on, interleaved, as 16-bit PCM onto `out`, but for what is still skipped
void XwmaDecoder::State::Emit(std::vector<int16_t> &out, int from)
{
    int n = (int)samples[0].size();
    if (skip > 0)
    {
        int k = (std::min)(skip, n - from);
        from += k;
        skip -= k;
    }
    out.reserve(out.size() + (size_t)(n - from) * channels);
    for (int i = from; i < n; i++)
        for (int ch = 0; ch < channels; ch++)
        {
            float v = samples[ch][i] * 32768.0f;
            v = v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v;
            out.push_back((int16_t)lrintf(v));
        }
}

XwmaDecoder::XwmaDecoder() : s(nullptr) {}

XwmaDecoder::~XwmaDecoder() { delete s; }

bool XwmaDecoder::Init(uint32_t channels, uint32_t rate, uint32_t avgBytesPerSec, uint32_t blockAlign)
{
    delete s;
    s = new State();
    if (!s->Init((int)channels, (int)rate, (int64_t)avgBytesPerSec * 8, (int)blockAlign))
    {
        delete s;
        s = nullptr;
        return false;
    }
    return true;
}

bool XwmaDecoder::Decode(const uint8_t *packet, std::vector<int16_t> &out)
{
    if (!s)
        return false;
    for (int ch = 0; ch < s->channels; ch++)
        s->samples[ch].clear();
    if (s->DecodeSuperframe(packet) < 0)
        return false;
    s->Emit(out, 0);
    return true;
}

void XwmaDecoder::Finish(std::vector<int16_t> &out)
{
    if (!s)
        return;
    // the second half of the last frame's window
    for (int ch = 0; ch < s->channels; ch++)
        s->samples[ch].assign(s->frame_out[ch], s->frame_out[ch] + s->frame_len);
    s->last_superframe_len = 0;
    s->Emit(out, 0);
}
