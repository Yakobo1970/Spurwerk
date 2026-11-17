#include "Convert.h"
#include "ModFile.h"
#include "Export.h"
#include "DawProject.h"
#include <map>
#include <set>

namespace sw
{
namespace
{
constexpr double kPtC5Rate = 7093789.2 / (2.0 * 428.0);   // ProTracker C-2 (our C-5) at finetune 0

struct Counter
{
    std::map<juce::String, int> lost;
    void add (const juce::String& what) { ++lost[what]; }
};

// S3M/IT letter effect (1 = A ... 26 = Z) -> XM-numbered effect for MOD/MED, false if no equivalent
bool letterToXm (int fx, int p, bool it, Cell& out)
{
    const int hi = p >> 4, lo = p & 15;
    auto set = [&out] (int f, int q) { out.fx = (juce::uint8) f; out.param = (juce::uint8) q; return true; };
    switch (fx)
    {
        case 1: return p > 0 && p < 32 ? set (0xF, p) : false;                         // A speed
        case 2: return set (0xB, p);                                                    // B jump
        case 3: return set (0xD, it ? (p / 10) * 16 + p % 10 : p);                      // C break (IT hex -> BCD)
        case 4:                                                                         // D volume slide
            if (lo == 0xF && hi) return set (0xE, 0xA0 | hi);
            if (hi == 0xF && lo) return set (0xE, 0xB0 | lo);
            return set (0xA, hi ? (hi << 4) : lo);
        case 5:                                                                         // E porta down
            if (hi == 0xF) return set (0xE, 0x20 | lo);
            if (hi == 0xE) return set (0xE, 0x20 | juce::jmax (1, lo / 4));
            return set (0x2, p);
        case 6:                                                                         // F porta up
            if (hi == 0xF) return set (0xE, 0x10 | lo);
            if (hi == 0xE) return set (0xE, 0x10 | juce::jmax (1, lo / 4));
            return set (0x1, p);
        case 7: return set (0x3, p);
        case 8: return set (0x4, p);
        case 10: return p ? set (0x0, p) : false;                                       // J arpeggio
        case 11: return set (0x6, p);
        case 12: return set (0x5, p);
        case 15: return set (0x9, p);
        case 17: return lo ? set (0xE, 0x90 | lo) : false;                              // Q retrigger
        case 18: return set (0x7, p);
        case 19:                                                                        // S special
            switch (hi)
            {
                case 0x1: return set (0xE, 0x30 | lo);
                case 0x2: return set (0xE, 0x50 | lo);
                case 0x3: return set (0xE, 0x40 | lo);
                case 0x4: return set (0xE, 0x70 | lo);
                case 0x8: return set (0xE, 0x80 | lo);
                case 0xB: return set (0xE, 0x60 | lo);
                case 0xC: return set (0xE, 0xC0 | lo);
                case 0xD: return set (0xE, 0xD0 | lo);
                case 0xE: return set (0xE, 0xE0 | lo);
                default: return false;
            }
        case 20: return p >= 32 ? set (0xF, p) : false;                                 // T tempo
        case 21: return set (0x4, (p & 0xF0) | juce::jmax (1, lo / 4));                  // U fine vibrato
        case 24: return set (0x8, it ? juce::jmin (255, p) : juce::jmin (255, p * 2));   // X panning
        default: return false;
    }
}

// XM-numbered / internal effect -> MOD-compatible effect (0..F), false if no equivalent
bool xmToMod (const Song& src, int fx, int p, Cell& out)
{
    const int lo = p & 15;
    auto set = [&out] (int f, int q) { out.fx = (juce::uint8) f; out.param = (juce::uint8) q; return true; };
    if (fx <= 0xF) return (fx == 0 && p == 0) ? false : set (fx, p);
    switch (fx)
    {
        case fxSpeed: return p > 0 ? set (0xF, juce::jmin (31, p)) : false;
        case fxTempo: return set (0xF, juce::jlimit (32, 255, p));
        case fxMedTempo: return set (0xF, juce::jlimit (32, 255, src.medTempo (p)));
        case fxXmR: return lo ? set (0xE, 0x90 | lo) : false;
        case fxXmX: return ((p >> 4) == 1 || (p >> 4) == 2) ? set (0xE, ((p >> 4) == 1 ? 0x10 : 0x20) | juce::jmax (1, lo / 4)) : false;
        case fxFineVib: return set (0x4, (p & 0xF0) | juce::jmax (1, lo / 4));
        default: return false;
    }
}

juce::String fxName (const Song& s, const Cell& c) { return effectText (s, c).substring (0, 1); }

// rate of a sample -> note transpose + ProTracker finetune (eighths of a semitone)
std::pair<int, int> pitchFix (const Sample& s)
{
    const double semis = 12.0 * std::log2 (juce::jmax (1.0, s.c5Rate()) / kPtC5Rate);
    const int eighths = juce::roundToInt (semis * 8.0);
    const int tr = juce::roundToInt (eighths / 8.0);
    return { tr, juce::jlimit (-8, 7, eighths - tr * 8) };
}

int fitNote (int note, bool mod, Counter& cnt)
{
    const int lo = 49, hi = mod ? 84 : 120;   // MOD: ProTracker C-1..B-3, MED: up to B-9
    if (note >= lo && note <= hi) return note;
    cnt.add (mod ? U ("Noten außerhalb C-1..B-3 oktaviert") : U ("Noten außerhalb des Bereichs oktaviert"));
    while (note < lo) note += 12;
    while (note > hi) note -= 12;
    return note;
}

// pattern layout for the target: MOD = 64 rows (short patterns padded + break, long ones split)
void fitPatterns (Song& s, bool mod, Counter& cnt)
{
    if (! mod) return;
    std::vector<Pattern> pats;
    std::vector<std::vector<int>> mapTo;   // old pattern -> new patterns
    for (auto& p : s.patterns)
    {
        std::vector<int> ids;
        for (int start = 0; start < p.rows; start += 64)
        {
            Pattern q;
            q.resize (64, s.channels);
            const int n = juce::jmin (64, p.rows - start);
            for (int r = 0; r < n; ++r) for (int c = 0; c < s.channels; ++c) q.at (r, c) = p.at (start + r, c);
            if (n < 64)
            {
                bool hasBreak = false;
                for (int r = 0; r < n; ++r) for (int c = 0; c < s.channels; ++c) hasBreak |= q.at (r, c).fx == 0xD || q.at (r, c).fx == 0xB;
                if (! hasBreak)
                {
                    int col = s.channels - 1;
                    for (int c = 0; c < s.channels; ++c) if (q.at (n - 1, c).fx == 0 && q.at (n - 1, c).param == 0) { col = c; break; }
                    q.at (n - 1, col).fx = 0xD; q.at (n - 1, col).param = 0;
                }
            }
            ids.push_back ((int) pats.size());
            pats.push_back (std::move (q));
        }
        if (p.rows > 64) cnt.add (U ("Patterns über 64 Zeilen geteilt"));
        mapTo.push_back (ids);
    }
    std::vector<int> orders;
    for (int o : s.orders)
        if (o >= 0 && o < (int) mapTo.size()) for (int id : mapTo[(size_t) o]) orders.push_back (id);
    if (orders.size() > 128) { cnt.add (U ("Positionen nach 128 abgeschnitten")); orders.resize (128); }
    if (pats.size() > 100) cnt.add (U ("mehr als 100 Patterns (manche Player laden nur 64)"));
    s.patterns = std::move (pats);
    s.orders = std::move (orders);
}

std::unique_ptr<Song> convertCells (const Song& src, bool mod, Counter& cnt)
{
    auto dst = std::make_unique<Song>();
    dst->format = mod ? Format::MOD : Format::MED;
    dst->title = src.title;
    dst->artist = src.artist;
    dst->message = src.message;
    const int maxCh = mod ? 32 : kMaxChannels;
    dst->channels = juce::jlimit (1, maxCh, src.channels);
    if (src.channels > maxCh) cnt.add (U ("Kanäle abgeschnitten"));
    dst->initialSpeed = juce::jlimit (1, 31, src.initialSpeed);
    dst->initialTempo = juce::jlimit (32, 255, src.initialTempo);
    dst->restartPos = src.restartPos;
    dst->orders = src.orders;

    const int maxSmp = mod ? 31 : 63;
    std::map<int, int> sampleMap;   // source sample -> target sample (0-based)
    auto target = [&] (int s) -> int {
        auto it = sampleMap.find (s);
        if (it != sampleMap.end()) return it->second;
        if ((int) sampleMap.size() >= maxSmp) { cnt.add (U ("Samples über ") + juce::String (maxSmp) + U (" weggelassen")); return -1; }
        const int id = (int) sampleMap.size();
        sampleMap[s] = id;
        return id;
    };
    std::vector<int> lastInstr ((size_t) src.channels, 0);
    const bool letters = src.s3mEffects(), it = src.format == Format::IT;
    for (const auto& p : src.patterns)
    {
        Pattern q;
        q.resize (p.rows, dst->channels);
        for (int r = 0; r < p.rows; ++r)
            for (int c = 0; c < dst->channels && c < p.channels; ++c)
            {
                const Cell& in = p.at (r, c);
                Cell out;
                if (in.instr > 0) lastInstr[(size_t) c] = in.instr;
                const int ins = lastInstr[(size_t) c];
                if (in.note > 0 && in.note <= 120 && ins > 0)
                {
                    const int s = src.sampleFor (ins, in.note);
                    const int t = s >= 0 ? target (s) : -1;
                    if (t >= 0)
                    {
                        const auto [tr, ft] = pitchFix (src.samples[(size_t) s]);
                        juce::ignoreUnused (ft);
                        out.note = (juce::uint8) fitNote (src.noteFor (ins, in.note) + tr, mod, cnt);
                        out.instr = (juce::uint8) (t + 1);
                    }
                }
                else if (in.instr > 0 && in.note == 0 && src.instruments.empty() && in.instr <= src.numSamples())
                {
                    const int t = target (in.instr - 1);   // instrument without note: volume reset in MOD
                    if (t >= 0) out.instr = (juce::uint8) (t + 1);
                }
                // effect
                bool fxOk = false;
                if (in.fx != 0 || in.param != 0)
                {
                    fxOk = letters ? letterToXm (in.fx, in.param, it, out) : xmToMod (src, in.fx, in.param, out);
                    if (! fxOk && ! (in.fx == 0 && in.param == 0)) cnt.add (U ("Effekte ohne Entsprechung (") + fxName (src, in) + ")");
                }
                // note off / cut
                if ((in.note == kNoteOff || in.note == kNoteCut || in.note == kNoteFade) && out.fx == 0 && out.param == 0) { out.fx = 0xE; out.param = 0xC0; }
                // volume column -> effect if free
                if (in.vcmd != vcNone || in.vol <= 64)
                {
                    const bool free = out.fx == 0 && out.param == 0;
                    const int v = in.vol;
                    if (in.vcmd == vcNone && v <= 64 && free) { out.fx = 0xC; out.param = (juce::uint8) v; }
                    else if (in.vcmd == vcSlideUp && free) { out.fx = 0xA; out.param = (juce::uint8) (juce::jmin (15, v) << 4); }
                    else if (in.vcmd == vcSlideDown && free) { out.fx = 0xA; out.param = (juce::uint8) juce::jmin (15, v); }
                    else if (in.vcmd == vcFineUp && free) { out.fx = 0xE; out.param = (juce::uint8) (0xA0 | juce::jmin (15, v)); }
                    else if (in.vcmd == vcFineDown && free) { out.fx = 0xE; out.param = (juce::uint8) (0xB0 | juce::jmin (15, v)); }
                    else if (in.vcmd == vcPan && free) { out.fx = 0x8; out.param = (juce::uint8) juce::jmin (255, v * 17); }
                    else if (in.vcmd == vcPanFine && free) { out.fx = 0x8; out.param = (juce::uint8) juce::jmin (255, v * 4); }
                    else if (in.vcmd == vcOffsetHigh) cnt.add (U ("Sample-Offset über 64K"));
                    else cnt.add (U ("Lautstärkespalte (Platz belegt oder ohne Entsprechung)"));
                }
                q.at (r, c) = out;
            }
        dst->patterns.push_back (std::move (q));
    }
    if (! src.instruments.empty()) cnt.add (U ("Instrument-Hüllkurven, Fadeout und Auto-Vibrato"));
    if (src.format == Format::IT) cnt.add (U ("New Note Actions (ausklingende Noten)"));
    // samples
    dst->samples.resize (sampleMap.size());
    for (auto& [s, t] : sampleMap)
    {
        Sample smp = src.samples[(size_t) s];
        const auto [tr, ft] = pitchFix (smp);
        juce::ignoreUnused (tr);
        smp.finetune = ft;
        smp.c5Speed = 0.0;
        smp.pan = -1;
        smp.globalGain = 1.0f;
        if (mod && smp.pcm.size() > 131070) { smp.pcm.resize (131070); cnt.add (U ("Samples auf 128 KB gekürzt")); if (smp.loopStart + smp.loopLength > 131070) smp.loopLength = 0; }
        dst->samples[(size_t) t] = std::move (smp);
    }
    if (dst->samples.empty()) dst->samples.resize (1);
    if (mod) while (dst->samples.size() < 31) dst->samples.push_back (Sample());
    fitPatterns (*dst, mod, cnt);
    return dst;
}

// YM / COSO / Future Composer: notes as played (MIDI export data), 8 rows per beat (speed 3)
std::unique_ptr<Song> convertPlayed (const Song& src, bool mod, Counter& cnt)
{
    MidiExportOptions o;
    o.perSample = true;
    std::vector<int> groups;
    o.trackGroups = &groups;
    const auto mf = exportMidi (src, o);
    auto dst = std::make_unique<Song>();
    dst->format = mod ? Format::MOD : Format::MED;
    dst->title = src.title;
    dst->artist = src.format == Format::YM ? src.ym.author : src.artist;
    double bpm = 125.0;
    for (auto* ev : *mf.getTrack (0)) if (ev->message.isTempoMetaEvent()) { bpm = 60.0 / ev->message.getTempoSecondsPerQuarterNote(); break; }
    // 8 rows per beat at speed 3: tracker tempo = bpm
    dst->initialSpeed = 3;
    dst->initialTempo = juce::jlimit (32, 255, juce::roundToInt (bpm));
    cnt.add (src.format == Format::YM ? U ("YM-Klang (Hüllkurven, Rauschen, Digidrums) – Rechteck/Rauschen als Samples")
                                      : U ("Instrument-Makros (Wellenwechsel, Vibrato, Slides) – nur Grundnoten"));
    const Sample square = ymSquare(), noise = ymNoise();
    std::vector<int> trackSample;   // target sample per MIDI track
    std::vector<Sample> samples;
    std::map<int, int> srcToTarget;
    const int maxSmp = mod ? 31 : 63, maxCh = mod ? 32 : kMaxChannels;
    int channels = 0;
    struct Ev { int row; int note; int vel; bool on; };
    std::vector<std::vector<Ev>> perCh;
    int lastRow = 0;
    for (int t = 1; t < mf.getNumTracks(); ++t)
    {
        auto seq = *mf.getTrack (t);
        seq.updateMatchedPairs();
        juce::String name;
        bool any = false;
        for (auto* ev : seq) { if (ev->message.isTrackNameEvent() && name.isEmpty()) name = ev->message.getTextFromTextMetaEvent(); any |= ev->message.isNoteOn(); }
        if (! any) continue;
        if (channels >= maxCh) { cnt.add (U ("Spuren über ") + juce::String (maxCh) + U (" weggelassen")); break; }
        int target = -1;
        if (src.format == Format::YM)
        {
            const bool isNoise = name.contains ("Rauschen");
            const int key = isNoise ? 1 : 0;
            if (srcToTarget.count (key) == 0) { srcToTarget[key] = (int) samples.size(); samples.push_back (isNoise ? noise : square); }
            target = srcToTarget[key];
        }
        else
        {
            const int s = t - 1 < (int) groups.size() ? groups[(size_t) t - 1] : -1;
            if (s < 0 || s >= src.numSamples()) continue;
            if (srcToTarget.count (s) == 0)
            {
                if ((int) samples.size() >= maxSmp) { cnt.add (U ("Samples weggelassen")); continue; }
                srcToTarget[s] = (int) samples.size();
                samples.push_back (src.samples[(size_t) s]);
            }
            target = srcToTarget[s];
        }
        std::vector<Ev> evs;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            const int row = juce::roundToInt (seq.getEventTime (i) / (kExportPpq / 8.0));
            if (m.isNoteOn()) evs.push_back ({ row, m.getNoteNumber() + 1, m.getVelocity(), true });
            else if (m.isNoteOff()) evs.push_back ({ row, 0, 0, false });
            lastRow = juce::jmax (lastRow, row);
        }
        trackSample.push_back (target);
        perCh.push_back (std::move (evs));
        ++channels;
    }
    dst->channels = juce::jmax (1, channels);
    const int numPats = juce::jmax (1, lastRow / 64 + 1);
    if (mod && numPats > 128) cnt.add (U ("Song nach 128 Patterns abgeschnitten"));
    for (int pi = 0; pi < juce::jmin (numPats, mod ? 128 : 256); ++pi)
    {
        Pattern p;
        p.resize (64, dst->channels);
        dst->patterns.push_back (std::move (p));
        dst->orders.push_back (pi);
    }
    for (int c = 0; c < channels; ++c)
    {
        const auto& smp = samples[(size_t) trackSample[(size_t) c]];
        const int tr = pitchFix (smp).first;
        for (auto& e : perCh[(size_t) c])
        {
            const int pi = e.row / 64, r = e.row % 64;
            if (pi >= (int) dst->patterns.size()) continue;
            Cell& cell = dst->patterns[(size_t) pi].at (r, c);
            if (e.on)
            {
                if (cell.note != 0) { cnt.add (U ("gleichzeitige Noten in einer Zeile")); continue; }
                cell.note = (juce::uint8) fitNote (e.note + tr, mod, cnt);
                cell.instr = (juce::uint8) (trackSample[(size_t) c] + 1);
                cell.fx = 0xC;
                cell.param = (juce::uint8) juce::jlimit (1, 64, (e.vel * 64 + 63) / 127);
            }
            else if (cell.note == 0 && cell.fx == 0) { cell.fx = 0xE; cell.param = 0xC0; }
        }
    }
    for (auto& s : samples)
    {
        s.finetune = pitchFix (s).second;
        s.c5Speed = 0.0;
        s.pan = -1;
    }
    dst->samples = std::move (samples);
    if (dst->samples.empty()) dst->samples.resize (1);
    if (mod) while (dst->samples.size() < 31) dst->samples.push_back (Sample());
    cnt.add (U ("Pitchbends, Lautstärkeverläufe"));
    return dst;
}

int medNote (int note) { return juce::jlimit (1, 127, note - 48); }

// common cell effect -> OctaMED command (MMD1)
std::pair<int, int> medCommand (const Cell& c)
{
    const int p = c.param, hi = p >> 4, lo = p & 15;
    switch (c.fx)
    {
        case 0x0: return { 0x00, p };
        case 0x1: return { 0x01, p };
        case 0x2: return { 0x02, p };
        case 0x3: return { 0x03, p };
        case 0x4: return { 0x14, p };   // ProTracker-compatible vibrato
        case 0x5: return { 0x05, p };
        case 0x6: return { 0x06, p };
        case 0x7: return { 0x07, p };
        case 0x8: return { 0x2E, (juce::uint8) (juce::int8) juce::jlimit (-16, 16, (p - 128) * 16 / 128) };
        case 0x9: return { 0x19, p };
        case 0xA: return { 0x0D, p };
        case 0xB: return { 0x0B, p };
        case 0xC: return { 0x0C, juce::jmin (64, p) };
        case 0xD: return { 0x1D, p };
        case 0xE:
            switch (hi)
            {
                case 0x1: return { 0x11, lo };
                case 0x2: return { 0x12, lo };
                case 0x5: return { 0x15, lo };
                case 0x6: return { 0x16, lo };
                case 0x9: return { 0x1F, lo };
                case 0xA: return { 0x1A, lo };
                case 0xB: return { 0x1B, lo };
                case 0xC: return { 0x18, lo };
                case 0xD: return { 0x1F, lo << 4 };
                case 0xE: return { 0x1E, lo };
                default: return { 0, 0 };
            }
        case 0xF: return p < 32 ? std::pair<int, int> { 0x09, p } : std::pair<int, int> { 0x0F, juce::jmin (240, p) };
        default: return { 0, 0 };
    }
}
} // namespace

juce::String saveTargetName (SaveTarget t) { return t == SaveTarget::MOD ? juce::String ("ProTracker MOD") : juce::String ("OctaMED (MMD1)"); }

std::unique_ptr<Song> convertSong (const Song& src, SaveTarget target, juce::StringArray& notes)
{
    Counter cnt;
    const bool mod = target == SaveTarget::MOD;
    std::unique_ptr<Song> out;
    if (src.format == Format::MOD && mod) out = std::make_unique<Song> (src);
    else if (src.format == Format::YM || src.format == Format::FC) out = convertPlayed (src, mod, cnt);
    else out = convertCells (src, mod, cnt);
    for (auto& [what, n] : cnt.lost) notes.add (what + (n > 1 ? " (" + juce::String (n) + "x)" : juce::String()));
    if (mod && out != nullptr)
    {
        out->formatName = out->channels == 4 ? U ("ProTracker MOD (M.K., 4 Kanäle)") : "ProTracker MOD (" + juce::String (out->channels) + U (" Kanäle)");
    }
    return out;
}

bool saveMed (const Song& s, juce::MemoryBlock& out, juce::String& error)
{
    const int ch = juce::jlimit (1, 64, s.channels);
    const int numBlocks = juce::jmin (65535, (int) s.patterns.size());
    const int numSamples = juce::jmin (63, s.numSamples());
    if (numBlocks == 0) { error = "Song ohne Patterns"; return false; }
    juce::MemoryOutputStream os (out, false);
    auto w8 = [&os] (int v) { os.writeByte ((char) (v & 0xff)); };
    auto w16 = [&os] (int v) { os.writeByte ((char) ((v >> 8) & 0xff)); os.writeByte ((char) (v & 0xff)); };
    auto w32 = [&os] (juce::int64 v) { for (int k = 3; k >= 0; --k) os.writeByte ((char) ((v >> (8 * k)) & 0xff)); };
    // offsets
    const juce::int64 songOff = 52, blockArr = songOff + 788;
    std::vector<juce::int64> blockOff;
    juce::int64 pos = blockArr + numBlocks * 4;
    for (int b = 0; b < numBlocks; ++b) { blockOff.push_back (pos); pos += 8 + (juce::int64) s.patterns[(size_t) b].rows * ch * 4; }
    const juce::int64 smplArr = pos;
    pos += numSamples * 4;
    std::vector<juce::int64> smpOff;
    for (int i = 0; i < numSamples; ++i)
    {
        const auto& sm = s.samples[(size_t) i];
        smpOff.push_back (sm.pcm.empty() ? 0 : pos);
        if (! sm.pcm.empty()) pos += 6 + (juce::int64) sm.pcm.size() + (sm.pcm.size() & 1);
    }
    const juce::int64 expOff = pos;
    pos += 128;
    const juce::int64 expSmp = pos;
    pos += numSamples * 4;
    const juce::int64 iinfo = pos;
    pos += numSamples * 42;
    const auto titleUtf = s.title.toStdString();
    const juce::int64 nameOff = pos;
    pos += (juce::int64) titleUtf.size() + 1;
    const auto anno = s.message.replace ("\n", "\n").toStdString();
    const juce::int64 annoOff = anno.empty() ? 0 : pos;
    pos += anno.empty() ? 0 : (juce::int64) anno.size() + 1;
    const juce::int64 total = pos;

    // header
    os.write ("MMD1", 4);
    w32 (total); w32 (songOff); w16 (0); w16 (0); w32 (blockArr);
    w8 (0); w8 (0); w8 (0); w8 (0);
    w32 (smplArr); w32 (0); w32 (expOff); w32 (0);
    w16 (0); w16 (0); w16 (0); w16 (0); w16 (0); w8 (0); w8 (0);
    // song: 63 sample entries
    for (int i = 0; i < 63; ++i)
    {
        if (i < numSamples)
        {
            const auto& sm = s.samples[(size_t) i];
            const bool loop = sm.hasLoop();
            w16 (loop ? sm.loopStart / 2 : 0);
            w16 (loop ? juce::jmax (2, sm.loopLength / 2) : 1);
            w8 (0); w8 (0); w8 (juce::jlimit (0, 64, sm.volume)); w8 (0);
        }
        else { w16 (0); w16 (1); w8 (0); w8 (0); w8 (64); w8 (0); }
    }
    w16 (numBlocks);
    const int songLen = juce::jlimit (1, 256, (int) s.orders.size());
    w16 (songLen);
    for (int i = 0; i < 256; ++i) w8 (i < songLen ? juce::jlimit (0, numBlocks - 1, s.orders[(size_t) i]) : 0);
    w16 (juce::jlimit (1, 240, s.initialTempo));   // BPM mode, 4 lines per beat: tempo = tracker tempo
    w8 (0);                                         // play transpose
    w8 (0x10 | 0x20);                               // hex volumes, ProTracker-style slides
    w8 (0x20 | 3);                                  // BPM mode, 4 lines per beat
    w8 (juce::jlimit (1, 32, s.initialSpeed));
    for (int c = 0; c < 16; ++c) w8 (c < (int) s.channelVolInit.size() ? juce::jlimit (1, 64, s.channelVolInit[(size_t) c]) : 64);
    w8 (64);
    w8 (numSamples);
    // block array + blocks
    for (auto o : blockOff) w32 (o);
    for (int b = 0; b < numBlocks; ++b)
    {
        const auto& p = s.patterns[(size_t) b];
        w16 (ch); w16 (p.rows - 1); w32 (0);
        for (int r = 0; r < p.rows; ++r)
            for (int c = 0; c < ch; ++c)
            {
                Cell cell;
                if (c < p.channels) cell = p.at (r, c);
                w8 (cell.note > 0 && cell.note <= 120 ? medNote (cell.note) : 0);
                w8 (juce::jmin (63, (int) cell.instr));
                const auto [cmd, prm] = medCommand (cell);
                w8 (cmd); w8 (prm);
            }
    }
    // samples
    for (auto o : smpOff) w32 (o);
    for (int i = 0; i < numSamples; ++i)
    {
        const auto& sm = s.samples[(size_t) i];
        if (sm.pcm.empty()) continue;
        w32 ((juce::int64) sm.pcm.size());
        w16 (0);
        for (auto v : sm.pcm) os.writeByte ((char) (juce::int8) juce::jlimit (-128, 127, juce::roundToInt (v * 128.0f)));
        if (sm.pcm.size() & 1) w8 (0);
    }
    // expansion data: sample extensions (finetune), instrument names, song name, annotation
    {
        std::vector<juce::uint8> e (128, 0);
        auto put32 = [&e] (int at, juce::int64 v) { for (int k = 0; k < 4; ++k) e[(size_t) at + (size_t) k] = (juce::uint8) ((v >> (8 * (3 - k))) & 0xff); };
        auto put16 = [&e] (int at, int v) { e[(size_t) at] = (juce::uint8) ((v >> 8) & 0xff); e[(size_t) at + 1] = (juce::uint8) (v & 0xff); };
        put32 (4, expSmp); put16 (8, numSamples); put16 (10, 4);
        put32 (12, annoOff); put32 (16, anno.empty() ? 0 : (juce::int64) anno.size());
        put32 (20, iinfo); put16 (24, numSamples); put16 (26, 42);
        put32 (44, nameOff); put32 (48, (juce::int64) titleUtf.size());
        os.write (e.data(), e.size());
    }
    for (int i = 0; i < numSamples; ++i) { w8 (0); w8 (0); w8 (0); w8 (s.samples[(size_t) i].finetune); }
    for (int i = 0; i < numSamples; ++i)
    {
        const auto n = s.samples[(size_t) i].name.toStdString();
        for (int k = 0; k < 42; ++k) w8 (k < 40 && k < (int) n.size() ? (juce::uint8) n[(size_t) k] : 0);
    }
    os.write (titleUtf.data(), titleUtf.size()); w8 (0);
    if (! anno.empty()) { os.write (anno.data(), anno.size()); w8 (0); }
    os.flush();
    if ((juce::int64) out.getSize() != total) { error = "interner Fehler beim MED-Schreiben"; return false; }
    return true;
}

bool saveSongAs (const Song& src, SaveTarget target, const juce::File& file, juce::String& report)
{
    juce::StringArray notes;
    auto conv = convertSong (src, target, notes);
    if (conv == nullptr) { report = U ("Umwandlung fehlgeschlagen"); return false; }
    juce::MemoryBlock mb;
    juce::String err;
    const bool ok = target == SaveTarget::MOD ? saveMod (*conv, mb, err) : saveMed (*conv, mb, err);
    if (! ok) { report = err; return false; }
    if (! file.replaceWithData (mb.getData(), mb.getSize())) { report = "Datei kann nicht geschrieben werden: " + file.getFullPathName(); return false; }
    report = saveTargetName (target) + ": " + juce::String (conv->channels) + U (" Kanäle, ") + juce::String ((int) conv->patterns.size()) + " Patterns";
    if (! notes.isEmpty()) report << U (" – nicht übernommen: ") << notes.joinIntoString (", ");
    return true;
}
} // namespace sw
