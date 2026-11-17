// Spurwerk – module archive (see ModArchive.h). Duplicate index after Midi-Fundus: identical files (hash),
// same notes (note hash) and similar melodies (bottom-k sketch of transposition-invariant n-grams).
#include "ModArchive.h"
#include "ModFile.h"
#include "Export.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <map>

namespace sw::archive
{
// ============================================================================================ analysis
namespace
{
juce::uint32 mix32 (juce::uint32 x)
{
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

juce::uint64 fnv (const void* data, size_t n)
{
    juce::uint64 h = 1469598103934665603ull;
    auto* p = static_cast<const juce::uint8*> (data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// melodic 5-step n-grams per track (= sample): transposition and tempo invariant
std::vector<juce::uint32> noteGrams (const std::vector<Note>& notes)
{
    std::map<int, std::vector<const Note*>> voices;
    for (auto& nt : notes) voices[nt.track].push_back (&nt);
    std::vector<juce::uint32> grams;
    for (auto& [k, v] : voices)
    {
        std::vector<std::pair<float, int>> line;   // onset, highest pitch
        for (auto* nt : v)
        {
            if (! line.empty() && nt->start - line.back().first < 0.0625f) { line.back().second = juce::jmax (line.back().second, (int) nt->pitch); continue; }
            line.push_back ({ nt->start, nt->pitch });
        }
        if (line.size() < 6) continue;
        for (size_t i = 0; i + 5 < line.size(); ++i)
        {
            juce::uint32 g = 0x9e3779b9u;
            bool allZero = true;
            for (size_t j = 0; j < 5; ++j)
            {
                const int d = juce::jlimit (-12, 12, line[i + j + 1].second - line[i + j].second);
                allZero &= d == 0;
                g = g * 31u + (juce::uint32) (d + 13);
            }
            for (size_t j = 0; j + 1 < 5; ++j)
            {
                const float a = line[i + j + 1].first - line[i + j].first, b = line[i + j + 2].first - line[i + j + 1].first;
                const int cls = b < a * 0.8f ? 0 : (b > a * 1.25f ? 2 : 1);
                g = g * 7u + (juce::uint32) cls;
            }
            if (! allZero) grams.push_back (mix32 (g));
        }
    }
    std::sort (grams.begin(), grams.end());
    grams.erase (std::unique (grams.begin(), grams.end()), grams.end());
    return grams;
}
} // namespace

juce::String shortFormat (const Song& s)
{
    switch (s.format)
    {
        case Format::MOD: return "MOD";
        case Format::XM: return "XM";
        case Format::S3M: return "S3M";
        case Format::IT: return s.formatName.contains ("MPTM") ? "MPTM" : "IT";
        case Format::MTM: return "MTM";
        case Format::OKT: return "OKT";
        case Format::F669: return "669";
        case Format::FAR: return "FAR";
        case Format::MDL: return "MDL";
        case Format::MED: return "MED";
        case Format::TCB: return "TCB";
        case Format::FC: return "FC";
        case Format::YM: return s.formatName.containsIgnoreCase ("hippel") || s.formatName.containsIgnoreCase ("coso") ? "COSO" : "YM";
    }
    return "?";
}

float sketchSimilarity (const std::vector<juce::uint32>& a, const std::vector<juce::uint32>& b)
{
    if (a.empty() || b.empty()) return 0.0f;
    const size_t k = juce::jmin (a.size(), b.size());
    size_t i = 0, j = 0, taken = 0, both = 0;
    while (taken < k && (i < a.size() || j < b.size()))
    {
        if (j >= b.size() || (i < a.size() && a[i] < b[j])) ++i;
        else if (i >= a.size() || b[j] < a[i]) ++j;
        else { ++both; ++i; ++j; }
        ++taken;
    }
    return (float) both / (float) k;
}

juce::String formatDuration (double s)
{
    const int t = juce::roundToInt (s);
    return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
}

Analysis analyseModule (const juce::File& f, bool keepNotes)
{
    Analysis a;
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb)) { a.error = "Datei kann nicht gelesen werden"; return a; }
    a.fileHash = fnv (mb.getData(), mb.getSize());
    juce::String err;
    auto song = loadSongData (mb, f.getFileName(), err);
    if (song == nullptr)
    {
        a.error = err.isNotEmpty() ? err : juce::String ("unbekanntes Format");
        a.notModule = ! hasModuleSignature (mb);   // unknown content: e.g. a game archive, a text file or an unsupported Amiga format
        if (a.notModule)
        {
            const auto known = unsupportedFormatName (mb);
            a.error = known.isNotEmpty() ? known + juce::String::fromUTF8 (" – Format wird nicht unterstützt") : juce::String::fromUTF8 ("unbekanntes Format (keine Modul-Kennung gefunden)");
        }   // no signature matched: e.g. a game archive called *.mod / *.stk
        return a;
    }
    a.ok = true;
    a.title = song->title;
    a.artist = song->format == Format::YM ? song->ym.author : song->artist;
    a.format = shortFormat (*song);
    a.formatName = song->formatName;
    a.message = song->format == Format::YM ? song->ym.comment : song->message;
    a.channels = song->format == Format::YM ? 3 : song->channels;
    a.patterns = (int) song->patterns.size();
    a.orders = (int) song->orders.size();
    a.instruments = (int) song->instruments.size();
    for (const auto& s : song->samples) if (! s.pcm.empty()) ++a.samples;

    // sample (and instrument) names: trackers used them as a notepad
    {
        juce::StringArray lines;
        for (const auto& s : song->samples) lines.add (s.name);
        if (! song->instruments.empty())
        {
            juce::StringArray ins;
            for (const auto& in : song->instruments) ins.add (in.name);
            while (! ins.isEmpty() && ins[ins.size() - 1].trim().isEmpty()) ins.remove (ins.size() - 1);
            if (ins.joinIntoString ("") != lines.joinIntoString ("") && ! ins.isEmpty()) { lines.add ({}); lines.addArray (ins); }
        }
        while (! lines.isEmpty() && lines[lines.size() - 1].trim().isEmpty()) lines.remove (lines.size() - 1);
        while (! lines.isEmpty() && lines[0].trim().isEmpty()) lines.remove (0);
        a.sampleText = lines.joinIntoString ("\n");
    }

    // notes: the song played once (with effects) like the MIDI export, one track per sample
    MidiExportOptions opt;
    opt.perSample = true;
    opt.maxMinutes = 20.0;
    auto mf = exportMidi (*song, opt);
    juce::MidiFile secs (mf);
    secs.convertTimestampTicksToSeconds();
    std::vector<Note> notes;
    std::vector<int> trackCounts;
    for (int t = 1; t < mf.getNumTracks(); ++t)
    {
        auto seq = *mf.getTrack (t);
        seq.updateMatchedPairs();
        juce::String name;
        int cnt = 0;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            if (m.isTrackNameEvent() && name.isEmpty()) name = m.getTextFromTextMetaEvent();
            if (! m.isNoteOn()) continue;
            const double on = seq.getEventTime (i), off = seq.getTimeOfMatchingKeyUp (i);
            Note n;
            n.start = (float) (on / kExportPpq);
            n.len = (float) (juce::jmax (on + 1.0, off) - on) / (float) kExportPpq;
            n.pitch = (juce::uint8) m.getNoteNumber();
            n.track = (short) (t - 1);
            notes.push_back (n);
            ++cnt;
        }
        trackCounts.push_back (cnt);
        a.trackNames.push_back (name);
    }
    for (int t = 0; t < secs.getNumTracks(); ++t)
    {
        const auto* tr = secs.getTrack (t);
        if (tr->getNumEvents() > 0) a.seconds = juce::jmax (a.seconds, (float) tr->getEndTime());
    }
    std::sort (notes.begin(), notes.end(), [] (const Note& x, const Note& y) { return x.start != y.start ? x.start < y.start : x.pitch < y.pitch; });
    a.notes = (int) notes.size();
    {
        juce::uint64 h = 1469598103934665603ull;
        auto feed = [&h] (juce::uint32 v) { for (int i = 0; i < 4; ++i) { h ^= (v >> (i * 8)) & 0xff; h *= 1099511628211ull; } };
        for (auto& nt : notes) { feed ((juce::uint32) juce::roundToInt (nt.start * 48.0f)); feed (nt.pitch); }
        a.noteHash = notes.empty() ? 0 : h;
    }
    {
        auto grams = noteGrams (notes);
        a.gramCount = (int) grams.size();
        a.sketch.assign (grams.begin(), grams.begin() + juce::jmin ((int) grams.size(), kSketch));
    }
    {
        // profile: note onsets over time, stacked by sample (most used first)
        std::vector<std::pair<int, int>> order;
        for (int t = 0; t < (int) trackCounts.size(); ++t) if (trackCounts[(size_t) t] > 0) order.push_back ({ trackCounts[(size_t) t], t });
        std::sort (order.begin(), order.end(), [] (auto& x, auto& y) { return x.first > y.first; });
        std::vector<int> slot (trackCounts.size(), kLanes - 1);
        for (int i = 0; i < juce::jmin ((int) order.size(), kLanes); ++i) slot[(size_t) order[(size_t) i].second] = i;
        float endBeat = 0;
        for (auto& nt : notes) endBeat = juce::jmax (endBeat, nt.start + nt.len);
        std::array<float, kBins * kLanes> acc {};
        if (endBeat > 0)
            for (auto& nt : notes)
            {
                const int b = juce::jlimit (0, kBins - 1, (int) (nt.start / endBeat * kBins));
                acc[(size_t) (b * kLanes + slot[(size_t) nt.track])] += 1.0f;
            }
        float mx = 0;
        for (int b = 0; b < kBins; ++b)
        {
            float sum = 0;
            for (int l = 0; l < kLanes; ++l) sum += acc[(size_t) (b * kLanes + l)];
            mx = juce::jmax (mx, sum);
        }
        if (mx > 0)
            for (size_t i = 0; i < acc.size(); ++i) a.profile[i] = (juce::uint8) juce::jlimit (0, 255, juce::roundToInt (acc[i] / mx * 255.0f));
    }
    // key / mode / scale (Krumhansl-Kessler, as in Midi-Fundus); percussion samples (at most two different
    // pitches) are left out – trackers play drums at a fixed note, which would pull the key towards it
    {
        std::map<int, std::set<int>> pitchesPerTrack;
        for (auto& nt : notes) pitchesPerTrack[nt.track].insert (nt.pitch);
        std::array<double, 12> pc {};
        for (auto& nt : notes)
            if (pitchesPerTrack[nt.track].size() > 2) pc[(size_t) (nt.pitch % 12)] += juce::jmin (4.0f, nt.len);
        double total = 0;
        for (auto v : pc) total += v;
        if (total > 0)
        {
            static const double maj[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
            static const double mnr[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };
            static const char* const majNames[12] = { "C", "Des", "D", "Es", "E", "F", "Fis", "G", "As", "A", "B", "H" };
            static const char* const minNames[12] = { "c", "cis", "d", "es", "e", "f", "fis", "g", "gis", "a", "b", "h" };
            auto corr = [&] (const double* prof, int tonic) {
                double mx = 0, my = 0;
                for (int i = 0; i < 12; ++i) { mx += pc[(size_t) i]; my += prof[i]; }
                mx /= 12; my /= 12;
                double sxy = 0, sxx = 0, syy = 0;
                for (int i = 0; i < 12; ++i)
                {
                    const double x = pc[(size_t) ((i + tonic) % 12)] - mx, y = prof[i] - my;
                    sxy += x * y; sxx += x * x; syy += y * y;
                }
                return sxx > 0 ? sxy / std::sqrt (sxx * syy) : 0.0;
            };
            double best = -2; int bt = 0; bool bm = false;
            for (int t = 0; t < 12; ++t)
            {
                const double x = corr (maj, t), y = corr (mnr, t);
                if (x > best) { best = x; bt = t; bm = false; }
                if (y > best) { best = y; bt = t; bm = true; }
            }
            a.keyConfidence = (float) best;
            a.key = juce::String (bm ? minNames[bt] : majNames[bt]) + (bm ? "-Moll" : "-Dur") + (best < 0.6 ? "?" : "");
            a.mode = bm ? "Moll" : "Dur";
            int used = 0; juce::uint16 set = 0;
            for (int i = 0; i < 12; ++i) if (pc[(size_t) i] >= 0.03 * total) { ++used; set |= (juce::uint16) (1u << i); }
            auto rot = [] (std::initializer_list<int> steps, int tonic) { juce::uint16 r = 0; for (int k : steps) r |= (juce::uint16) (1u << ((k + tonic) % 12)); return r; };
            const int majTonic = bm ? (bt + 3) % 12 : bt;
            const auto diat = rot ({ 0, 2, 4, 5, 7, 9, 11 }, majTonic), penta = rot ({ 0, 2, 4, 7, 9 }, majTonic);
            const auto harm = rot ({ 0, 2, 3, 5, 7, 8, 11 }, bt), blues = rot ({ 0, 3, 5, 6, 7, 10 }, bt);
            if (used <= 5 && (set & ~penta) == 0) a.scale = "Pentatonik";
            else if ((set & ~diat) == 0) a.scale = "Diatonisch";
            else if (bm && (set & ~harm) == 0) a.scale = "Harmonisch Moll";
            else if ((set & ~(blues | diat)) == 0 && (set & blues) == blues) a.scale = "Blues";
            else if (used >= 10) a.scale = "Chromatisch";
            else a.scale = "Erweitert";
            a.scale << juce::String::fromUTF8 (" · ") << used;
        }
    }
    if (keepNotes) a.noteList = std::move (notes);
    return a;
}

void Entry::fill (const Analysis& a)
{
    title = a.title; artist = a.artist; format = a.format; formatName = a.formatName; message = a.message; sampleText = a.sampleText;
    error = a.error;
    channels = a.channels; patterns = a.patterns; orders = a.orders; samples = a.samples; instruments = a.instruments; notes = a.notes;
    seconds = a.seconds; fileHash = a.fileHash; noteHash = a.noteHash; sketch = a.sketch; gramCount = a.gramCount; profile = a.profile;
    key = a.key; mode = a.mode; scale = a.scale; keyConfidence = a.keyConfidence;
    flags &= ~(juce::uint32) (fError | fMissing | fNotModule);   // fExcluded stays
    if (! a.ok) flags |= fError;
    if (a.notModule) flags |= fNotModule;
    if (title.trim().isEmpty()) title = juce::File::createFileWithoutCheckingPath (path).getFileNameWithoutExtension();
}

// ============================================================================================ duplicate index
struct DupIndex
{
    struct FP { juce::uint64 fileHash, noteHash; int notes, gramCount; std::vector<juce::uint32> sketch; bool error; };
    std::unordered_map<juce::uint64, std::vector<int>> fileIdx, noteIdx;
    std::unordered_map<juce::uint32, std::vector<int>> gramIdx;
    std::unordered_map<int, std::vector<DupLink>> links;
    std::unordered_map<int, int> parent;
    std::unordered_map<int, FP> fps;
    std::unordered_map<int, int> shared;
    std::set<juce::uint64> notDup;
    float threshold = 0.45f;

    static juce::uint64 pairKey (int a, int b)
    {
        return a < b ? ((juce::uint64) (juce::uint32) a << 32) | (juce::uint32) b : ((juce::uint64) (juce::uint32) b << 32) | (juce::uint32) a;
    }
    int find (int id)
    {
        auto it = parent.find (id);
        if (it == parent.end()) { parent[id] = id; return id; }
        int r = id;
        while (parent[r] != r) r = parent[r];
        while (parent[id] != r) { const int nx = parent[id]; parent[id] = r; id = nx; }
        return r;
    }
    void unite (int a, int b)
    {
        a = find (a); b = find (b);
        if (a == b) return;
        if (a < b) parent[b] = a; else parent[a] = b;
    }
    void add (int id, const FP& fp)
    {
        fps[id] = fp;
        if (fp.error) return;
        auto link = [&] (int other, float score, juce::uint8 kind) {
            if (other == id || notDup.count (pairKey (other, id)) != 0) return;
            auto put = [] (std::vector<DupLink>& v, int o, float s, juce::uint8 k) {
                for (auto& l : v) if (l.other == o) { if (k > l.kind || (k == l.kind && s > l.score)) { l.score = s; l.kind = k; } return; }
                v.push_back ({ o, s, k });
            };
            put (links[id], other, score, kind);
            put (links[other], id, score, kind);
            unite (id, other);
        };
        auto& c = fileIdx[fp.fileHash];
        for (int o : c) link (o, 1.0f, dupIdentical);
        c.push_back (id);
        if (fp.noteHash != 0 && fp.notes >= 8)
        {
            auto& v = noteIdx[fp.noteHash];
            for (int o : v) if (fps[o].fileHash != fp.fileHash) link (o, 1.0f, dupSameNotes);
            v.push_back (id);
        }
        if (fp.gramCount >= 20 && ! fp.sketch.empty())
        {
            shared.clear();
            for (auto g : fp.sketch)
            {
                auto& post = gramIdx[g];
                if (post.size() < 400)
                    for (int o : post) ++shared[o];
                post.push_back (id);
            }
            for (auto& [o, cnt] : shared)
            {
                if (cnt < 4) continue;
                auto& ofp = fps[o];
                if (ofp.gramCount < 20 || ofp.noteHash == fp.noteHash) continue;
                const float s = sketchSimilarity (fp.sketch, ofp.sketch);
                if (s >= threshold) link (o, s, dupSimilar);
            }
        }
    }
    void remove (int id)
    {
        auto it = fps.find (id);
        if (it == fps.end()) return;
        auto drop = [id] (std::vector<int>& v) { v.erase (std::remove (v.begin(), v.end(), id), v.end()); };
        if (auto c = fileIdx.find (it->second.fileHash); c != fileIdx.end()) drop (c->second);
        if (auto n = noteIdx.find (it->second.noteHash); n != noteIdx.end()) drop (n->second);
        for (auto g : it->second.sketch) if (auto p = gramIdx.find (g); p != gramIdx.end()) drop (p->second);
        if (auto l = links.find (id); l != links.end())
        {
            for (auto& x : l->second)
                if (auto o = links.find (x.other); o != links.end())
                    o->second.erase (std::remove_if (o->second.begin(), o->second.end(), [id] (auto& y) { return y.other == id; }), o->second.end());
            links.erase (l);
        }
        fps.erase (it);
        parent.clear();
        for (auto& [a, v] : links) for (auto& x : v) unite (a, x.other);
    }
    // minimum cut between a and b (Edmonds-Karp); strong links cost more than weak "similar" ones
    std::vector<juce::uint64> separate (int a, int b)
    {
        if (find (a) != find (b)) return {};
        const int root = find (a);
        std::unordered_map<int, int> node;
        std::vector<int> ids;
        for (auto& [id, v] : links)
            if (! v.empty() && find (id) == root) { node[id] = (int) ids.size(); ids.push_back (id); }
        const int n = (int) ids.size();
        struct Edge { int to; double cap; };
        std::vector<Edge> edges;
        std::vector<std::vector<int>> adj ((size_t) n);
        auto weight = [] (const DupLink& l) { return l.kind == dupIdentical ? 1000.0 : l.kind == dupSameNotes ? 100.0 : 1.0 + l.score; };
        for (int i = 0; i < n; ++i)
            for (auto& l : links[ids[(size_t) i]])
            {
                auto it = node.find (l.other);
                if (it == node.end() || it->second < i) continue;
                const double w = weight (l);
                adj[(size_t) i].push_back ((int) edges.size()); edges.push_back ({ it->second, w });
                adj[(size_t) it->second].push_back ((int) edges.size()); edges.push_back ({ i, w });
            }
        const int s = node[a], t = node[b];
        std::vector<int> prevEdge ((size_t) n);
        auto bfs = [&] {
            std::fill (prevEdge.begin(), prevEdge.end(), -1);
            std::vector<char> seen ((size_t) n, 0);
            std::vector<int> q { s };
            seen[(size_t) s] = 1;
            for (size_t qi = 0; qi < q.size(); ++qi)
                for (int ei : adj[(size_t) q[qi]])
                {
                    auto& e = edges[(size_t) ei];
                    if (e.cap <= 1e-9 || seen[(size_t) e.to]) continue;
                    seen[(size_t) e.to] = 1; prevEdge[(size_t) e.to] = ei; q.push_back (e.to);
                }
            return seen;
        };
        for (int guard = 0; guard < 100000; ++guard)
        {
            bfs();
            if (prevEdge[(size_t) t] < 0) break;
            double f = 1e18;
            for (int v = t; v != s; v = edges[(size_t) (prevEdge[(size_t) v] ^ 1)].to) f = std::min (f, edges[(size_t) prevEdge[(size_t) v]].cap);
            for (int v = t; v != s; v = edges[(size_t) (prevEdge[(size_t) v] ^ 1)].to)
            {
                edges[(size_t) prevEdge[(size_t) v]].cap -= f;
                edges[(size_t) (prevEdge[(size_t) v] ^ 1)].cap += f;
            }
        }
        const auto side = bfs();
        std::vector<juce::uint64> out;
        for (int i = 0; i < n; ++i)
            if (side[(size_t) i])
                for (auto& l : links[ids[(size_t) i]])
                    if (auto it = node.find (l.other); it != node.end() && ! side[(size_t) it->second])
                        out.push_back (pairKey (ids[(size_t) i], l.other));
        return out;
    }
    void cut (const std::vector<juce::uint64>& pairs)
    {
        if (pairs.empty()) return;
        const std::set<juce::uint64> drop (pairs.begin(), pairs.end());
        for (auto& [id, v] : links)
            v.erase (std::remove_if (v.begin(), v.end(), [&, me = id] (const DupLink& l) { return drop.count (pairKey (me, l.other)) != 0; }), v.end());
        parent.clear();
        for (auto& [x, v] : links) for (auto& l : v) unite (x, l.other);
    }
};

static DupIndex::FP fingerprintOf (const Entry& e)
{
    return { e.fileHash, e.noteHash, e.notes, e.gramCount, e.sketch, e.has (fError) || e.has (fMissing) || e.has (fExcluded) };
}

// ============================================================================================ scanner
struct Archive::Scanner
{
    Archive& owner;
    juce::StringArray dirs;
    std::unordered_map<juce::String, std::pair<juce::int64, juce::int64>> known;   // path -> size, mtime (skip unchanged)
    bool full = false;
    std::atomic<bool> stop { false };
    std::vector<std::thread> threads;
    std::thread lister;
    juce::CriticalSection qLock;
    std::deque<juce::File> queue;
    std::atomic<bool> listed { false };
    std::atomic<int> running { 0 };

    Scanner (Archive& a) : owner (a) {}
    ~Scanner()
    {
        stop = true;
        if (lister.joinable()) lister.join();
        for (auto& t : threads) if (t.joinable()) t.join();
    }
    void start()
    {
        lister = std::thread ([this] {
            for (auto& d : dirs)
            {
                for (const auto& it : juce::RangedDirectoryIterator (juce::File (d), true, "*", juce::File::findFiles))
                {
                    if (stop) break;
                    const auto f = it.getFile();
                    if (! isSupportedFile (f)) continue;
                    {
                        const juce::ScopedLock sl (owner.pendingLock);
                        owner.seenPaths.insert (f.getFullPathName());
                    }
                    if (! full)
                        if (auto k = known.find (f.getFullPathName()); k != known.end() && k->second.first == it.getFileSize()
                            && k->second.second == it.getModificationTime().toMilliseconds())
                            continue;
                    const juce::ScopedLock sl (qLock);
                    queue.push_back (f);
                    ++owner.total;
                }
                if (stop) break;
            }
            listed = true;
        });
        const int n = juce::jlimit (1, 8, (int) std::thread::hardware_concurrency() - 1);
        running = n;
        for (int i = 0; i < n; ++i)
            threads.emplace_back ([this] {
                for (;;)
                {
                    if (stop) break;
                    juce::File f;
                    {
                        const juce::ScopedLock sl (qLock);
                        if (! queue.empty()) { f = queue.front(); queue.pop_front(); }
                    }
                    if (f == juce::File())
                    {
                        if (listed) break;
                        std::this_thread::sleep_for (std::chrono::milliseconds (20));
                        continue;
                    }
                    auto e = std::make_unique<Entry>();
                    e->path = f.getFullPathName();
                    e->name = f.getFileName();
                    e->folder = f.getParentDirectory().getFullPathName();
                    e->size = f.getSize();
                    e->mtime = f.getLastModificationTime().toMilliseconds();
                    Analysis a;
                    try { a = analyseModule (f, false); }
                    catch (...) { a.error = "Analyse fehlgeschlagen"; }
                    e->fill (a);
                    {
                        const juce::ScopedLock sl (owner.pendingLock);
                        owner.pending.push_back (std::move (e));
                    }
                    ++owner.done;
                }
                --running;
            });
    }
    bool finished() const { return listed && running == 0; }
};

// ============================================================================================ archive
Archive::Archive()
{
    load();
    startIndexBuild();
    startTimer (250);
    if (upgradeScan && ! folders.isEmpty()) startScan();   // older database: fill in key / mode / scale
}

Archive::~Archive()
{
    stopTimer();
    scanner.reset();
    if (indexThread.joinable()) indexThread.join();
    if (dirty) save();
}

Entry* Archive::byId (int id) const { auto it = idIndex.find (id); return it == idIndex.end() ? nullptr : it->second; }
Entry* Archive::byPath (const juce::String& p) const { auto it = pathIndex.find (p); return it == pathIndex.end() ? nullptr : it->second; }

juce::String Archive::addFolder (const juce::File& dir)
{
    const auto p = dir.getFullPathName();
    for (auto& f : folders)
        if (p == f || dir.isAChildOf (juce::File (f))) return f;   // already covered
    for (int i = folders.size(); --i >= 0;)
        if (juce::File (folders[i]).isAChildOf (dir)) folders.remove (i);   // the new folder covers older ones
    folders.add (p);
    folders.sort (true);
    saveSoon();
    changed();
    return p;
}

int Archive::countInFolder (const juce::String& path) const
{
    const juce::File dir (path);
    int n = 0;
    for (auto& e : list) if (juce::File (e->path).isAChildOf (dir)) ++n;
    return n;
}

int Archive::removeFolder (const juce::String& path)
{
    stopScan();
    folders.removeString (path);
    const juce::File dir (path);
    std::vector<int> ids;
    for (auto& e : list) if (juce::File (e->path).isAChildOf (dir)) ids.push_back (e->id);
    removeEntries (ids);
    saveSoon();
    changed();
    return (int) ids.size();
}

int Archive::removeMissing()
{
    std::vector<int> ids;
    for (auto& e : list) if (e->has (fMissing)) ids.push_back (e->id);
    removeEntries (ids);
    return (int) ids.size();
}

void Archive::startScan (bool fullRescan, const juce::StringArray& only)
{
    stopScan();
    scanner = std::make_unique<Scanner> (*this);
    scanner->dirs = only.isEmpty() ? folders : only;
    scanner->full = fullRescan;
    for (auto& e : list)   // failed files are tried again (newer loaders), excluded ones stay in the pool
        if (! e->has (fError) || e->has (fExcluded)) scanner->known[e->path] = { e->size, e->mtime };
    scannedFolders = scanner->dirs;
    seenPaths.clear();
    total = 0;
    done = 0;
    scanStart = lastScanSave = juce::Time::getMillisecondCounter();
    scanner->start();
    changed();
}

void Archive::stopScan()
{
    if (scanner == nullptr) return;
    scanner->stop = true;
    scanner.reset();
    pump();
    seenPaths.clear();   // incomplete listing: do not mark anything as missing
    changed();
}

juce::String Archive::scanStatus() const
{
    if (scanner == nullptr) return {};
    const int t = total.load(), d = done.load();
    juce::String s = juce::String::fromUTF8 ("Einlesen: ") + juce::String (d) + " / " + juce::String (t);
    if (! scanner->listed) s << juce::String::fromUTF8 (" (Ordner werden durchsucht …)");
    else if (d > 0 && t > d)
    {
        const double el = (juce::Time::getMillisecondCounter() - scanStart) / 1000.0;
        s << juce::String::fromUTF8 (" – noch ca. ") << formatDuration (el / d * (t - d));
    }
    return s;
}

void Archive::insertOrReplace (std::unique_ptr<Entry> e)
{
    if (auto* old = byPath (e->path))
    {
        e->id = old->id;
        e->rating = old->rating;
        e->favourite = old->favourite;
        e->comment = old->comment;
        e->composer = old->composer;
        e->arranger = old->arranger;
        e->edited = old->edited;
        if (old->edited & 1) e->title = old->title;
        if (old->edited & 2) e->artist = old->artist;
        e->flags |= old->flags & fExcluded;
        if (index != nullptr) index->remove (old->id);
        *old = std::move (*e);
        if (index != nullptr) index->add (old->id, fingerprintOf (*old));
        return;
    }
    e->id = nextId++;
    auto* raw = e.get();
    pathIndex[raw->path] = raw;
    idIndex[raw->id] = raw;
    list.push_back (std::move (e));
    if (index != nullptr) index->add (raw->id, fingerprintOf (*raw));
}

void Archive::pump()
{
    std::deque<std::unique_ptr<Entry>> got;
    {
        const juce::ScopedLock sl (pendingLock);
        got.swap (pending);
    }
    for (auto& e : got) insertOrReplace (std::move (e));
    bool finishedNow = false;
    if (scanner != nullptr && scanner->finished())
    {
        {
            const juce::ScopedLock sl (pendingLock);
            for (auto& e : pending) insertOrReplace (std::move (e));
            pending.clear();
        }
        // files of the scanned folders that were not seen any more: missing
        for (auto& e : list)
        {
            bool inside = false;
            for (auto& d : scannedFolders) inside |= juce::File (e->path).isAChildOf (juce::File (d));
            if (! inside) continue;
            const bool seen = seenPaths.count (e->path) != 0;
            if (seen) e->flags &= ~(juce::uint32) fMissing;
            else e->flags |= fMissing;
        }
        scanner.reset();
        seenPaths.clear();
        finishedNow = true;
    }
    if (! got.empty() || finishedNow)
    {
        finishGroups();
        dirty = true;
        changed();
    }
}

void Archive::waitForScan()
{
    while (scanner != nullptr)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        pump();
    }
    waitForIndex();
}

void Archive::timerCallback()
{
    if (indexReady.load() && installIndex()) changed();
    if (scanner != nullptr)
    {
        pump();
        if (scanner != nullptr && juce::Time::getMillisecondCounter() - lastScanSave > 90000) { lastScanSave = juce::Time::getMillisecondCounter(); save(); }
        else changed();   // progress
    }
    if (dirty && ++saveCountdown >= 8) { saveCountdown = 0; save(); }
}

void Archive::openUndo (const juce::String& label) { undoStack.push_back ({ label, {} }); undoOpen = true; }

void Archive::closeUndo()
{
    undoOpen = false;
    if (! undoStack.empty() && undoStack.back().items.empty()) undoStack.pop_back();
    while (undoStack.size() > 200) undoStack.erase (undoStack.begin());
}

void Archive::remember (const Entry& e)
{
    if (! undoOpen) return;
    auto& items = undoStack.back().items;
    for (auto& it : items) if (it.id == e.id) return;   // first state of this step counts
    items.push_back ({ e.id, e.title, e.artist, e.composer, e.arranger, e.comment, e.edited, e.flags & fExcluded, e.rating, e.favourite });
}

juce::String Archive::undo()
{
    if (undoStack.empty()) return {};
    auto step = std::move (undoStack.back());
    undoStack.pop_back();
    for (auto& it : step.items)
        if (auto* e = byId (it.id))
        {
            e->title = it.title; e->artist = it.artist; e->composer = it.composer; e->arranger = it.arranger; e->comment = it.comment;
            e->edited = it.edited; e->rating = it.rating; e->favourite = it.favourite;
            const bool wasExcluded = (it.flags & fExcluded) != 0;
            if (wasExcluded != e->has (fExcluded))
            {
                if (wasExcluded) e->flags |= fExcluded; else e->flags &= ~(juce::uint32) fExcluded;
                if (index != nullptr) { index->remove (e->id); index->add (e->id, fingerprintOf (*e)); }
            }
        }
    finishGroups();
    saveSoon();
    changed();
    return step.label;
}

void Archive::setRating (Entry& e, int r) { UndoGroup g (*this, "Bewertung"); remember (e); e.rating = juce::jlimit (0, 5, r); saveSoon(); changed(); }
void Archive::setFavourite (Entry& e, bool fav) { UndoGroup g (*this, "Favorit"); remember (e); e.favourite = fav; saveSoon(); changed(); }
void Archive::setComment (Entry& e, const juce::String& t) { UndoGroup g (*this, "Notiz"); remember (e); e.comment = t; saveSoon(); changed(); }

void Archive::setField (Entry& e, Field f, const juce::String& v)
{
    UndoGroup g (*this, f == Field::title ? "Titel" : f == Field::artist ? juce::String::fromUTF8 ("Künstler") : f == Field::composer ? "Komponist" : "Arrangeur");
    remember (e);
    const auto t = v.trim();
    switch (f)
    {
        case Field::title: e.title = t; e.edited |= 1; break;
        case Field::artist: e.artist = t; e.edited |= 2; break;
        case Field::composer: e.composer = t; break;
        case Field::arranger: e.arranger = t; break;
    }
    saveSoon();
    changed();
}

juce::StringArray Archive::protectedFolders() const
{
    juce::StringArray a;
    a.addLines (view.getProperty ("protected").toString());
    a.removeEmptyStrings();
    return a;
}

void Archive::setProtected (const juce::String& folder, bool on)
{
    auto a = protectedFolders();
    if (on) a.addIfNotAlreadyThere (folder);
    else a.removeString (folder);
    view.setProperty ("protected", a.joinIntoString ("\n"), nullptr);
    protectedCache = a;
    saveSoon();
    changed();
}

bool Archive::isProtected (const juce::String& path) const
{
    if (protectedCacheVersion != view.getProperty ("protected").toString().hashCode()) { protectedCache = protectedFolders(); protectedCacheVersion = view.getProperty ("protected").toString().hashCode(); }
    if (path.isEmpty()) return false;
    const juce::File f (path);
    for (auto& d : protectedCache)
        if (f == juce::File (d) || f.isAChildOf (juce::File (d))) return true;
    return false;
}

int Archive::countPool() const
{
    int n = 0;
    for (auto& e : list) if (e->inPool()) ++n;
    return n;
}

void Archive::setExcluded (const std::vector<int>& ids, bool excluded)
{
    UndoGroup g (*this, excluded ? juce::String::fromUTF8 ("Ausschließen") : juce::String ("Wieder aufnehmen"));
    for (int id : ids)
        if (auto* e = byId (id))
        {
            remember (*e);
            if (excluded) e->flags |= fExcluded; else e->flags &= ~(juce::uint32) fExcluded;
            if (index != nullptr) { index->remove (id); index->add (id, fingerprintOf (*e)); }
        }
    finishGroups();
    saveSoon();
    changed();
}

void Archive::removeEntries (const std::vector<int>& ids)
{
    if (ids.empty()) return;
    const std::set<int> drop (ids.begin(), ids.end());
    for (int id : ids) if (index != nullptr) index->remove (id);
    for (auto& e : list) if (drop.count (e->id)) { pathIndex.erase (e->path); idIndex.erase (e->id); }
    list.erase (std::remove_if (list.begin(), list.end(), [&] (auto& e) { return e == nullptr || drop.count (e->id) != 0; }), list.end());
    finishGroups();
    saveSoon();
    changed();
}

void Archive::markNotDuplicate (int a, int b)
{
    if (a == b) return;
    waitForIndex();
    std::vector<juce::uint64> pairs { DupIndex::pairKey (a, b) };
    if (index != nullptr)
    {
        for (auto k : index->separate (a, b)) pairs.push_back (k);
        for (auto k : pairs) index->notDup.insert (k);
        index->cut (pairs);
    }
    for (auto k : pairs) notDup.insert (k);
    finishGroups();
    saveSoon();
    changed();
}

std::vector<int> Archive::groupMembers (int group) const
{
    std::vector<int> out;
    if (group == 0) return out;
    for (auto& e : list) if (e->group == group) out.push_back (e->id);
    return out;
}

std::vector<DupLink> Archive::linksOf (int id) const
{
    if (index == nullptr) return {};
    auto it = index->links.find (id);
    return it == index->links.end() ? std::vector<DupLink>() : it->second;
}

void Archive::setSimilarityThreshold (float t)
{
    threshold = juce::jlimit (0.1f, 1.0f, t);
    startIndexBuild();
    saveSoon();
    changed();
}

int Archive::duplicateGroups() const
{
    std::set<int> g;
    for (auto& e : list) if (e->group != 0) g.insert (e->group);
    return (int) g.size();
}

void Archive::startIndexBuild()
{
    if (indexThread.joinable()) indexThread.join();
    index.reset();
    indexReady = false;
    std::vector<std::pair<int, DupIndex::FP>> snap;
    snap.reserve (list.size());
    for (auto& e : list) snap.push_back ({ e->id, fingerprintOf (*e) });
    const float thr = threshold;
    auto nd = notDup;
    indexThread = std::thread ([this, snap = std::move (snap), thr, nd] {
        auto idx = std::make_unique<DupIndex>();
        idx->threshold = thr;
        idx->notDup = nd;
        for (auto& [id, fp] : snap) idx->add (id, fp);
        const juce::ScopedLock sl (builtLock);
        builtIndex = std::move (idx);
        indexReady = true;
    });
}

bool Archive::installIndex()
{
    std::unique_ptr<DupIndex> idx;
    {
        const juce::ScopedLock sl (builtLock);
        idx = std::move (builtIndex);
        indexReady = false;
    }
    if (idx == nullptr) return false;
    if (indexThread.joinable()) indexThread.join();
    // entries that arrived while the index was built in the background
    for (auto& e : list) if (idx->fps.count (e->id) == 0) idx->add (e->id, fingerprintOf (*e));
    index = std::move (idx);
    finishGroups();
    return true;
}

void Archive::waitForIndex()
{
    if (index != nullptr) return;
    if (indexThread.joinable()) indexThread.join();
    installIndex();
}

void Archive::finishGroups()
{
    for (auto& e : list) { e->group = 0; e->groupSize = 0; e->bestMatch = 0; e->bestScore = 0; e->dupKind = dupNone; }
    if (index == nullptr) return;
    std::unordered_map<int, std::vector<Entry*>> groups;
    for (auto& e : list)
    {
        auto l = index->links.find (e->id);
        if (l == index->links.end() || l->second.empty()) continue;
        groups[index->find (e->id)].push_back (e.get());
        for (auto& x : l->second)
            if (x.kind > e->dupKind || (x.kind == e->dupKind && x.score > e->bestScore))
            {
                e->dupKind = x.kind;
                e->bestScore = x.score;
                e->bestMatch = x.other;
            }
    }
    for (auto& [root, members] : groups)
    {
        if (members.size() < 2) { for (auto* m : members) m->dupKind = dupNone; continue; }
        int gid = members.front()->id;
        for (auto* m : members) gid = juce::jmin (gid, m->id);
        for (auto* m : members) { m->group = gid; m->groupSize = (int) members.size(); }
    }
}

// ============================================================================================ persistence
juce::File Archive::databaseFile() const
{
    const auto env = juce::SystemStats::getEnvironmentVariable ("SPURWERK_DB", {});
    if (env.isNotEmpty()) return juce::File (env);
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Spurwerk").getChildFile ("archiv.swdb");
}

namespace
{
constexpr int kMagic = 0x42445753;   // "SWDB"
constexpr int kVersion = 4;   // 2: list of ignored files, 3: composer / arranger / edited, 4: key / mode / scale
}

void Archive::save()
{
    dirty = false;
    juce::MemoryOutputStream os;
    os.writeInt (kMagic);
    os.writeInt (kVersion);
    os.writeFloat (threshold);
    os.writeInt (folders.size());
    for (auto& f : folders) os.writeString (f);
    os.writeInt ((int) notDup.size());
    for (auto k : notDup) os.writeInt64 ((juce::int64) k);
    os.writeString (view.toXmlString());
    os.writeInt ((int) ignored.size());
    for (auto& [p, sm] : ignored) { os.writeString (p); os.writeInt64 (sm.first); os.writeInt64 (sm.second); }
    os.writeInt (nextId);
    os.writeInt ((int) list.size());
    for (auto& e : list)
    {
        os.writeInt (e->id);
        for (auto* s : { &e->path, &e->title, &e->artist, &e->format, &e->formatName, &e->message, &e->sampleText, &e->comment, &e->error })
            os.writeString (*s);
        os.writeInt64 (e->size);
        os.writeInt64 (e->mtime);
        os.writeInt ((int) e->flags);
        os.writeString (e->key);
        os.writeString (e->mode);
        os.writeString (e->scale);
        os.writeFloat (e->keyConfidence);
        os.writeString (e->composer);
        os.writeString (e->arranger);
        os.writeInt ((int) e->edited);
        for (int v : { e->channels, e->patterns, e->orders, e->samples, e->instruments, e->notes, e->rating, e->gramCount, (int) e->favourite })
            os.writeInt (v);
        os.writeFloat (e->seconds);
        os.writeInt64 ((juce::int64) e->fileHash);
        os.writeInt64 ((juce::int64) e->noteHash);
        os.writeByte ((char) e->sketch.size());
        for (auto g : e->sketch) os.writeInt ((int) g);
        os.write (e->profile.data(), e->profile.size());
    }
    const auto f = databaseFile();
    f.getParentDirectory().createDirectory();
    const auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    if (tmp.replaceWithData (os.getData(), os.getDataSize())) tmp.moveFileTo (f);
}

bool Archive::load()
{
    juce::MemoryBlock mb;
    if (! databaseFile().loadFileAsData (mb)) return false;
    juce::MemoryInputStream is (mb, false);
    if (is.readInt() != kMagic) return false;
    const int fileVersion = is.readInt();
    if (fileVersion < 1 || fileVersion > kVersion) return false;
    threshold = juce::jlimit (0.1f, 1.0f, is.readFloat());
    const int nf = is.readInt();
    for (int i = 0; i < nf && ! is.isExhausted(); ++i) folders.add (is.readString());
    const int nd = is.readInt();
    for (int i = 0; i < nd && ! is.isExhausted(); ++i) notDup.insert ((juce::uint64) is.readInt64());
    if (auto xml = juce::parseXML (is.readString())) view = juce::ValueTree::fromXml (*xml);
    if (! view.isValid()) view = juce::ValueTree ("VIEW");
    if (fileVersion >= 2)
    {
        const int ni = is.readInt();
        for (int i = 0; i < ni && ! is.isExhausted(); ++i)
        {
            const auto p = is.readString();
            const auto sz = is.readInt64();
            ignored[p] = { sz, is.readInt64() };
        }
    }
    nextId = is.readInt();
    const int n = is.readInt();
    list.reserve ((size_t) juce::jmax (0, n));
    for (int i = 0; i < n && ! is.isExhausted(); ++i)
    {
        auto e = std::make_unique<Entry>();
        e->id = is.readInt();
        for (auto* s : { &e->path, &e->title, &e->artist, &e->format, &e->formatName, &e->message, &e->sampleText, &e->comment, &e->error })
            *s = is.readString();
        const juce::File f (e->path);
        e->name = f.getFileName();
        e->folder = f.getParentDirectory().getFullPathName();
        e->size = is.readInt64();
        e->mtime = is.readInt64();
        e->flags = (juce::uint32) is.readInt();
        if (fileVersion < 4 && (e->flags & fError) == 0) e->mtime = 0;   // no key yet: analysed again by the next scan
        if (fileVersion >= 3)
        {
            if (fileVersion >= 4)
            {
                e->key = is.readString();
                e->mode = is.readString();
                e->scale = is.readString();
                e->keyConfidence = is.readFloat();
            }
            e->composer = is.readString();
            e->arranger = is.readString();
            e->edited = (juce::uint32) is.readInt();
        }
        if ((e->flags & fError) != 0 && (e->error == juce::String::fromUTF8 ("Kein gültiges MOD-Format") || e->error == "Datei zu klein")) e->flags |= fNotModule;   // databases of 0.7.0
        for (int* v : { &e->channels, &e->patterns, &e->orders, &e->samples, &e->instruments, &e->notes, &e->rating, &e->gramCount })
            *v = is.readInt();
        e->favourite = is.readInt() != 0;
        e->seconds = is.readFloat();
        e->fileHash = (juce::uint64) is.readInt64();
        e->noteHash = (juce::uint64) is.readInt64();
        e->sketch.resize ((size_t) (juce::uint8) is.readByte());
        for (auto& g : e->sketch) g = (juce::uint32) is.readInt();
        is.read (e->profile.data(), (int) e->profile.size());
        pathIndex[e->path] = e.get();
        idIndex[e->id] = e.get();
        nextId = juce::jmax (nextId, e->id + 1);
        list.push_back (std::move (e));
    }
    // files removed with "ausschließen" in 0.8.x become pool entries (excluded, skipped while unchanged)
    for (auto& [p, sm] : ignored)
    {
        if (byPath (p) != nullptr) continue;
        auto e = std::make_unique<Entry>();
        e->id = nextId++;
        e->path = p;
        const juce::File f (p);
        e->name = f.getFileName();
        e->folder = f.getParentDirectory().getFullPathName();
        e->title = f.getFileNameWithoutExtension();
        e->size = sm.first;
        e->mtime = sm.second;
        e->flags = fExcluded;
        e->error = juce::String::fromUTF8 ("ausgeschlossen (aus Version 0.8)");
        pathIndex[e->path] = e.get();
        idIndex[e->id] = e.get();
        list.push_back (std::move (e));
    }
    if (! ignored.empty()) { ignored.clear(); dirty = true; }
    upgradeScan = fileVersion < 4 && ! list.empty();
    return true;
}
} // namespace sw::archive
