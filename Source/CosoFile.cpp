// Jochen Hippel COSO (Atari ST, YM2149) loader.
// The song is played through a port of the Hippel COSO replay logic (structure and sequence handling follow
// NostalgicPlayer's Hippel player, MIT licence, (c) Thomas Neumann) and recorded as YM2149 register frames,
// so playback, pattern view and MIDI export reuse the YM code path.
// ST specific sequence commands (derived from the files): E4 xx tone+noise (noise period xx), E5 noise only
// (noise period = note), E6 tone only; E9 xx fine detune (added to the tone period), EA hh ll fixed tone period.
#include "CosoFile.h"
#include "YmFile.h"

namespace sw
{
namespace
{
int be16 (const juce::uint8* p) { return (p[0] << 8) | p[1]; }
juce::uint32 be32 (const juce::uint8* p) { return ((juce::uint32) p[0] << 24) | ((juce::uint32) p[1] << 16) | ((juce::uint32) p[2] << 8) | p[3]; }

using Bytes = std::vector<juce::uint8>;
const Bytes kDefaultSeq { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe1 };

struct Envelope { int speed = 1, freqNumber = 0, vibSpeed = 0, vibDepth = 0, vibDelay = 0; Bytes table; };
struct PosEntry { int track = 0, noteTranspose = 0, envTranspose = 0; };

struct CosoData
{
    std::vector<Bytes> freqs, tracks;
    std::vector<Envelope> envs;
    std::vector<std::array<PosEntry, 3>> positions;
    struct Sub { int start = 0, last = 0, speed = 1; };
    std::vector<Sub> songs;
    juce::String id;
};

struct Voice
{
    const Bytes* envTable = &kDefaultSeq; int envPos = 0, origEnv = 0, curEnv = 0;
    Bytes envScratch;
    const Bytes* freqTable = &kDefaultSeq; int freqPos = 0, origFreq = 0, curFreq = 0;
    int nextPos = 0;
    const Bytes* track = nullptr; int trackPos = 0;
    int trackTranspose = 0, envTranspose = 0;
    int transpose = 0, note = 0, info = 0, prevInfo = 0;
    int tick = 0, cosoCounter = 0, cosoSpeed = 0;
    int volume = 0, envCounter = 1, envSpeed = 1, envSustain = 0;
    int vibFlag = 0, vibSpeed = 0, vibDelay = 0, vibDepth = 0, vibDelta = 0;
    juce::uint32 portaDelta = 0;
    // ST mixer
    bool tone = true, noise = false;
    int noisePeriod = 0, detune = 0, fixedPeriod = 0;
};

int ymPeriodForNote (int n)
{
    // note 0 = C-1 (32.70 Hz) on a 2 MHz YM2149
    const double f = 32.703 * std::pow (2.0, n / 12.0);
    return juce::jlimit (1, 4095, juce::roundToInt (2000000.0 / (16.0 * f)));
}

class Replayer
{
public:
    Replayer (const CosoData& d, int sub) : data (d), song (d.songs[(size_t) sub])
    {
        speed = speedCounter = juce::jmax (1, song.speed);
        speedCounter = 1;
        for (int i = 0; i < 3; ++i)
        {
            auto& v = voices[(size_t) i];
            const auto& pe = data.positions[(size_t) song.start][(size_t) i];
            v.nextPos = song.start + 1;
            v.track = &data.tracks[(size_t) juce::jlimit (0, (int) data.tracks.size() - 1, pe.track)];
            v.trackTranspose = pe.noteTranspose;
            v.envTranspose = pe.envTranspose;
        }
        visited.assign (data.positions.size() + 1, 0);
        visited[(size_t) song.start] = 1;
    }

    bool ended = false;
    int loopFrame = -1;

    void frame (std::array<juce::uint8, 16>& regs)
    {
        if (--speedCounter <= 0)
        {
            speedCounter = speed;
            for (int i = 0; i < 3; ++i) readRow (i);
        }
        regs.fill (0);
        regs[13] = 0xff;
        int mixer = 0x3f;
        int noiseP = -1;
        for (int i = 0; i < 3; ++i)
        {
            auto& v = voices[(size_t) i];
            parseFreq (v);
            const int period = runEffects (v);
            regs[(size_t) (i * 2)] = (juce::uint8) (period & 0xff);
            regs[(size_t) (i * 2 + 1)] = (juce::uint8) ((period >> 8) & 0x0f);
            regs[(size_t) (8 + i)] = (juce::uint8) juce::jlimit (0, 15, v.volume & 0x0f);
            if (v.tone) mixer &= ~(1 << i);
            if (v.noise)
            {
                mixer &= ~(8 << i);
                noiseP = v.noisePeriod;
            }
        }
        regs[6] = (juce::uint8) (noiseP < 0 ? 0 : (noiseP & 31));
        regs[7] = (juce::uint8) mixer;
    }

private:
    const CosoData& data;
    const CosoData::Sub& song;
    std::array<Voice, 3> voices;
    int speed = 1, speedCounter = 1;
    std::vector<juce::uint8> visited;

    juce::uint8 trackByte (const Voice& v, int pos) const
    {
        if (v.track == nullptr || pos < 0 || pos >= (int) v.track->size()) return 0xff;
        return (*v.track)[(size_t) pos];
    }

    void readRow (int voice)
    {
        auto& v = voices[(size_t) voice];
        if (--v.cosoCounter >= 0) return;
        v.cosoCounter = v.cosoSpeed;
        for (int guard = 0; guard < 64; ++guard)
        {
            const int val = trackByte (v, v.trackPos);
            if (val == 0xff)
            {
                if (v.nextPos >= song.last || v.nextPos >= (int) data.positions.size())
                {
                    v.nextPos = song.start;
                    ended = true;
                }
                const auto& pe = data.positions[(size_t) v.nextPos][(size_t) voice];
                v.trackTranspose = pe.noteTranspose;
                v.envTranspose = pe.envTranspose;
                v.track = &data.tracks[(size_t) juce::jlimit (0, (int) data.tracks.size() - 1, pe.track)];
                v.trackPos = 0;
                if (voice == 0)
                {
                    if (visited[(size_t) v.nextPos]) ended = true;
                    visited[(size_t) v.nextPos] = 1;
                }
                ++v.nextPos;
                continue;
            }
            if (val == 0xfe || val == 0xfd)
            {
                v.cosoSpeed = (juce::int8) trackByte (v, v.trackPos + 1);
                v.cosoCounter = v.cosoSpeed;
                v.trackPos += 2;
                if (val == 0xfd) return;
                continue;
            }
            v.note = val;
            v.info = trackByte (v, v.trackPos + 1);
            if ((v.info & 0xe0) != 0) { v.prevInfo = trackByte (v, v.trackPos + 2); v.trackPos += 3; }
            else v.trackPos += 2;
            v.portaDelta = 0;
            if (val < 128)
            {
                int e = (v.info & 0x1f) + v.envTranspose;
                if (e < 0 || e >= (int) data.envs.size()) e = 0;
                const auto& env = data.envs[(size_t) e];
                v.envCounter = v.envSpeed = juce::jmax (1, env.speed);
                v.envSustain = 0;
                v.vibFlag = 0x40;
                v.vibSpeed = env.vibSpeed;
                v.vibDepth = env.vibDepth;
                v.vibDelta = env.vibDepth;
                v.vibDelay = env.vibDelay;
                v.envTable = &env.table;
                v.envPos = 0;
                v.origEnv = v.curEnv = e;
                int fn = env.freqNumber;
                if (fn != 0x80)
                {
                    if ((v.info & 0x40) != 0) fn = v.prevInfo;
                    v.freqTable = fn < (int) data.freqs.size() ? &data.freqs[(size_t) fn] : &kDefaultSeq;
                    v.freqPos = 0;
                    v.origFreq = v.curFreq = fn;
                    v.tick = 0;
                    v.detune = 0;
                    v.fixedPeriod = 0;
                }
            }
            return;
        }
    }

    juce::uint8 fbyte (const Voice& v, int pos) const
    {
        if (pos < 0 || pos >= (int) v.freqTable->size()) return 0xe1;
        return (*v.freqTable)[(size_t) pos];
    }

    void parseFreq (Voice& v)
    {
        for (int big = 0; big < 8; ++big)
        {
            if (v.tick != 0) { --v.tick; return; }
            bool bigLoop = false;
            for (int guard = 0; guard < 64; ++guard)
            {
                if (v.freqPos >= (int) v.freqTable->size())
                {
                    if (v.curFreq + 1 < (int) data.freqs.size()) { v.freqTable = &data.freqs[(size_t) ++v.curFreq]; v.freqPos = 0; }
                    else { v.freqTable = &kDefaultSeq; v.freqPos = (int) kDefaultSeq.size() - 1; }
                }
                int val = fbyte (v, v.freqPos);
                if (val == 0xe1) return;
                if (val == 0xe0)
                {
                    v.freqPos = fbyte (v, v.freqPos + 1) & 0x3f;
                    if (v.origFreq < (int) data.freqs.size()) v.freqTable = &data.freqs[(size_t) v.origFreq];
                    v.curFreq = v.origFreq;
                    val = fbyte (v, v.freqPos);
                    if (val == 0xe1) return;
                }
                bool more = false;
                switch (val)
                {
                    case 0xe2: v.envPos = 0; v.envCounter = 1; v.freqPos += 1; more = true; break;   // restart volume sequence
                    case 0xe3: v.vibSpeed = fbyte (v, v.freqPos + 1); v.vibDepth = fbyte (v, v.freqPos + 2); v.freqPos += 3; more = true; break;
                    case 0xe4: v.tone = true; v.noise = true; v.noisePeriod = fbyte (v, v.freqPos + 1) & 31; v.freqPos += 2; more = true; break;
                    case 0xe5: v.tone = false; v.noise = true; v.freqPos += 1; more = true; break;
                    case 0xe6: v.tone = true; v.noise = false; v.freqPos += 1; more = true; break;
                    case 0xe7:
                    {
                        const int fn = fbyte (v, v.freqPos + 1);
                        if (fn < (int) data.freqs.size()) { v.freqTable = &data.freqs[(size_t) fn]; v.origFreq = v.curFreq = fn; }
                        v.freqPos = 0;
                        more = true;
                        break;
                    }
                    case 0xe8: v.tick = fbyte (v, v.freqPos + 1); v.freqPos += 2; bigLoop = true; break;
                    case 0xe9: v.detune = fbyte (v, v.freqPos + 1); v.freqPos += 2; more = true; break;
                    case 0xea:
                    {
                        const int hi = fbyte (v, v.freqPos + 1), lo = fbyte (v, v.freqPos + 2);
                        v.fixedPeriod = (hi & 0x80) ? 0 : (((hi & 0x0f) << 8) | lo);
                        v.freqPos += 3;
                        more = true;
                        break;
                    }
                    default:
                        if (val >= 0xeb && val <= 0xef) { v.freqPos += 1; more = true; break; }
                        break;
                }
                if (bigLoop) break;
                if (! more)
                {
                    v.transpose = fbyte (v, v.freqPos++);
                    return;
                }
            }
            if (! bigLoop) return;
        }
    }

    int runEffects (Voice& v)
    {
        // volume sequence
        for (int big = 0; big < 8; ++big)
        {
            bool bigLoop = false;
            if (v.envSustain == 0)
            {
                if (--v.envCounter <= 0)
                {
                    v.envCounter = v.envSpeed;
                    for (int guard = 0; guard < 64; ++guard)
                    {
                        if (v.envPos >= (int) v.envTable->size())
                        {
                            // continue into the next envelope (including its parameter bytes)
                            if (v.curEnv + 1 >= (int) data.envs.size()) { v.envPos = (int) v.envTable->size(); break; }
                            const auto& ne = data.envs[(size_t) ++v.curEnv];
                            v.envScratch = { (juce::uint8) ne.speed, (juce::uint8) ne.freqNumber, (juce::uint8) ne.vibSpeed, (juce::uint8) ne.vibDepth, (juce::uint8) ne.vibDelay };
                            v.envScratch.insert (v.envScratch.end(), ne.table.begin(), ne.table.end());
                            v.envTable = &v.envScratch;
                            v.envPos = 0;
                        }
                        const int val = (*v.envTable)[(size_t) v.envPos];
                        if (val == 0xe0)
                        {
                            const int target = v.envPos + 1 < (int) v.envTable->size() ? ((*v.envTable)[(size_t) v.envPos + 1] & 0x3f) - 5 : 0;
                            v.envTable = &data.envs[(size_t) v.origEnv].table;
                            v.curEnv = v.origEnv;
                            v.envPos = juce::jmax (0, target);
                            continue;
                        }
                        if (val == 0xe1) break;
                        if (val == 0xe8)
                        {
                            v.envSustain = v.envPos + 1 < (int) v.envTable->size() ? (*v.envTable)[(size_t) v.envPos + 1] : 0;
                            v.envPos += 2;
                            bigLoop = true;
                            break;
                        }
                        v.volume = val;
                        ++v.envPos;
                        break;
                    }
                }
            }
            else --v.envSustain;
            if (! bigLoop) break;
        }

        int note = v.transpose;
        if (note < 128) note = note + v.note + v.trackTranspose;
        note &= 0x7f;
        if (v.noise && ! v.tone) v.noisePeriod = note & 31;
        int period = v.fixedPeriod > 0 ? v.fixedPeriod : ymPeriodForNote (note);

        // vibrato (Hippel version 1)
        if (v.vibDelay == 0)
        {
            int value = note * 2, depth = v.vibDepth * 2, delta = v.vibDelta;
            if ((v.vibFlag & 0x80) == 0 || (v.vibFlag & 0x01) == 0)
            {
                if ((v.vibFlag & 0x20) == 0) { delta -= v.vibSpeed; if (delta < 0) { v.vibFlag |= 0x20; delta = 0; } }
                else { delta += v.vibSpeed; if (delta >= depth) { v.vibFlag &= ~0x20; delta = depth; } }
                v.vibDelta = delta & 0xff;
            }
            delta -= v.vibDepth;
            if (delta != 0)
            {
                value += 160;
                while (value < 256) { delta += delta; value += 24; }
                period += delta;
            }
        }
        else --v.vibDelay;
        v.vibFlag ^= 0x01;

        // portamento (version 1)
        if ((v.info & 0x20) != 0)
        {
            const int val = (juce::int8) v.prevInfo;
            if (val >= 0) { v.portaDelta += (juce::uint32) (val << 11); period -= (int) (v.portaDelta >> 16); }
            else { v.portaDelta -= (juce::uint32) (val * 2048); period += (int) (v.portaDelta >> 16); }
        }
        period += v.detune;
        return juce::jlimit (1, 4095, period);
    }
};

bool parse (const juce::MemoryBlock& mb, CosoData& c, juce::String& error)
{
    const auto* d = static_cast<const juce::uint8*> (mb.getData());
    const size_t size = mb.getSize();
    if (size < 52 || memcmp (d, "COSO", 4) != 0) { error = "Keine COSO-Datei"; return false; }
    juce::uint32 o[7];
    for (int i = 0; i < 7; ++i) o[i] = be32 (d + 4 + i * 4);
    c.id = juce::String ((const char*) d + 32, 4);
    const int nf = be16 (d + 36) + 1, nv = be16 (d + 38) + 1, nt = be16 (d + 40) + 1, np = be16 (d + 42) + 1, ns = be16 (d + 48);
    if (o[0] >= size || o[1] >= size || o[2] >= size || o[3] >= size || o[4] >= size || ns <= 0) { error = "COSO-Header fehlerhaft"; return false; }
    auto table = [&] (juce::uint32 off, int n, juce::uint32 end, std::vector<Bytes>& out) {
        if (off + (size_t) n * 2 > size) return false;
        std::vector<int> offs;
        for (int i = 0; i < n; ++i) offs.push_back (be16 (d + off + i * 2));
        offs.push_back ((int) end);
        out.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            int len = offs[(size_t) i + 1] - offs[(size_t) i];
            for (int j = 2; len == 0 && i + j <= n; ++j) len = offs[(size_t) (i + j)] - offs[(size_t) i];
            if (len < 0 || offs[(size_t) i] + len > (int) size) len = 0;
            out[(size_t) i].assign (d + offs[(size_t) i], d + offs[(size_t) i] + len);
        }
        return true;
    };
    std::vector<Bytes> envRaw;
    if (! table (o[0], nf, o[1], c.freqs) || ! table (o[1], nv, o[2], envRaw) || ! table (o[2], nt, o[3], c.tracks))
    { error = "COSO-Tabellen fehlerhaft"; return false; }
    for (auto& r : envRaw)
    {
        Envelope e;
        if (r.size() >= 5)
        {
            e.speed = r[0]; e.freqNumber = r[1]; e.vibSpeed = r[2]; e.vibDepth = r[3]; e.vibDelay = r[4];
            e.table.assign (r.begin() + 5, r.end());
        }
        c.envs.push_back (e);
    }
    // positions: 3 voices x (track, note transpose, envelope transpose, unused)
    for (int i = 0; i < np; ++i)
    {
        const size_t p = o[3] + (size_t) i * 12;
        if (p + 12 > size) break;
        std::array<PosEntry, 3> pe;
        for (int v = 0; v < 3; ++v)
            pe[(size_t) v] = { d[p + v * 4], (juce::int8) d[p + v * 4 + 1], (juce::int8) d[p + v * 4 + 2] };
        c.positions.push_back (pe);
    }
    for (int i = 0; i < ns; ++i)
    {
        const size_t p = o[4] + (size_t) i * 6;
        if (p + 6 > size) break;
        CosoData::Sub s { be16 (d + p), be16 (d + p + 2) + 1, be16 (d + p + 4) };
        if (s.speed != 0 && s.start < s.last && s.start < (int) c.positions.size()) c.songs.push_back (s);
    }
    if (c.songs.empty() || c.positions.empty() || c.tracks.empty() || c.envs.empty()) { error = "COSO ohne spielbare Songs"; return false; }
    return true;
}
} // namespace

bool isCoso (const juce::MemoryBlock& data)
{
    return data.getSize() > 4 && memcmp (data.getData(), "COSO", 4) == 0;
}

std::unique_ptr<Song> loadCoso (const juce::MemoryBlock& data, int subSong, juce::String& error)
{
    CosoData c;
    if (! parse (data, c, error)) return nullptr;
    subSong = juce::jlimit (0, (int) c.songs.size() - 1, subSong);
    // record the song as YM register frames
    YmData ym;
    ym.version = "COSO";
    ym.clock = 2000000;
    ym.rate = 50;
    {
        Replayer r (c, subSong);
        const int maxFrames = 50 * 60 * 15;
        std::array<juce::uint8, 16> regs {};
        for (int f = 0; f < maxFrames; ++f)
        {
            r.frame (regs);
            if (r.ended && f > 0) break;
            ym.regs.push_back (regs);
        }
    }
    ym.frames = (int) ym.regs.size();
    if (ym.frames < 2) { error = "COSO-Song ist leer"; return nullptr; }
    ym.author = "Jochen Hippel (COSO)";
    ym.comment = juce::String (c.songs.size()) + " Teilsongs, gespielt: " + juce::String (subSong + 1);

    // reuse the YM path (patterns from register frames)
    juce::MemoryBlock ymFile;
    {
        juce::MemoryOutputStream os (ymFile, false);
        os.write ("YM5!LeOnArD!", 12);
        auto w32 = [&os] (juce::uint32 v) { os.writeByte ((char) (v >> 24)); os.writeByte ((char) (v >> 16)); os.writeByte ((char) (v >> 8)); os.writeByte ((char) v); };
        auto w16 = [&os] (int v) { os.writeByte ((char) (v >> 8)); os.writeByte ((char) v); };
        w32 ((juce::uint32) ym.frames); w32 (0); w16 (0); w32 ((juce::uint32) ym.clock); w16 (ym.rate); w32 (0); w16 (0);
        os.writeByte (0); os.write (ym.author.toRawUTF8(), ym.author.getNumBytesAsUTF8()); os.writeByte (0); os.writeByte (0);
        for (const auto& rg : ym.regs) os.write (rg.data(), 16);
        os.write ("End!", 4);
    }
    auto s = loadYm (ymFile, error);
    if (s == nullptr) return nullptr;
    s->ym.version = "COSO";
    s->ym.author = ym.author;
    s->ym.comment = ym.comment;
    s->subSongs = (int) c.songs.size();
    s->subSong = subSong;
    s->formatName = "Hippel COSO (" + c.id.trim() + ", Teilsong " + juce::String (subSong + 1) + "/" + juce::String ((int) c.songs.size()) + ")";
    return s;
}
} // namespace sw
