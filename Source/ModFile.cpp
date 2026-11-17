#include "ModFile.h"
#include "YmFile.h"
#include "CosoFile.h"
#include "TrackerFormats.h"
#include <cstring>

namespace sw
{
namespace
{
const int kPeriods[12] = { 1712, 1616, 1525, 1440, 1357, 1281, 1209, 1141, 1077, 1017, 961, 907 };   // C-3 .. B-3 (octave below PT C-1)

int be16 (const juce::uint8* p) { return (p[0] << 8) | p[1]; }

juce::String fixedString (const juce::uint8* p, int len)
{
    juce::String s;
    for (int i = 0; i < len && p[i] != 0; ++i)
        s += (juce::juce_wchar) (p[i] >= 32 && p[i] < 127 ? p[i] : (p[i] >= 160 ? p[i] : ' '));
    return s.trimEnd();
}

int channelsForTag (const char* t)
{
    const juce::String tag (t, 4);
    if (tag == "M.K." || tag == "M!K!" || tag == "M&K!" || tag == "FLT4" || tag == "4CHN" || tag == "N.T.") return 4;
    if (tag == "FLT8" || tag == "OKTA" || tag == "OCTA" || tag == "CD81" || tag == "EXO8") return 8;
    if (tag == "EXO4") return 4;   // StarTrekker
    if (tag.endsWith ("CHN") && juce::CharacterFunctions::isDigit (tag[0])) return tag[0] - '0';
    if ((tag.endsWith ("CH") || tag.endsWith ("CN")) && juce::CharacterFunctions::isDigit (tag[0]) && juce::CharacterFunctions::isDigit (tag[1]))
        return (tag[0] - '0') * 10 + (tag[1] - '0');
    if (tag.startsWith ("TDZ") && juce::CharacterFunctions::isDigit (tag[3])) return tag[3] - '0';
    return 0;
}
} // namespace

juce::String noteName (int note)
{
    if (note == kNoteOff) return "===";
    if (note == kNoteCut) return "^^^";
    if (note == kNoteFade) return "~~~";
    if (note <= 0 || note > 120) return "...";
    static const char* names[12] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
    return juce::String (names[(note - 1) % 12]) + juce::String ((note - 1) / 12);
}

int periodForNote (int note, int finetune)
{
    // C-3 (note 37) = 1712 ... B-7; C-5 = 428
    const int idx = note - 37;
    if (idx < 0) return 1712 << ((-idx + 11) / 12);   // far below: just clamp downwards
    const int oct = idx / 12, semi = idx % 12;
    double p = (double) kPeriods[semi] / (double) (1 << oct);
    if (finetune != 0) p *= std::pow (2.0, -finetune / 96.0);
    return juce::jmax (1, juce::roundToInt (p));
}

int noteForPeriod (int period)
{
    if (period <= 0) return 0;
    // note = 61 + 12*log2(428/period)
    const double n = 61.0 + 12.0 * std::log2 (428.0 / (double) period);
    return juce::jlimit (1, 120, juce::roundToInt (n));
}

std::unique_ptr<Song> newEmptySong (int channels)
{
    auto s = std::make_unique<Song>();
    s->format = Format::MOD;
    s->formatName = "ProTracker MOD";
    s->channels = juce::jlimit (1, kMaxChannels, channels);
    s->title = "Neues Lied";
    s->orders = { 0 };
    s->patterns.resize (1);
    s->patterns[0].resize (64, s->channels);
    s->samples.resize (31);
    return s;
}

std::unique_ptr<Song> loadMod (const juce::MemoryBlock& data, juce::String& error)
{
    const auto* d = static_cast<const juce::uint8*> (data.getData());
    const int size = (int) data.getSize();
    if (size < 600) { error = "Datei zu klein"; return nullptr; }

    int channels = size >= 1084 ? channelsForTag ((const char*) d + 1080) : 0;
    const bool st15 = channels == 0;
    if (st15)
    {
        // Ultimate Soundtracker / 15 instruments: plausibility check on names and song length
        for (int i = 0; i < 20; ++i)
            if (d[i] != 0 && (d[i] < 32 || d[i] > 126)) { error = U ("Kein gültiges MOD-Format"); return nullptr; }
        if (d[470] == 0 || d[470] > 128) { error = U ("Kein gültiges MOD-Format"); return nullptr; }
        channels = 4;
    }
    if (channels < 1 || channels > kMaxChannels) { error = U ("Kanalzahl nicht unterstützt"); return nullptr; }

    auto s = std::make_unique<Song>();
    s->format = Format::MOD;
    s->channels = channels;
    s->fifteenSamples = st15;
    s->title = fixedString (d, 20);
    const int numSamples = st15 ? 15 : 31;
    const int hdr = 20 + numSamples * 30;
    const int songLen = juce::jlimit (1, 128, (int) d[hdr]);
    s->restartPos = d[hdr + 1] < 127 && d[hdr + 1] < songLen ? d[hdr + 1] : 0;
    const int patStart = st15 ? 600 : 1084;
    const int patBytes = 64 * channels * 4;

    int maxPat = 0;
    for (int i = 0; i < 128; ++i) maxPat = juce::jmax (maxPat, (int) d[hdr + 2 + i]);
    int sampleBytes = 0;
    for (int i = 0; i < numSamples; ++i) sampleBytes += be16 (d + 20 + i * 30 + 22) * 2;
    if (patStart + (maxPat + 1) * patBytes > size)
    {
        maxPat = 0;   // garbage behind the song length: only count used orders
        for (int i = 0; i < songLen; ++i) maxPat = juce::jmax (maxPat, (int) d[hdr + 2 + i]);
    }
    const int numPatterns = maxPat + 1;
    {
        // text-mode transfer damage (LF -> CR LF): repair when it explains the extra bytes exactly
        const size_t expected = (size_t) (patStart + numPatterns * patBytes + sampleBytes);
        juce::MemoryBlock copy (data);
        if ((size_t) size > expected + 1 && repairCrLf (copy, expected))
        {
            auto fixed = loadMod (copy, error);
            if (fixed != nullptr) fixed->formatName += U (" – repariert (CR/LF)");
            return fixed;
        }
    }
    if (patStart + numPatterns * patBytes > size) { error = U ("Datei unvollständig (Patterns fehlen)"); return nullptr; }

    for (int i = 0; i < songLen; ++i) s->orders.push_back (d[hdr + 2 + i]);

    juce::String tag = st15 ? juce::String ("Soundtracker (15)") : juce::String ((const char*) d + 1080, 4);
    s->formatName = st15 ? juce::String ("Soundtracker MOD (15 Samples)")
                         : "ProTracker MOD (" + tag.trim() + ", " + juce::String (channels) + U (" Kanäle)");

    s->patterns.resize ((size_t) numPatterns);
    for (int p = 0; p < numPatterns; ++p)
    {
        auto& pat = s->patterns[(size_t) p];
        pat.resize (64, channels);
        const auto* pd = d + patStart + p * patBytes;
        for (int r = 0; r < 64; ++r)
            for (int c = 0; c < channels; ++c)
            {
                const auto* b = pd + (r * channels + c) * 4;
                auto& cell = pat.at (r, c);
                const int period = ((b[0] & 0x0f) << 8) | b[1];
                cell.instr = (juce::uint8) ((b[0] & 0xf0) | (b[2] >> 4));
                cell.note = (juce::uint8) (period ? noteForPeriod (period) : 0);
                cell.fx = b[2] & 0x0f;
                cell.param = b[3];
            }
    }

    int pos = patStart + numPatterns * patBytes;
    s->samples.resize (31);
    for (int i = 0; i < numSamples; ++i)
    {
        const auto* sh = d + 20 + i * 30;
        auto& smp = s->samples[(size_t) i];
        smp.name = fixedString (sh, 22);
        int len = be16 (sh + 22) * 2;
        smp.finetune = st15 ? 0 : ((sh[24] & 0x0f) >= 8 ? (sh[24] & 0x0f) - 16 : (sh[24] & 0x0f));
        smp.volume = juce::jmin (64, (int) sh[25]);
        int ls = be16 (sh + 26) * 2, ll = be16 (sh + 28) * 2;
        if (st15 && ls / 2 + ll <= len) ls /= 2;   // Soundtracker: loop start in bytes
        const int avail = juce::jmax (0, size - pos);
        const int take = juce::jmin (len, avail);
        smp.pcm.resize ((size_t) take);
        for (int k = 0; k < take; ++k) smp.pcm[(size_t) k] = (float) (juce::int8) d[pos + k] / 128.0f;
        pos += len;
        if (ll > 2 && ls < take)
        {
            smp.loopStart = ls;
            smp.loopLength = juce::jmin (ll, take - ls);
        }
    }
    return s;
}

bool saveMod (const Song& song, juce::MemoryBlock& out, juce::String& error)
{
    if (song.format != Format::MOD) { error = U ("Nur MOD-Songs können als .mod gespeichert werden"); return false; }
    const int ch = song.channels;
    juce::MemoryOutputStream os (out, false);
    auto writeFixed = [&os] (const juce::String& s, int len) {
        auto utf = s.toRawUTF8();
        int n = 0;
        for (; n < len && utf[n] != 0; ++n) os.writeByte ((char) utf[n]);
        for (; n < len; ++n) os.writeByte (0);
    };
    auto w16 = [&os] (int v) { os.writeByte ((char) ((v >> 8) & 0xff)); os.writeByte ((char) (v & 0xff)); };

    writeFixed (song.title, 20);
    for (int i = 0; i < 31; ++i)
    {
        const Sample empty;
        const auto& smp = i < song.numSamples() ? song.samples[(size_t) i] : empty;
        writeFixed (smp.name, 22);
        const int len = juce::jmin (131070, (int) smp.pcm.size()) & ~1;
        w16 (len / 2);
        os.writeByte ((char) (smp.finetune & 0x0f));
        os.writeByte ((char) juce::jlimit (0, 64, smp.volume));
        if (smp.hasLoop() && smp.loopStart < len)
        {
            const int ls = smp.loopStart & ~1;
            w16 (ls / 2);
            w16 (juce::jmax (1, juce::jmin (smp.loopLength, len - ls) / 2));
        }
        else { w16 (0); w16 (1); }
    }
    const int songLen = juce::jlimit (1, 128, (int) song.orders.size());
    os.writeByte ((char) songLen);
    os.writeByte ((char) juce::jlimit (0, 127, song.restartPos));
    int maxPat = 0;
    for (int i = 0; i < 128; ++i)
    {
        const int o = i < songLen ? juce::jlimit (0, 127, song.orders[(size_t) i]) : 0;
        maxPat = juce::jmax (maxPat, o);
        os.writeByte ((char) o);
    }
    juce::String tag = ch == 4 ? juce::String ("M.K.") : ch < 10 ? juce::String (ch) + "CHN" : juce::String (ch) + "CH";
    os.write (tag.toRawUTF8(), 4);
    for (int p = 0; p <= maxPat; ++p)
    {
        for (int r = 0; r < 64; ++r)
            for (int c = 0; c < ch; ++c)
            {
                Cell cell;
                if (p < (int) song.patterns.size())
                {
                    const auto& pat = song.patterns[(size_t) p];
                    if (r < pat.rows && c < pat.channels) cell = pat.at (r, c);
                }
                const int period = (cell.note > 0 && cell.note <= 120) ? juce::jlimit (0, 4095, periodForNote (cell.note, 0)) : 0;
                os.writeByte ((char) ((cell.instr & 0xf0) | (period >> 8)));
                os.writeByte ((char) (period & 0xff));
                os.writeByte ((char) (((cell.instr & 0x0f) << 4) | (cell.fx & 0x0f)));
                os.writeByte ((char) cell.param);
            }
    }
    for (int i = 0; i < 31; ++i)
    {
        if (i >= song.numSamples()) continue;
        const auto& smp = song.samples[(size_t) i];
        const int len = juce::jmin (131070, (int) smp.pcm.size()) & ~1;
        for (int k = 0; k < len; ++k)
            os.writeByte ((char) (juce::int8) juce::jlimit (-128, 127, juce::roundToInt (smp.pcm[(size_t) k] * 128.0f)));
    }
    os.flush();
    return true;
}

juce::String supportedWildcards() { return "*.mod;*.xm;*.s3m;*.it;*.mptm;*.mtm;*.669;*.far;*.mdl;*.med;*.mmd0;*.mmd1;*.mmd2;*.mmd3;*.fc;*.fc13;*.fc14;*.smod;*.tcb;*.okt;*.ym;*.cos;*.nst;*.stk;*.mdz;*.s3z;*.xmz;*.itz;*.mdgz;*.s3gz;*.xmgz;*.itgz;*.pp"; }

// Amiga naming: format in front ("mod.title", "med.title", "mmd1.title" ...)
static bool hasAmigaPrefix (const juce::String& lowerName)
{
    for (const char* p : { "mod.", "med.", "mmd0.", "mmd1.", "mmd2.", "mmd3.", "xm.", "s3m.", "it.", "mptm.", "mtm.", "669.", "far.", "mdl.", "okt.", "okta.", "fc.", "fc13.", "fc14.", "smod.", "tcb." })
        if (lowerName.startsWith (p) && lowerName.length() > (int) std::strlen (p)) return true;
    return false;
}

bool isSupportedFile (const juce::File& f)
{
    const auto ext = f.getFileExtension().toLowerCase();
    const auto name = f.getFileName().toLowerCase();
    return ext == ".mod" || ext == ".xm" || ext == ".s3m" || ext == ".it" || ext == ".mptm" || ext == ".mtm" || ext == ".669" || ext == ".far" || ext == ".mdl" || ext == ".med" || ext == ".mmd0" || ext == ".mmd1" || ext == ".mmd2" || ext == ".mmd3" || ext == ".fc" || ext == ".fc13" || ext == ".fc14" || ext == ".smod" || ext == ".tcb" || ext == ".okt" || ext == ".ym" || ext == ".cos" || ext == ".nst" || ext == ".stk" || ext == ".mdz" || ext == ".s3z" || ext == ".xmz" || ext == ".itz"
        || ext == ".mdgz" || ext == ".s3gz" || ext == ".xmgz" || ext == ".itgz" || ext == ".pp" || hasAmigaPrefix (name);
}

// ---- packed modules: PowerPacker (PP20), ZIP (.mdz/.s3z/.itz/.zip) and GZIP
bool ppDecrunch (const juce::uint8* src, size_t srcLen, const juce::uint8* offLens, juce::uint8* dest, size_t destLen, int skipBits)
{
    const juce::uint8* in = src + srcLen;
    juce::uint32 buf = 0;
    int left = 0;
    auto bits = [&] (int n, juce::uint32& v) {
        while (left < n) { if (in <= src) return false; buf |= (juce::uint32) *--in << left; left += 8; }
        v = 0;
        left -= n;
        while (n--) { v = (v << 1) | (buf & 1); buf >>= 1; }
        return true;
    };
    juce::uint8* out = dest + destLen;
    juce::uint32 x = 0;
    size_t written = 0;
    if (! bits (skipBits, x)) return false;
    while (written < destLen)
    {
        if (! bits (1, x)) return false;
        if (x == 0)
        {
            juce::uint32 todo = 1;
            do { if (! bits (2, x)) return false; todo += x; } while (x == 3);
            while (todo--)
            {
                if (! bits (8, x) || out <= dest) return false;
                *--out = (juce::uint8) x;
                ++written;
            }
            if (written == destLen) break;
        }
        if (! bits (2, x)) return false;
        juce::uint32 offBits = offLens[x], todo = x + 2, offset = 0;
        if (x == 3)
        {
            if (! bits (1, x)) return false;
            if (x == 0) offBits = 7;
            if (! bits ((int) offBits, offset)) return false;
            do { if (! bits (3, x)) return false; todo += x; } while (x == 7);
        }
        else if (! bits ((int) offBits, offset)) return false;
        if (out + offset >= dest + destLen) return false;
        while (todo--)
        {
            if (out <= dest) return false;
            const juce::uint8 v = out[offset];
            *--out = v;
            ++written;
        }
    }
    return true;
}

// replaces `data` by the unpacked module; returns false if it was not packed (or unpacking failed: error set)
bool unpackModule (juce::MemoryBlock& data, juce::String& name, juce::String& error)
{
    const auto* d = static_cast<const juce::uint8*> (data.getData());
    const size_t n = data.getSize();
    if (n > 16 && std::memcmp (d, "PP20", 4) == 0)
    {
        const size_t destLen = ((size_t) d[n - 4] << 16) | ((size_t) d[n - 3] << 8) | d[n - 2];
        if (destLen == 0 || destLen > 64 * 1024 * 1024) { error = U ("PowerPacker-Datei beschädigt"); return false; }
        juce::MemoryBlock out (destLen, true);
        if (! ppDecrunch (d + 8, n - 12, d + 4, static_cast<juce::uint8*> (out.getData()), destLen, d[n - 1]))
        { error = U ("PowerPacker-Datei lässt sich nicht entpacken"); return false; }
        data = std::move (out);
        return true;
    }
    if (n > 30 && d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4)
    {
        juce::MemoryInputStream mis (data, false);
        juce::ZipFile zip (mis);
        for (int i = 0; i < zip.getNumEntries(); ++i)
        {
            const auto* e = zip.getEntry (i);
            const auto fn = e->filename.fromLastOccurrenceOf ("/", false, false);
            if (fn.isEmpty() || e->uncompressedSize < 64 || e->uncompressedSize > 256 * 1024 * 1024) continue;
            const auto ext = fn.fromLastOccurrenceOf (".", true, false).toLowerCase();
            if (! isSupportedFile (juce::File::createFileWithoutCheckingPath ("C:\\" + fn)) && ext != ".mdz") continue;
            std::unique_ptr<juce::InputStream> in (zip.createStreamForEntry (i));
            if (in == nullptr) continue;
            juce::MemoryBlock out;
            in->readIntoMemoryBlock (out);
            if (out.getSize() == 0) continue;
            data = std::move (out);
            name = fn;
            return true;
        }
        error = U ("ZIP-Archiv enthält kein unterstütztes Modul");
        return false;
    }
    if (n > 18 && d[0] == 0x1f && d[1] == 0x8b && d[2] == 8)
    {
        juce::MemoryInputStream mis (data, false);
        juce::GZIPDecompressorInputStream gz (&mis, false, juce::GZIPDecompressorInputStream::gzipFormat);
        juce::MemoryBlock out;
        gz.readIntoMemoryBlock (out);
        if (out.getSize() == 0) { error = U ("GZIP-Datei lässt sich nicht entpacken"); return false; }
        data = std::move (out);
        name = name.upToLastOccurrenceOf (".", false, false);
        return true;
    }
    return false;
}

bool hasModuleSignature (const juce::MemoryBlock& data)
{
    const auto* d = static_cast<const juce::uint8*> (data.getData());
    const size_t n = data.getSize();
    if (n < 8) return false;
    auto starts = [&] (const char* t) { return std::memcmp (d, t, std::strlen (t)) == 0; };
    if (starts ("PP20") || (d[0] == 'P' && d[1] == 'K' && d[2] == 3 && d[3] == 4) || (d[0] == 0x1f && d[1] == 0x8b)) return true;
    if (n > 7 && d[2] == '-' && d[3] == 'l' && d[4] == 'h' && d[6] == '-') return true;   // LHA (YM)
    if (d[0] == 'Y' && d[1] == 'M' && d[3] == '!') return true;
    if (isCoso (data) || isXm (data) || isS3m (data) || isOkt (data) || isIt (data) || isMtm (data) || isFar (data) || isMdl (data) || isMed (data) || is669 (data) || isTcb (data) || isFc (data))
        return true;
    if (n >= 1084 && channelsForTag ((const char*) d + 1080) > 0) return true;   // ProTracker family
    // 15-sample Soundtracker: no tag, but readable sample names and sane lengths
    if (n >= 600)
    {
        for (int i = 0; i < 20; ++i) if (d[i] != 0 && (d[i] < 32 || d[i] > 126)) return false;
        for (int i = 0; i < 15; ++i)   // finetune 0..15 and volume 0..64 in every sample header (text files fail here)
            if (d[20 + i * 30 + 24] > 15 || d[20 + i * 30 + 25] > 64) return false;
        return d[470] > 0 && d[470] <= 128;
    }
    return false;
}

juce::String unsupportedFormatName (const juce::MemoryBlock& data)
{
    const auto* d = static_cast<const juce::uint8*> (data.getData());
    const size_t n = data.getSize();
    if (n < 8) return {};
    auto at = [&] (size_t o, const char* t) { const auto l = std::strlen (t); return o + l <= n && std::memcmp (d + o, t, l) == 0; };
    if (at (0, "FC13")) return "Future Composer 1.3 (FC13)";
    if (at (0, "MUGICIAN")) return "Digital Mugician";
    if (at (0, "AON4") || at (0, "AON8")) return "Art Of Noise";
    if (at (0, "THX")) return "AHX/THX";
    if (at (0, "HVL")) return "HivelyTracker";
    if (at (0, "SONG")) return "Sonic Arranger";
    if (at (0, "XPKF")) return "XPK-gepackt";
    if (at (0, "ICE!") || at (0, "Ice!")) return "ICE-gepackt (Atari)";
    if (at (0, "IMP!")) return "Imploder-gepackt";
    if (at (0, "RNC")) return "Rob-Northen-gepackt";
    if (d[0] == 0 && d[1] == 0 && d[2] == 3 && d[3] == 0xF3) return "Amiga-Programm (Musik mit eingebautem Player)";
    if (n >= 1084 && d[1080] == 'W' && d[1081] == 'N') return "Wanton Packer (gepacktes MOD)";
    if (at (0, "SNDH") || (n > 16 && std::memcmp (d + 12, "SNDH", 4) == 0)) return "SNDH (Atari)";
    return {};
}

std::unique_ptr<Song> loadSongData (const juce::MemoryBlock& packed, const juce::String& packedName, juce::String& error, int subSong)
{
    juce::MemoryBlock data (packed);
    juce::String fileName (packedName);
    for (int depth = 0; depth < 3; ++depth)
    {
        juce::String uerr;
        if (unpackModule (data, fileName, uerr)) continue;
        if (uerr.isNotEmpty()) { error = uerr; return nullptr; }
        break;
    }
    const auto* d = static_cast<const char*> (data.getData());
    const auto lower = fileName.toLowerCase();
    const bool lha = data.getSize() > 7 && d[2] == '-' && d[3] == 'l' && d[4] == 'h' && d[6] == '-';
    const bool ymMagic = data.getSize() > 4 && d[0] == 'Y' && d[1] == 'M' && d[3] == '!';
    std::unique_ptr<Song> s;
    if (isCoso (data)) s = loadCoso (data, subSong, error);
    else if (isXm (data)) s = loadXm (data, error);
    else if (isS3m (data)) s = loadS3m (data, error);
    else if (isOkt (data)) s = loadOkt (data, error);
    else if (isIt (data)) s = loadIt (data, error);
    else if (isMtm (data)) s = loadMtm (data, error);
    else if (isFar (data)) s = loadFar (data, error);
    else if (isMdl (data)) s = loadMdl (data, error);
    else if (isMed (data)) s = loadMed (data, error);
    else if (isTcb (data)) s = loadTcb (data, error);
    else if (isFc (data)) s = loadFc (data, error);
    else if (is669 (data)) s = load669 (data, error);
    else if (lha || ymMagic || lower.endsWith (".ym")) s = loadYm (data, error);
    else s = loadMod (data, error);
    if (s != nullptr && s->title.isEmpty())
        s->title = juce::File::createFileWithoutCheckingPath ("C:\\" + fileName).getFileNameWithoutExtension();
    return s;
}

std::unique_ptr<Song> loadSongFile (const juce::File& file, juce::String& error)
{
    juce::MemoryBlock mb;
    if (! file.loadFileAsData (mb)) { error = "Datei kann nicht gelesen werden"; return nullptr; }
    auto s = loadSongData (mb, file.getFileName(), error);
    if (s != nullptr) s->sourcePath = file.getFullPathName();
    return s;
}
} // namespace sw
