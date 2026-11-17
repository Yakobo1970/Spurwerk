#include "DawProject.h"
#include "Export.h"
#include "SampleShare.h"
#include "Engine.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <map>

namespace sw
{
namespace
{
// VST3 preset file (as written by the VST3 SDK's PresetFile): header, component state chunk, chunk list
juce::MemoryBlock makeVstPreset (const char* cid32, const juce::MemoryBlock& comp)
{
    juce::MemoryBlock mb;
    juce::int64 listOffset = 0;
    {
        juce::MemoryOutputStream os (mb, false);
        os.write ("VST3", 4);
        os.writeInt (1);                              // format version
        os.write (cid32, 32);                         // class ID, ASCII hex
        os.writeInt64 (0);                            // chunk list offset (patched below)
        const juce::int64 compOffset = os.getPosition();
        os.write (comp.getData(), comp.getSize());
        listOffset = os.getPosition();
        os.write ("List", 4);
        os.writeInt (1);
        os.write ("Comp", 4);
        os.writeInt64 (compOffset);
        os.writeInt64 ((juce::int64) comp.getSize());
    }
    mb.copyFrom (&listOffset, 40, 8);                 // little endian like the SDK on x86/x64
    return mb;
}

juce::String num (double v) { return juce::String (v, 6); }

struct Ids
{
    int next = 0;
    juce::String operator()() { return "id" + juce::String (next++); }
};

juce::XmlElement* addParam (juce::XmlElement& parent, const char* tag, Ids& id, const char* name, const char* unit, double value, double mn, double mx)
{
    auto* e = parent.createNewChildElement (tag);
    if (unit != nullptr) { e->setAttribute ("unit", unit); e->setAttribute ("min", num (mn)); e->setAttribute ("max", num (mx)); }
    e->setAttribute ("value", num (value));
    e->setAttribute ("id", id());
    e->setAttribute ("name", name);
    return e;
}

// returns the id of the Pan parameter (target for pan automation)
juce::String addMixer (juce::XmlElement& ch, Ids& id, double volume, double pan = 0.5, bool muted = false)
{
    auto* mute = ch.createNewChildElement ("Mute");
    mute->setAttribute ("value", muted ? "true" : "false");
    mute->setAttribute ("id", id());
    mute->setAttribute ("name", "Mute");
    auto* p = addParam (ch, "Pan", id, "Pan", "normalized", pan, 0.0, 1.0);
    addParam (ch, "Volume", id, "Volume", "linear", volume, 0.0, 2.0);
    return p->getStringAttribute ("id");
}

const char* kColours[8] = { "#e5484d", "#f08c3a", "#f2c94c", "#5cc97b", "#3bc4b0", "#4ea4f5", "#9b7cf2", "#e06bb3" };
} // namespace


// YM/COSO: chip voices for the Spurwerk Player (MIDI 60 = middle C, like the YM MIDI export)
Sample ymSquare()
{
    Sample s;
    s.name = "YM Rechteck";
    // 59 periods in 4973 frames at 22050 Hz = 261.60 Hz (middle C, -0.2 cent); a standard rate keeps the WAV export happy
    constexpr int frames = 4973, periods = 59;
    for (int i = 0; i < frames; ++i) s.pcm.push_back (std::fmod ((double) i * periods / frames, 1.0) < 0.5 ? 0.45f : -0.45f);
    s.loopStart = 0;
    s.loopLength = frames;
    s.c5Speed = 22050.0;
    return s;
}

Sample ymNoise()
{
    Sample s;
    s.name = "YM Rauschen";
    juce::Random rnd (2149);
    juce::uint32 lfsr = 1;
    for (int i = 0; i < 32768; ++i)
    {
        lfsr = (lfsr >> 1) ^ ((lfsr & 1u) ? 0x12000u : 0u);   // 17-bit LFSR like the YM2149
        s.pcm.push_back ((lfsr & 1u) ? 0.4f : -0.4f);
    }
    s.loopStart = 0;
    s.loopLength = 32768;
    s.c5Speed = 44100.0;
    return s;
}

// one YM voice (with its noise and digidrums) rendered alone, stereo 16 bit; returns seconds
double renderYmVoice (const Song& song, int voice, const juce::File& file, double sr)
{
    YmEngine ye;
    ye.prepare (sr);
    ye.setData (&song.ym);
    ye.stereoSeparation = song.stereoSeparation;
    const juce::uint32 mute = 7u & ~(1u << voice);
    const int block = 1024, maxSamples = (int) (20.0 * 60.0 * sr);
    std::vector<float> L, R;
    juce::AudioBuffer<float> buf (2, block);
    while ((int) L.size() < maxSamples)
    {
        buf.clear();
        ye.render (buf.getWritePointer (0), buf.getWritePointer (1), block, mute, 1.0f);
        L.insert (L.end(), buf.getReadPointer (0), buf.getReadPointer (0) + block);
        R.insert (R.end(), buf.getReadPointer (1), buf.getReadPointer (1) + block);
        if (ye.songLoops > 0) break;
    }
    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (file);
    auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (16));
    if (writer == nullptr) return 0.0;
    const float* ptrs[2] = { L.data(), R.data() };
    writer->writeFromFloatArrays (ptrs, 2, (int) L.size());
    return (double) L.size() / sr;
}

bool exportDawProject (const Song& song, const juce::File& out, juce::String& log)
{
    log.clear();
    const bool ym = song.format == Format::YM;   // YM and COSO: no samples -> rendered voices + chip-like player samples
    const Sample ymVoice = ymSquare(), ymNoiseVoice = ymNoise();

    // notes per sample (same data as the MIDI export "one track per sample"); the player's bend range is ±12
    MidiExportOptions o;
    o.perSample = true;
    o.bendRange = 12;
    std::vector<int> groups;   // sample index of every exported track
    o.trackGroups = &groups;
    juce::String sum;
    const auto mf = exportMidi (song, o, &sum);
    const double ppq = (double) kExportPpq;
    if (mf.getNumTracks() < 2) { log = U ("Keine Noten im Song"); return false; }

    // tempo map
    std::vector<std::pair<double, double>> tempos;   // beat, bpm
    double endBeat = 4.0;
    for (int t = 0; t < mf.getNumTracks(); ++t)
        for (auto* ev : *mf.getTrack (t))
        {
            endBeat = juce::jmax (endBeat, ev->message.getTimeStamp() / ppq);
            if (t == 0 && ev->message.isTempoMetaEvent())
            {
                const double beat = ev->message.getTimeStamp() / ppq;
                const double bpm = 60.0 / ev->message.getTempoSecondsPerQuarterNote();
                if (! tempos.empty() && std::abs (tempos.back().first - beat) < 1e-9) tempos.back().second = bpm;
                else if (tempos.empty() || std::abs (tempos.back().second - bpm) > 1e-6) tempos.push_back ({ beat, bpm });
            }
        }
    if (tempos.empty()) tempos.push_back ({ 0.0, 125.0 });
    endBeat = std::ceil (endBeat / 4.0) * 4.0;


    Ids id;
    juce::XmlElement project ("Project");
    project.setAttribute ("version", "1.0");
    auto* app = project.createNewChildElement ("Application");
    app->setAttribute ("name", "Spurwerk");
    app->setAttribute ("version", SW_VERSION);
    auto* transport = project.createNewChildElement ("Transport");
    auto* tempoParam = addParam (*transport, "Tempo", id, "Tempo", "bpm", tempos.front().second, 20.0, 999.0);
    auto* ts = transport->createNewChildElement ("TimeSignature");
    ts->setAttribute ("denominator", 4);
    ts->setAttribute ("numerator", 4);
    ts->setAttribute ("id", id());
    auto* structure = project.createNewChildElement ("Structure");
    auto* arrangement = project.createNewChildElement ("Arrangement");
    arrangement->setAttribute ("id", id());
    auto* lanes = arrangement->createNewChildElement ("Lanes");
    lanes->setAttribute ("timeUnit", "beats");
    lanes->setAttribute ("id", id());

    const juce::String masterChannelId = "master_channel";
    juce::ZipFile::Builder zip;
    auto addBlock = [&] (const juce::MemoryBlock& data, const juce::String& path, int compression) {
        zip.addEntry (std::make_unique<juce::MemoryInputStream> (data, true), compression, path, juce::Time::getCurrentTime());
    };

    int trackCount = 0, noteCount = 0;
    const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Spurwerk").getChildFile ("dawproject");
    tempDir.deleteRecursively();
    tempDir.createDirectory();

    for (int t = 1; t < mf.getNumTracks(); ++t)
    {
        const auto* seq = mf.getTrack (t);
        juce::String name;
        for (auto* ev : *seq)
            if (ev->message.isTrackNameEvent()) { name = ev->message.getTextFromTextMetaEvent(); break; }
        int si = 0;
        const Sample* sp = nullptr;
        if (ym)
        {
            bool any = false;
            for (auto* ev : *seq) any |= ev->message.isNoteOn();
            if (! any) continue;
            const bool noise = name.contains ("Rauschen");
            sp = noise ? &ymNoiseVoice : &ymVoice;
            si = t - 1;
        }
        else
        {
            if (t - 1 >= (int) groups.size()) continue;
            si = groups[(size_t) t - 1];
            if (si < 0 || si >= song.numSamples()) continue;
            sp = &song.samples[(size_t) si];
        }
        const auto& smp = *sp;
        if (smp.pcm.empty()) continue;

        // MIDI of this sample alone (tempo track + its notes) for the player's MIDI field
        juce::MidiFile single;
        single.setTicksPerQuarterNote (kExportPpq);
        single.addTrack (*mf.getTrack (0));
        single.addTrack (*seq);
        juce::MemoryOutputStream midiOut;
        single.writeTo (midiOut);

        const auto base = juce::String (si + 1).paddedLeft ('0', 2) + "_" + juce::String (t);
        const auto state = makePlayerState (smp, name, song.title, {}, si, 1.0f, midiOut.getMemoryBlock());
        addBlock (makeVstPreset (kPlayerVst3Cid, state), "plugins/" + base + ".vstpreset", 0);
        const auto wav = tempDir.getChildFile (safeFileName (name) + ".wav");
        if (writeSampleWav (smp, wav))
        {
            juce::MemoryBlock wb;
            wav.loadFileAsData (wb);
            addBlock (wb, "samples/" + wav.getFileName(), 0);
        }

        // structure: track + channel + Spurwerk Player
        const auto trackId = id();
        auto* track = structure->createNewChildElement ("Track");
        track->setAttribute ("contentType", "notes");
        track->setAttribute ("loaded", "true");
        track->setAttribute ("id", trackId);
        track->setAttribute ("name", name);
        track->setAttribute ("color", kColours[si % 8]);
        auto* ch = track->createNewChildElement ("Channel");
        ch->setAttribute ("audioChannels", 2);
        ch->setAttribute ("destination", masterChannelId);
        ch->setAttribute ("role", "regular");
        ch->setAttribute ("solo", "false");
        ch->setAttribute ("id", id());
        auto* plug = ch->createNewChildElement ("Devices")->createNewChildElement ("Vst3Plugin");
        plug->setAttribute ("deviceID", kPlayerVst3Cid);
        plug->setAttribute ("deviceName", "Spurwerk Player");
        plug->setAttribute ("deviceRole", "instrument");
        plug->setAttribute ("deviceVendor", "Un(d)AbtanzBAR");
        plug->setAttribute ("loaded", "true");
        plug->setAttribute ("id", id());
        plug->setAttribute ("name", "Spurwerk Player");
        plug->createNewChildElement ("Parameters");
        auto* en = plug->createNewChildElement ("Enabled");
        en->setAttribute ("value", "true");
        en->setAttribute ("id", id());
        en->setAttribute ("name", "On/Off");
        plug->createNewChildElement ("State")->setAttribute ("path", "plugins/" + base + ".vstpreset");
        // panning of the tracker channel: first CC10 = track pan, later changes = pan automation
        double firstPan = 0.5;
        for (auto* ev : *seq)
            if (ev->message.isController() && ev->message.getControllerNumber() == 10) { firstPan = ev->message.getControllerValue() / 127.0; break; }
        const auto panId = addMixer (*ch, id, 1.0, firstPan, ym);   // YM: the rendered voices sound, the note tracks are muted

        // arrangement: one clip with all notes, pitch bend + CC11 as automation on the track
        auto* trackLanes = lanes->createNewChildElement ("Lanes");
        trackLanes->setAttribute ("track", trackId);
        trackLanes->setAttribute ("id", id());
        auto* clips = trackLanes->createNewChildElement ("Clips");
        clips->setAttribute ("id", id());
        auto* clip = clips->createNewChildElement ("Clip");
        clip->setAttribute ("time", num (0.0));
        clip->setAttribute ("duration", num (endBeat));
        clip->setAttribute ("playStart", num (0.0));
        clip->setAttribute ("name", name);
        auto* notes = clip->createNewChildElement ("Notes");
        notes->setAttribute ("id", id());
        juce::XmlElement bend ("Points"), cc11 ("Points"), panPts ("Points"), off110 ("Points"), off111 ("Points");
        int bendPoints = 0, ccPoints = 0, panChanges = 0;
        bool anyOffset = false;
        double lastPanValue = firstPan;
        auto addPoint = [] (juce::XmlElement& lane, double time, double value) {
            auto* p = lane.createNewChildElement ("RealPoint");
            p->setAttribute ("time", num (juce::jmax (0.0, time)));
            p->setAttribute ("value", num (value));
            p->setAttribute ("interpolation", "hold");
        };
        for (auto* ev : *seq)
        {
            const auto& m = ev->message;
            const double beat = m.getTimeStamp() / ppq;
            if (m.isNoteOn())
            {
                const double offBeat = ev->noteOffObject != nullptr ? ev->noteOffObject->message.getTimeStamp() / ppq : endBeat;
                auto* n = notes->createNewChildElement ("Note");
                n->setAttribute ("time", num (beat));
                n->setAttribute ("duration", num (juce::jmax (1.0 / 64.0, offBeat - beat)));
                n->setAttribute ("channel", 0);
                n->setAttribute ("key", m.getNoteNumber());
                n->setAttribute ("vel", num (m.getVelocity() / 127.0));
                n->setAttribute ("rel", num (0.5));
                ++noteCount;
            }
            else if (m.isPitchWheel())
            {
                auto* p = bend.createNewChildElement ("RealPoint");
                p->setAttribute ("time", num (beat));
                p->setAttribute ("value", num (m.getPitchWheelValue() / 16383.0));
                p->setAttribute ("interpolation", "hold");
                ++bendPoints;
            }
            else if (m.isController() && m.getControllerNumber() == 10)
            {
                const double v = m.getControllerValue() / 127.0;
                if (std::abs (v - lastPanValue) > 1e-6 || panChanges > 0)
                {
                    if (panChanges == 0) addPoint (panPts, 0.0, firstPan);   // automation starts at the initial pan
                    addPoint (panPts, beat, v);
                    ++panChanges;
                }
                lastPanValue = v;
            }
            else if (m.isController() && (m.getControllerNumber() == 110 || m.getControllerNumber() == 111))
            {
                // sample offset for the next note: slightly before the note so the player has it when the note starts
                addPoint (m.getControllerNumber() == 110 ? off110 : off111, beat - 0.001, m.getControllerValue() / 127.0);
                anyOffset |= m.getControllerValue() != 0;
            }
            else if (m.isController() && m.getControllerNumber() == 11)
            {
                auto* p = cc11.createNewChildElement ("RealPoint");
                p->setAttribute ("time", num (beat));
                p->setAttribute ("value", num (m.getControllerValue() / 127.0));
                p->setAttribute ("interpolation", "hold");
                ++ccPoints;
            }
        }
        auto addPoints = [&] (juce::XmlElement& src, bool isBend, int controller) {
            auto* pts = trackLanes->createNewChildElement ("Points");
            pts->setAttribute ("unit", "normalized");
            pts->setAttribute ("id", id());
            auto* target = pts->createNewChildElement ("Target");
            target->setAttribute ("expression", isBend ? "pitchBend" : "channelController");
            target->setAttribute ("channel", 0);
            if (! isBend) target->setAttribute ("controller", controller);
            for (auto* p : src.getChildIterator()) pts->addChildElement (new juce::XmlElement (*p));
        };
        if (bendPoints > 0) addPoints (bend, true, 0);
        if (ccPoints > 0) addPoints (cc11, false, 11);
        if (anyOffset) { addPoints (off111, false, 111); addPoints (off110, false, 110); }
        if (panChanges > 0)
        {
            auto* pts = trackLanes->createNewChildElement ("Points");
            pts->setAttribute ("unit", "normalized");
            pts->setAttribute ("id", id());
            pts->createNewChildElement ("Target")->setAttribute ("parameter", panId);
            for (auto* p : panPts.getChildIterator()) pts->addChildElement (new juce::XmlElement (*p));
        }
        ++trackCount;
    }
    // YM/COSO: every chip voice rendered exactly (envelopes, noise, digidrums, effects) as an audio track
    int audioTracks = 0;
    if (ym)
    {
        const double sr = 48000.0, bpm = tempos.front().second;
        const char* voiceNames[3] = { "Stimme A (Audio)", "Stimme B (Audio)", "Stimme C (Audio)" };
        for (int v = 0; v < 3; ++v)
        {
            const auto wav = tempDir.getChildFile ("ym_voice_" + juce::String::charToString ((juce::juce_wchar) ('A' + v)) + ".wav");
            const double secs = renderYmVoice (song, v, wav, sr);
            if (secs <= 0.0) continue;
            juce::MemoryBlock wb;
            wav.loadFileAsData (wb);
            const auto path = "audio/" + wav.getFileName();
            addBlock (wb, path, 0);
            const double beats = secs * bpm / 60.0;
            endBeat = juce::jmax (endBeat, std::ceil (beats / 4.0) * 4.0);
            const auto trackId = id();
            auto* track = structure->createNewChildElement ("Track");
            track->setAttribute ("contentType", "audio");
            track->setAttribute ("loaded", "true");
            track->setAttribute ("id", trackId);
            track->setAttribute ("name", voiceNames[v]);
            track->setAttribute ("color", kColours[v]);
            auto* ch = track->createNewChildElement ("Channel");
            ch->setAttribute ("audioChannels", 2);
            ch->setAttribute ("destination", masterChannelId);
            ch->setAttribute ("role", "regular");
            ch->setAttribute ("solo", "false");
            ch->setAttribute ("id", id());
            addMixer (*ch, id, 1.0);
            auto* trackLanes = lanes->createNewChildElement ("Lanes");
            trackLanes->setAttribute ("track", trackId);
            trackLanes->setAttribute ("id", id());
            auto* clips = trackLanes->createNewChildElement ("Clips");
            clips->setAttribute ("id", id());
            auto* clip = clips->createNewChildElement ("Clip");
            clip->setAttribute ("time", num (0.0));
            clip->setAttribute ("duration", num (beats));
            clip->setAttribute ("playStart", num (0.0));
            clip->setAttribute ("name", voiceNames[v]);
            auto* warps = clip->createNewChildElement ("Warps");
            warps->setAttribute ("contentTimeUnit", "seconds");
            warps->setAttribute ("timeUnit", "beats");
            warps->setAttribute ("id", id());
            auto* audio = warps->createNewChildElement ("Audio");
            audio->setAttribute ("channels", 2);
            audio->setAttribute ("sampleRate", (int) sr);
            audio->setAttribute ("duration", num (secs));
            audio->setAttribute ("id", id());
            audio->createNewChildElement ("File")->setAttribute ("path", path);
            for (const auto& [t, c] : { std::pair<double, double> { 0.0, 0.0 }, std::pair<double, double> { beats, secs } })
            {
                auto* w = warps->createNewChildElement ("Warp");
                w->setAttribute ("time", num (t));
                w->setAttribute ("contentTime", num (c));
            }
            ++audioTracks;
        }
    }
    if (trackCount == 0 && audioTracks == 0) { log = U ("Keine Spuren mit Noten gefunden"); return false; }

    // master track
    auto* master = structure->createNewChildElement ("Track");
    master->setAttribute ("contentType", "audio notes");
    master->setAttribute ("loaded", "true");
    master->setAttribute ("id", id());
    master->setAttribute ("name", "Master");
    auto* mch = master->createNewChildElement ("Channel");
    mch->setAttribute ("audioChannels", 2);
    mch->setAttribute ("role", "master");
    mch->setAttribute ("solo", "false");
    mch->setAttribute ("id", masterChannelId);
    addMixer (*mch, id, 1.0);

    if (tempos.size() > 1)
    {
        auto* ta = arrangement->createNewChildElement ("TempoAutomation");
        ta->setAttribute ("unit", "bpm");
        ta->setAttribute ("id", id());
        ta->createNewChildElement ("Target")->setAttribute ("parameter", tempoParam->getStringAttribute ("id"));
        for (const auto& [beat, bpm] : tempos)
        {
            auto* p = ta->createNewChildElement ("RealPoint");
            p->setAttribute ("time", num (beat));
            p->setAttribute ("value", num (bpm));
            p->setAttribute ("interpolation", "hold");
        }
    }

    juce::XmlElement meta ("MetaData");
    meta.createNewChildElement ("Title")->addTextElement (song.title);
    meta.createNewChildElement ("Comment")->addTextElement ("Spurwerk: " + song.formatName + (song.sourcePath.isNotEmpty() ? " – " + song.sourcePath : juce::String()));

    auto toBlock = [] (const juce::XmlElement& x) {
        juce::MemoryBlock b;
        juce::MemoryOutputStream os (b, false);
        x.writeTo (os);
        return b;
    };
    addBlock (toBlock (project), "project.xml", 6);
    addBlock (toBlock (meta), "metadata.xml", 6);

    out.deleteFile();
    bool ok = false;
    {
        juce::FileOutputStream fos (out);
        ok = fos.openedOk() && zip.writeToStream (fos, nullptr);
    }
    tempDir.deleteRecursively();
    if (ym) log << audioTracks << U (" Audiospuren (YM-Stimmen) + ") << trackCount << U (" stummgeschaltete Notenspuren mit Spurwerk Player, ");
    else log << trackCount << " Spuren mit Spurwerk Player, ";
    log << noteCount << " Noten, Tempo " << juce::roundToInt (tempos.front().second) << " BPM"
        << (tempos.size() > 1 ? U (" (mit Tempowechseln)") : juce::String());
    if (! ok) log = "Datei kann nicht geschrieben werden: " + out.getFullPathName();
    return ok;
}
} // namespace sw
