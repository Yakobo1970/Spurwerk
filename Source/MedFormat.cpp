// Spurwerk – MED / OctaMED loader (MMD0, MMD1, MMD2, MMD3), written from the public MMD format description.
// Plain samples only: synth and hybrid instruments (and multi-octave IFF samples) stay silent.
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
    juce::String str (size_t o, size_t len) const
    {
        juce::String s;
        for (size_t i = 0; i < len; ++i)
        {
            const int c = u8 (o + i);
            if (c == 0) break;
            s += (juce::juce_wchar) (c >= 32 && c < 127 ? c : (c >= 160 ? c : ' '));
        }
        return s.trimEnd();
    }
};

// MED effect -> common cell effect (XM numbering). mmd0: only 4-bit commands.
void medEffect (int cmd, int p, bool volHex, int speed, Cell& c)
{
    auto set = [&c] (int fx, int prm) { c.fx = (juce::uint8) fx; c.param = (juce::uint8) prm; };
    const int hi = p >> 4, lo = p & 15;
    switch (cmd)
    {
        case 0x00: if (p) set (0x0, p); break;                       // arpeggio
        case 0x01: set (0x1, p); break;                              // slide up
        case 0x02: set (0x2, p); break;                              // slide down
        case 0x03: set (0x3, p); break;                              // portamento to note
        case 0x04: set (0x4, p); break;                              // vibrato
        case 0x05: set (0x5, p); break;                              // porta + volume slide
        case 0x06: set (0x6, p); break;                              // vibrato + volume slide
        case 0x07: set (0x7, p); break;                              // tremolo
        case 0x09: if (p >= 1 && p <= 32) set (fxSpeed, p); break;   // secondary tempo = ticks per row
        case 0x0A: case 0x0D: set (0xA, p); break;                   // volume slide
        case 0x0B: set (0xB, p); break;                              // position jump
        case 0x0C:                                                   // volume (decimal digits unless the song uses hex volumes)
            set (0xC, juce::jmin (64, volHex ? p : hi * 10 + lo));
            break;
        case 0x0F:
            if (p == 0) set (0xD, 0);                                // next block
            else if (p < 3) set (fxTempo, 0x70);                     // OctaMED quirk (as in OpenMPT)
            else if (p <= 0xF0) set (fxMedTempo, p);                 // tempo
            else if (p == 0xF1) set (0xE, 0x90 | juce::jlimit (1, 15, speed / 2));   // play note twice
            else if (p == 0xF2) set (0xE, 0xD0 | juce::jlimit (1, 15, speed / 2));   // delay note by half a row
            else if (p == 0xF3) set (0xE, 0x90 | juce::jlimit (1, 15, speed / 3));   // play note three times
            else if (p == 0xFF) set (0xE, 0xC0);                     // note off
            break;
        case 0x11: set (0xE, 0x10 | juce::jmin (15, p)); break;      // fine slide up
        case 0x12: set (0xE, 0x20 | juce::jmin (15, p)); break;      // fine slide down
        case 0x14: set (0x4, p); break;                              // ProTracker vibrato
        case 0x15: set (0xE, 0x50 | (p & 15)); break;                // finetune
        case 0x16: set (0xE, 0x60 | juce::jmin (15, p)); break;      // loop
        case 0x18: set (0xE, 0xC0 | juce::jmin (15, p)); break;      // note cut
        case 0x19: set (0x9, p); break;                              // sample offset
        case 0x1A: set (0xE, 0xA0 | juce::jmin (15, p)); break;      // fine volume up
        case 0x1B: set (0xE, 0xB0 | juce::jmin (15, p)); break;      // fine volume down
        case 0x1D: set (0xD, p); break;                              // next pattern at row
        case 0x1E: set (0xE, 0xE0 | juce::jmin (15, p)); break;      // pattern delay
        case 0x1F: if (hi) set (0xE, 0xD0 | hi); else if (lo) set (0xE, 0x90 | lo); break;   // note delay / retrigger
        case 0x2E: set (0x8, juce::jlimit (0, 255, ((juce::int8) p + 16) * 255 / 32)); break;  // track panning -16..16
        default: break;
    }
}
} // namespace

bool isMed (const juce::MemoryBlock& d)
{
    Be r (d);
    return r.n >= 52 && r.p[0] == 'M' && r.p[1] == 'M' && r.p[2] == 'D' && r.p[3] >= '0' && r.p[3] <= '3';
}

std::unique_ptr<Song> loadMed (const juce::MemoryBlock& d, juce::String& error)
{
    Be r (d);
    if (! isMed (d)) { error = "Keine OctaMED-Datei"; return nullptr; }
    const int ver = r.p[3] - '0';
    const size_t songOff = r.u32 (8), blockArr = r.u32 (16), smplArr = r.u32 (24), expOff = r.u32 (32);
    if (! r.has (songOff, 788)) { error = U ("OctaMED-Datei unvollständig"); return nullptr; }

    auto song = std::make_unique<Song>();
    song->format = Format::MED;
    const int numBlocks = r.u16 (songOff + 504);
    const int songLen = r.u16 (songOff + 506);
    const int defTempo = r.u16 (songOff + 764);
    const int playTransp = r.s8 (songOff + 766);
    const int flags = r.u8 (songOff + 767), flags2 = r.u8 (songOff + 768);
    const int tempo2 = r.u8 (songOff + 769);
    const int numSamples = juce::jmin (63, r.u8 (songOff + 787));
    const bool volHex = (flags & 0x10) != 0;
    song->medLinesPerBeat = (flags2 & 0x20) ? (flags2 & 0x1F) + 1 : 0;
    song->med8Ch = (flags & 0x40) != 0;
    song->medMixing = (flags2 & 0x80) != 0;

    // orders: MMD0/1 one play sequence, MMD2/3 sections of play sequences (commands >= 0x8000 skipped)
    if (ver < 2)
        for (int i = 0; i < juce::jmin (songLen, 256); ++i) song->orders.push_back (r.u8 (songOff + 508 + (size_t) i));
    else
    {
        const size_t pseqTable = r.u32 (songOff + 508), sectionTable = r.u32 (songOff + 512);
        const int numPseqs = r.u16 (songOff + 522);
        for (int s = 0; s < songLen && song->orders.size() < 1024; ++s)
        {
            const int ps = sectionTable ? r.u16 (sectionTable + (size_t) s * 2) : s;
            if (ps >= numPseqs) continue;
            const size_t seq = r.u32 (pseqTable + (size_t) ps * 4);
            if (! r.has (seq, 42)) continue;
            const int len = r.u16 (seq + 40);
            for (int k = 0; k < len; ++k)
            {
                const int b = r.u16 (seq + 42 + (size_t) k * 2);
                if (b < 0x8000) song->orders.push_back (b);
            }
        }
    }

    // blocks
    int numCh = 1;
    std::vector<std::pair<size_t, std::pair<int, int>>> blocks;   // offset, tracks, rows
    for (int b = 0; b < numBlocks; ++b)
    {
        const size_t bo = r.u32 (blockArr + (size_t) b * 4);
        int tracks = 0, rows = 0;
        if (bo != 0 && r.has (bo, 8))
        {
            if (ver == 0) { tracks = r.u8 (bo); rows = r.u8 (bo + 1) + 1; }
            else { tracks = r.u16 (bo); rows = r.u16 (bo + 2) + 1; }
        }
        numCh = juce::jmax (numCh, tracks);
        blocks.push_back ({ bo, { tracks, rows } });
    }
    numCh = juce::jlimit (1, kMaxChannels, numCh);
    song->channels = numCh;
    for (auto& [bo, tr] : blocks)
    {
        Pattern pat;
        const int tracks = tr.first, rows = juce::jlimit (1, 3200, tr.second);
        pat.resize (rows, numCh);
        if (bo != 0)
        {
            const size_t data = bo + (ver == 0 ? 2 : 8);
            const size_t cellSize = ver == 0 ? 3 : 4;
            for (int row = 0; row < rows; ++row)
                for (int c = 0; c < tracks; ++c)
                {
                    const size_t o = data + ((size_t) row * (size_t) tracks + (size_t) c) * cellSize;
                    if (! r.has (o, cellSize)) break;
                    int note, ins, cmd, prm;
                    if (ver == 0)
                    {
                        const int b0 = r.u8 (o), b1 = r.u8 (o + 1);
                        note = b0 & 0x3F;
                        ins = (b1 >> 4) | ((b0 & 0x80) >> 3) | ((b0 & 0x40) >> 1);
                        cmd = b1 & 0x0F;
                        prm = r.u8 (o + 2);
                    }
                    else { note = r.u8 (o) & 0x7F; ins = r.u8 (o + 1) & 0x3F; cmd = r.u8 (o + 2); prm = r.u8 (o + 3); }
                    if (c >= numCh) continue;
                    Cell& cell = pat.at (row, c);
                    if (note > 0 && note <= 0x7F)
                    {
                        const int strans = ins >= 1 && ins <= 63 ? r.s8 (songOff + (size_t) (ins - 1) * 8 + 7) : 0;
                        cell.note = (juce::uint8) juce::jlimit (1, 120, note + 48 + playTransp + strans);   // MED C-1 = ProTracker C-1
                    }
                    cell.instr = (juce::uint8) ins;
                    medEffect (cmd, prm, volHex, tempo2 > 0 ? tempo2 : 6, cell);
                }
        }
        song->patterns.push_back (std::move (pat));
    }

    // samples (song sample table + instrument headers + optional extension data)
    size_t expSmp = 0, iinfo = 0;
    int expSmpCount = 0, expSmpSize = 0, iinfoCount = 0, iinfoSize = 0;
    if (expOff != 0 && r.has (expOff, 52))
    {
        expSmp = r.u32 (expOff + 4); expSmpCount = r.u16 (expOff + 8); expSmpSize = r.u16 (expOff + 10);
        const size_t anno = r.u32 (expOff + 12), annoLen = r.u32 (expOff + 16);
        iinfo = r.u32 (expOff + 20); iinfoCount = r.u16 (expOff + 24); iinfoSize = r.u16 (expOff + 26);
        if (anno != 0 && annoLen > 0 && r.has (anno, juce::jmin (annoLen, (size_t) 65536)))
        {
            juce::String m;
            for (size_t i = 0; i < juce::jmin (annoLen, (size_t) 65536); ++i)
            {
                const int ch = r.u8 (anno + i);
                if (ch == 0) break;
                if (ch == '\n' || ch == '\r') m << "\n"; else if (ch >= 32) m += (juce::juce_wchar) ch;
            }
            song->message = m.trim();
        }
        const size_t sn = r.u32 (expOff + 44), snLen = r.u32 (expOff + 48);
        if (sn != 0 && snLen > 0) song->title = r.str (sn, juce::jmin (snLen, (size_t) 255));
    }
    for (int i = 0; i < numSamples; ++i)
    {
        Sample s;
        const size_t so = songOff + (size_t) i * 8;
        const int rep = r.u16 (so), replen = r.u16 (so + 2);
        s.volume = juce::jmin (64, r.u8 (so + 6));
        if (iinfo != 0 && i < iinfoCount && iinfoSize >= 40) s.name = r.str (iinfo + (size_t) i * (size_t) iinfoSize, 40);
        if (expSmp != 0 && i < expSmpCount && expSmpSize >= 4)
        {
            const int ft = r.s8 (expSmp + (size_t) i * (size_t) expSmpSize + 3);
            s.finetune = juce::jlimit (-8, 7, ft);
        }
        const size_t ih = smplArr != 0 ? r.u32 (smplArr + (size_t) i * 4) : 0;
        if (ih != 0 && r.has (ih, 6))
        {
            const juce::uint32 len = r.u32 (ih);
            const int type = r.s16 (ih + 4);
            const bool is16 = type >= 0 && (type & 0x10) != 0, stereo = type >= 0 && (type & 0x20) != 0;
            if (type >= 0 && (type & 0x0F) == 0 && len > 0)
            {
                const size_t data = ih + 6;
                const size_t bytes = juce::jmin ((size_t) len, r.n - juce::jmin (r.n, data));
                const size_t frameBytes = (is16 ? 2u : 1u);
                size_t frames = bytes / frameBytes;
                if (stereo) frames /= 2;   // stereo: left block, then right block
                s.pcm.resize (frames);
                for (size_t k = 0; k < frames; ++k)
                {
                    float v = is16 ? (float) r.s16 (data + k * 2) / 32768.0f : (float) r.s8 (data + k) / 128.0f;
                    if (stereo)
                    {
                        const size_t k2 = k + frames;
                        v = 0.5f * (v + (is16 ? (float) r.s16 (data + k2 * 2) / 32768.0f : (float) r.s8 (data + k2) / 128.0f));
                    }
                    s.pcm[k] = v;
                }
                if (replen > 1)
                {
                    const juce::int64 ls = (juce::int64) rep * 2, ll = (juce::int64) replen * 2;
                    if (ls < (juce::int64) frames) { s.loopStart = (int) ls; s.loopLength = (int) juce::jmin (ll, (juce::int64) frames - ls); }
                }
            }
        }
        song->samples.push_back (std::move (s));
    }
    while (! song->samples.empty() && song->samples.back().pcm.empty() && song->samples.back().name.isEmpty()) song->samples.pop_back();

    // track volumes (MMD0/1: song table, MMD2/3: pointer)
    if (ver < 2) for (int c = 0; c < juce::jmin (numCh, 16); ++c) { const int v = r.u8 (songOff + 770 + (size_t) c); song->channelVolInit.push_back (v == 0 ? 64 : juce::jmin (64, v)); }   // valid 1..64
    else
    {
        const size_t tv = r.u32 (songOff + 516), tp = r.u32 (songOff + 524);
        const int nt = r.u16 (songOff + 520);
        for (int c = 0; c < numCh; ++c) { const int v = tv != 0 && c < nt ? r.u8 (tv + (size_t) c) : 64; song->channelVolInit.push_back (v == 0 ? 64 : juce::jmin (64, v)); }
        if (tp != 0)
            for (int c = 0; c < numCh; ++c) song->channelPanInit.push_back (c < nt ? juce::jlimit (-1.0f, 1.0f, (float) r.s8 (tp + (size_t) c) / 16.0f) : 0.0f);
    }
    if (song->channelPanInit.empty() && numCh > 4)
        for (int c = 0; c < numCh; ++c) song->channelPanInit.push_back ((float) Song::channelPan (c) * 0.6f);

    song->initialSpeed = juce::jlimit (1, 32, tempo2 > 0 ? tempo2 : 6);
    song->initialTempo = song->medTempo (defTempo > 0 ? defTempo : 33);
    song->periodMode = 0;
    song->stereoSeparation = 100;
    song->formatName = (ver == 0 ? juce::String ("MED / OctaMED (MMD0, ") : "OctaMED (MMD" + juce::String (ver) + ", ") + juce::String (numCh) + U (" Spuren)")
                     + (song->medLinesPerBeat > 0 && ! song->med8Ch ? U (" – BPM-Modus") : song->med8Ch ? U (" – 8-Kanal-Modus") : juce::String());
    // orders pointing at missing blocks, empty song
    if (song->orders.empty()) song->orders.push_back (0);
    for (auto& o : song->orders) if (o >= (int) song->patterns.size()) o = 0;
    if (song->patterns.empty()) { Pattern p; p.resize (64, numCh); song->patterns.push_back (std::move (p)); }
    return song;
}
} // namespace sw
