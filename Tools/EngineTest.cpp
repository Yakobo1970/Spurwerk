// Spurwerk engine test: loads files, renders audio, exports MIDI, MOD round trip.
// Usage: EngineTest <outFolder|-> <file> [file ...]
#include "../Source/ModFile.h"
#include "../Source/Engine.h"
#include "../Source/Export.h"
#include "../Source/TrackerFormats.h"
#include "../Source/SampleShare.h"
#include "../Source/DawProject.h"
#include "../Source/ModArchive.h"
#include "../Source/Convert.h"
#include "../Source/UpdateCheck.h"
#include <map>
#include <set>
#include <iostream>

using namespace sw;

static bool sameSong (const Song& a, const Song& b, juce::String& why)
{
    if (a.channels != b.channels) { why = "channels"; return false; }
    if (a.orders != b.orders) { why = "orders"; return false; }
    for (size_t p = 0; p < a.patterns.size() && p < b.patterns.size(); ++p)
        for (size_t i = 0; i < a.patterns[p].cells.size(); ++i)
            if (! (a.patterns[p].cells[i] == b.patterns[p].cells[i])) { why = "cell p" + juce::String ((int) p) + " i" + juce::String ((int) i); return false; }
    for (int i = 0; i < 31; ++i)
    {
        const auto& x = a.samples[(size_t) i]; const auto& y = b.samples[(size_t) i];
        if (x.volume != y.volume || x.finetune != y.finetune || std::abs ((int) x.pcm.size() - (int) y.pcm.size()) > 1) { why = "sample " + juce::String (i + 1); return false; }
    }
    return true;
}

int main (int argc, char** argv)
{

    if (argc >= 2 && juce::String (argv[1]) == "update")
    {
        // EngineTest update: version comparison + one real request to GitHub
        int bad = 0;
        auto t = [&bad] (const char* x, const char* y, bool want) {
            const bool got = sw::UpdateCheck::isNewer (x, y);
            std::cout << (got == want ? "ok:   " : "FAIL: ") << x << (got ? " > " : " !> ") << y << "\n";
            if (got != want) ++bad;
        };
        t ("1.0.1", "1.0.0", true); t ("v1.0.1", "1.0.0", true); t ("1.0.0", "1.0.0", false); t ("0.9.8", "1.0.0", false);
        t ("1.0.10", "1.0.9", true); t ("1.1", "1.0.5", true); t ("1.0", "1.0.0", false); t ("", "1.0.0", false); t ("2.0.0", "10.0.0", false);
        juce::ScopedJuceInitialiser_GUI gui;
        const auto tag = sw::UpdateCheck::fetchLatest (8000);
        std::cout << "GitHub latest: '" << tag << "' (installed " SW_VERSION ")" << (sw::UpdateCheck::isNewer (tag, SW_VERSION) ? " -> update" : " -> current") << "\n";
        if (tag.isEmpty()) ++bad;
        std::cout << (bad == 0 ? "ALL OK" : "FAILURES") << std::endl;
        return bad == 0 ? 0 : 1;
    }
    if (argc < 3) { std::cout << "usage: EngineTest <outFolder|-> files...\n"; return 2; }
    if (juce::String (argv[1]) == "scan" && argc >= 4)
    {
        // EngineTest scan <db> <folder>: plain archive scan (for demo databases / screenshots)
        juce::ScopedJuceInitialiser_GUI gui;
        _putenv_s ("SPURWERK_DB", juce::String::fromUTF8 (argv[2]).toRawUTF8());
        archive::Archive a;
        a.addFolder (juce::File (juce::String::fromUTF8 (argv[3])));
        a.startScan();
        a.waitForScan();
        a.save();
        std::cout << "scan: " << a.entries().size() << " entries, groups " << a.duplicateGroups() << std::endl;
        return 0;
    }
    if (juce::String (argv[1]) == "archive" && argc >= 4)
    {
        // EngineTest archive <db> <folder> [groups]: scans the folder into the database (SPURWERK_DB), prints the
        // duplicate groups, saves, reloads and compares; a second scan must analyse nothing (unchanged files)
        juce::ScopedJuceInitialiser_GUI gui;
        const juce::String db = juce::String::fromUTF8 (argv[2]);
        _putenv_s ("SPURWERK_DB", db.toRawUTF8());
        int shownGroups = argc >= 5 ? juce::String (argv[4]).getIntValue() : 8;
        std::size_t n1 = 0;
        {
            archive::Archive a;
            a.addFolder (juce::File (juce::String::fromUTF8 (argv[3])));
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            a.startScan();
            a.waitForScan();
            const auto t1 = juce::Time::getMillisecondCounterHiRes();
            int errors = 0;
            std::map<juce::String, int> formats;
            for (auto& e : a.entries()) { if (e->has (archive::fError)) ++errors; ++formats[e->format]; }
            n1 = a.entries().size();
            std::cout << "scan: " << n1 << " entries, " << errors << " errors, " << juce::String ((t1 - t0) / 1000.0, 1) << " s, groups " << a.duplicateGroups() << std::endl;
            for (auto& [f, c] : formats) std::cout << "   " << f << ": " << c << std::endl;
            for (int k = 0; k < juce::jmin (25, (int) a.entries().size()); ++k)
            {
                auto& e = *a.entries()[(size_t) k];
                std::cout << "   KEY " << e.name.substring (0, 34).paddedRight (' ', 34) << " " << e.key.paddedRight (' ', 9) << " " << e.mode << " | " << e.scale << " | conf " << juce::String (e.keyConfidence, 2) << std::endl;
            }
            for (auto& e : a.entries())
                if (e->has (archive::fError)) std::cout << "   ERR " << e->path << ": " << e->error << std::endl;
            std::set<int> seen;
            for (auto& e : a.entries())
            {
                if (e->group == 0 || seen.count (e->group) || shownGroups <= 0) continue;
                seen.insert (e->group);
                --shownGroups;
                std::cout << "group " << e->group << ":" << std::endl;
                for (int id : a.groupMembers (e->group))
                    if (auto* m = a.byId (id))
                        std::cout << "   [" << (int) m->dupKind << " " << juce::roundToInt (m->bestScore * 100) << "%] " << m->name << " | " << m->title
                                  << " | " << archive::formatDuration (m->seconds) << " | notes " << m->notes << std::endl;
            }
            // first readable module into the pool (excluded) – must stay excluded after reload and rescan
            std::vector<int> ex;
            for (auto& e : a.entries()) if (! e->inPool()) { ex.push_back (e->id); break; }
            a.setExcluded (ex, true);
            a.setProtected (a.folders[0], true);
            std::cout << "protected: " << (int) a.isProtected (a.entries().front()->path) << " outside: " << (int) a.isProtected ("C:/Windows/x.mod") << std::endl;
            n1 = a.entries().size();
            std::cout << "pool " << a.countPool() << " (excluded " << ex.size() << ")" << std::endl;
            if (! a.entries().empty())
            {
                auto& e = *a.entries().front();
                a.setRating (e, 4);
                a.setComment (e, juce::String::fromUTF8 ("Testnotiz äöü"));
                a.setField (e, archive::Archive::Field::title, "Eigener Titel");
                a.setField (e, archive::Archive::Field::composer, juce::String::fromUTF8 ("Komponist Ä"));
                std::cout << "sample text of " << e.name << ":\n" << e.sampleText.substring (0, 400) << std::endl;
            }
            a.save();
        }
        {
            archive::Archive b;
            b.waitForIndex();
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            b.startScan();
            b.waitForScan();
            const auto t1 = juce::Time::getMillisecondCounterHiRes();
            bool ok = b.entries().size() == n1;
            int excl = 0;
            for (auto& e : b.entries()) if (e->has (archive::fExcluded)) ++excl;
            ok &= excl == 1;
            std::cout << "excluded after reload + rescan: " << excl << std::endl;
            ok &= b.isProtected (b.entries().front()->path);
            std::cout << "protected after reload: " << (int) b.isProtected (b.entries().front()->path) << std::endl;
            if (! b.entries().empty())
            {
                auto& e = *b.entries().front();
                ok &= e.rating == 4 && e.comment == juce::String::fromUTF8 ("Testnotiz äöü");
                ok &= e.title == "Eigener Titel" && e.composer == juce::String::fromUTF8 ("Komponist Ä");
                std::cout << "edited title after rescan: " << e.title << " / " << e.composer << std::endl;
            }
            std::cout << "reload: " << b.entries().size() << " entries, rescan " << juce::String ((t1 - t0) / 1000.0, 2) << " s, groups " << b.duplicateGroups()
                      << (ok ? " OK" : " MISMATCH") << std::endl;
            return ok ? 0 : 1;
        }
    }
    if (juce::String (argv[1]) == "regs" && argc >= 4)
    {
        // EngineTest regs <file> <out.bin> [subsong]: YM register frames (16 bytes each)
        juce::MemoryBlock mb;
        const juce::File in (juce::String::fromUTF8 (argv[2]));
        in.loadFileAsData (mb);
        juce::String err;
        auto song = loadSongData (mb, in.getFileName(), err, argc > 4 ? atoi (argv[4]) : 0);
        if (song == nullptr || song->format != Format::YM) { std::cout << "FAIL " << err << "\n"; return 1; }
        juce::MemoryBlock out;
        for (auto& r : song->ym.regs) out.append (r.data(), 16);
        juce::File (juce::String::fromUTF8 (argv[3])).replaceWithData (out.getData(), out.getSize());
        std::cout << song->formatName << " frames " << song->ym.frames << "\n";
        return 0;
    }
    if (juce::String (argv[1]) == "wavmidi" && argc >= 4)
    {
        // EngineTest wavmidi <module> <folder>: all samples as WAV + one MIDI with a track per sample
        juce::String err, log;
        auto song = loadSongFile (juce::File (juce::String::fromUTF8 (argv[2])), err);
        if (song == nullptr) { std::cout << "LOAD FAIL " << err << std::endl; return 1; }
        const juce::File dir (juce::String::fromUTF8 (argv[3]));
        const bool ok = exportSamplesWavMidi (*song, dir, log);
        std::cout << (ok ? "OK " : "FAIL ") << log << std::endl;
        for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.wav")) std::cout << "   wav: " << f.getFileName() << std::endl;
        for (auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.mid"))
        {
            juce::FileInputStream in (f);
            juce::MidiFile mf;
            mf.readFrom (in);
            for (int t = 0; t < mf.getNumTracks(); ++t)
            {
                const auto* tr = mf.getTrack (t);
                int notes = 0;
                juce::String name;
                for (auto* ev : *tr)
                {
                    if (ev->message.isNoteOn()) ++notes;
                    if (ev->message.isTrackNameEvent()) name = ev->message.getTextFromTextMetaEvent();
                }
                std::cout << "   midi track " << t << ": '" << name << "' notes " << notes << std::endl;
            }
        }
        return ok ? 0 : 1;
    }
    if (juce::String (argv[1]) == "dawproject" && argc >= 4)
    {
        // EngineTest dawproject <module> <out.dawproject>
        juce::String err, log;
        auto song = loadSongFile (juce::File (juce::String::fromUTF8 (argv[2])), err);
        if (song == nullptr) { std::cout << "LOAD FAIL " << err << std::endl; return 1; }
        const bool ok = exportDawProject (*song, juce::File (juce::String::fromUTF8 (argv[3])), log);
        std::cout << (ok ? "OK " : "FAIL ") << log << std::endl;
        return ok ? 0 : 1;
    }
    if (juce::String (argv[1]) == "save" && argc >= 5)
    {
        // EngineTest save <module> <mod|med> <out>: convert + write, reload, render both 20 s and compare
        juce::String err, rep;
        auto song = loadSongFile (juce::File (juce::String::fromUTF8 (argv[2])), err);
        if (song == nullptr) { std::cout << "LOAD FAIL " << err << std::endl; return 1; }
        const auto target = juce::String (argv[3]) == "med" ? SaveTarget::MED : SaveTarget::MOD;
        const juce::File out (juce::String::fromUTF8 (argv[4]));
        if (! saveSongAs (*song, target, out, rep)) { std::cout << "SAVE FAIL " << rep << std::endl; return 1; }
        std::cout << "saved: " << rep << std::endl;
        auto back = loadSongFile (out, err);
        if (back == nullptr) { std::cout << "RELOAD FAIL " << err << std::endl; return 1; }
        auto rms = [] (const Song& s, int& notes) {
            ModEngine me; YmEngine ye;
            me.prepare (48000.0); ye.prepare (48000.0);
            if (s.format == Format::YM) ye.setData (&s.ym); else me.setSong (&s);
            juce::AudioBuffer<float> buf (2, 48000 * 20);
            buf.clear();
            for (int pos = 0; pos < buf.getNumSamples(); pos += 512)
            {
                const int n = juce::jmin (512, buf.getNumSamples() - pos);
                if (s.format == Format::YM) ye.render (buf.getWritePointer (0, pos), buf.getWritePointer (1, pos), n, 0, 1.0f);
                else me.render (buf.getWritePointer (0, pos), buf.getWritePointer (1, pos), n, 0, 1.0f);
            }
            juce::String sum;
            MidiExportOptions o; o.perInstrument = false;
            exportMidi (s, o, &sum);
            notes = sum.getIntValue();
            return buf.getRMSLevel (0, 0, buf.getNumSamples());
        };
        int n1 = 0, n2 = 0;
        const float r1 = rms (*song, n1), r2 = rms (*back, n2);
        std::cout << "reloaded: " << back->formatName << " | ch " << back->channels << " | orders " << back->orders.size() << " | samples " << back->numSamples()
                  << " | rms " << r1 << " -> " << r2 << " | notes " << n1 << " -> " << n2 << std::endl;
        return 0;
    }
    if (juce::String (argv[1]) == "als" && argc >= 4)
    {
        // EngineTest als <module> <out.als>: Ableton Live Set from the templates of the installed Live
        juce::String err, log;
        auto song = loadSongFile (juce::File (juce::String::fromUTF8 (argv[2])), err);
        if (song == nullptr) { std::cout << "LOAD FAIL " << err << std::endl; return 1; }
        const bool ok = exportAbletonLiveSet (*song, juce::File (juce::String::fromUTF8 (argv[3])), log);
        std::cout << (ok ? "OK " : "FAIL ") << log << std::endl;
        return ok ? 0 : 1;
    }
    if (juce::String (argv[1]) == "share" && argc >= 3)
    {
        // EngineTest share <module>: publish for the Spurwerk Player, then list the shared folder
        juce::String err;
        auto song = loadSongFile (juce::File (juce::String::fromUTF8 (argv[2])), err);
        if (song == nullptr) { std::cout << "LOAD FAIL " << err << std::endl; return 1; }
        publishSongSamples (*song);
        for (const auto& m : listSharedModules())
        {
            int midis = 0;
            for (const auto& s : m.samples) if (s.midi.existsAsFile()) ++midis;
            std::cout << m.title << " | " << m.folder.getFileName() << " | " << (int) m.samples.size() << " samples, " << midis << " MIDI" << std::endl;
        }
        return 0;
    }
    if (juce::String (argv[1]) == "cells" && argc >= 3)
    {
        juce::String err;
        auto song = loadSongFile (juce::File (juce::String::fromUTF8 (argv[2])), err);
        if (song == nullptr) return 1;
        int shown = 0;
        for (auto& pat : song->patterns)
            for (auto& c : pat.cells)
                if (c.note && shown < 12) { ++shown; std::cout << noteName (c.note) << " i" << (int) c.instr << " v[" << volumeText (c) << "] vc" << (int) c.vcmd << " vol" << (int) c.vol << " fx[" << effectText (*song, c) << "]" << std::endl; }
        return 0;
    }
    if (juce::String (argv[1]) == "dump" && argc >= 3)
    {
        // EngineTest dump <module>: header, samples (length, loop, first values), instruments and every non-empty cell
        juce::String err;
        auto song = loadSongFile (juce::File (juce::String::fromUTF8 (argv[2])), err);
        if (song == nullptr) { std::cout << "LOAD FAIL " << err << std::endl; return 1; }
        std::cout << song->formatName << " | title '" << song->title << "' | artist '" << song->artist << "' | ch " << song->channels
                  << " | speed " << song->initialSpeed << " tempo " << song->initialTempo << " gvol " << song->globalVolume << " | restart " << song->restartPos << std::endl;
        std::cout << "message: " << song->message.replace ("\n", " / ") << std::endl << "orders:";
        for (int o : song->orders) std::cout << " " << o;
        std::cout << std::endl << "pan:";
        for (int c = 0; c < song->channels; ++c) std::cout << " " << juce::String (song->initialPan (c), 2) << "/" << song->initialChannelVolume (c);
        std::cout << std::endl;
        for (int i = 0; i < song->numSamples(); ++i)
        {
            const auto& s = song->samples[(size_t) i];
            std::cout << "smp " << i + 1 << " '" << s.name << "' len " << s.pcm.size() << " loop " << s.loopStart << "+" << s.loopLength << " vol " << s.volume
                      << " pan " << s.pan << " c5 " << s.c5Speed << " |";
            for (size_t k = 0; k < juce::jmin ((size_t) 8, s.pcm.size()); ++k) std::cout << " " << juce::roundToInt (s.pcm[k] * 32768.0f);
            std::cout << std::endl;
        }
        for (int i = 0; i < (int) song->instruments.size(); ++i)
        {
            const auto& in = song->instruments[(size_t) i];
            std::cout << "ins " << i + 1 << " '" << in.name << "' map C-0:" << in.map[0] << " C-4:" << in.map[48] << " C-6:" << in.map[72] << " B-9:" << in.map[119]
                      << " fade " << in.fadeout << " volEnv " << (in.vol.on ? (int) in.vol.points.size() : -1) << " panEnv " << (in.pan.on ? (int) in.pan.points.size() : -1)
                      << " vib " << in.vibType << "/" << in.vibSweep << "/" << in.vibDepth << "/" << in.vibRate << std::endl;
        }
        for (int pi = 0; pi < (int) song->patterns.size(); ++pi)
        {
            const auto& pat = song->patterns[(size_t) pi];
            std::cout << "pattern " << pi << " rows " << pat.rows << std::endl;
            for (int r = 0; r < pat.rows; ++r)
                for (int c = 0; c < pat.channels; ++c)
                {
                    const auto& cell = pat.at (r, c);
                    if (cell.isEmpty()) continue;
                    std::cout << "  r" << r << " c" << c << " " << noteName (cell.note) << " " << (int) cell.instr << " " << volumeText (cell) << " " << effectText (*song, cell) << std::endl;
                }
        }
        return 0;
    }
    const juce::String outArg (argv[1]);
    const juce::File out = (outArg == "-" || outArg == "load") ? juce::File() : juce::File (outArg);
    if (out != juce::File()) out.createDirectory();
    int failures = 0;
    for (int a = 2; a < argc; ++a)
    {
        const juce::File f (juce::String::fromUTF8 (argv[a]));
        juce::String err;
        auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto song = loadSongFile (f, err);
        std::cout << "== " << f.getFileName() << "\n";
        if (song == nullptr) { std::cout << "   LOAD FAIL: " << err << "\n"; ++failures; continue; }
        std::cout << "   " << song->formatName << " | '" << song->title << "' | ch " << song->channels << " | orders " << song->orders.size()
                  << " | patterns " << song->patterns.size();
        if (song->format == Format::YM) std::cout << " | frames " << song->ym.frames << " | drums " << song->ym.drums.size() << " | " << song->ym.author;
        std::cout << " | load " << juce::String (juce::Time::getMillisecondCounterHiRes() - t0, 1) << " ms\n";

        // render 12 s
        ModEngine me; YmEngine ye;
        me.prepare (48000.0); ye.prepare (48000.0);
        if (song->format == Format::YM) ye.setData (&song->ym); else me.setSong (song.get());
        juce::AudioBuffer<float> buf (2, 48000 * 12);
        buf.clear();
        for (int pos = 0; pos < buf.getNumSamples(); pos += 512)
        {
            const int n = juce::jmin (512, buf.getNumSamples() - pos);
            if (song->format == Format::YM) ye.render (buf.getWritePointer (0, pos), buf.getWritePointer (1, pos), n, 0, 1.0f);
            else me.render (buf.getWritePointer (0, pos), buf.getWritePointer (1, pos), n, 0, 1.0f);
        }
        const float rms = juce::jmax (buf.getRMSLevel (0, 0, buf.getNumSamples()), buf.getRMSLevel (1, 0, buf.getNumSamples()));   // louder side (Amiga panning: an intro may run on one side only)
        const float peak = buf.getMagnitude (0, buf.getNumSamples());
        std::cout << "   audio 12s: rms " << rms << " peak " << peak << (rms < 0.005f ? "  <-- STILL?" : "") << (peak > 1.2f ? "  <-- CLIP" : "") << "\n";
        if (rms < 0.002f) ++failures;

        MidiExportOptions mo;
        mo.perInstrument = false;
        juce::String sum;
        auto mf = exportMidi (*song, mo, &sum);
        std::cout << "   MIDI: " << mf.getNumTracks() << " Spuren, " << sum << "\n";
        if (mf.getNumTracks() < 2) ++failures;
        if (song->format == Format::MOD)
        {
            mo.perInstrument = true;
            auto mf2 = exportMidi (*song, mo, &sum);
            std::cout << "   MIDI/Instr: " << mf2.getNumTracks() << " Spuren, " << sum << "\n";
            juce::MemoryBlock mb;
            juce::String e2;
            if (! saveMod (*song, mb, e2)) { std::cout << "   SAVE FAIL " << e2 << "\n"; ++failures; }
            else
            {
                auto back = loadMod (mb, e2);
                juce::String why;
                if (back == nullptr || ! sameSong (*song, *back, why)) { std::cout << "   ROUNDTRIP FAIL " << e2 << " " << why << "\n"; ++failures; }
                else std::cout << "   roundtrip ok (" << (int) mb.getSize() << " bytes)\n";
            }
        }
        if (out != juce::File())
        {
            const auto base = safeFileName (f.getFileNameWithoutExtension());
            juce::String log;
            renderSongWav (*song, out.getChildFile (base + ".wav"), 44100.0, 1.0, log);
            std::cout << "   wav: " << log << "\n";
            writeMidiFile (mf, out.getChildFile (base + ".mid"));
            if (song->format == Format::MOD)
            {
                exportSamples (*song, out.getChildFile (base + "_samples"), log);
                std::cout << "   samples: " << log << "\n";
            }
        }
    }
    std::cout << (failures == 0 ? "ALL OK\n" : "FAILURES: ") << (failures ? juce::String (failures) : juce::String()) << "\n";
    return failures == 0 ? 0 : 1;
}
