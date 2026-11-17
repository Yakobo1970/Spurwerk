#include "YmFile.h"

namespace sw
{
namespace
{
// ------------------------------------------------------------------------------------------ LH5 decoder
class LhDecoder
{
public:
    LhDecoder (const juce::uint8* src, size_t srcLen, int dicBits) : in (src), inLen (srcLen), dicbit (dicBits)
    {
        np = dicbit + 1;
        pbit = dicbit >= 16 ? 5 : 4;   // lh6/lh7 use 5 bits
        if (dicbit == 12) pbit = 4;
    }

    bool decode (std::vector<juce::uint8>& out, size_t origSize)
    {
        out.clear();
        out.reserve (origSize);
        bitbuf = 0; subbitbuf = 0; bitcount = 0; blocksize = 0;
        fillbuf (16);
        while (out.size() < origSize)
        {
            if (failed) return false;
            const int c = decodeC();
            if (failed) return false;
            if (c <= 255) out.push_back ((juce::uint8) c);
            else
            {
                const int len = c - (256 - 3);
                const int dist = decodeP() + 1;
                if (failed) return false;
                // LHA pre-fills the dictionary with spaces
                for (int k = 0; k < len && out.size() < origSize; ++k)
                {
                    const long long from = (long long) out.size() - dist;
                    out.push_back (from < 0 ? (juce::uint8) ' ' : out[(size_t) from]);
                }
            }
        }
        return true;
    }

private:
    static constexpr int NC = 510, NT = 19, TBIT = 5, CBIT = 9, NPT = 0x80;
    const juce::uint8* in;
    size_t inLen, inPos = 0;
    int dicbit, np, pbit;
    unsigned bitbuf = 0, subbitbuf = 0;
    int bitcount = 0, blocksize = 0;
    bool failed = false;
    juce::uint16 left[2 * NC - 1] {}, right[2 * NC - 1] {};
    juce::uint8 c_len[NC] {}, pt_len[NPT] {};
    juce::uint16 c_table[4096] {}, pt_table[256] {};

    void fillbuf (int n)
    {
        bitbuf = (bitbuf << n) & 0xffff;
        while (n > bitcount)
        {
            n -= bitcount;
            bitbuf |= (subbitbuf << n) & 0xffff;
            subbitbuf = inPos < inLen ? in[inPos++] : 0;
            bitcount = 8;
        }
        bitcount -= n;
        bitbuf |= subbitbuf >> bitcount;
        bitbuf &= 0xffff;
    }

    unsigned getbits (int n)
    {
        if (n == 0) return 0;
        const unsigned x = bitbuf >> (16 - n);
        fillbuf (n);
        return x;
    }

    void makeTable (int nchar, const juce::uint8* bitlen, int tablebits, juce::uint16* table)
    {
        unsigned count[17] {}, weight[17] {}, start[18] {};
        for (int i = 0; i < nchar; ++i)
            if (bitlen[i] <= 16) count[bitlen[i]]++;
        start[1] = 0;
        for (int i = 1; i <= 16; ++i) start[i + 1] = start[i] + (count[i] << (16 - i));
        if (start[17] != 0x10000u) { failed = true; return; }
        const int jutbits = 16 - tablebits;
        for (int i = 1; i <= tablebits; ++i) { start[i] >>= jutbits; weight[i] = 1u << (tablebits - i); }
        for (int i = tablebits + 1; i <= 16; ++i) weight[i] = 1u << (16 - i);
        {
            unsigned i = start[tablebits + 1] >> jutbits;
            const unsigned k = 1u << tablebits;
            while (i < k) table[i++] = 0;
        }
        int avail = nchar;
        const unsigned mask = 1u << (15 - tablebits);
        for (int ch = 0; ch < nchar; ++ch)
        {
            const int len = bitlen[ch];
            if (len == 0) continue;
            const unsigned nextcode = start[len] + weight[len];
            if (len <= tablebits)
            {
                if (nextcode > (1u << tablebits)) { failed = true; return; }
                for (unsigned j = start[len]; j < nextcode; ++j) table[j] = (juce::uint16) ch;
            }
            else
            {
                unsigned k = start[len];
                juce::uint16* p = &table[k >> jutbits];
                int n = len - tablebits;
                while (n != 0)
                {
                    if (*p == 0)
                    {
                        if (avail >= 2 * NC - 1) { failed = true; return; }
                        right[avail] = left[avail] = 0;
                        *p = (juce::uint16) avail++;
                    }
                    p = (k & mask) ? &right[*p] : &left[*p];
                    k <<= 1;
                    --n;
                }
                *p = (juce::uint16) ch;
            }
            start[len] = nextcode;
        }
    }

    void readPtLen (int nn, int nbit, int iSpecial)
    {
        const int n = (int) getbits (nbit);
        if (n == 0)
        {
            const int c = (int) getbits (nbit);
            for (int i = 0; i < nn; ++i) pt_len[i] = 0;
            for (int i = 0; i < 256; ++i) pt_table[i] = (juce::uint16) c;
            return;
        }
        int i = 0;
        while (i < n && i < NPT)
        {
            int c = (int) (bitbuf >> 13);
            if (c == 7)
            {
                unsigned mask = 1u << 12;
                while ((mask & bitbuf) != 0 && c < 16) { mask >>= 1; ++c; }
            }
            fillbuf (c < 7 ? 3 : c - 3);
            pt_len[i++] = (juce::uint8) c;
            if (i == iSpecial)
            {
                int z = (int) getbits (2);
                while (--z >= 0 && i < NPT) pt_len[i++] = 0;
            }
        }
        while (i < nn) pt_len[i++] = 0;
        makeTable (nn, pt_len, 8, pt_table);
    }

    void readCLen()
    {
        const int n = (int) getbits (CBIT);
        if (n == 0)
        {
            const int c = (int) getbits (CBIT);
            for (int i = 0; i < NC; ++i) c_len[i] = 0;
            for (int i = 0; i < 4096; ++i) c_table[i] = (juce::uint16) c;
            return;
        }
        int i = 0;
        while (i < n && i < NC)
        {
            int c = pt_table[bitbuf >> 8];
            if (c >= NT)
            {
                unsigned mask = 1u << 7;
                int guard = 0;
                do { c = (bitbuf & mask) ? right[c] : left[c]; mask >>= 1; } while (c >= NT && ++guard < 32);
            }
            fillbuf (pt_len[c]);
            if (c <= 2)
            {
                if (c == 0) c = 1;
                else if (c == 1) c = (int) getbits (4) + 3;
                else c = (int) getbits (CBIT) + 20;
                while (--c >= 0 && i < NC) c_len[i++] = 0;
            }
            else c_len[i++] = (juce::uint8) (c - 2);
        }
        while (i < NC) c_len[i++] = 0;
        makeTable (NC, c_len, 12, c_table);
    }

    int decodeC()
    {
        if (blocksize == 0)
        {
            blocksize = (int) getbits (16);
            readPtLen (NT, TBIT, 3);
            readCLen();
            readPtLen (np, pbit, -1);
            if (failed) return 0;
        }
        --blocksize;
        int j = c_table[bitbuf >> 4];
        if (j >= NC)
        {
            unsigned mask = 1u << 3;
            int guard = 0;
            do { j = (bitbuf & mask) ? right[j] : left[j]; mask >>= 1; } while (j >= NC && ++guard < 32);
        }
        fillbuf (c_len[j]);
        return j;
    }

    int decodeP()
    {
        int j = pt_table[bitbuf >> 8];
        if (j >= np)
        {
            unsigned mask = 1u << 7;
            int guard = 0;
            do { j = (bitbuf & mask) ? right[j] : left[j]; mask >>= 1; } while (j >= np && ++guard < 32);
        }
        fillbuf (pt_len[j]);
        if (j != 0) j = (1 << (j - 1)) + (int) getbits (j - 1);
        return j;
    }
};

juce::uint32 le32 (const juce::uint8* p) { return (juce::uint32) p[0] | ((juce::uint32) p[1] << 8) | ((juce::uint32) p[2] << 16) | ((juce::uint32) p[3] << 24); }
juce::uint32 be32 (const juce::uint8* p) { return ((juce::uint32) p[0] << 24) | ((juce::uint32) p[1] << 16) | ((juce::uint32) p[2] << 8) | (juce::uint32) p[3]; }
int be16 (const juce::uint8* p) { return (p[0] << 8) | p[1]; }

// 4 bit ST digidrum sample -> 8 bit level (as used by the ST replay routines)
const juce::uint8 kDrum4To8[16] = { 0, 1, 2, 2, 4, 6, 9, 12, 17, 24, 35, 48, 72, 103, 165, 255 };
} // namespace

bool repairCrLf (juce::MemoryBlock& data, size_t expected)
{
    // files that went through a text-mode transfer have every LF expanded to CR LF
    const auto* d = static_cast<const juce::uint8*> (data.getData());
    const size_t size = data.getSize();
    if (size <= expected) return false;
    size_t crlf = 0;
    for (size_t i = 0; i + 1 < size; ++i)
    {
        if (d[i] == 13 && d[i + 1] == 10) ++crlf;
        else if (d[i + 1] == 10) return false;   // a lone LF: not this kind of damage
    }
    if (crlf == 0 || (size - crlf != expected && size - crlf != expected + 1)) return false;
    juce::MemoryBlock fixed;
    fixed.ensureSize (size);
    juce::MemoryOutputStream os (fixed, false);
    for (size_t i = 0; i < size; ++i)
        if (! (d[i] == 13 && i + 1 < size && d[i + 1] == 10)) os.writeByte ((char) d[i]);
    os.flush();
    data = fixed;
    data.setSize (os.getDataSize());
    return true;
}

bool depackLha (const juce::MemoryBlock& original, juce::MemoryBlock& out, juce::String& error)
{
    juce::MemoryBlock inBlock (original);
    {
        const auto* h = static_cast<const juce::uint8*> (inBlock.getData());
        if (inBlock.getSize() > 24 && h[20] <= 1)
        {
            if (repairCrLf (inBlock, (size_t) h[0] + 2 + le32 (h + 7)))
                error = "repaired";
        }
        else if (inBlock.getSize() > 24)
        {
            // the damage may sit inside the header itself: strip every CR LF and check the result
            juce::MemoryBlock stripped;
            juce::MemoryOutputStream os (stripped, false);
            for (size_t i = 0; i < inBlock.getSize(); ++i)
                if (! (h[i] == 13 && i + 1 < inBlock.getSize() && h[i + 1] == 10)) os.writeByte ((char) h[i]);
            os.flush();
            stripped.setSize (os.getDataSize());
            const auto* sh = static_cast<const juce::uint8*> (stripped.getData());
            const size_t expected = (size_t) sh[0] + 2 + le32 (sh + 7);
            if (stripped.getSize() > 24 && sh[20] <= 1 && (stripped.getSize() == expected || stripped.getSize() == expected + 1))
            {
                inBlock = stripped;
                error = "repaired";
            }
        }
    }
    const auto* d = static_cast<const juce::uint8*> (inBlock.getData());
    const size_t size = inBlock.getSize();
    if (size < 24 || d[2] != '-' || d[3] != 'l' || d[4] != 'h' || d[6] != '-') { error = "Kein LHA-Archiv"; return false; }
    const char method = (char) d[5];
    const int level = d[20];
    const size_t packed = le32 (d + 7), orig = le32 (d + 11);
    size_t dataStart = 0;
    size_t packedSize = packed;
    if (level == 0 || level == 1)
    {
        dataStart = (size_t) d[0] + 2;
        if (level == 1)
        {
            // extended headers follow; their size is included in the packed size
            size_t pos = dataStart;
            int ext = (int) (d[dataStart - 2] | (d[dataStart - 1] << 8));
            while (ext > 0 && pos + (size_t) ext <= size)
            {
                pos += (size_t) ext;
                packedSize -= (size_t) ext;
                ext = (int) (d[pos - 2] | (d[pos - 1] << 8));
            }
            dataStart = pos;
        }
    }
    else if (level == 2)
    {
        dataStart = (size_t) (d[0] | (d[1] << 8));
    }
    else { error = U ("LHA-Header-Level nicht unterstützt"); return false; }

    if (dataStart >= size) { error = "LHA-Daten fehlen"; return false; }
    packedSize = juce::jmin (packedSize, size - dataStart);
    if (orig == 0 || orig > 64 * 1024 * 1024) { error = U ("Ungültige Größe im LHA-Header"); return false; }

    if (method == '0')
    {
        out.replaceAll (d + dataStart, juce::jmin (orig, packedSize));
        return true;
    }
    int dicbit = 13;
    if (method == '4') dicbit = 12;
    else if (method == '5') dicbit = 13;
    else if (method == '6') dicbit = 15;
    else if (method == '7') dicbit = 16;
    else { error = "LHA-Methode -lh" + juce::String::charToString ((juce::juce_wchar) method) + U ("- nicht unterstützt"); return false; }

    auto dec = std::make_unique<LhDecoder> (d + dataStart, packedSize, dicbit);
    std::vector<juce::uint8> buf;
    if (! dec->decode (buf, orig)) { error = "LHA-Daten fehlerhaft (" + juce::String ((int) buf.size()) + "/" + juce::String ((int) orig) + ")"; return false; }
    out.replaceAll (buf.data(), buf.size());
    return true;
}

double ymPeriodToMidi (int period, int clock)
{
    if (period <= 0) return 0.0;
    const double f = (double) clock / (16.0 * period);
    return 69.0 + 12.0 * std::log2 (f / 440.0);
}

YmVoice ymVoice (const YmData& ym, int frame, int ch)
{
    YmVoice v;
    if (frame < 0 || frame >= ym.frames) return v;
    const auto& r = ym.regs[(size_t) frame];
    v.period = r[(size_t) (ch * 2)] | ((r[(size_t) (ch * 2 + 1)] & 0x0f) << 8);
    v.tone = (r[7] & (1 << ch)) == 0;
    v.noise = (r[7] & (8 << ch)) == 0;
    v.env = (r[(size_t) (8 + ch)] & 0x10) != 0;
    v.volume = r[(size_t) (8 + ch)] & 0x0f;
    v.midi = ymPeriodToMidi (v.period, ym.clock);
    v.audible = (v.tone || v.noise) && (v.env || v.volume > 0);
    if (v.tone && v.period < 2 && ! v.noise) v.audible = v.env || v.volume > 0;   // DC level (samples via volume)
    return v;
}

std::unique_ptr<Song> loadYm (const juce::MemoryBlock& raw, juce::String& error)
{
    juce::MemoryBlock data;
    bool repaired = false;
    const auto* r0 = static_cast<const juce::uint8*> (raw.getData());
    if (raw.getSize() > 7 && r0[2] == '-' && r0[3] == 'l' && r0[4] == 'h')
    {
        if (! depackLha (raw, data, error)) return nullptr;
        repaired = error == "repaired";
        error.clear();
    }
    else data = raw;

    const auto* d = static_cast<const juce::uint8*> (data.getData());
    const size_t size = data.getSize();
    if (size < 4 || d[0] != 'Y' || d[1] != 'M') { error = "Keine YM-Datei"; return nullptr; }

    auto s = std::make_unique<Song>();
    s->format = Format::YM;
    auto& ym = s->ym;
    ym.version = juce::String ((const char*) d, 4);
    const char v = (char) d[2];

    if (v == '2' || v == '3')
    {
        const bool v3b = d[3] == 'b';
        const size_t body = size - 4 - (v3b ? 4 : 0);
        ym.frames = (int) (body / 14);
        if (ym.frames <= 0) { error = "YM-Datei leer"; return nullptr; }
        ym.regs.resize ((size_t) ym.frames);
        for (int f = 0; f < ym.frames; ++f)
        {
            auto& rg = ym.regs[(size_t) f];
            rg.fill (0);
            for (int r = 0; r < 14; ++r) rg[(size_t) r] = d[4 + (size_t) r * (size_t) ym.frames + (size_t) f];
        }
        if (v3b) ym.loopFrame = (int) le32 (d + size - 4);
        s->formatName = "Atari ST YM (" + ym.version + ")";
    }
    else if (v == '5' || v == '6')
    {
        if (size < 34 || memcmp (d + 4, "LeOnArD!", 8) != 0) { error = "YM-Header fehlerhaft"; return nullptr; }
        ym.frames = (int) be32 (d + 12);
        const juce::uint32 attr = be32 (d + 16);
        const int nDrums = be16 (d + 20);
        ym.clock = (int) be32 (d + 22);
        ym.rate = juce::jmax (1, be16 (d + 26));
        ym.loopFrame = (int) be32 (d + 28);
        size_t pos = 34 + (size_t) be16 (d + 32);
        ym.drums4bit = (attr & 4) != 0;
        for (int i = 0; i < nDrums && pos + 4 <= size; ++i)
        {
            const size_t len = be32 (d + pos);
            pos += 4;
            if (pos + len > size) { error = "YM-Digidrums fehlerhaft"; return nullptr; }
            std::vector<juce::uint8> dr (d + pos, d + pos + len);
            if (attr & 2) for (auto& b : dr) b ^= 0x80;   // signed -> unsigned
            if (ym.drums4bit) for (auto& b : dr) b = kDrum4To8[b & 15];
            ym.drums.push_back (std::move (dr));
            pos += len;
        }
        auto readNt = [&]() {
            juce::String str;
            while (pos < size && d[pos] != 0) str += (juce::juce_wchar) d[pos++];
            ++pos;
            return str.trim();
        };
        s->title = readNt();
        ym.author = readNt();
        ym.comment = readNt();
        const bool interleaved = (attr & 1) != 0;
        if (ym.frames <= 0 || pos + (size_t) ym.frames * 16 > size) { error = U ("YM-Registerdaten unvollständig"); return nullptr; }
        ym.regs.resize ((size_t) ym.frames);
        for (int f = 0; f < ym.frames; ++f)
        {
            auto& rg = ym.regs[(size_t) f];
            for (int r = 0; r < 16; ++r)
                rg[(size_t) r] = interleaved ? d[pos + (size_t) r * (size_t) ym.frames + (size_t) f] : d[pos + (size_t) f * 16 + (size_t) r];
        }
        s->formatName = "Atari ST YM (" + ym.version + ", " + juce::String (ym.rate) + " Hz)";
    }
    else { error = "YM-Version " + ym.version + U (" wird nicht unterstützt"); return nullptr; }

    if (ym.loopFrame < 0 || ym.loopFrame >= ym.frames) ym.loopFrame = 0;

    // ---- convert to read-only patterns: 1 row = 1 frame, 3 channels
    s->channels = 3;
    s->initialSpeed = 1;
    s->initialTempo = juce::roundToInt (ym.rate * 2.5);
    const int rows = 64;
    const int numPat = (ym.frames + rows - 1) / rows;
    s->patterns.resize ((size_t) numPat);
    YmVoice last[3];
    int lastNote[3] = { 0, 0, 0 };
    for (int p = 0; p < numPat; ++p)
    {
        auto& pat = s->patterns[(size_t) p];
        pat.resize (rows, 3);
        s->orders.push_back (p);
        for (int row = 0; row < rows; ++row)
        {
            const int f = p * rows + row;
            if (f >= ym.frames) break;
            const auto& rg = ym.regs[(size_t) f];
            for (int c = 0; c < 3; ++c)
            {
                auto& cell = pat.at (row, c);
                const auto vc = ymVoice (ym, f, c);
                int note = 0;
                if (vc.audible)
                {
                    if (vc.tone && vc.period > 0) note = juce::jlimit (1, 120, juce::roundToInt (vc.midi) + 1);
                    else note = lastNote[c] != 0 ? lastNote[c] : 49;
                }
                if (vc.audible && (! last[c].audible || note != lastNote[c])) cell.note = (juce::uint8) note;
                else if (! vc.audible && last[c].audible) cell.note = kNoteOff;
                if (vc.audible && (vc.volume != last[c].volume || vc.env != last[c].env || cell.note != 0))
                    cell.vol = (juce::uint8) (vc.env ? 16 : vc.volume);
                if (vc.noise && (! last[c].noise || cell.note != 0)) { cell.fx = ymFxNoise; cell.param = rg[6] & 31; }
                if (vc.env && rg[13] != 0xff && cell.fx == 0) { cell.fx = ymFxEnv; cell.param = rg[13] & 15; }
                // YM5/6 special effects (SID voice, digidrum, sync buzzer) on this voice
                if (ym.version[2] >= '5')
                {
                    const int c1 = (rg[1] >> 4) & 3, c2 = (rg[3] >> 4) & 3;
                    if ((c1 == c + 1 && (rg[6] >> 5) != 0) || (c2 == c + 1 && (rg[8] >> 5) != 0))
                    { cell.fx = ymFxSpecial; cell.param = (juce::uint8) (c1 == c + 1 ? (rg[1] >> 6) & 3 : (ym.version[2] == '5' ? 1 : (rg[3] >> 6) & 3)); }
                }
                last[c] = vc;
                if (note != 0) lastNote[c] = note;
                if (! vc.audible) lastNote[c] = 0;
            }
        }
    }
    // the last pattern may be shorter
    const int lastRows = ym.frames - (numPat - 1) * rows;
    if (numPat > 0 && lastRows < rows)
    {
        auto& lp = s->patterns.back();
        Pattern np;
        np.resize (lastRows, 3);
        for (int r = 0; r < lastRows; ++r)
            for (int c = 0; c < 3; ++c) np.at (r, c) = lp.at (r, c);
        lp = np;
    }
    s->restartPos = ym.loopFrame / rows;
    if (repaired) s->formatName += U (" – repariert (CR/LF)");
    return s;
}
} // namespace sw
