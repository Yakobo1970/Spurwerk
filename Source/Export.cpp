#include "Export.h"
#include "Engine.h"
#include "YmFile.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <map>

namespace sw
{
namespace
{
const int kChannelMap[15] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16 };   // skip GM drum channel 10

void addBendRange (juce::MidiMessageSequence& sq, int midiCh, int range)
{
    sq.addEvent (juce::MidiMessage::controllerEvent (midiCh, 101, 0), 0);
    sq.addEvent (juce::MidiMessage::controllerEvent (midiCh, 100, 0), 0);
    sq.addEvent (juce::MidiMessage::controllerEvent (midiCh, 6, range), 0);
    sq.addEvent (juce::MidiMessage::controllerEvent (midiCh, 38, 0), 0);
    sq.addEvent (juce::MidiMessage::controllerEvent (midiCh, 101, 127), 0);
    sq.addEvent (juce::MidiMessage::controllerEvent (midiCh, 100, 127), 0);
}

int bendValue (double semis, int range)
{
    return juce::jlimit (0, 16383, 8192 + juce::roundToInt (semis / (double) range * 8192.0));
}

struct Track
{
    juce::MidiMessageSequence seq;
    int midiCh = 1;
    juce::String name;
    bool used = false;
    int lastProgram = -1, lastBend = 8192, lastCc11 = 127;
};

juce::MidiFile finish (std::vector<Track>& tracks, juce::MidiMessageSequence& meta, double endTick, int bendRange)
{
    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (kExportPpq);
    meta.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0);
    meta.addEvent (juce::MidiMessage::endOfTrack(), endTick);
    meta.sort();
    mf.addTrack (meta);
    for (auto& t : tracks)
    {
        if (! t.used) continue;
        // track name (and instrument name) as the very first events: DAWs name the imported track after it
        juce::MidiMessageSequence out;
        out.addEvent (juce::MidiMessage::textMetaEvent (3, t.name), 0);
        out.addEvent (juce::MidiMessage::textMetaEvent (4, t.name), 0);
        addBendRange (out, t.midiCh, bendRange);
        t.seq.sort();
        out.addSequence (t.seq, 0.0);
        out.updateMatchedPairs();
        out.addEvent (juce::MidiMessage::endOfTrack(), endTick);
        mf.addTrack (out);
    }
    return mf;
}

juce::MidiFile exportModMidi (const Song& song, const MidiExportOptions& opt, juce::String* summary)
{
    ModEngine e;
    e.prepare (48000.0);
    e.setSong (&song);
    const int numCh = song.channels;
    const int range = juce::jlimit (1, 48, opt.bendRange);
    const bool channelMode = ! opt.perSample && opt.onlySample < 0 && ! opt.perInstrument;
    const float sep = (float) song.stereoSeparation / 100.0f;
    constexpr double kEchoWindow = 6 * 240.0;   // 6 rows: a second voice of the same sample within this time = echo/chord

    // group = what a track stands for: sample (perSample/onlySample), instrument or tracker channel
    auto groupName = [&] (int g) -> juce::String {
        if (channelMode) return "Kanal " + juce::String (g + 1);
        if (opt.perSample || opt.onlySample >= 0 || song.instruments.empty()) return sampleTitle (song, g);
        const auto nm = song.instrumentName (g + 1);
        return juce::String (g + 1).paddedLeft ('0', 2) + " " + (nm.isNotEmpty() ? nm : song.title);
    };
    // every group gets as many voice tracks as it needs at the same time (lane 0 = plain name,
    // lane 1.. = "(Stimme 2)" ...): echoes and chords keep their own volume, panning and note-offs
    struct Lane { Track tr; int pan = -1, offset = 0, lastChannel = -1; };
    std::map<std::pair<int, int>, Lane> lanes;

    struct Active { bool on = false; int group = 0, lane = 0, note = 0, trigVol = 64, basePeriod = 428; double start = 0.0; };
    std::vector<Active> act ((size_t) numCh);
    juce::MidiMessageSequence meta;
    meta.addEvent (juce::MidiMessage::textMetaEvent (3, song.title.isNotEmpty() ? song.title : juce::String ("Spurwerk")), 0);
    double t = 0.0;
    int lastSpeed = -1, lastTempo = -1;
    const double limitSecs = opt.maxMinutes * 60.0;
    double secs = 0.0;
    int notes = 0;

    auto lane = [&] (int g, int l) -> Lane& {
        auto& ln = lanes[{ g, l }];
        if (! ln.tr.used) { ln.tr.used = true; ln.tr.midiCh = kChannelMap[g % 15]; }
        return ln;
    };
    auto noteOff = [&] (Active& a, double when) {
        if (! a.on) return;
        auto& tr = lane (a.group, a.lane).tr;
        tr.seq.addEvent (juce::MidiMessage::noteOff (tr.midiCh, a.note), when);
        a.on = false;
    };
    auto panValue = [&] (float p) { return juce::jlimit (0, 127, juce::roundToInt ((juce::jlimit (-1.0f, 1.0f, p * sep) + 1.0f) * 63.5f)); };

    while (! e.stopped && e.songLoops == 0 && secs < limitSecs)
    {
        if (e.speed != lastSpeed || e.tempo != lastTempo)
        {
            lastSpeed = e.speed; lastTempo = e.tempo;
            const double bpm = 60.0 / (4.0 * juce::jmax (1, e.speed) * e.tickSeconds());   // a row = a 16th; also right for MPTM modern tempo
            meta.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60000000.0 / bpm)), t);
        }
        const double when = std::round (t);
        for (int i = 0; i < numCh; ++i)
        {
            auto& c = e.ch[(size_t) i];
            auto& a = act[(size_t) i];
            if (c.triggered)
            {
                c.triggered = false;
                noteOff (a, when);
                const int instr = c.trigInstr;
                if (instr < 1) continue;
                if (opt.perInstrument && ! opt.perSample && opt.onlySample < 0 && instr > juce::jmax (1, song.numInstruments())) continue;
                if ((opt.perSample || opt.onlySample >= 0) && (c.sample < 0 || c.sample >= song.numSamples())) continue;
                if (opt.onlySample >= 0 && c.sample != opt.onlySample) continue;
                const int g = opt.perSample || opt.onlySample >= 0 ? c.sample : opt.perInstrument ? instr - 1 : i;

                // lowest lane that is not held by a recent, audible note of this group on another channel
                int l = 0;
                if (! channelMode)
                    for (;; ++l)
                    {
                        bool busy = false;
                        for (int j = 0; j < numCh && ! busy; ++j)
                        {
                            const auto& o = act[(size_t) j];
                            busy = j != i && o.on && o.group == g && o.lane == l && when - o.start <= kEchoWindow && e.ch[(size_t) j].mixVol > 0.5f;
                        }
                        if (! busy) break;
                    }
                // an older note still ringing on that lane ends here (as on a single tracker channel)
                for (int j = 0; j < numCh; ++j)
                {
                    auto& o = act[(size_t) j];
                    if (j != i && o.on && o.group == g && o.lane == l) noteOff (o, when);
                }
                auto& ln = lane (g, l);
                auto& tr = ln.tr;
                const bool force = ln.lastChannel != i;   // lane continues on another tracker channel: restate controllers
                ln.lastChannel = i;
                if (channelMode && tr.lastProgram != instr - 1)
                {
                    tr.seq.addEvent (juce::MidiMessage::programChange (tr.midiCh, juce::jmin (127, instr - 1)), when);
                    tr.lastProgram = instr - 1;
                }
                a.on = true;
                a.group = g;
                a.lane = l;
                a.start = when;
                a.note = juce::jlimit (0, 127, c.trigNote - 1);
                a.trigVol = juce::jmax (1, juce::roundToInt (c.mixVol));
                a.basePeriod = e.notePeriod (c.trigNote, c.sample, c.finetune);
                if (force || tr.lastBend != 8192) { tr.seq.addEvent (juce::MidiMessage::pitchWheel (tr.midiCh, 8192), when); tr.lastBend = 8192; }
                if (force || tr.lastCc11 != 127) { tr.seq.addEvent (juce::MidiMessage::controllerEvent (tr.midiCh, 11, 127), when); tr.lastCc11 = 127; }
                const int pan = panValue (c.mixPan);
                if (force || ln.pan != pan) { tr.seq.addEvent (juce::MidiMessage::controllerEvent (tr.midiCh, 10, pan), when); ln.pan = pan; }
                // sample offset (9xx): CC111 = offset/256 high bits, CC110 = low 7 bits (Spurwerk Player / Sampler-Modus)
                const int offset = juce::jlimit (0, 16383, (int) (c.pos / 256.0 + 0.5));
                if (force || ln.offset != offset)
                {
                    tr.seq.addEvent (juce::MidiMessage::controllerEvent (tr.midiCh, 111, offset >> 7), when);
                    tr.seq.addEvent (juce::MidiMessage::controllerEvent (tr.midiCh, 110, offset & 127), when);
                    ln.offset = offset;
                }
                const int vel = juce::jlimit (1, 127, juce::roundToInt (c.mixVol * 127.0 / 64.0));
                tr.seq.addEvent (juce::MidiMessage::noteOn (tr.midiCh, a.note, (juce::uint8) vel), when);
                ++notes;
            }
            if (! a.on) { c.cutFlag = false; continue; }
            if (! c.active || c.cutFlag)
            {
                c.cutFlag = false;
                noteOff (a, when);
                continue;
            }
            auto& ln = lane (a.group, a.lane);
            if (ln.lastChannel != i) continue;   // lane taken over by a newer voice
            auto& tr = ln.tr;
            if (c.outPeriod > 0)
            {
                const double semis = e.periodSemis (a.basePeriod, c.outPeriod);
                const int b = bendValue (semis, range);
                if (std::abs (b - tr.lastBend) > 8)
                {
                    tr.seq.addEvent (juce::MidiMessage::pitchWheel (tr.midiCh, b), when);
                    tr.lastBend = b;
                }
            }
            const int cc = juce::jlimit (0, 127, juce::roundToInt (127.0 * c.mixVol / a.trigVol));
            if (cc != tr.lastCc11)
            {
                tr.seq.addEvent (juce::MidiMessage::controllerEvent (tr.midiCh, 11, cc), when);
                tr.lastCc11 = cc;
            }
            const int pan = panValue (c.mixPan);
            if (pan != ln.pan)
            {
                tr.seq.addEvent (juce::MidiMessage::controllerEvent (tr.midiCh, 10, pan), when);
                ln.pan = pan;
            }
        }
        t += 240.0 / juce::jmax (1, e.speed);
        secs += e.tickSeconds();
        e.analysisTick();
    }
    const double end = std::round (t);
    for (auto& a : act) noteOff (a, end);

    std::vector<Track> tracks;
    std::vector<int> groups;
    for (auto& [key, ln] : lanes)
    {
        if (! ln.tr.used) continue;
        ln.tr.name = groupName (key.first) + (key.second > 0 ? " (Stimme " + juce::String (key.second + 1) + ")" : juce::String());
        tracks.push_back (std::move (ln.tr));
        groups.push_back (key.first);
    }
    if (opt.trackGroups != nullptr) *opt.trackGroups = groups;
    if (summary != nullptr)
        *summary = juce::String (notes) + " Noten, " + juce::String (secs / 60.0, 1).replaceCharacter ('.', ',') + U (" Min., ")
                 + (opt.perSample ? U ("Spur je Sample") : opt.onlySample >= 0 ? U ("ein Sample") : opt.perInstrument ? U ("Spur je Instrument") : U ("Spur je Kanal"));
    return finish (tracks, meta, end, range);
}

juce::MidiFile exportYmMidi (const Song& song, const MidiExportOptions& opt, juce::String* summary)
{
    const auto& ym = song.ym;
    const int range = juce::jlimit (1, 48, opt.bendRange);
    const double bpm = 125.0;
    const double ticksPerFrame = kExportPpq * bpm / (60.0 * ym.rate);
    std::vector<Track> tracks (4);
    const char* names[3] = { "YM Stimme A", "YM Stimme B", "YM Stimme C" };
    for (int c = 0; c < 3; ++c) { tracks[(size_t) c].midiCh = c + 1; tracks[(size_t) c].name = names[c]; }
    tracks[3].midiCh = 10; tracks[3].name = "YM Rauschen";
    juce::MidiMessageSequence meta;
    meta.addEvent (juce::MidiMessage::textMetaEvent (3, song.title.isNotEmpty() ? song.title : juce::String ("Spurwerk YM")), 0);
    meta.addEvent (juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60000000.0 / bpm)), 0);

    struct Active { bool on = false; int note = 0, vol = 15; bool drum = false; };
    Active act[3];
    int notes = 0;
    const int maxFrames = juce::jmin (ym.frames, (int) (opt.maxMinutes * 60.0 * ym.rate));
    for (int f = 0; f < maxFrames; ++f)
    {
        const double when = std::round (f * ticksPerFrame);
        for (int c = 0; c < 3; ++c)
        {
            const auto v = ymVoice (ym, f, c);
            auto& a = act[c];
            const bool toneVoice = v.tone && v.period > 1;
            const bool drumVoice = ! toneVoice && v.noise;
            if (! v.audible || (! toneVoice && ! drumVoice))
            {
                if (a.on)
                {
                    auto& tr = tracks[a.drum ? 3 : (size_t) c];
                    tr.seq.addEvent (juce::MidiMessage::noteOff (tr.midiCh, a.note), when);
                    a.on = false;
                }
                continue;
            }
            const int volume = v.env ? 13 : v.volume;
            if (drumVoice)
            {
                const int dn = (song.ym.regs[(size_t) f][6] & 31) < 8 ? 42 : 38;   // short noise period: hi-hat, else snare
                if (! a.on || ! a.drum || dn != a.note)
                {
                    if (a.on) { auto& tr0 = tracks[a.drum ? 3 : (size_t) c]; tr0.seq.addEvent (juce::MidiMessage::noteOff (tr0.midiCh, a.note), when); }
                    auto& tr = tracks[3];
                    tr.used = true;
                    tr.seq.addEvent (juce::MidiMessage::noteOn (10, dn, (juce::uint8) juce::jlimit (1, 127, volume * 8 + 7)), when);
                    a = { true, dn, volume, true };
                    ++notes;
                }
                continue;
            }
            auto& tr = tracks[(size_t) c];
            const int note = juce::jlimit (0, 127, juce::roundToInt (v.midi));
            const bool attack = a.on && ! a.drum && volume > a.vol + 2;
            if (! a.on || a.drum || note != a.note || attack)
            {
                if (a.on) { auto& tr0 = tracks[a.drum ? 3 : (size_t) c]; tr0.seq.addEvent (juce::MidiMessage::noteOff (tr0.midiCh, a.note), when); }
                tr.used = true;
                if (tr.lastCc11 != 127) { tr.seq.addEvent (juce::MidiMessage::controllerEvent (tr.midiCh, 11, 127), when); tr.lastCc11 = 127; }
                tr.seq.addEvent (juce::MidiMessage::noteOn (tr.midiCh, note, (juce::uint8) juce::jlimit (1, 127, volume * 8 + 7)), when);
                a = { true, note, volume, false };
                ++notes;
            }
            const int b = bendValue (v.midi - a.note, range);
            if (std::abs (b - tr.lastBend) > 8) { tr.seq.addEvent (juce::MidiMessage::pitchWheel (tr.midiCh, b), when); tr.lastBend = b; }
            const int cc = juce::jlimit (0, 127, juce::roundToInt (127.0 * volume / juce::jmax (1, a.vol)));
            if (cc != tr.lastCc11) { tr.seq.addEvent (juce::MidiMessage::controllerEvent (tr.midiCh, 11, cc), when); tr.lastCc11 = cc; }
        }
    }
    const double end = std::round (maxFrames * ticksPerFrame);
    for (int c = 0; c < 3; ++c)
        if (act[c].on) { auto& tr = tracks[act[c].drum ? 3 : (size_t) c]; tr.seq.addEvent (juce::MidiMessage::noteOff (tr.midiCh, act[c].note), end); }
    if (summary != nullptr)
        *summary = juce::String (notes) + " Noten, " + juce::String (maxFrames / (double) ym.rate / 60.0, 1).replaceCharacter ('.', ',') + U (" Min., 3 Stimmen + Rauschen");
    return finish (tracks, meta, end, range);
}
} // namespace

juce::MidiFile exportMidi (const Song& song, const MidiExportOptions& opt, juce::String* summary)
{
    return song.format == Format::YM ? exportYmMidi (song, opt, summary) : exportModMidi (song, opt, summary);
}

bool writeMidiFile (const juce::MidiFile& mf, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream os (file);
    if (! os.openedOk()) return false;
    return mf.writeTo (os, 1);
}

juce::String sampleTitle (const Song& song, int index)
{
    const auto nm = index >= 0 && index < song.numSamples() ? song.samples[(size_t) index].name.trim() : juce::String();
    const auto fallback = song.title.trim().isNotEmpty() ? song.title.trim() : juce::String ("Spurwerk");
    return juce::String (index + 1).paddedLeft ('0', 2) + " " + (nm.isNotEmpty() ? nm : fallback);
}

bool exportSamplesWavMidi (const Song& song, const juce::File& folder, juce::String& log)
{
    log.clear();
    if (! folder.createDirectory()) { log = "Ordner kann nicht angelegt werden"; return false; }
    int wavs = 0;
    for (int i = 0; i < song.numSamples(); ++i)
    {
        const auto& smp = song.samples[(size_t) i];
        if (smp.pcm.empty()) continue;
        const auto f = folder.getChildFile (safeFileName (sampleTitle (song, i)) + ".wav");
        if (! writeSampleWav (smp, f)) { log << "Fehler: " << f.getFileName() << "\n"; continue; }
        ++wavs;
    }
    MidiExportOptions o;
    o.perSample = true;
    juce::String sum;
    const auto mf = exportMidi (song, o, &sum);
    const auto mid = folder.getChildFile (safeFileName (song.title.isNotEmpty() ? song.title : juce::String ("Spurwerk")) + ".mid");
    const bool ok = writeMidiFile (mf, mid);
    log << wavs << " WAV-Dateien + " << mid.getFileName() << " (" << sum << ")";
    return ok && wavs > 0;
}

juce::String safeFileName (const juce::String& s)
{
    auto r = juce::File::createLegalFileName (s.trim()).replaceCharacters (" ", "_");
    while (r.contains ("__")) r = r.replace ("__", "_");
    return r.substring (0, 40);
}

bool writeSampleWav (const Sample& s, const juce::File& file)
{
    if (s.pcm.empty()) return false;
    file.deleteFile();
    std::unordered_map<juce::String, juce::String> meta;
    meta["MidiUnityNote"] = "60";
    if (s.hasLoop())
    {
        meta["NumSampleLoops"] = "1";
        meta["Loop0Type"] = "0";
        meta["Loop0Start"] = juce::String (s.loopStart);
        meta["Loop0End"] = juce::String (s.loopStart + s.loopLength - 1);
    }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (file);
    if (! static_cast<juce::FileOutputStream*> (os.get())->openedOk()) return false;
    auto opts = juce::AudioFormatWriterOptions().withSampleRate ((double) juce::roundToInt (s.c5Rate()))
                    .withNumChannels (1).withBitsPerSample (16).withMetadataValues (meta);
    auto writer = wav.createWriterFor (os, opts);
    if (writer == nullptr) return false;
    juce::AudioBuffer<float> buf (1, (int) s.pcm.size());
    for (int i = 0; i < (int) s.pcm.size(); ++i) buf.setSample (0, i, s.pcm[(size_t) i]);
    return writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
}

juce::Array<juce::File> exportSamples (const Song& song, const juce::File& folder, juce::String& log)
{
    juce::Array<juce::File> files;
    log.clear();
    if (! folder.createDirectory()) { log = "Ordner kann nicht angelegt werden"; return files; }
    const auto base = safeFileName (song.title.isNotEmpty() ? song.title : juce::String ("Spurwerk"));
    juce::String kit, ds;
    kit << "// " << song.title << " - Spurwerk Kit: Sample N liegt auf Taste 36+N-1 (C2 aufwaerts)\n<control> default_path=\n";
    ds << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<DecentSampler minVersion=\"1.0.0\">\n  <ui width=\"812\" height=\"375\"><tab name=\"main\">"
       << "<label x=\"20\" y=\"20\" width=\"700\" height=\"40\" text=\"" << base << " (Spurwerk)\"/></tab></ui>\n  <groups>\n";
    int n = 0;
    for (int i = 0; i < song.numSamples(); ++i)
    {
        const auto& s = song.samples[(size_t) i];
        if (s.pcm.empty()) continue;
        const auto name = safeFileName (sampleTitle (song, i));
        const auto wavFile = folder.getChildFile (name + ".wav");
        if (! writeSampleWav (s, wavFile)) { log << "Fehler: " << wavFile.getFileName() << "\n"; continue; }
        files.add (wavFile);
        ++n;
        const double vol = s.volume > 0 ? 20.0 * std::log10 (s.volume / 64.0) : -144.0;
        juce::String region;
        region << "sample=" << wavFile.getFileName() << " volume=" << juce::String (vol, 2);
        if (s.hasLoop())
            region << " loop_mode=loop_continuous loop_start=" << s.loopStart << " loop_end=" << (s.loopStart + s.loopLength - 1);
        else region << " loop_mode=one_shot";
        juce::String sfz;
        sfz << "// " << s.name << " (" << song.title << ") - Spurwerk\n<region> " << region << " pitch_keycenter=60 lokey=0 hikey=127\n";
        folder.getChildFile (name + ".sfz").replaceWithText (sfz);
        const int key = juce::jlimit (0, 127, 36 + i);
        kit << "<region> " << region << " pitch_keycenter=" << key << " key=" << key << "\n";
        ds << "    <group><sample path=\"" << wavFile.getFileName() << "\" rootNote=\"" << key << "\" loNote=\"" << key << "\" hiNote=\"" << key
           << "\" volume=\"" << juce::String (vol, 2) << "dB\"";
        if (s.hasLoop()) ds << " loopEnabled=\"true\" loopStart=\"" << s.loopStart << "\" loopEnd=\"" << (s.loopStart + s.loopLength - 1) << "\"";
        ds << "/></group>\n";
    }
    ds << "  </groups>\n</DecentSampler>\n";
    folder.getChildFile (base + "_Kit.sfz").replaceWithText (kit);
    folder.getChildFile (base + "_Kit.dspreset").replaceWithText (ds);
    log << n << " Samples als WAV + SFZ, dazu " << base << "_Kit.sfz und " << base << "_Kit.dspreset";
    return files;
}

bool renderSongWav (const Song& song, const juce::File& file, double sr, double maxMinutes, juce::String& log)
{
    ModEngine me;
    YmEngine ye;
    me.prepare (sr);
    ye.prepare (sr);
    const bool isYm = song.format == Format::YM;
    if (isYm) { ye.setData (&song.ym); ye.stereoSeparation = song.stereoSeparation; }
    else me.setSong (&song);
    const int block = 1024;
    const int maxSamples = (int) (maxMinutes * 60.0 * sr);
    juce::AudioBuffer<float> all (2, 0);
    std::vector<float> L, R;
    juce::AudioBuffer<float> buf (2, block);
    int total = 0;
    while (total < maxSamples)
    {
        buf.clear();
        if (isYm) ye.render (buf.getWritePointer (0), buf.getWritePointer (1), block, 0, 1.0f);
        else me.render (buf.getWritePointer (0), buf.getWritePointer (1), block, 0, 1.0f);
        L.insert (L.end(), buf.getReadPointer (0), buf.getReadPointer (0) + block);
        R.insert (R.end(), buf.getReadPointer (1), buf.getReadPointer (1) + block);
        total += block;
        if ((isYm ? ye.songLoops : me.songLoops) > 0 || (! isYm && me.stopped)) break;
    }
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (16));
    if (writer == nullptr) { log = "WAV kann nicht geschrieben werden"; return false; }
    const float* ptrs[2] = { L.data(), R.data() };
    const bool ok = writer->writeFromFloatArrays (ptrs, 2, (int) L.size());
    float peak = 0.0f;
    for (size_t i = 0; i < L.size(); ++i) peak = juce::jmax (peak, std::abs (L[i]), std::abs (R[i]));
    log = juce::String ((double) L.size() / sr, 1) + " s, Spitze " + juce::String (peak, 3);
    return ok;
}
} // namespace sw
