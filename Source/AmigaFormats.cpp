// Spurwerk – TCB Tracker (Atari ST) and Future Composer 1.0–1.4 (Amiga) loaders.
// Written after the format descriptions in OpenMPT's Load_tcb.cpp / Load_fc.cpp (BSD). Future Composer instruments are
// macro scripts (volume + frequency sequences); they are kept raw in Song::fc and interpreted by the engine per tick.
#include "TrackerFormats.h"
#include <cstring>

namespace sw
{
namespace
{
struct Be
{
    const juce::uint8* p;
    size_t n;
    explicit Be (const juce::MemoryBlock& d) : p (static_cast<const juce::uint8*> (d.getData())), n (d.getSize()) {}
    bool has (size_t o, size_t len) const { return o <= n && len <= n - o; }
    int u8 (size_t o) const { return o < n ? p[o] : 0; }
    int s8 (size_t o) const { return (juce::int8) u8 (o); }
    int u16 (size_t o) const { return (u8 (o) << 8) | u8 (o + 1); }
    int s16 (size_t o) const { return (juce::int16) u16 (o); }
    juce::uint32 u32 (size_t o) const { return ((juce::uint32) u16 (o) << 16) | (juce::uint32) u16 (o + 2); }
    bool tag (size_t o, const char* t) const { const auto l = std::strlen (t); return has (o, l) && std::memcmp (p + o, t, l) == 0; }
    juce::String str (size_t o, size_t len) const
    {
        juce::String s;
        for (size_t i = 0; i < len; ++i)
        {
            const int c = u8 (o + i);
            if (c == 0) break;
            s += (juce::juce_wchar) (c >= 32 && c < 127 ? c : ' ');
        }
        return s.trimEnd();
    }
};

void finishOrders (Song& song)
{
    if (song.patterns.empty()) { Pattern p; p.resize (64, song.channels); song.patterns.push_back (std::move (p)); }
    if (song.orders.empty()) song.orders.push_back (0);
    for (auto& o : song.orders) if (o < 0 || o >= (int) song.patterns.size()) o = 0;
}
} // namespace

// ============================================================================================ TCB Tracker
bool isTcb (const juce::MemoryBlock& d)
{
    Be r (d);
    if (r.n < 0x132 || ! (r.tag (0, "AN COOL.") || r.tag (0, "AN COOL!"))) return false;
    if (r.u8 (12) > 15 || r.u8 (13) != 0 || r.u8 (0x8E) > 127 || r.u8 (0x8F) != 0 || r.u32 (8) > 128) return false;
    for (int i = 0; i < 128; ++i) if (r.u8 (14 + (size_t) i) >= 128) return false;
    return true;
}

std::unique_ptr<Song> loadTcb (const juce::MemoryBlock& d, juce::String& error)
{
    Be r (d);
    if (! isTcb (d)) { error = "Keine TCB-Datei"; return nullptr; }
    const bool newFormat = r.u8 (7) == '.';
    const int numPatterns = (int) r.u32 (8);
    auto song = std::make_unique<Song>();
    song->format = Format::TCB;
    song->channels = 4;
    song->initialSpeed = 16 - r.u8 (12);
    song->initialTempo = 125;
    const int numOrders = juce::jmax (1, r.u8 (0x8E));
    for (int i = 0; i < numOrders; ++i) song->orders.push_back (r.u8 (14 + (size_t) i));

    size_t pos = 0x90;
    bool amiga = true;
    if (newFormat)
    {
        const int af = r.u16 (pos);
        if (af > 1) { error = U ("TCB-Datei beschädigt"); return nullptr; }
        amiga = af != 0;
        pos += 2;
    }
    const size_t namesOff = pos;
    pos += 128;
    std::array<int, 16> special {};
    if (newFormat)
    {
        for (int i = 0; i < 16; ++i) special[(size_t) i] = r.s16 (pos + (size_t) i * 2);
        pos += 32;
    }
    const int noteOffset = amiga ? 0 : 3;

    for (int pi = 0; pi < numPatterns; ++pi)
    {
        Pattern pat;
        pat.resize (64, 4);
        for (int row = 0; row < 64; ++row)
            for (int c = 0; c < 4; ++c)
            {
                const size_t o = pos + (size_t) ((row * 4 + c) * 2);
                const int note = r.u8 (o), ie = r.u8 (o + 1);
                Cell& cell = pat.at (row, c);
                if (note >= 0x10 && note <= 0x3B)
                {
                    cell.note = (juce::uint8) juce::jlimit (1, 120, 61 - 24 + (note >> 4) * 12 + (note & 15) + noteOffset);
                    cell.instr = (juce::uint8) ((ie >> 4) + 1);
                }
                const int fx = ie & 15;
                if (fx == 0x0D) { cell.fx = 0xD; cell.param = 0; }
                else if (fx >= 1 && fx <= 0x0A)
                {
                    const int v = special[(size_t) fx];
                    if (v > 0) { cell.fx = 0x2; cell.param = (juce::uint8) juce::jmin (255, (v + 16) / 32); }
                    else if (v < 0) { cell.fx = 0x1; cell.param = (juce::uint8) juce::jmin (255, (-v + 16) / 32); }
                }
            }
        song->patterns.push_back (std::move (pat));
        pos += 512;
    }

    // samples: size word, 16 x (volume, -, loop-from-end), 16 x (offset, length); offsets relative to sampleStart
    const size_t sampleStart = pos;
    const size_t h1 = sampleStart + 4, h2 = h1 + 64;
    for (int i = 0; i < 16; ++i)
    {
        Sample s;
        s.name = r.str (namesOff + (size_t) i * 8, 8);
        s.volume = juce::jmin (64, juce::jmin (127, r.u8 (h1 + (size_t) i * 4)) / 2);
        const int loopFromEnd = r.u16 (h1 + (size_t) i * 4 + 2);
        const juce::uint32 off = r.u32 (h2 + (size_t) i * 8), len = r.u32 (h2 + (size_t) i * 8 + 4);
        const size_t data = sampleStart + off;
        if (len > 1 && r.has (data, 1))
        {
            const size_t avail = juce::jmin ((size_t) len, r.n - data);
            s.pcm.resize (avail);
            for (size_t k = 0; k < avail; ++k) s.pcm[k] = (float) (r.u8 (data + k) - 128) / 128.0f;   // unsigned 8 bit
            if (loopFromEnd > 0 && loopFromEnd < (int) avail) { s.loopStart = (int) avail - loopFromEnd; s.loopLength = loopFromEnd; }
        }
        if (! amiga) s.finetune = 5;
        song->samples.push_back (std::move (s));
    }
    song->periodMode = 0;
    song->stereoSeparation = 100;
    song->formatName = juce::String ("TCB Tracker (Atari ST") + (newFormat ? "" : ", Beta") + (amiga ? juce::String (")") : U (", ST-Frequenzen)"));
    finishOrders (*song);
    return song;
}

// ============================================================================================ Future Composer
namespace
{
// built-in waveforms of Future Composer 1.0–1.3 (47 waveforms, lengths in words)
const juce::uint8 kFcWaveLen[47] = { 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
                                     16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
                                     8, 8, 8, 8, 8, 8, 8, 8, 16, 8, 16, 16, 8, 8, 24 };
void fcBuiltinWaves (std::vector<Sample>& out)
{
    // 32 "pulse ramps" (16 triangle-ish + 16 pulse), then short pulses, saws, two "brass" shapes, sine-ish and a mix
    std::vector<juce::int8> all;
    auto push = [&all] (std::initializer_list<int> v) { for (int x : v) all.push_back ((juce::int8) (juce::uint8) x); };
    // waveforms 0..15: ramp down from 0xC0 with a growing negative half
    for (int w = 0; w < 16; ++w)
    {
        push ({ 0xC0, 0xC0, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8, 0x00, 0xF8, 0xF0, 0xE8, 0xE0, 0xD8, 0xD0, 0xC8 });
        static const int tailPos[16] = { 0x3F, 0x37, 0x2F, 0x27, 0x1F, 0x17, 0x0F, 0x07, 0xFF, 0x07, 0x0F, 0x17, 0x1F, 0x27, 0x2F, 0x37 };
        static const int tailNeg[16] = { 0xC0, 0xB8, 0xB0, 0xA8, 0xA0, 0x98, 0x90, 0x88, 0x80, 0x88, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8 };
        for (int k = 0; k < 16; ++k) all.push_back ((juce::int8) (juce::uint8) (k < w ? tailNeg[k] : tailPos[k]));
    }
    // 16..29: pulses 0x81 / 0x7F with growing width (16..29 low bytes) – 32 waveforms of 32 bytes in total with the two below
    for (int w = 0; w < 14; ++w)
        for (int k = 0; k < 32; ++k) all.push_back ((juce::int8) (juce::uint8) (k < 16 + w ? 0x81 : 0x7F));
    // 30, 31
    for (int k = 0; k < 32; ++k) all.push_back ((juce::int8) (juce::uint8) (k < 30 ? 0x80 : 0x7F));
    for (int k = 0; k < 32; ++k) all.push_back ((juce::int8) (juce::uint8) (k < 31 ? 0x80 : 0x7F));
    // 32..39: 16-byte pulses
    static const int width[8] = { 8, 7, 6, 5, 4, 3, 2, 2 };
    for (int w = 0; w < 8; ++w)
        for (int k = 0; k < 16; ++k) all.push_back ((juce::int8) (juce::uint8) (k < width[w] ? 0x80 : 0x7F));
    push ({ 0x80, 0x80, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, 0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8,
            0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x7F });
    push ({ 0x80, 0x80, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 });
    push ({ 0x45, 0x45, 0x79, 0x7D, 0x7A, 0x77, 0x70, 0x66, 0x61, 0x58, 0x53, 0x4D, 0x2C, 0x20, 0x18, 0x12,
            0x04, 0xDB, 0xD3, 0xCD, 0xC6, 0xBC, 0xB5, 0xAE, 0xA8, 0xA3, 0x9D, 0x99, 0x93, 0x8E, 0x8B, 0x8A });
    push ({ 0x45, 0x45, 0x79, 0x7D, 0x7A, 0x77, 0x70, 0x66, 0x5B, 0x4B, 0x43, 0x37, 0x2C, 0x20, 0x18, 0x12,
            0x04, 0xF8, 0xE8, 0xDB, 0xCF, 0xC6, 0xBE, 0xB0, 0xA8, 0xA4, 0x9E, 0x9A, 0x95, 0x94, 0x8D, 0x83 });
    push ({ 0x00, 0x00, 0x40, 0x60, 0x7F, 0x60, 0x40, 0x20, 0x00, 0xE0, 0xC0, 0xA0, 0x80, 0xA0, 0xC0, 0xE0 });
    push ({ 0x00, 0x00, 0x40, 0x60, 0x7F, 0x60, 0x40, 0x20, 0x00, 0xE0, 0xC0, 0xA0, 0x80, 0xA0, 0xC0, 0xE0 });
    push ({ 0x80, 0x80, 0x90, 0x98, 0xA0, 0xA8, 0xB0, 0xB8, 0xC0, 0xC8, 0xD0, 0xD8, 0xE0, 0xE8, 0xF0, 0xF8,
            0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x7F,
            0x80, 0x80, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x00, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70 });
    size_t p = 0;
    for (int w = 0; w < 47; ++w)
    {
        Sample s;
        s.name = "FC-Welle " + juce::String (w);
        const size_t len = (size_t) kFcWaveLen[w] * 2;
        for (size_t k = 0; k < len && p + k < all.size(); ++k) s.pcm.push_back ((float) all[p + k] / 128.0f);
        p += len;
        s.loopStart = 0;
        s.loopLength = (int) s.pcm.size();
        out.push_back (std::move (s));
    }
}

void fcSample (const Be& r, size_t& pos, int lenWords, int loopStartBytes, int loopLenWords, bool fc14, Sample& s)
{
    size_t len = (size_t) lenWords * 2;
    if (fc14 && len) len += 2;
    const size_t next = pos + len;
    size_t total = len;
    const bool loop = loopLenWords > 1;
    const size_t loopEnd = (size_t) loopStartBytes + (size_t) loopLenWords * 2;
    if (loop && loopEnd > total) total = loopEnd;   // loop may reach into the next sample
    const size_t avail = pos < r.n ? juce::jmin (total, r.n - pos) : 0;
    s.pcm.resize (avail);
    for (size_t k = 0; k < avail; ++k) s.pcm[k] = (float) r.s8 (pos + k) / 128.0f;
    if (loop && loopStartBytes < (int) avail)
    {
        s.loopStart = loopStartBytes;
        s.loopLength = (int) juce::jmin (loopEnd, avail) - loopStartBytes;
    }
    pos = next;
}
} // namespace

bool isFc (const juce::MemoryBlock& d)
{
    Be r (d);
    if (r.n < 100 || ! (r.tag (0, "SMOD") || r.tag (0, "FC14"))) return false;
    const juce::uint32 lim = 0x80000;
    const juce::uint32 ps = r.u32 (12), fs = r.u32 (20), vs = r.u32 (28);
    return ps % 64 == 0 && ps > 0 && ps <= 64 * 256 && fs % 64 == 0 && fs > 0 && fs <= 64 * 256 && vs % 64 == 0 && vs > 0 && vs <= 64 * 256
        && r.u32 (8) <= lim && r.u32 (16) <= lim && r.u32 (24) <= lim && r.u32 (32) <= lim;
}

std::unique_ptr<Song> loadFc (const juce::MemoryBlock& d, juce::String& error)
{
    Be r (d);
    if (! isFc (d)) { error = "Keine Future-Composer-Datei"; return nullptr; }
    const bool fc14 = r.tag (0, "FC14");
    const size_t headerSize = fc14 ? 180 : 100;
    juce::uint32 seqSize = r.u32 (4);
    const size_t patOff = r.u32 (8), patSize = r.u32 (12), freqOff = r.u32 (16), freqSize = r.u32 (20), volOff = r.u32 (24), volSize = r.u32 (28);
    const size_t smpOff = r.u32 (32), waveOff = r.u32 (36);
    if (seqSize == 0 && patOff > headerSize) seqSize = (juce::uint32) (patOff - headerSize);
    const int numOrders = juce::jmin (256, (int) (seqSize / 13));
    if (numOrders < 1) { error = U ("Future-Composer-Datei ohne Abspielfolge"); return nullptr; }

    auto song = std::make_unique<Song>();
    song->format = Format::FC;
    song->channels = 4;
    song->initialSpeed = 3;
    song->initialTempo = 125;
    song->periodMode = 0;
    song->stereoSeparation = 100;
    auto& fc = song->fc;
    fc.fc14 = fc14;
    fc.freq.assign (64 * 256, 0);
    for (size_t i = 0; i < freqSize && i < fc.freq.size(); ++i) fc.freq[i] = (juce::uint8) r.u8 (freqOff + i);
    for (size_t i = 0; i < volSize; ++i) fc.vol.push_back ((juce::uint8) r.u8 (volOff + i));
    fc.numInstruments = (int) (volSize / 64);
    static const juce::uint8 emptyInstr[8] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE1 };   // out-of-range instruments
    for (auto b : emptyInstr) fc.vol.push_back (b);
    fc.vol.resize (fc.vol.size() + 64 * 5, 0xE1);   // the E0 underflow can jump up to 260 bytes ahead

    // sequence: per order 4 x (pattern, note transpose, instrument transpose) + speed
    const size_t seq = headerSize;
    const size_t numTracks = patSize / 64;
    for (int o = 0; o < numOrders; ++o)
    {
        const size_t e = seq + (size_t) o * 13;
        Pattern pat;
        pat.resize (32, 4);
        int lastRow = 32;
        for (int c = 0; c < 4; ++c)
        {
            const size_t trk = (size_t) r.u8 (e + (size_t) c * 3);
            const int ntr = r.s8 (e + (size_t) c * 3 + 1), itr = r.s8 (e + (size_t) c * 3 + 2);
            if (trk >= numTracks) continue;
            const size_t base = patOff + trk * 64;
            for (int row = 0; row < 32; ++row)
            {
                const int p0 = r.u8 (base + (size_t) row * 2), p1 = r.u8 (base + (size_t) row * 2 + 1);
                Cell& cell = pat.at (row, c);
                if (p0 == 0x49) lastRow = juce::jmin (lastRow, row);
                if (p0 > 0 && p0 != 0x49)
                {
                    const int idx = (ntr + p0) & 0x7F;
                    const int ins = (p1 & 0x3F) + itr;
                    cell.note = (juce::uint8) juce::jlimit (1, 120, Song::fcNoteForIndex (idx));
                    cell.vcmd = vcFcIndex;
                    cell.vol = (juce::uint8) idx;
                    cell.instr = (juce::uint8) (ins >= 0 && ins < fc.numInstruments ? ins + 1 : fc.numInstruments + 1);
                }
                if (p1 & 0x80)   // portamento: value in the next row's info byte
                {
                    const int data = row + 1 < 32 ? r.u8 (base + (size_t) (row + 1) * 2 + 1) : 0;
                    int prm = data & 0x1F;
                    if (! fc14) prm *= 2;
                    if (data > 0x1F) prm = -prm;
                    cell.fx = fxFcPorta;
                    cell.param = (juce::uint8) (juce::int8) prm;
                }
                else if ((p1 & 0xC0) == 0x40 && cell.fx == 0) { cell.fx = fxFcPorta; cell.param = 0; }
            }
        }
        const int speed = r.u8 (e + 12);
        auto put = [&pat] (int row, juce::uint8 fx, juce::uint8 prm) {
            for (int c = 0; c < 4; ++c)
                if (pat.at (row, c).fx == 0) { pat.at (row, c).fx = fx; pat.at (row, c).param = prm; return; }
        };
        if (speed > 0) put (0, fxSpeed, (juce::uint8) speed);
        if (lastRow < 32) put (juce::jmax (lastRow, 1) - 1, 0xD, 0);
        song->patterns.push_back (std::move (pat));
        song->orders.push_back (o);
    }

    // samples 0..9 from the file (FC14: optionally SSMP packs of 10), then the waveforms
    std::vector<Sample> smp (10);
    std::vector<Sample> packs;
    size_t pos = smpOff;
    for (int i = 0; i < 10; ++i)
    {
        const size_t si = 0x28 + (size_t) i * 6;
        const int len = r.u16 (si), ls = r.u16 (si + 2), ll = r.u16 (si + 4);
        smp[(size_t) i].name = "Sample " + juce::String (i);
        if (len == 0) continue;
        if (fc14 && r.tag (pos, "SSMP"))
        {
            pos += 4;
            const size_t hdr = pos;
            pos += 160;
            if (packs.size() < 100) packs.resize (100);
            for (int k = 0; k < 10; ++k)
            {
                const size_t h = hdr + (size_t) k * 16 + 4;
                auto& s = packs[(size_t) (i * 10 + k)];
                s.name = "Pack " + juce::String (i) + "." + juce::String (k);
                fcSample (r, pos, r.u16 (h), r.u16 (h + 2), r.u16 (h + 4), true, s);
            }
        }
        else fcSample (r, pos, len, ls, ll, fc14, smp[(size_t) i]);
    }
    if (fc14)
    {
        size_t wp = waveOff;
        for (int w = 0; w < 80; ++w)
        {
            Sample s;
            s.name = "FC-Welle " + juce::String (w);
            const int lenWords = r.u8 (100 + (size_t) w);
            for (int k = 0; k < lenWords * 2; ++k) s.pcm.push_back ((float) r.s8 (wp + (size_t) k) / 128.0f);
            wp += (size_t) lenWords * 2;
            s.loopStart = 0;
            s.loopLength = (int) s.pcm.size();
            smp.push_back (std::move (s));
        }
    }
    else fcBuiltinWaves (smp);
    if (! packs.empty()) for (auto& s : packs) smp.push_back (std::move (s));   // sample index 90 + pack*10 + sub
    song->samples = std::move (smp);

    song->formatName = fc14 ? juce::String ("Future Composer 1.4 (FC14)") : juce::String::fromUTF8 ("Future Composer 1.0–1.3 (SMOD)");
    finishOrders (*song);
    return song;
}
} // namespace sw
