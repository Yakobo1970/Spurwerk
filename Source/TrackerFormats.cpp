// Spurwerk – loaders for FastTracker 2 (.xm), Scream Tracker 3 (.s3m), Impulse Tracker (.it), MultiTracker (.mtm), Composer 669 (.669), Farandole (.far), Digitrakker (.mdl) and Oktalyzer (.okt).
// Written from the public format descriptions; all data is converted into the common Song model.
#include "TrackerFormats.h"
#include <cstring>
#include <map>

namespace sw
{
namespace
{
struct Reader
{
    const juce::uint8* p;
    size_t n;
    explicit Reader (const juce::MemoryBlock& d) : p (static_cast<const juce::uint8*> (d.getData())), n (d.getSize()) {}
    bool has (size_t off, size_t len) const { return off <= n && len <= n - off; }
    int u8 (size_t o) const { return o < n ? p[o] : 0; }
    int s8 (size_t o) const { return (juce::int8) u8 (o); }
    int le16 (size_t o) const { return u8 (o) | (u8 (o + 1) << 8); }
    juce::uint32 le32 (size_t o) const { return (juce::uint32) le16 (o) | ((juce::uint32) le16 (o + 2) << 16); }
    int be16 (size_t o) const { return (u8 (o) << 8) | u8 (o + 1); }
    juce::uint32 be32 (size_t o) const { return ((juce::uint32) be16 (o) << 16) | (juce::uint32) be16 (o + 2); }
    juce::String str (size_t o, int len) const
    {
        juce::String s;
        for (int i = 0; i < len; ++i)
        {
            const int c = u8 (o + (size_t) i);
            if (c == 0) break;
            s += (juce::juce_wchar) (c >= 32 && c < 127 ? c : (c >= 160 ? c : ' '));
        }
        return s.trimEnd();
    }
    bool tag (size_t o, const char* t) const { const auto l = std::strlen (t); return has (o, l) && std::memcmp (p + o, t, l) == 0; }
};

// ping-pong loops are unrolled into a forward loop (loop + reversed loop)
void unrollPingPong (Sample& s)
{
    if (! s.hasLoop()) return;
    const int a = s.loopStart, len = s.loopLength;
    s.pcm.resize ((size_t) (a + len));
    for (int i = len - 2; i >= 1; --i) s.pcm.push_back (s.pcm[(size_t) (a + i)]);
    s.loopLength = (int) s.pcm.size() - a;
}

void setLoop (Sample& s, juce::int64 start, juce::int64 length)
{
    const juce::int64 n = (juce::int64) s.pcm.size();
    if (length <= 2 || start >= n) { s.loopStart = s.loopLength = 0; return; }
    if (start + length > n) length = n - start;
    s.loopStart = (int) start;
    s.loopLength = (int) length;
}

void fixOrders (Song& song)
{
    int maxPat = -1;
    for (int o : song.orders) maxPat = juce::jmax (maxPat, o);
    while ((int) song.patterns.size() <= maxPat)
    {
        Pattern p;
        p.resize (64, song.channels);
        song.patterns.push_back (std::move (p));
    }
    if (song.orders.empty())
    {
        song.orders.push_back (0);
        if (song.patterns.empty()) { Pattern p; p.resize (64, song.channels); song.patterns.push_back (std::move (p)); }
    }
    song.restartPos = juce::jlimit (0, (int) song.orders.size() - 1, song.restartPos);
}
} // namespace

// ============================================================================================ S3M
bool isS3m (const juce::MemoryBlock& d)
{
    Reader r (d);
    return r.n >= 0x60 && r.tag (0x2C, "SCRM");
}

std::unique_ptr<Song> loadS3m (const juce::MemoryBlock& d, juce::String& error)
{
    Reader r (d);
    if (! isS3m (d)) { error = "Keine S3M-Datei"; return nullptr; }
    auto song = std::make_unique<Song>();
    song->format = Format::S3M;
    song->title = r.str (0, 28);
    const int ordNum = r.le16 (0x20), insNum = r.le16 (0x22), patNum = r.le16 (0x24);
    const int cwtv = r.le16 (0x28), ffi = r.le16 (0x2A);
    const int gv = r.u8 (0x30), is = r.u8 (0x31), it = r.u8 (0x32), mv = r.u8 (0x33), dp = r.u8 (0x35);
    const bool stereo = (mv & 0x80) != 0;

    // channels: compact the enabled PCM channels
    int chMap[32];
    int numCh = 0;
    for (int c = 0; c < 32; ++c)
    {
        const int set = r.u8 (0x40 + (size_t) c);
        chMap[c] = (set < 16) ? numCh++ : -1;
    }
    if (numCh == 0) { error = U ("S3M ohne PCM-Kanäle (nur AdLib wird nicht unterstützt)"); return nullptr; }
    numCh = juce::jmin (numCh, kMaxChannels);
    song->channels = numCh;
    const size_t insPtrOff = 0x60 + (size_t) ordNum, patPtrOff = insPtrOff + (size_t) insNum * 2, panOff = patPtrOff + (size_t) patNum * 2;
    song->channelPanInit.assign ((size_t) numCh, 0.0f);
    for (int c = 0; c < 32; ++c)
    {
        if (chMap[c] < 0 || chMap[c] >= numCh) continue;
        const int set = r.u8 (0x40 + (size_t) c);
        float pan = stereo ? (set < 8 ? -0.6f : 0.6f) : 0.0f;
        if (dp == 0xFC)
        {
            const int pv = r.u8 (panOff + (size_t) c);
            if (pv & 0x20) pan = stereo ? (float) (pv & 15) / 7.5f - 1.0f : 0.0f;
        }
        song->channelPanInit[(size_t) chMap[c]] = pan;
    }

    // orders: 255 = end, 254 = marker (dropped; jump targets are remapped)
    std::vector<int> orderMap;
    for (int i = 0; i < ordNum; ++i)
    {
        const int o = r.u8 (0x60 + (size_t) i);
        orderMap.push_back ((int) song->orders.size());
        if (o == 255) break;
        if (o == 254) continue;
        song->orders.push_back (o);
    }

    // patterns
    for (int pi = 0; pi < patNum; ++pi)
    {
        Pattern pat;
        pat.resize (64, numCh);
        const size_t ptr = (size_t) r.le16 (patPtrOff + (size_t) pi * 2) * 16;
        if (ptr != 0 && r.has (ptr, 2))
        {
            const size_t end = juce::jmin (r.n, ptr + (size_t) juce::jmax (2, r.le16 (ptr)) + 2);
            size_t o = ptr + 2;
            for (int row = 0; row < 64 && o < end;)
            {
                const int what = r.u8 (o++);
                if (what == 0) { ++row; continue; }
                const int c = chMap[what & 31];
                Cell cell;
                if (what & 32)
                {
                    const int n = r.u8 (o), ins = r.u8 (o + 1);
                    o += 2;
                    if (n == 254) cell.note = kNoteCut;
                    else if (n < 0xF0) cell.note = (juce::uint8) juce::jlimit (1, 120, (n >> 4) * 12 + (n & 15) + 13);
                    cell.instr = (juce::uint8) ins;
                }
                if (what & 64)
                {
                    const int v = r.u8 (o++);
                    if (v <= 64) cell.vol = (juce::uint8) v;
                }
                if (what & 128)
                {
                    const int cmd = r.u8 (o), info = r.u8 (o + 1);
                    o += 2;
                    if (cmd >= 1 && cmd <= 26) { cell.fx = (juce::uint8) cmd; cell.param = (juce::uint8) info; }
                    if (cmd == 2) cell.param = (juce::uint8) (info < (int) orderMap.size() ? orderMap[(size_t) info] : info);
                }
                if (c >= 0 && c < numCh) pat.at (row, c) = cell;
            }
        }
        song->patterns.push_back (std::move (pat));
    }

    // samples
    for (int i = 0; i < insNum; ++i)
    {
        Sample s;
        const size_t h = (size_t) r.le16 (insPtrOff + (size_t) i * 2) * 16;
        if (h != 0 && r.has (h, 80))
        {
            s.name = r.str (h + 48, 28);
            if (s.name.isEmpty()) s.name = r.str (h + 1, 12);
            s.volume = juce::jmin (64, r.u8 (h + 28));
            const juce::uint32 c2 = r.le32 (h + 32);
            s.c5Speed = c2 > 0 ? (double) c2 : 8363.0;
            if (r.u8 (h) == 1)
            {
                const size_t data = (((size_t) r.u8 (h + 13) << 16) | (size_t) r.le16 (h + 14)) * 16;
                const int flags = r.u8 (h + 31);
                const bool is16 = flags & 4, st = flags & 2, sgn = ffi == 1;
                juce::int64 len = r.le32 (h + 16);
                const int bps = is16 ? 2 : 1;
                const juce::int64 avail = data < r.n ? (juce::int64) ((r.n - data) / (size_t) (bps * (st ? 2 : 1))) : 0;
                len = juce::jlimit ((juce::int64) 0, avail, len);
                s.pcm.resize ((size_t) len);
                auto get = [&] (juce::int64 k) -> float {
                    const size_t o = data + (size_t) k * (size_t) bps;
                    if (is16) { int v = r.le16 (o); if (! sgn) v -= 32768; else if (v >= 32768) v -= 65536; return (float) v / 32768.0f; }
                    int v = r.u8 (o); if (! sgn) v -= 128; else if (v >= 128) v -= 256; return (float) v / 128.0f;
                };
                for (juce::int64 k = 0; k < len; ++k)
                    s.pcm[(size_t) k] = st ? 0.5f * (get (k) + get (k + len)) : get (k);
                if (flags & 1)
                {
                    const juce::int64 a = r.le32 (h + 20), b = r.le32 (h + 24);
                    setLoop (s, a, b - a);
                }
            }
        }
        song->samples.push_back (std::move (s));
    }

    song->initialSpeed = (is == 0 || is == 255) ? 6 : is;
    song->initialTempo = it < 32 ? 125 : it;
    song->globalVolume = juce::jmin (64, gv == 0 ? 64 : gv);
    song->periodMode = 1;
    song->stereoSeparation = 100;
    const int tracker = cwtv >> 12;
    song->formatName = juce::String ("Scream Tracker 3 (S3M") + (tracker == 1 ? juce::String (")") : tracker == 3 ? juce::String (", Impulse Tracker)")
                                                                           : tracker == 5 ? juce::String (", OpenMPT)") : juce::String (")"));
    fixOrders (*song);
    return song;
}

// ============================================================================================ XM
bool isXm (const juce::MemoryBlock& d)
{
    Reader r (d);
    return r.n >= 80 && juce::String ((const char*) r.p, 16).equalsIgnoreCase ("Extended Module:");
}

std::unique_ptr<Song> loadXm (const juce::MemoryBlock& d, juce::String& error)
{
    Reader r (d);
    if (! isXm (d)) { error = "Keine XM-Datei"; return nullptr; }
    auto song = std::make_unique<Song>();
    song->format = Format::XM;
    song->title = r.str (17, 20);
    const juce::String tracker = r.str (38, 20);
    const size_t hdrSize = r.le32 (60);
    const int songLen = juce::jmin (256, r.le16 (64)), restart = r.le16 (66), nch = r.le16 (68);
    const int npat = r.le16 (70), nins = r.le16 (72), flags = r.le16 (74);
    const int speed = r.le16 (76), bpm = r.le16 (78);
    if (nch < 1) { error = U ("XM ohne Kanäle"); return nullptr; }
    const int numCh = juce::jmin (nch, kMaxChannels);
    song->channels = numCh;
    for (int i = 0; i < songLen; ++i) song->orders.push_back (r.u8 (80 + (size_t) i));
    song->restartPos = restart;

    size_t pos = 60 + hdrSize;
    for (int pi = 0; pi < npat; ++pi)
    {
        if (! r.has (pos, 9)) { error = U ("XM: Patterns unvollständig"); return nullptr; }
        const size_t phl = r.le32 (pos);
        int rows = r.le16 (pos + 5);
        const size_t psize = (size_t) r.le16 (pos + 7);
        if (rows <= 0 || rows > 256) rows = 64;
        Pattern pat;
        pat.resize (rows, numCh);
        size_t o = pos + phl;
        const size_t end = juce::jmin (r.n, o + psize);
        if (psize > 0)
            for (int row = 0; row < rows; ++row)
                for (int c = 0; c < nch; ++c)
                {
                    if (o >= end) break;
                    int b = r.u8 (o++);
                    int note = 0, ins = 0, vol = 0, fx = 0, prm = 0;
                    if (b & 0x80)
                    {
                        if (b & 1) note = r.u8 (o++);
                        if (b & 2) ins = r.u8 (o++);
                        if (b & 4) vol = r.u8 (o++);
                        if (b & 8) fx = r.u8 (o++);
                        if (b & 16) prm = r.u8 (o++);
                    }
                    else
                    {
                        note = b; ins = r.u8 (o); vol = r.u8 (o + 1); fx = r.u8 (o + 2); prm = r.u8 (o + 3);
                        o += 4;
                    }
                    if (c >= numCh) continue;
                    Cell cell;
                    if (note == 97) cell.note = kNoteOff;
                    else if (note >= 1 && note <= 96) cell.note = (juce::uint8) (note + 12);
                    cell.instr = (juce::uint8) ins;
                    if (vol >= 0x10 && vol <= 0x50) cell.vol = (juce::uint8) (vol - 0x10);
                    else if (vol >= 0x60)
                    {
                        static const juce::uint8 map[10] = { vcSlideDown, vcSlideUp, vcFineDown, vcFineUp, vcVibSpeed, vcVibDepth, vcPan, vcPanLeft, vcPanRight, vcPorta };
                        cell.vcmd = map[(vol >> 4) - 6];
                        cell.vol = (juce::uint8) (vol & 15);
                    }
                    if (fx <= 35) { cell.fx = (juce::uint8) fx; cell.param = (juce::uint8) prm; }
                    pat.at (row, c) = cell;
                }
        song->patterns.push_back (std::move (pat));
        pos = pos + phl + psize;
    }

    // instruments + samples
    for (int ii = 0; ii < nins; ++ii)
    {
        if (! r.has (pos, 29)) break;
        const size_t isz = juce::jmax ((size_t) 29, (size_t) r.le32 (pos));
        Instrument ins;
        ins.name = r.str (pos + 4, 22);
        const int ns = r.le16 (pos + 27);
        size_t shsz = 40;
        std::array<int, 96> keymap {};
        if (ns > 0 && isz >= 241)
        {
            shsz = juce::jmax ((size_t) 40, (size_t) r.le32 (pos + 29));
            for (int k = 0; k < 96; ++k) keymap[(size_t) k] = r.u8 (pos + 33 + (size_t) k);
            auto readEnv = [&] (Envelope& e, size_t ptsOff, int count, int sus, int ls, int le, int type) {
                count = juce::jmin (12, count);
                for (int k = 0; k < count; ++k)
                    e.points.push_back ({ r.le16 (ptsOff + (size_t) k * 4), juce::jlimit (0, 64, r.le16 (ptsOff + (size_t) k * 4 + 2)) });
                e.on = (type & 1) && count > 0;
                e.sustain = (type & 2) != 0; e.loop = (type & 4) != 0;
                e.sus = e.susEnd = juce::jlimit (0, juce::jmax (0, count - 1), sus);
                e.loopStart = juce::jlimit (0, juce::jmax (0, count - 1), ls);
                e.loopEnd = juce::jlimit (0, juce::jmax (0, count - 1), le);
            };
            readEnv (ins.vol, pos + 129, r.u8 (pos + 225), r.u8 (pos + 227), r.u8 (pos + 228), r.u8 (pos + 229), r.u8 (pos + 233));
            readEnv (ins.pan, pos + 177, r.u8 (pos + 226), r.u8 (pos + 230), r.u8 (pos + 231), r.u8 (pos + 232), r.u8 (pos + 234));
            ins.vibType = r.u8 (pos + 235); ins.vibSweep = r.u8 (pos + 236); ins.vibDepth = r.u8 (pos + 237); ins.vibRate = r.u8 (pos + 238);
            ins.fadeout = r.le16 (pos + 239);
        }
        pos += isz;
        const int base = (int) song->samples.size();
        struct Hdr { juce::uint32 len, ls, ll; int type, adpcm; };
        std::vector<Hdr> hdrs;
        for (int k = 0; k < ns; ++k)
        {
            const size_t h = pos + (size_t) k * shsz;
            Sample s;
            s.name = r.str (h + 18, 22);
            if (s.name.isEmpty()) s.name = ins.name;
            s.volume = juce::jmin (64, r.u8 (h + 12));
            const int ft = r.s8 (h + 13), rel = r.s8 (h + 16);
            s.c5Speed = 8363.0 * std::pow (2.0, (rel * 128 + ft) / 1536.0);
            s.pan = r.u8 (h + 15);
            hdrs.push_back ({ r.le32 (h), r.le32 (h + 4), r.le32 (h + 8), r.u8 (h + 14), r.u8 (h + 17) });
            song->samples.push_back (std::move (s));
        }
        pos += (size_t) ns * shsz;
        for (int k = 0; k < ns; ++k)
        {
            auto& s = song->samples[(size_t) (base + k)];
            const auto& hd = hdrs[(size_t) k];
            const bool is16 = hd.type & 16;
            if (hd.adpcm == 0xAD && ! is16) { pos += (hd.len + 1) / 2 + 16; continue; }   // ModPlug ADPCM: skipped
            const size_t bytes = juce::jmin ((size_t) hd.len, pos < r.n ? r.n - pos : 0);
            if (is16)
            {
                int old = 0;
                s.pcm.resize (bytes / 2);
                for (size_t q = 0; q < bytes / 2; ++q)
                {
                    old = (juce::int16) (old + r.le16 (pos + q * 2));
                    s.pcm[q] = (float) old / 32768.0f;
                }
            }
            else
            {
                int old = 0;
                s.pcm.resize (bytes);
                for (size_t q = 0; q < bytes; ++q)
                {
                    old = (juce::int8) (old + r.u8 (pos + q));
                    s.pcm[q] = (float) old / 128.0f;
                }
            }
            pos += hd.len;
            const int div = is16 ? 2 : 1;
            if ((hd.type & 3) != 0) setLoop (s, hd.ls / (juce::uint32) div, hd.ll / (juce::uint32) div);
            if ((hd.type & 3) == 2) unrollPingPong (s);
        }
        for (int n = 0; n < 120; ++n)
        {
            const int k = keymap[(size_t) juce::jlimit (0, 95, n - 12)];
            ins.map[(size_t) n] = (short) (k < ns ? base + k : -1);
        }
        song->instruments.push_back (std::move (ins));
    }

    song->initialSpeed = speed > 0 && speed < 32 ? speed : 6;
    song->initialTempo = bpm >= 32 ? juce::jmin (255, bpm) : 125;
    song->periodMode = (flags & 1) ? 2 : 1;
    song->stereoSeparation = 100;
    song->channelPanInit.assign ((size_t) numCh, 0.0f);
    song->formatName = "FastTracker 2 (XM" + (tracker.isNotEmpty() ? ", " + tracker : juce::String()) + ")";
    fixOrders (*song);
    return song;
}

// ============================================================================================ OKT
bool isOkt (const juce::MemoryBlock& d)
{
    Reader r (d);
    return r.tag (0, "OKTASONG");
}

std::unique_ptr<Song> loadOkt (const juce::MemoryBlock& d, juce::String& error)
{
    Reader r (d);
    if (! isOkt (d)) { error = "Keine Oktalyzer-Datei"; return nullptr; }
    auto song = std::make_unique<Song>();
    song->format = Format::OKT;
    bool split[4] = { false, false, false, false };
    int numOrders = 0, numPatterns = 0;
    struct SmpHdr { juce::uint32 len; int ls, ll, vol; };
    std::vector<SmpHdr> hdrs;
    std::vector<int> orderBytes;
    size_t sbodNext = 0;
    size_t pos = 8;
    while (r.has (pos, 8))
    {
        const juce::String id ((const char*) r.p + pos, 4);
        const size_t len = r.be32 (pos + 4);
        const size_t data = pos + 8;
        if (! r.has (data, 0)) break;
        if (id == "CMOD")
        {
            for (int i = 0; i < 4; ++i) split[i] = r.be16 (data + (size_t) i * 2) != 0;
            song->channels = 4;
            for (bool b : split) if (b) ++song->channels;
        }
        else if (id == "SAMP")
        {
            for (size_t k = 0; k + 32 <= len; k += 32)
            {
                const size_t h = data + k;
                Sample s;
                s.name = r.str (h, 20);
                s.volume = juce::jmin (64, r.u8 (h + 29));
                hdrs.push_back ({ r.be32 (h + 20), r.be16 (h + 24) * 2, r.be16 (h + 26) * 2, s.volume });
                song->samples.push_back (std::move (s));
            }
        }
        else if (id == "SPEE") song->initialSpeed = juce::jlimit (1, 31, r.be16 (data));
        else if (id == "SLEN") numPatterns = r.be16 (data);
        else if (id == "PLEN") numOrders = r.be16 (data);
        else if (id == "PATT") { for (size_t k = 0; k < len; ++k) orderBytes.push_back (r.u8 (data + k)); }
        else if (id == "PBOD")
        {
            const int rows = juce::jlimit (1, 256, r.be16 (data));
            const int nc = song->channels;
            Pattern pat;
            pat.resize (rows, nc);
            for (int row = 0; row < rows; ++row)
                for (int c = 0; c < nc; ++c)
                {
                    const size_t o = data + 2 + ((size_t) row * (size_t) nc + (size_t) c) * 4;
                    if (o + 4 > data + len) break;
                    const int note = r.u8 (o), ins = r.u8 (o + 1), fx = r.u8 (o + 2), p = r.u8 (o + 3);
                    Cell cell;
                    if (note >= 1 && note <= 36) { cell.note = (juce::uint8) (note + 48); cell.instr = (juce::uint8) (ins + 1); }
                    auto set = [&] (int f, int v) { cell.fx = (juce::uint8) f; cell.param = (juce::uint8) v; };
                    switch (fx)
                    {
                        case 1: if (p) set (0x1, p); break;
                        case 2: if (p) set (0x2, p); break;
                        case 10: if (p) set (fxOktArp1, p); break;
                        case 11: if (p) set (fxOktArp2, p); break;
                        case 12: if (p) set (fxOktArp3, p); break;
                        case 13: if (p) set (fxOktSlideDown, p); break;
                        case 30: if (p) set (fxOktSlideUp, p); break;
                        case 21: if (p) set (fxOktDownOnce, p); break;
                        case 17: if (p) set (fxOktUpOnce, p); break;
                        case 25: set (0xB, (p >> 4) * 10 + (p & 15)); break;
                        case 28: if (p & 15) set (0xF, p & 15); break;
                        case 31:
                            if (p <= 0x40) set (0xC, p);
                            else if (p <= 0x50) set (0xA, juce::jmin (15, p - 0x40));
                            else if (p <= 0x60) set (0xA, juce::jmin (15, p - 0x50) << 4);
                            else if (p <= 0x70) set (0xE, 0xB0 | juce::jmin (15, p - 0x60));
                            else if (p <= 0x80) set (0xE, 0xA0 | juce::jmin (15, p - 0x70));
                            break;
                        default: break;
                    }
                    pat.at (row, c) = cell;
                }
            song->patterns.push_back (std::move (pat));
        }
        else if (id == "SBOD")
        {
            while (sbodNext < hdrs.size() && hdrs[sbodNext].len == 0) ++sbodNext;
            if (sbodNext < hdrs.size())
            {
                auto& s = song->samples[sbodNext];
                const size_t n = juce::jmin ((size_t) hdrs[sbodNext].len, juce::jmin (len, r.n - data));
                s.pcm.resize (n);
                for (size_t k = 0; k < n; ++k) s.pcm[k] = (float) r.s8 (data + k) / 128.0f;
                setLoop (s, hdrs[sbodNext].ls, hdrs[sbodNext].ll);
                ++sbodNext;
            }
        }
        pos = data + len;
    }
    if (song->patterns.empty()) { error = "Oktalyzer: keine Patterns gefunden"; return nullptr; }
    juce::ignoreUnused (numPatterns);
    numOrders = juce::jmin (numOrders > 0 ? numOrders : (int) orderBytes.size(), (int) orderBytes.size());
    for (int i = 0; i < numOrders; ++i) song->orders.push_back (orderBytes[(size_t) i]);
    // Amiga channels L R R L; split channels contribute two voices on the same side
    static const float side[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
    for (int i = 0; i < 4; ++i)
    {
        song->channelPanInit.push_back (side[i]);
        if (split[i]) song->channelPanInit.push_back (side[i]);
    }
    song->initialTempo = 125;
    song->periodMode = 0;
    song->formatName = juce::String ("Oktalyzer (OKT, ") + juce::String (song->channels) + U (" Stimmen)");
    fixOrders (*song);
    return song;
}

// ============================================================================================ IT
namespace
{
// IT 2.14 / 2.15 sample decompression (block based, variable bit width, delta / double delta)
struct BitReader
{
    const juce::uint8* p; size_t n, pos = 0; int bit = 0;
    juce::uint32 read (int count)
    {
        juce::uint32 v = 0;
        for (int i = 0; i < count; ++i)
        {
            if (pos >= n) return v;
            v |= (juce::uint32) ((p[pos] >> bit) & 1) << i;
            if (++bit == 8) { bit = 0; ++pos; }
        }
        return v;
    }
};

// decodes `len` samples starting at file offset `o`; returns the offset after the compressed data
size_t itDecompress (const Reader& r, size_t o, juce::int64 len, bool is16, bool it215, std::vector<float>& out)
{
    const juce::int64 blockMax = is16 ? 0x4000 : 0x8000;
    const int maxWidth = is16 ? 17 : 9;
    while (len > 0 && r.has (o, 2))
    {
        const size_t blockBytes = (size_t) r.le16 (o);
        o += 2;
        if (! r.has (o, 0)) break;
        BitReader br { r.p + o, juce::jmin (blockBytes, r.n - o) };
        o += blockBytes;
        const juce::int64 blockLen = juce::jmin (len, blockMax);
        int width = maxWidth;
        juce::int32 d1 = 0, d2 = 0;
        juce::int64 k = 0;
        while (k < blockLen && br.pos < br.n)
        {
            juce::uint32 v = br.read (width);
            if (width < 7)
            {
                if (v == (1u << (width - 1)))
                {
                    v = br.read (is16 ? 4 : 3) + 1;
                    width = (int) v < width ? (int) v : (int) v + 1;
                    continue;
                }
            }
            else if (width < maxWidth)
            {
                const juce::uint32 border = ((is16 ? 0xFFFFu : 0xFFu) >> (maxWidth - width)) - (is16 ? 8u : 4u);
                if (v > border && v <= border + (is16 ? 16u : 8u))
                {
                    v -= border;
                    width = (int) v < width ? (int) v : (int) v + 1;
                    continue;
                }
            }
            else
            {
                if (v & (is16 ? 0x10000u : 0x100u)) { width = (int) ((v + 1) & 0xFF); if (width < 1 || width > maxWidth) break; continue; }
            }
            // sign extend
            const int bits = juce::jmin (width, is16 ? 16 : 8);
            juce::int32 sv = (juce::int32) (v & ((1u << bits) - 1));
            if (sv & (1 << (bits - 1))) sv -= 1 << bits;
            if (is16) { d1 = (juce::int16) (d1 + sv); d2 = (juce::int16) (d2 + d1); }
            else { d1 = (juce::int8) (d1 + sv); d2 = (juce::int8) (d2 + d1); }
            const juce::int32 s = it215 ? d2 : d1;
            out.push_back ((float) s / (is16 ? 32768.0f : 128.0f));
            ++k;
        }
        for (; k < blockLen; ++k) out.push_back (0.0f);
        len -= blockLen;
    }
    return o;
}

void readItSample (const Reader& r, size_t h, Sample& s)
{
    s.name = r.str (h + 0x14, 26);
    if (s.name.isEmpty()) s.name = r.str (h + 4, 12);
    s.globalGain = (float) juce::jmin (64, r.u8 (h + 0x11)) / 64.0f;
    const int flags = r.u8 (h + 0x12);
    s.volume = juce::jmin (64, r.u8 (h + 0x13));
    const int cvt = r.u8 (h + 0x2E), dfp = r.u8 (h + 0x2F);
    if (dfp & 0x80) s.pan = juce::jmin (255, (dfp & 0x7F) * 4);
    const juce::int64 len = r.le32 (h + 0x30);
    const juce::int64 ls = r.le32 (h + 0x34), le = r.le32 (h + 0x38);
    const juce::uint32 c5 = r.le32 (h + 0x3C);
    const juce::int64 sls = r.le32 (h + 0x40), sle = r.le32 (h + 0x44);
    const size_t data = r.le32 (h + 0x48);
    s.c5Speed = c5 > 0 ? (double) c5 : 8363.0;
    if (! (flags & 1) || len <= 0 || len > 64 * 1024 * 1024 || ! r.has (data, 1)) return;
    const bool is16 = flags & 2, stereo = flags & 4, packed = flags & 8, sgn = cvt & 1;
    if (packed)
    {
        std::vector<float> left, right;
        const size_t next = itDecompress (r, data, len, is16, cvt & 4, left);
        if (stereo) itDecompress (r, next, len, is16, cvt & 4, right);
        s.pcm.assign ((size_t) len, 0.0f);
        for (size_t k = 0; k < (size_t) len && k < left.size(); ++k)
            s.pcm[k] = stereo && k < right.size() ? 0.5f * (left[k] + right[k]) : left[k];
    }
    else
    {
        const int bps = is16 ? 2 : 1;
        const juce::int64 avail = (juce::int64) ((r.n - data) / (size_t) (bps * (stereo ? 2 : 1)));
        const juce::int64 n = juce::jmin (len, avail);
        auto get = [&] (juce::int64 k) -> float {
            const size_t o = data + (size_t) k * (size_t) bps;
            if (is16) { int v = r.le16 (o); if (! sgn) v -= 32768; else if (v >= 32768) v -= 65536; return (float) v / 32768.0f; }
            int v = r.u8 (o); if (! sgn) v -= 128; else if (v >= 128) v -= 256; return (float) v / 128.0f;
        };
        s.pcm.resize ((size_t) n);
        for (juce::int64 k = 0; k < n; ++k) s.pcm[(size_t) k] = stereo ? 0.5f * (get (k) + get (k + len)) : get (k);
    }
    // normal loop, else the sustain loop (released at key off – approximated as a plain loop)
    if ((flags & 0x10) && le > ls)
    {
        setLoop (s, ls, le - ls);
        if (flags & 0x40) unrollPingPong (s);
    }
    else if ((flags & 0x20) && sle > sls)
    {
        setLoop (s, sls, sle - sls);
        if (flags & 0x80) unrollPingPong (s);
    }
}

// IT envelope (new instrument format): flags, count, loop, sustain loop, 25 nodes (value, tick)
void readItEnvelope (const Reader& r, size_t o, Envelope& e, int offset)
{
    const int flags = r.u8 (o), num = juce::jmin (25, r.u8 (o + 1));
    for (int k = 0; k < num; ++k)
    {
        const size_t q = o + 6 + (size_t) k * 3;
        e.points.push_back ({ r.le16 (q + 1), juce::jlimit (0, 64, r.s8 (q) + offset) });
    }
    // ticks must rise
    for (size_t k = 1; k < e.points.size(); ++k) e.points[k].first = juce::jmax (e.points[k].first, e.points[k - 1].first);
    const int last = juce::jmax (0, num - 1);
    e.on = (flags & 1) && num > 0;
    e.loop = (flags & 2) != 0;
    e.sustain = (flags & 4) != 0;
    e.loopStart = juce::jlimit (0, last, r.u8 (o + 2));
    e.loopEnd = juce::jlimit (e.loopStart, last, r.u8 (o + 3));
    e.sus = juce::jlimit (0, last, r.u8 (o + 4));
    e.susEnd = juce::jlimit (e.sus, last, r.u8 (o + 5));
}
} // namespace

bool isIt (const juce::MemoryBlock& d)
{
    Reader r (d);
    return r.n >= 0xC0 && r.tag (0, "IMPM");
}

std::unique_ptr<Song> loadIt (const juce::MemoryBlock& d, juce::String& error)
{
    Reader r (d);
    if (! isIt (d)) { error = "Keine IT-Datei"; return nullptr; }
    auto song = std::make_unique<Song>();
    song->format = Format::IT;
    song->title = r.str (4, 26);
    const int ordNum = r.le16 (0x20), insNum = r.le16 (0x22), smpNum = r.le16 (0x24), patNum = r.le16 (0x26);
    const int cwt = r.le16 (0x28), cmwt = r.le16 (0x2A), flags = r.le16 (0x2C);
    // OpenMPT's own format MPTM: an IT file with tracker version 0x888+ and extensions (up to 127 channels, 1024 rows, tempo modes)
    const bool mptm = cwt >= 0x0888 && cwt <= 0x0FFF;
    const int chMask = mptm ? 127 : 63;
    const int gv = r.u8 (0x30), is = r.u8 (0x32), it = r.u8 (0x33), sep = r.u8 (0x34);
    const bool useInstruments = (flags & 4) != 0;
    if ((r.le16 (0x2E) & 1) != 0)   // song message (CR line ends)
    {
        const size_t mo = r.le32 (0x38), ml = (size_t) r.le16 (0x36);
        juce::String m;
        for (size_t i = 0; i < ml && r.has (mo + i, 1); ++i)
        {
            const int c = r.u8 (mo + i);
            if (c == 0) break;
            if (c == '\r') m << "\n"; else if (c >= 32 && c != 127) m += (juce::juce_wchar) c;
        }
        song->message = m.trim();
    }
    const size_t insOff = 0xC0 + (size_t) ordNum, smpOff = insOff + (size_t) insNum * 4, patOff = smpOff + (size_t) smpNum * 4;

    // orders: 255 = end, 254 = "+++" (dropped; jump targets are remapped)
    std::vector<int> orderMap;
    for (int i = 0; i < ordNum; ++i)
    {
        const int o = r.u8 (0xC0 + (size_t) i);
        orderMap.push_back ((int) song->orders.size());
        if (o == 255) break;
        if (o == 254) continue;
        song->orders.push_back (o);
    }

    // patterns (channel count = highest channel actually used)
    int usedCh = 1;
    for (int pi = 0; pi < patNum; ++pi)
    {
        const size_t ptr = r.le32 (patOff + (size_t) pi * 4);
        Pattern pat;
        if (ptr == 0 || ! r.has (ptr, 8)) { pat.resize (64, 128); song->patterns.push_back (std::move (pat)); continue; }
        const int rows = juce::jlimit (1, mptm ? 1024 : 256, r.le16 (ptr + 2));
        pat.resize (rows, 128);
        const size_t end = juce::jmin (r.n, ptr + 8 + (size_t) r.le16 (ptr));
        size_t o = ptr + 8;
        int lastMask[128] {}, lastNote[128] {}, lastIns[128] {}, lastVol[128] {}, lastCmd[128] {}, lastPrm[128] {};
        for (int row = 0; row < rows && o < end;)
        {
            const int cv = r.u8 (o++);
            if (cv == 0) { ++row; continue; }
            const int c = (cv - 1) & chMask;
            int mask = lastMask[c];
            if (cv & 128) mask = lastMask[c] = r.u8 (o++);
            int note = -1, ins = 0, vol = -1, cmd = 0, prm = 0;
            if (mask & 1) note = lastNote[c] = r.u8 (o++);
            if (mask & 2) ins = lastIns[c] = r.u8 (o++);
            if (mask & 4) vol = lastVol[c] = r.u8 (o++);
            if (mask & 8) { cmd = lastCmd[c] = r.u8 (o); prm = lastPrm[c] = r.u8 (o + 1); o += 2; }
            if (mask & 16) note = lastNote[c];
            if (mask & 32) ins = lastIns[c];
            if (mask & 64) vol = lastVol[c];
            if (mask & 128) { cmd = lastCmd[c]; prm = lastPrm[c]; }
            Cell cell;
            if (note >= 0 && note <= 119) cell.note = (juce::uint8) (note + 1);
            else if (note == 255) cell.note = kNoteOff;
            else if (note == 254) cell.note = kNoteCut;
            else if (note >= 120) cell.note = kNoteFade;
            cell.instr = (juce::uint8) ins;
            if (vol >= 0)
            {
                auto vc = [&] (int cmdv, int v) { cell.vcmd = (juce::uint8) cmdv; cell.vol = (juce::uint8) v; };
                if (vol <= 64) cell.vol = (juce::uint8) vol;
                else if (vol <= 74) vc (vcFineUp, vol - 65);
                else if (vol <= 84) vc (vcFineDown, vol - 75);
                else if (vol <= 94) vc (vcSlideUp, vol - 85);
                else if (vol <= 104) vc (vcSlideDown, vol - 95);
                else if (vol <= 114) vc (vcPitchDown, vol - 105);
                else if (vol <= 124) vc (vcPitchUp, vol - 115);
                else if (vol >= 128 && vol <= 192) vc (vcPanFine, vol - 128);
                else if (vol >= 193 && vol <= 202) vc (vcPorta, vol - 193);
                else if (vol >= 203 && vol <= 212) vc (vcVibDepth, vol - 203);
            }
            if (cmd >= 1 && cmd <= 26)
            {
                cell.fx = (juce::uint8) cmd;
                cell.param = (juce::uint8) prm;
                if (cmd == 2) cell.param = (juce::uint8) (prm < (int) orderMap.size() ? orderMap[(size_t) prm] : prm);
            }
            if (! cell.isEmpty()) { pat.at (row, c) = cell; usedCh = juce::jmax (usedCh, c + 1); }
        }
        song->patterns.push_back (std::move (pat));
    }
    const int numCh = juce::jmin (usedCh, kMaxChannels);
    song->channels = numCh;
    for (auto& p : song->patterns)
    {
        Pattern q;
        q.resize (p.rows, numCh);
        for (int row = 0; row < p.rows; ++row)
            for (int c = 0; c < numCh; ++c) q.at (row, c) = p.at (row, c);
        p = std::move (q);
    }

    // channel panning / volume: pan 0..64, 100 = surround (centre), +128 = channel off
    const bool stereo = (flags & 1) != 0;
    for (int c = 0; c < numCh; ++c)
    {
        const int pv = r.u8 (0x40 + (size_t) c), cv = r.u8 (0x80 + (size_t) c);
        const int pan = pv & 127;
        song->channelPanInit.push_back (stereo && pan <= 64 ? (float) pan / 32.0f - 1.0f : 0.0f);
        song->channelVolInit.push_back ((pv & 128) ? 0 : juce::jmin (64, cv));
    }

    // samples
    for (int i = 0; i < smpNum; ++i)
    {
        Sample s;
        const size_t h = r.le32 (smpOff + (size_t) i * 4);
        if (h != 0 && r.has (h, 0x50) && r.tag (h, "IMPS")) readItSample (r, h, s);
        song->samples.push_back (std::move (s));
    }

    // instruments
    if (useInstruments)
        for (int i = 0; i < insNum; ++i)
        {
            Instrument ins;
            const size_t h = r.le32 (insOff + (size_t) i * 4);
            if (h != 0 && r.has (h, 0x130) && r.tag (h, "IMPI"))
            {
                ins.name = r.str (h + 0x20, 26);
                if (ins.name.isEmpty()) ins.name = r.str (h + 4, 12);
                const bool oldFormat = cmwt < 0x200;
                for (int n = 0; n < 120; ++n)
                {
                    const int tn = r.u8 (h + 0x40 + (size_t) n * 2), sm = r.u8 (h + 0x41 + (size_t) n * 2);
                    ins.noteMap[(size_t) n] = (juce::uint8) (tn <= 119 ? tn + 1 : n + 1);
                    ins.map[(size_t) n] = (short) (sm >= 1 && sm <= smpNum ? sm - 1 : -1);
                }
                if (oldFormat)
                {
                    // Impulse Tracker 1.x instrument: fadeout 0..64, NNA at 0x1A; envelopes not converted
                    ins.fadeout = r.le16 (h + 0x18) * 64 * 32;
                    ins.nna = juce::jlimit (0, 3, r.u8 (h + 0x1A));
                }
                else
                {
                    ins.nna = juce::jlimit (0, 3, r.u8 (h + 0x11));
                    ins.fadeout = juce::jmin (1024, r.le16 (h + 0x14)) * 32;
                    ins.globalGain = (float) juce::jmin (128, r.u8 (h + 0x18)) / 128.0f;
                    const int dfp = r.u8 (h + 0x19);
                    if (! (dfp & 0x80)) ins.panDefault = juce::jmin (255, (dfp & 0x7F) * 4);
                    if (r.has (h, 0x1D4 + 82))
                    {
                        readItEnvelope (r, h + 0x130, ins.vol, 0);
                        readItEnvelope (r, h + 0x182, ins.pan, 32);
                        readItEnvelope (r, h + 0x1D4, ins.pitch, 32);
                        if (r.u8 (h + 0x1D4) & 0x80) ins.pitch.on = false;   // filter envelope: not emulated
                    }
                }
            }
            song->instruments.push_back (std::move (ins));
        }

    song->initialSpeed = is > 0 ? is : 6;
    song->initialTempo = it >= 32 ? it : 125;
    song->globalVolume = juce::jmin (64, (gv + 1) / 2);
    song->periodMode = (flags & 8) ? 2 : 1;
    song->stereoSeparation = juce::jlimit (0, 100, sep * 100 / 128);
    const int tracker = cwt >> 12;
    if (mptm)
    {
        // extension block "STPM": fields <4-char id, reversed><uint16 size><data>; TM.. = tempo mode (2 = modern: tempo is real BPM),
        // RPB. = rows per beat, DT.. = default tempo
        size_t at = 0;
        for (size_t i = r.n >= 4 ? r.n - 4 : 0; i > 0xC0; --i)
            if (r.tag (i, "STPM")) { at = i + 4; break; }
        // rows per beat: IT header "highlight minor" (0x1E); RPB. only when it does not fit there
        int tempoMode = 0, rowsPerBeat = r.u8 (0x1E) > 0 ? r.u8 (0x1E) : 4, defTempo = song->initialTempo, fract = 0;
        for (size_t p = at; at != 0 && r.has (p, 6);)
        {
            const juce::String id ((const char*) r.p + p, 4);
            const size_t len = (size_t) r.le16 (p + 4);
            if (! r.has (p + 6, len)) break;
            auto value = [&] { juce::int64 v = 0; for (size_t k = 0; k < juce::jmin ((size_t) 4, len); ++k) v |= (juce::int64) r.u8 (p + 6 + k) << (8 * k); return (int) v; };
            if (id == "..MT") tempoMode = value();
            else if (id == ".BPR") rowsPerBeat = juce::jmax (1, value());
            else if (id == "..TD") defTempo = value();
            else if (id == "DTFR") fract = value();   // 1/10000 BPM
            p += 6 + len;
        }
        if (defTempo >= 31 && defTempo <= 1000) song->initialTempo = defTempo;
        if (tempoMode == 2)   // modern: the engine keeps the tempo as BPM (see ModEngine::tickSeconds)
        {
            song->tempoMode = 2;
            song->rowsPerBeat = juce::jlimit (1, 64, rowsPerBeat);
            song->tempoFraction = juce::jlimit (0, 9999, fract) / 10000.0;
        }
        song->formatName = U ("OpenMPT (MPTM, ") + juce::String (numCh) + U (" Kanäle") + (useInstruments ? U (", Instrumente)") : juce::String (")"))
                         + (tempoMode == 2 ? U (" – modernes Tempo") : juce::String());
    }
    else song->formatName = juce::String ("Impulse Tracker (IT") + (tracker == 5 ? juce::String (", OpenMPT") : tracker == 0 || tracker == 2 ? juce::String() : juce::String (", Version ") + juce::String::toHexString (cwt))
                     + (useInstruments ? U (", Instrumente)") : juce::String (")"));
    fixOrders (*song);
    return song;
}

// ============================================================================================ MTM
bool isMtm (const juce::MemoryBlock& d)
{
    Reader r (d);
    return r.n >= 66 && r.tag (0, "MTM") && r.u8 (33) >= 1 && r.u8 (33) <= 32;
}

std::unique_ptr<Song> loadMtm (const juce::MemoryBlock& d, juce::String& error)
{
    Reader r (d);
    if (! isMtm (d)) { error = "Keine MTM-Datei"; return nullptr; }
    auto song = std::make_unique<Song>();
    song->format = Format::MTM;
    song->title = r.str (4, 20);
    const int numTracks = r.le16 (24), lastPattern = r.u8 (26), lastOrder = r.u8 (27);
    const int commentLen = r.le16 (28), numSamples = r.u8 (30);
    int rows = r.u8 (32);
    if (rows < 1 || rows > 64) rows = 64;
    const int numCh = juce::jmin (r.u8 (33), kMaxChannels);
    song->channels = numCh;
    for (int c = 0; c < numCh; ++c) song->channelPanInit.push_back ((float) (r.u8 (34 + (size_t) c) & 15) / 7.5f - 1.0f);

    const size_t smpHdr = 66, ordOff = smpHdr + (size_t) numSamples * 37, trkOff = ordOff + 128;
    const size_t seqOff = trkOff + (size_t) numTracks * 192;
    const size_t dataOff = seqOff + (size_t) (lastPattern + 1) * 64 + (size_t) commentLen;
    if (! r.has (seqOff, (size_t) (lastPattern + 1) * 64)) { error = U ("MTM-Datei unvollständig"); return nullptr; }

    for (int i = 0; i <= lastOrder && i < 128; ++i) song->orders.push_back (r.u8 (ordOff + (size_t) i));
    if (commentLen > 0)   // comment: lines of 40 characters
    {
        juce::StringArray lines;
        const size_t co = seqOff + (size_t) (lastPattern + 1) * 64;
        for (int i = 0; i < commentLen; i += 40) lines.add (r.str (co + (size_t) i, juce::jmin (40, commentLen - i)));
        song->message = lines.joinIntoString ("\n").trim();
    }

    for (int pi = 0; pi <= lastPattern; ++pi)
    {
        Pattern pat;
        pat.resize (rows, numCh);
        for (int c = 0; c < numCh; ++c)
        {
            const int t = r.le16 (seqOff + (size_t) pi * 64 + (size_t) c * 2);
            if (t == 0 || t > numTracks) continue;
            const size_t base = trkOff + (size_t) (t - 1) * 192;
            for (int row = 0; row < rows; ++row)
            {
                const size_t o = base + (size_t) row * 3;
                const int b0 = r.u8 (o), b1 = r.u8 (o + 1), b2 = r.u8 (o + 2);
                Cell cell;
                const int note = b0 >> 2;
                if (note > 0) cell.note = (juce::uint8) juce::jlimit (1, 120, note + 37);
                cell.instr = (juce::uint8) (((b0 & 3) << 4) | (b1 >> 4));
                const int fx = b1 & 15;
                if (fx != 0 || b2 != 0) { cell.fx = (juce::uint8) fx; cell.param = (juce::uint8) b2; }
                pat.at (row, c) = cell;
            }
        }
        song->patterns.push_back (std::move (pat));
    }

    size_t pos = dataOff;
    for (int i = 0; i < numSamples; ++i)
    {
        const size_t h = smpHdr + (size_t) i * 37;
        Sample s;
        s.name = r.str (h, 22);
        const juce::int64 len = r.le32 (h + 22), ls = r.le32 (h + 26), le = r.le32 (h + 30);
        int ft = r.u8 (h + 34) & 15;
        s.finetune = ft >= 8 ? ft - 16 : ft;
        s.volume = juce::jmin (64, r.u8 (h + 35));
        const bool is16 = (r.u8 (h + 36) & 1) != 0;
        const size_t bytes = (size_t) juce::jmax ((juce::int64) 0, len);
        const size_t avail = pos < r.n ? juce::jmin (bytes, r.n - pos) : 0;
        if (is16)
        {
            s.pcm.resize (avail / 2);
            for (size_t k = 0; k < avail / 2; ++k) s.pcm[k] = (float) (r.le16 (pos + k * 2) - 32768) / 32768.0f;
            if (le > ls + 4) setLoop (s, ls / 2, (le - ls) / 2);
        }
        else
        {
            s.pcm.resize (avail);
            for (size_t k = 0; k < avail; ++k) s.pcm[k] = (float) (r.u8 (pos + k) - 128) / 128.0f;
            if (le > ls + 2) setLoop (s, ls, le - ls);
        }
        pos += bytes;
        song->samples.push_back (std::move (s));
    }

    song->initialSpeed = 6;
    song->initialTempo = 125;
    song->periodMode = 0;
    song->stereoSeparation = 100;
    song->formatName = juce::String ("MultiTracker (MTM, ") + juce::String (numCh) + U (" Kanäle)");
    fixOrders (*song);
    return song;
}

// ============================================================================================ 669
// Composer 669 / UNIS 669: 8 channels, 64 rows, per-pattern speed and break row
bool is669 (const juce::MemoryBlock& d)
{
    Reader r (d);
    if (r.n < 497 || ! (r.tag (0, "if") || r.tag (0, "JN"))) return false;
    const int ns = r.u8 (110), np = r.u8 (111);
    if (ns > 64 || np > 128 || r.u8 (112) >= 128) return false;
    for (int i = 0; i < 128; ++i)
    {
        const int o = r.u8 (113 + (size_t) i);
        if (o >= 128 && o < 0xFE) return false;
        if (r.u8 (241 + (size_t) i) > 15 || r.u8 (369 + (size_t) i) >= 64) return false;
    }
    return r.n >= 497 + (size_t) ns * 25 + (size_t) np * 1536;
}

std::unique_ptr<Song> load669 (const juce::MemoryBlock& d, juce::String& error)
{
    Reader r (d);
    if (! is669 (d)) { error = "Keine 669-Datei"; return nullptr; }
    const bool unis = r.tag (0, "JN");
    auto song = std::make_unique<Song>();
    song->format = Format::F669;
    song->channels = 8;
    for (int c = 0; c < 8; ++c) song->channelPanInit.push_back ((c & 1) ? 0xD0 / 127.5f - 1.0f : 0x30 / 127.5f - 1.0f);
    juce::StringArray lines;
    for (int l = 0; l < 3; ++l) lines.add (r.str (2 + (size_t) l * 36, 36));
    song->title = lines[0];
    song->message = lines.joinIntoString ("\n").trim();
    const int numSamples = r.u8 (110), numPatterns = r.u8 (111);
    for (int i = 0; i < 128; ++i)
    {
        const int o = r.u8 (113 + (size_t) i);
        if (o == 0xFF) break;
        if (o < numPatterns) song->orders.push_back (o);
    }
    song->restartPos = r.u8 (112);

    size_t pos = 497 + (size_t) numSamples * 25;
    for (int pi = 0; pi < numPatterns; ++pi)
    {
        Pattern pat;
        pat.resize (64, 8);
        std::array<int, 8> effect;
        effect.fill (0xFF);
        for (int row = 0; row < 64; ++row)
            for (int c = 0; c < 8; ++c)
            {
                const size_t o = pos + (size_t) (row * 8 + c) * 3;
                const int ni = r.u8 (o), iv = r.u8 (o + 1), ep = r.u8 (o + 2);
                Cell& cell = pat.at (row, c);
                if (ni < 0xFE)
                {
                    cell.note = (juce::uint8) juce::jlimit (1, 120, (ni >> 2) + 37);
                    cell.instr = (juce::uint8) ((((ni & 3) << 4) | (iv >> 4)) + 1);
                    effect[(size_t) c] = 0xFF;
                }
                if (ni <= 0xFE) cell.vol = (juce::uint8) (((iv & 15) * 64 + 8) / 15);
                if (ep != 0xFF) effect[(size_t) c] = ep;
                const int e = effect[(size_t) c];
                if (e == 0xFF) continue;
                const int cmd = e >> 4, prm = e & 15;
                bool keep = false;
                switch (cmd)
                {
                    case 0: cell.fx = 0x1; cell.param = (juce::uint8) prm; break;            // slide up
                    case 1: cell.fx = 0x2; cell.param = (juce::uint8) prm; break;            // slide down
                    case 2: cell.fx = 0x3; cell.param = (juce::uint8) prm; break;            // slide to note
                    case 3: cell.fx = 0xE; cell.param = (juce::uint8) (0x10 | prm); break;   // frequency adjust ~ fine slide up
                    case 4: cell.fx = 0x4; cell.param = (juce::uint8) ((prm << 4) | prm); break;   // frequency vibrato
                    case 5: if (prm) { cell.fx = fxSpeed; cell.param = (juce::uint8) prm; } break;
                    case 6:                                                                   // UNIS: fine balance, keeps going
                        if (prm == 0) { cell.fx = fxItPanSlide; cell.param = 0x4F; keep = true; }
                        else if (prm == 1) { cell.fx = fxItPanSlide; cell.param = 0xF4; keep = true; }
                        break;
                    case 7: if (unis && cell.note > 0 && prm) { cell.fx = fxXmR; cell.param = (juce::uint8) prm; } break;
                    default: keep = true; break;
                }
                if (! keep) effect[(size_t) c] = 0xFF;
            }
        // speed at row 0 and break row of this pattern (into a free effect slot)
        auto put = [&pat] (int row, juce::uint8 fx, juce::uint8 prm) {
            for (int c = 0; c < 8; ++c)
                if (pat.at (row, c).fx == 0 && pat.at (row, c).param == 0) { pat.at (row, c).fx = fx; pat.at (row, c).param = prm; return; }
            pat.at (row, 7).fx = fx;
            pat.at (row, 7).param = prm;
        };
        const int sp = r.u8 (241 + (size_t) pi), br = r.u8 (369 + (size_t) pi);
        if (br < 63) put (br, 0xD, 0);
        if (sp > 0) put (0, fxSpeed, (juce::uint8) sp);
        song->patterns.push_back (std::move (pat));
        pos += 1536;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const size_t h = 497 + (size_t) i * 25;
        Sample s;
        s.name = r.str (h, 13);
        const juce::int64 len = r.le32 (h + 13), ls = r.le32 (h + 17), le = r.le32 (h + 21);
        s.c5Speed = 8363.0;
        const size_t avail = pos < r.n ? (size_t) juce::jmin ((juce::int64) (r.n - pos), len) : 0;
        s.pcm.resize (avail);
        for (size_t k = 0; k < avail; ++k) s.pcm[k] = (float) (r.u8 (pos + k) - 128) / 128.0f;
        if (! (le > len && ls == 0) && le > ls) setLoop (s, ls, juce::jmin (le, len) - ls);
        pos += (size_t) len;
        song->samples.push_back (std::move (s));
    }
    song->initialSpeed = 4;
    song->initialTempo = 78;
    song->periodMode = 1;
    song->stereoSeparation = 100;
    song->formatName = unis ? U ("UNIS 669 (8 Kanäle)") : U ("Composer 669 (8 Kanäle)");
    fixOrders (*song);
    return song;
}

// ============================================================================================ FAR
// Farandole Composer: 16 channels, patterns of variable length
bool isFar (const juce::MemoryBlock& d)
{
    Reader r (d);
    return r.n >= 98 + 771 && r.tag (0, "FAR\xFE") && r.u8 (44) == 0x0D && r.u8 (45) == 0x0A && r.u8 (46) == 0x1A && r.le16 (47) >= 98;
}

std::unique_ptr<Song> loadFar (const juce::MemoryBlock& d, juce::String& error)
{
    Reader r (d);
    if (! isFar (d)) { error = "Keine FAR-Datei"; return nullptr; }
    auto song = std::make_unique<Song>();
    song->format = Format::FAR;
    song->title = r.str (4, 40);
    const int headerLen = r.le16 (47), msgLen = r.le16 (96);
    song->channels = 16;
    for (int c = 0; c < 16; ++c)
    {
        song->channelPanInit.push_back ((float) (((r.u8 (76 + (size_t) c) & 15) << 4) + 8) / 127.5f - 1.0f);
        song->channelVolInit.push_back (r.u8 (50 + (size_t) c) ? 64 : 0);
    }
    if (msgLen > 0)
    {
        juce::String m;
        for (int i = 0; i < msgLen; i += 132) m << r.str (98 + (size_t) i, juce::jmin (132, msgLen - i)) << "\n";
        song->message = m.trim();
    }
    const size_t oh = 98 + (size_t) msgLen;
    if (! r.has (oh, 771)) { error = U ("FAR-Datei unvollständig"); return nullptr; }
    const int numOrders = r.u8 (oh + 257);
    for (int i = 0; i < numOrders && i < 256; ++i)
    {
        const int o = r.u8 (oh + (size_t) i);
        if (o == 0xFF) break;
        song->orders.push_back (o);
    }
    song->restartPos = r.u8 (oh + 258);

    size_t pos = (size_t) headerLen;
    int lastPat = -1;
    for (int pi = 0; pi < 256; ++pi)
        if (r.le16 (oh + 259 + (size_t) pi * 2) != 0) lastPat = pi;
    for (int pi = 0; pi <= lastPat; ++pi)
    {
        const int size = r.le16 (oh + 259 + (size_t) pi * 2);
        Pattern pat;
        if (size == 0) { pat.resize (64, 16); song->patterns.push_back (std::move (pat)); continue; }
        const int rows = juce::jlimit (1, 256, (size - 2) / 64);
        pat.resize (rows, 16);
        int breakRow = r.u8 (pos);
        breakRow = (breakRow > 0 && breakRow < rows - 2) ? breakRow + 1 : -1;
        for (int row = 0; row < rows; ++row)
            for (int c = 0; c < 16; ++c)
            {
                const size_t o = pos + 2 + (size_t) (row * 16 + c) * 4;
                if (! r.has (o, 4)) break;
                const int note = r.u8 (o), ins = r.u8 (o + 1), vol = r.u8 (o + 2), eff = r.u8 (o + 3);
                Cell& cell = pat.at (row, c);
                if (note > 0 && note <= 72) { cell.note = (juce::uint8) (note + 36); cell.instr = (juce::uint8) (ins + 1); }
                if (vol > 0 && vol <= 16) cell.vol = (juce::uint8) ((vol - 1) * 64 / 15);
                const int lo = eff & 15;
                switch (eff >> 4)
                {
                    case 0x1: cell.fx = 0xE; cell.param = (juce::uint8) (0x10 | lo); break;          // fine porta up
                    case 0x2: cell.fx = 0xE; cell.param = (juce::uint8) (0x20 | lo); break;          // fine porta down
                    case 0x3: cell.fx = 0x3; cell.param = (juce::uint8) (lo ? 60 / lo : 0); break;    // porta to note
                    case 0x4: cell.fx = fxXmR; cell.param = (juce::uint8) (6 / (1 + lo) + 1); break;  // retrigger
                    case 0x5: cell.fx = 0x4; cell.param = (juce::uint8) lo; break;                    // vibrato depth
                    case 0x6: cell.fx = 0x4; cell.param = (juce::uint8) ((lo * 8) & 0xF0); break;     // vibrato speed
                    case 0x7: cell.fx = 0xA; cell.param = (juce::uint8) (juce::jmin (15, lo) << 4); break;   // volume up
                    case 0x8: cell.fx = 0xA; cell.param = (juce::uint8) lo; break;                    // volume down
                    case 0x9: cell.fx = 0x4; cell.param = (juce::uint8) lo; break;                    // sustained vibrato
                    case 0xA: cell.vol = (juce::uint8) juce::jmin (64, (lo << 2) + 4); break;         // slide to volume
                    case 0xB: cell.fx = 0xE; cell.param = (juce::uint8) (0x80 | lo); break;          // panning
                    case 0xF: if (lo) { cell.fx = fxSpeed; cell.param = (juce::uint8) lo; } break;
                    default: break;
                }
            }
        if (breakRow >= 0 && breakRow < rows)
        {
            bool done = false;
            for (int c = 0; c < 16 && ! done; ++c)
                if (pat.at (breakRow, c).fx == 0 && pat.at (breakRow, c).param == 0) { pat.at (breakRow, c).fx = 0xD; done = true; }
            if (! done) { pat.at (breakRow, 15).fx = 0xD; pat.at (breakRow, 15).param = 0; }
        }
        pos += (size_t) size;
        song->patterns.push_back (std::move (pat));
    }

    // sample bitmap, then header + data for each present sample
    const size_t bitmap = pos;
    pos += 8;
    for (int i = 0; i < 64; ++i)
    {
        Sample s;
        if ((r.u8 (bitmap + (size_t) (i >> 3)) >> (i & 7)) & 1)
        {
            if (! r.has (pos, 48)) { song->samples.push_back (std::move (s)); continue; }
            s.name = r.str (pos, 32);
            const juce::int64 len = r.le32 (pos + 32);
            s.volume = juce::jmin (64, r.u8 (pos + 37) * 4);
            juce::int64 ls = r.le32 (pos + 38), le = r.le32 (pos + 42);
            const bool is16 = (r.u8 (pos + 46) & 1) != 0, loop = (r.u8 (pos + 47) & 8) != 0;
            s.c5Speed = 8363.0 * 2.0;
            pos += 48;
            const size_t avail = pos < r.n ? (size_t) juce::jmin ((juce::int64) (r.n - pos), len) : 0;
            if (is16)
            {
                s.pcm.resize (avail / 2);
                for (size_t k = 0; k < avail / 2; ++k) s.pcm[k] = (float) (juce::int16) r.le16 (pos + k * 2) / 32768.0f;
                ls /= 2;
                le /= 2;
            }
            else
            {
                s.pcm.resize (avail);
                for (size_t k = 0; k < avail; ++k) s.pcm[k] = (float) r.s8 (pos + k) / 128.0f;
            }
            if (loop && le > ls) setLoop (s, ls, le - ls);
            pos += (size_t) len;
        }
        song->samples.push_back (std::move (s));
    }
    while (! song->samples.empty() && song->samples.back().pcm.empty() && song->samples.back().name.isEmpty()) song->samples.pop_back();

    song->initialSpeed = juce::jmax (1, r.u8 (75));
    song->initialTempo = 80;
    song->periodMode = 1;
    song->stereoSeparation = 100;
    song->formatName = U ("Farandole Composer (FAR, 16 Kanäle)");
    fixOrders (*song);
    return song;
}

// ============================================================================================ MDL
// Digitrakker: chunked file ("IN", "PA", "TR", "II", "IS", "SA", envelopes); tracks are shared between patterns
bool isMdl (const juce::MemoryBlock& d)
{
    Reader r (d);
    return r.n >= 11 && r.tag (0, "DMDL") && r.u8 (4) < 0x20;
}

namespace
{
struct MdlBits
{
    const Reader& r;
    size_t pos, end;
    juce::uint32 buf = 0;
    int bits = 0;
    MdlBits (const Reader& rr, size_t p, size_t e) : r (rr), pos (p), end (e) {}
    bool read (int n, int& out)   // LSB first
    {
        while (bits < n)
        {
            if (pos >= end) return false;
            buf |= (juce::uint32) r.u8 (pos++) << bits;
            bits += 8;
        }
        out = (int) (buf & ((1u << n) - 1));
        buf >>= n;
        bits -= n;
        return true;
    }
};

// Digitrakker packed sample: sign bit + short/escaped magnitude, delta on the high byte (16 bit: raw low byte first)
void mdlUnpack (const Reader& r, size_t pos, size_t end, bool is16, size_t frames, std::vector<float>& out)
{
    out.assign (frames, 0.0f);
    MdlBits b (r, pos, end);
    juce::uint8 dlt = 0;
    int lowbyte = 0, v = 0;
    for (size_t j = 0; j < frames; ++j)
    {
        if (is16 && ! b.read (8, lowbyte)) return;
        int sign = 0, flag = 0;
        if (! b.read (1, sign) || ! b.read (1, flag)) return;
        juce::uint8 hb;
        if (flag)
        {
            if (! b.read (3, v)) return;
            hb = (juce::uint8) v;
        }
        else
        {
            hb = 8;
            for (;;)
            {
                if (! b.read (1, v)) return;
                if (v) break;
                hb = (juce::uint8) (hb + 0x10);
            }
            if (! b.read (4, v)) return;
            hb = (juce::uint8) (hb + v);
        }
        if (sign) hb = (juce::uint8) ~hb;
        dlt = (juce::uint8) (dlt + hb);
        out[j] = is16 ? (float) (juce::int16) (juce::uint16) (lowbyte | (dlt << 8)) / 32768.0f : (float) (juce::int8) dlt / 128.0f;
    }
}

// one MDL effect column -> common cell effect (XM numbering); column-2 commands 1..6 arrive as 0x10..0x15.
// Returns true for the sample offset command EFx (high part in prm).
bool mdlEffect (int cmd, int p, int& fx, int& prm)
{
    fx = 0;
    prm = 0;
    const int hi = p >> 4, lo = p & 15;
    switch (cmd)
    {
        case 0x1: if (p >= 0xF0) { fx = 0xE; prm = 0x10 | lo; } else if (p >= 0xE0) { fx = fxXmX; prm = 0x10 | lo; } else { fx = 0x1; prm = p; } break;
        case 0x2: if (p >= 0xF0) { fx = 0xE; prm = 0x20 | lo; } else if (p >= 0xE0) { fx = fxXmX; prm = 0x20 | lo; } else { fx = 0x2; prm = p; } break;
        case 0x3: fx = 0x3; prm = p; break;
        case 0x4: fx = 0x4; prm = p; break;
        case 0x5: if (p) { fx = 0x0; prm = p; } break;
        case 0x7: fx = fxTempo; prm = juce::jmax (0x20, p); break;
        case 0x8: fx = 0x8; prm = (p & 0x7F) * 2; break;
        case 0xB: fx = 0xB; prm = p; break;
        case 0xC: fx = fxXmG; prm = juce::jmin (64, (p + 1) / 2); break;
        case 0xD: fx = 0xD; prm = p; break;   // BCD like the common cell
        case 0xE:
            switch (hi)
            {
                case 0x1: fx = fxItPanSlide; prm = (juce::jmin (lo, 14) << 4) | 0x0F; break;
                case 0x2: fx = fxItPanSlide; prm = 0xF0 | juce::jmin (lo, 14); break;
                case 0x4: fx = 0xE; prm = 0x40 | lo; break;
                case 0x5: fx = 0xE; prm = 0x50 | lo; break;
                case 0x6: fx = 0xE; prm = 0x60 | lo; break;
                case 0x7: fx = 0xE; prm = 0x70 | lo; break;
                case 0x9: fx = fxXmR; prm = lo; break;
                case 0xA: fx = fxXmH; prm = ((lo + 1) << 3) & 0xF0; break;
                case 0xB: fx = fxXmH; prm = (lo + 1) >> 1; break;
                case 0xC: fx = 0xE; prm = 0xC0 | lo; break;
                case 0xD: fx = 0xE; prm = 0xD0 | lo; break;
                case 0xE: fx = 0xE; prm = 0xE0 | lo; break;
                case 0xF: fx = 0x9; prm = lo; return true;
                default: break;
            }
            break;
        case 0xF: if (p) { fx = fxSpeed; prm = p; } break;
        case 0x10:   // G: volume slide up (4x finer than XM)
            if (p < 0xE0) { fx = 0xA; prm = juce::jmin (15, p >> 2) << 4; }
            else if (p < 0xF0) { fx = 0xE; prm = 0xA0 | (lo >> 2); }
            else { fx = 0xE; prm = 0xA0 | lo; }
            break;
        case 0x11:   // H: volume slide down
            if (p < 0xE0) { fx = 0xA; prm = juce::jmin (15, p >> 2); }
            else if (p < 0xF0) { fx = 0xE; prm = 0xB0 | (lo >> 2); }
            else { fx = 0xE; prm = 0xB0 | lo; }
            break;
        case 0x12: fx = fxXmR; prm = p; break;
        case 0x13: fx = 0x7; prm = p; break;
        case 0x14: fx = fxXmT; prm = p; break;
        default: break;
    }
    return false;
}

void mdlEnvelope (const Reader& r, size_t o, Envelope& e, bool volume)
{
    int tick = -r.u8 (o + 1);
    for (int n = 0; n < 15; ++n)
    {
        const int x = r.u8 (o + 1 + (size_t) n * 2), y = r.u8 (o + 2 + (size_t) n * 2);
        if (x == 0) break;
        tick += x;
        e.points.push_back ({ tick, juce::jmin (64, y) });
    }
    const int flags = r.u8 (o + 31), loop = r.u8 (o + 32), last = juce::jmax (0, (int) e.points.size() - 1);
    e.loopStart = juce::jmin (last, loop & 15);
    e.loopEnd = juce::jmin (last, loop >> 4);
    e.sus = e.susEnd = juce::jmin (last, flags & 15);
    e.sustain = (flags & 0x10) != 0;
    e.loop = (flags & 0x20) != 0;
    if (volume && ! e.loop && ! e.points.empty()) { e.loop = true; e.loopStart = e.loopEnd = last; }   // hold the last value
}
} // namespace

std::unique_ptr<Song> loadMdl (const juce::MemoryBlock& d, juce::String& error)
{
    Reader r (d);
    if (! isMdl (d)) { error = "Keine MDL-Datei"; return nullptr; }
    const int version = r.u8 (4);
    std::map<int, std::pair<size_t, size_t>> chunks;   // id -> offset, length (first occurrence)
    for (size_t pos = 5; r.has (pos, 6);)
    {
        const int id = r.u8 (pos) | (r.u8 (pos + 1) << 8);
        const size_t len = r.le32 (pos + 2);
        if (! chunks.count (id)) chunks[id] = { pos + 6, juce::jmin (len, r.n - (pos + 6)) };
        if (len > r.n - (pos + 6)) break;
        pos += 6 + len;
    }
    auto chunk = [&chunks] (const char* t) -> std::pair<size_t, size_t> {
        const int id = (juce::uint8) t[0] | ((juce::uint8) t[1] << 8);
        const auto it = chunks.find (id);
        return it == chunks.end() ? std::pair<size_t, size_t> { 0, 0 } : it->second;
    };
    const auto in = chunk ("IN");
    if (in.second < 91) { error = U ("MDL-Datei ohne Info-Block"); return nullptr; }

    auto song = std::make_unique<Song>();
    song->format = Format::MDL;
    const size_t ib = in.first;
    song->title = r.str (ib, 32);
    song->artist = r.str (ib + 32, 20);
    const int numOrders = r.le16 (ib + 52);
    song->restartPos = r.le16 (ib + 54);
    song->globalVolume = juce::jlimit (0, 64, (r.u8 (ib + 56) + 1) / 4);
    song->initialSpeed = juce::jmax (1, r.u8 (ib + 57));
    song->initialTempo = juce::jmax (32, r.u8 (ib + 58));
    int numCh = 1;
    for (int c = 0; c < 32; ++c)
        if (! (r.u8 (ib + 59 + (size_t) c) & 0x80)) numCh = c + 1;
    for (int i = 0; i < numOrders && (size_t) (91 + i) < in.second; ++i) song->orders.push_back (r.u8 (ib + 91 + (size_t) i));

    const auto me = chunk ("ME");
    if (me.second > 0)
    {
        juce::String m;
        for (size_t i = 0; i < me.second; ++i)
        {
            const int c = r.u8 (me.first + i);
            if (c == 0) break;
            if (c == '\r') m << "\n";
            else if (c >= 32) m += (juce::juce_wchar) c;
        }
        song->message = m.trim();
    }

    // tracks (index 0 = empty)
    std::vector<std::pair<size_t, size_t>> tracks (1, { 0, 0 });
    const auto tr = chunk ("TR");
    if (tr.second >= 2)
    {
        const int nt = r.le16 (tr.first);
        size_t p = tr.first + 2;
        const size_t end = tr.first + tr.second;
        for (int t = 0; t < nt && p + 2 <= end; ++t)
        {
            const size_t len = (size_t) r.le16 (p);
            tracks.push_back ({ p + 2, juce::jmin (len, end - (p + 2)) });
            p += 2 + len;
        }
    }

    // pattern headers; track indices may reach beyond the enabled channels
    struct PatHdr { int chans, rows; size_t trk; };
    std::vector<PatHdr> phs;
    const auto pa = chunk ("PA");
    if (pa.second >= 1)
    {
        const int np = r.u8 (pa.first);
        size_t p = pa.first + 1;
        for (int i = 0; i < np; ++i)
        {
            PatHdr h { 32, 64, 0 };
            if (version >= 0x10) { h.chans = juce::jmin (32, r.u8 (p)); h.rows = r.u8 (p + 1) + 1; p += 18; }
            h.trk = p;
            if (! r.has (p, (size_t) h.chans * 2)) break;
            for (int c = 0; c < h.chans; ++c)
                if (r.le16 (p + (size_t) c * 2) != 0) numCh = juce::jmax (numCh, c + 1);
            p += (size_t) h.chans * 2;
            phs.push_back (h);
        }
    }
    numCh = juce::jlimit (1, kMaxChannels, numCh);
    song->channels = numCh;
    for (int c = 0; c < numCh; ++c)
    {
        const int cs = r.u8 (ib + 59 + (size_t) c);
        song->channelPanInit.push_back ((float) ((cs & 0x7F) * 2) / 127.0f - 1.0f);
        song->channelVolInit.push_back ((cs & 0x80) ? 0 : 64);
    }

    for (const auto& h : phs)
    {
        Pattern pat;
        pat.resize (juce::jlimit (1, 256, h.rows), numCh);
        for (int c = 0; c < h.chans && c < numCh; ++c)
        {
            const int t = r.le16 (h.trk + (size_t) c * 2);
            if (t <= 0 || t >= (int) tracks.size()) continue;
            size_t p = tracks[(size_t) t].first;
            const size_t end = p + tracks[(size_t) t].second;
            int row = 0;
            while (row < pat.rows && p < end)
            {
                const int b = r.u8 (p++), x = b >> 2;
                switch (b & 3)
                {
                    case 0: row += x + 1; break;                                   // empty rows
                    case 1:                                                         // repeat previous row
                        if (row > 0)
                        {
                            const Cell orig = pat.at (row - 1, c);
                            for (int k = 0; k <= x && row < pat.rows; ++k) pat.at (row++, c) = orig;
                        }
                        else ++row;
                        break;
                    case 2: if (row > x) pat.at (row, c) = pat.at (x, c); ++row; break;   // copy of row x
                    default:
                    {
                        Cell& cell = pat.at (row, c);
                        int note = 0, ins = 0, vol = 0, e1 = 0, e2 = 0, p1 = 0, p2 = 0;
                        if (x & 1) note = r.u8 (p++);
                        if (x & 2) ins = r.u8 (p++);
                        if (x & 4) vol = r.u8 (p++);
                        if (x & 8) { const int eb = r.u8 (p++); e1 = eb & 15; e2 = eb >> 4; }
                        if (x & 16) p1 = r.u8 (p++);
                        if (x & 32) p2 = r.u8 (p++);
                        if (note > 120) cell.note = kNoteOff;
                        else if (note > 0) cell.note = (juce::uint8) note;
                        cell.instr = (juce::uint8) ins;
                        if (e2 >= 1 && e2 <= 6) e2 += 15;
                        int f1, q1, f2, q2;
                        const bool o1 = mdlEffect (e1, p1, f1, q1);
                        const bool o2 = mdlEffect (e2, p2, f2, q2);
                        ++row;
                        if (o1 || o2)
                        {
                            // EFy in column 1 with xx in column 2: offset yxx; EFy in column 2 alone: offset y00
                            const int high = o1 ? q1 : q2, low = o1 ? p2 : 0;
                            cell.fx = 0x9;
                            cell.param = (juce::uint8) low;
                            if (high > 0) { cell.vcmd = vcOffsetHigh; cell.vol = (juce::uint8) high; }
                            else if (vol) cell.vol = (juce::uint8) juce::jmin (64, (vol + 2) / 4);
                            break;
                        }
                        if (vol) cell.vol = (juce::uint8) juce::jmin (64, (vol + 2) / 4);
                        // two columns -> one effect; a spare volume slide goes into the volume column
                        auto toVolColumn = [&cell] (int f, int q) {
                            if (cell.vol != kVolNone || cell.vcmd != vcNone) return false;
                            if (f == 0xA && (q >> 4) && ! (q & 15)) { cell.vcmd = vcSlideUp; cell.vol = (juce::uint8) (q >> 4); return true; }
                            if (f == 0xA && ! (q >> 4) && (q & 15)) { cell.vcmd = vcSlideDown; cell.vol = (juce::uint8) (q & 15); return true; }
                            if (f == 0xE && (q >> 4) == 0xA) { cell.vcmd = vcFineUp; cell.vol = (juce::uint8) (q & 15); return true; }
                            if (f == 0xE && (q >> 4) == 0xB) { cell.vcmd = vcFineDown; cell.vol = (juce::uint8) (q & 15); return true; }
                            return false;
                        };
                        const bool has1 = f1 != 0 || q1 != 0, has2 = f2 != 0 || q2 != 0;
                        if (has1 && has2)
                        {
                            if (! toVolColumn (f2, q2) && toVolColumn (f1, q1)) { f1 = f2; q1 = q2; }
                            cell.fx = (juce::uint8) f1;
                            cell.param = (juce::uint8) q1;
                        }
                        else if (has1) { cell.fx = (juce::uint8) f1; cell.param = (juce::uint8) q1; }
                        else if (has2) { cell.fx = (juce::uint8) f2; cell.param = (juce::uint8) q2; }
                        break;
                    }
                }
            }
        }
        song->patterns.push_back (std::move (pat));
    }

    // samples (IS = headers, SA = data in the same order)
    const auto is = chunk ("IS"), sa = chunk ("SA");
    if (is.second >= 1)
    {
        const int ns = r.u8 (is.first);
        size_t p = is.first + 1, dp = sa.first;
        const size_t dend = sa.first + sa.second;
        const size_t recLen = version >= 0x10 ? 59 : 57;
        for (int i = 0; i < ns && p + recLen <= is.first + is.second; ++i, p += recLen)
        {
            const int idx = r.u8 (p);
            if (idx == 0) break;
            Sample s;
            s.name = r.str (p + 1, 32);
            if (s.name.isEmpty()) s.name = r.str (p + 33, 8);
            size_t q = p + 41;
            const double c4 = version >= 0x10 ? (double) r.le32 (q) : (double) r.le16 (q);
            q += version >= 0x10 ? 4 : 2;
            s.c5Speed = c4 > 0 ? c4 * 2.0 : 16726.0;
            juce::int64 len = r.le32 (q), ls = r.le32 (q + 4), ll = r.le32 (q + 8);
            const int vol = r.u8 (q + 12), flags = r.u8 (q + 13);
            if (version < 0x10) s.volume = juce::jmin (64, (vol + 2) / 4);
            const bool is16 = (flags & 1) != 0, packed = (flags & 0x0C) != 0;
            if (is16) { len /= 2; ls /= 2; ll /= 2; }
            const size_t frames = (size_t) juce::jlimit ((juce::int64) 0, (juce::int64) 64 * 1024 * 1024, len);
            if (packed)
            {
                const size_t plen = dp + 4 <= dend ? (size_t) r.le32 (dp) : 0;
                const size_t start = dp + 4, stop = juce::jmin (dend, start + plen);
                if (plen >= 4) mdlUnpack (r, start, stop, is16, frames, s.pcm);
                dp = stop;
            }
            else
            {
                const size_t bytes = frames * (is16 ? 2 : 1);
                const size_t avail = dp < dend ? juce::jmin (bytes, dend - dp) : 0;
                s.pcm.resize (avail / (is16 ? 2 : 1));
                for (size_t k = 0; k < s.pcm.size(); ++k)
                    s.pcm[k] = is16 ? (float) (juce::int16) r.le16 (dp + k * 2) / 32768.0f : (float) r.s8 (dp + k) / 128.0f;
                dp += bytes;
            }
            if (ll > 0) setLoop (s, ls, ll);
            if ((flags & 2) && s.hasLoop()) unrollPingPong (s);
            if ((int) song->samples.size() < idx) song->samples.resize ((size_t) idx);
            song->samples[(size_t) idx - 1] = std::move (s);
        }
    }

    // envelopes + instruments
    std::array<std::pair<bool, size_t>, 64> volEnv {}, panEnv {};
    auto readEnvs = [&] (const char* id, std::array<std::pair<bool, size_t>, 64>& arr) {
        const auto ch = chunk (id);
        if (ch.second < 1) return;
        const int n = r.u8 (ch.first);
        for (int i = 0; i < n && (size_t) (1 + (i + 1) * 33) <= ch.second; ++i)
        {
            const size_t o = ch.first + 1 + (size_t) i * 33;
            const int num = r.u8 (o);
            if (num < 64) arr[(size_t) num] = { true, o };
        }
    };
    readEnvs ("VE", volEnv);
    readEnvs ("PE", panEnv);
    const auto ii = chunk ("II");
    if (ii.second >= 1)
    {
        const int ni = r.u8 (ii.first);
        size_t p = ii.first + 1;
        const size_t end = ii.first + ii.second;
        for (int i = 0; i < ni && p + 34 <= end; ++i)
        {
            const int insNum = r.u8 (p), nsmp = r.u8 (p + 1);
            Instrument ins;
            ins.name = r.str (p + 2, 32);
            p += 34;
            int firstNote = 0;
            bool envDone = false;
            for (int k = 0; k < nsmp && p + 14 <= end; ++k, p += 14)
            {
                const int smp = r.u8 (p), lastNote = juce::jmin (119, r.u8 (p + 1));
                if (smp == 0 || smp > (int) song->samples.size()) continue;
                for (int n = firstNote; n <= lastNote; ++n) ins.map[(size_t) n] = (short) (smp - 1);
                firstNote = lastNote + 1;
                auto& s = song->samples[(size_t) smp - 1];
                const int vf = r.u8 (p + 3), pf = r.u8 (p + 5);
                if (vf & 0x40) s.volume = juce::jmin (64, (r.u8 (p + 2) + 2) / 4);
                if (pf & 0x40) s.pan = juce::jmin (254, r.u8 (p + 4) * 2);
                if (! envDone)   // one envelope set per instrument: taken from its first sample
                {
                    envDone = true;
                    ins.fadeout = (r.le16 (p + 6) + 1) / 2;
                    ins.vibRate = r.u8 (p + 8);
                    ins.vibDepth = (r.u8 (p + 9) + 3) / 4;
                    ins.vibSweep = r.u8 (p + 10);
                    static const int vt[4] = { 0, 2, 1, 0 };
                    ins.vibType = vt[r.u8 (p + 11) & 3];
                    const auto& ve = volEnv[(size_t) (vf & 0x3F)];
                    if ((vf & 0x80) && ve.first) { mdlEnvelope (r, ve.second, ins.vol, true); ins.vol.on = ! ins.vol.points.empty(); }
                    const auto& pe = panEnv[(size_t) (pf & 0x3F)];
                    if ((pf & 0x80) && pe.first) { mdlEnvelope (r, pe.second, ins.pan, false); ins.pan.on = ! ins.pan.points.empty(); }
                }
            }
            if (insNum == 0) continue;
            if ((int) song->instruments.size() < insNum) song->instruments.resize ((size_t) insNum);
            song->instruments[(size_t) insNum - 1] = std::move (ins);
        }
    }
    if (song->instruments.empty())
        for (int i = 0; i < (int) song->samples.size(); ++i)
        {
            Instrument ins;
            ins.name = song->samples[(size_t) i].name;
            for (auto& m : ins.map) m = (short) i;
            song->instruments.push_back (std::move (ins));
        }

    song->periodMode = 1;
    song->stereoSeparation = 100;
    const juce::String ver = version == 0x11 ? "3" : version == 0x10 ? "2.3" : "2";
    song->formatName = "Digitrakker " + ver + " (MDL, " + juce::String (numCh) + U (" Kanäle)");
    fixOrders (*song);
    return song;
}

// ============================================================================================ display
juce::String effectText (const Song& s, const Cell& c)
{
    const auto hex2 = [] (int v) { return juce::String::toHexString (v & 255).toUpperCase().paddedLeft ('0', 2); };
    if (c.fx == 0 && c.param == 0) return "...";
    if (s.s3mEffects())
        return (c.fx >= 1 && c.fx <= 26 ? juce::String::charToString ((juce::juce_wchar) ('A' + c.fx - 1)) : juce::String (".")) + hex2 (c.param);
    juce::juce_wchar ch;
    if (c.fx < 10) ch = (juce::juce_wchar) ('0' + c.fx);
    else if (c.fx < 36) ch = (juce::juce_wchar) ('A' + c.fx - 10);
    else
    {
        switch (c.fx)
        {
            case fxOktSlideUp: ch = 'u'; break;
            case fxOktSlideDown: ch = 'd'; break;
            case fxOktUpOnce: ch = 'h'; break;
            case fxOktDownOnce: ch = 'l'; break;
            case fxOktArp1: ch = 'a'; break;
            case fxOktArp2: ch = 'b'; break;
            case fxOktArp3: ch = 'c'; break;
            case fxSpeed: case fxTempo: case fxMedTempo: ch = 'F'; break;      // 669/FAR/MDL speed and tempo (F < 20 = speed)
            case fxItPanSlide: ch = 'P'; break;
            case fxFcPorta: ch = 'p'; break;   // Future Composer portamento
            default: ch = '?'; break;
        }
    }
    return juce::String::charToString (ch) + hex2 (c.param);
}

juce::String volumeText (const Cell& c)
{
    if (c.vcmd == vcNone) return c.vol == kVolNone ? juce::String ("...") : "v" + juce::String ((int) c.vol).paddedLeft ('0', 2);
    static const char letters[] = " dcbauhplrgefpo";
    const int v = c.vcmd == vcPan ? juce::roundToInt (c.vol * 64.0 / 15.0) : c.vol;
    return juce::String::charToString ((juce::juce_wchar) letters[juce::jlimit (1, 14, (int) c.vcmd)]) + juce::String (v).paddedLeft ('0', 2);
}
} // namespace sw
