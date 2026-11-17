// Spurwerk – export as Ableton Live Set (.als = gzip'ed XML). Ableton's format is not documented, so the set is built from
// the templates of the installed Live (Resources: Builtin/Templates/DefaultLiveSet.als, Core Library/Defaults/Instruments/
// Simpler.adv and the clip / sample-part structures of a Core Library demo set). Nothing of Ableton is shipped with Spurwerk.
#include "DawProject.h"
#include "Export.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <map>

namespace sw
{
namespace
{
using Xml = juce::XmlElement;

std::unique_ptr<Xml> loadGzXml (const juce::File& f)
{
    juce::FileInputStream in (f);
    if (! in.openedOk()) return nullptr;
    juce::GZIPDecompressorInputStream gz (&in, false, juce::GZIPDecompressorInputStream::gzipFormat);
    return juce::parseXML (gz.readEntireStreamAsString());
}

Xml* child (Xml* e, std::initializer_list<const char*> path)
{
    for (auto* p : path)
    {
        if (e == nullptr) return nullptr;
        e = e->getChildByName (p);
    }
    return e;
}

void setValue (Xml* e, const juce::String& v) { if (e != nullptr) e->setAttribute ("Value", v); }
void setValue (Xml* e, double v) { if (e != nullptr) e->setAttribute ("Value", juce::String (v, 6)); }

Xml* findFirst (Xml* e, const juce::String& tag)
{
    if (e == nullptr) return nullptr;
    if (e->hasTagName (tag)) return e;
    for (auto* c : e->getChildIterator())
        if (auto* r = findFirst (c, tag)) return r;
    return nullptr;
}

Xml* findFirstWithin (Xml* e, const juce::String& outer, const juce::String& inner)
{
    if (e == nullptr) return nullptr;
    if (e->hasTagName (outer)) if (auto* r = findFirst (e, inner)) return r;
    for (auto* c : e->getChildIterator())
        if (auto* r = findFirstWithin (c, outer, inner)) return r;
    return nullptr;
}

// automation / modulation targets and pointees need set-wide unique ids
void renumber (Xml& e, int& next)
{
    const auto tag = e.getTagName();
    if (e.hasAttribute ("Id") && (tag.endsWith ("Target") || tag.startsWith ("ControllerTargets.") || tag == "Pointee"))
        e.setAttribute ("Id", next++);
    for (auto* c : e.getChildIterator()) renumber (*c, next);
}

void clearChildren (Xml* e) { if (e != nullptr) e->deleteAllChildElements(); }

void setFileRef (Xml* sampleRef, const juce::File& wav, juce::int64 frames, double rate)
{
    if (sampleRef == nullptr) return;
    if (auto* fr = sampleRef->getChildByName ("FileRef"))
    {
        setValue (fr->getChildByName ("RelativePathType"), "0");
        setValue (fr->getChildByName ("RelativePath"), juce::String());
        setValue (fr->getChildByName ("Path"), wav.getFullPathName());
        setValue (fr->getChildByName ("LivePackName"), juce::String());
        setValue (fr->getChildByName ("LivePackId"), juce::String());
        setValue (fr->getChildByName ("OriginalFileSize"), juce::String (wav.getSize()));
        setValue (fr->getChildByName ("OriginalCrc"), "0");
    }
    setValue (sampleRef->getChildByName ("LastModDate"), juce::String (wav.getLastModificationTime().toMilliseconds() / 1000));
    if (auto* sc = sampleRef->getChildByName ("SourceContext")) sc->deleteAllChildElements();
    setValue (sampleRef->getChildByName ("DefaultDuration"), juce::String (frames));
    setValue (sampleRef->getChildByName ("DefaultSampleRate"), juce::String (juce::roundToInt (rate)));
}

void setClipRange (Xml& clip, double endBeat, const juce::String& name, int colour)
{
    clip.setAttribute ("Time", "0");
    setValue (clip.getChildByName ("CurrentStart"), "0");
    setValue (clip.getChildByName ("CurrentEnd"), endBeat);
    if (auto* loop = clip.getChildByName ("Loop"))
    {
        setValue (loop->getChildByName ("LoopStart"), "0");
        setValue (loop->getChildByName ("LoopEnd"), endBeat);
        setValue (loop->getChildByName ("StartRelative"), "0");
        setValue (loop->getChildByName ("LoopOn"), "false");
        setValue (loop->getChildByName ("OutMarker"), endBeat);
        setValue (loop->getChildByName ("HiddenLoopStart"), "0");
        setValue (loop->getChildByName ("HiddenLoopEnd"), endBeat);
    }
    setValue (clip.getChildByName ("Name"), name);
    setValue (clip.getChildByName ("Color"), juce::String (colour));
    if (auto* sp = clip.getChildByName ("ScrollerTimePreserver")) { setValue (sp->getChildByName ("LeftTime"), "0"); setValue (sp->getChildByName ("RightTime"), endBeat); }
    if (auto* ts = clip.getChildByName ("TimeSelection")) { setValue (ts->getChildByName ("AnchorTime"), "0"); setValue (ts->getChildByName ("OtherTime"), "0"); }
    if (auto* env = child (&clip, { "Envelopes", "Envelopes" })) env->deleteAllChildElements();
}

void setTrackName (Xml& track, const juce::String& name, int colour)
{
    if (auto* n = track.getChildByName ("Name"))
    {
        setValue (n->getChildByName ("EffectiveName"), name);
        setValue (n->getChildByName ("UserName"), name);
    }
    setValue (track.getChildByName ("Color"), juce::String (colour));
}

Xml* mixerOf (Xml& track) { return child (&track, { "DeviceChain", "Mixer" }); }

const int kLiveColours[8] = { 14, 1, 4, 6, 9, 12, 16, 19 };
} // namespace

juce::File findAbletonResources()
{
    juce::Array<juce::File> dirs;
    const juce::File base ("C:\\ProgramData\\Ableton");
    for (const auto& d : base.findChildFiles (juce::File::findDirectories, false, "Live *"))
        if (d.getChildFile ("Resources").getChildFile ("Builtin").getChildFile ("Templates").getChildFile ("DefaultLiveSet.als").existsAsFile())
            dirs.add (d.getChildFile ("Resources"));
    if (dirs.isEmpty()) return {};
    std::sort (dirs.begin(), dirs.end(), [] (const juce::File& a, const juce::File& b) { return a.getFullPathName().compareNatural (b.getFullPathName()) > 0; });
    return dirs.getFirst();
}

bool exportAbletonLiveSet (const Song& song, const juce::File& chosen, juce::String& log)
{
    // project folder named after the song: "<Name>/<Name>.als" + "<Name>/Samples" (no extra level if the chosen folder already is it)
    const auto stem = chosen.getFileNameWithoutExtension();
    const auto projectDir = chosen.getParentDirectory().getFileName() == stem ? chosen.getParentDirectory() : chosen.getParentDirectory().getChildFile (stem);
    projectDir.createDirectory();
    const auto out = projectDir.getChildFile (stem + ".als");
    log.clear();
    const auto res = findAbletonResources();
    if (res == juce::File()) { log = U ("Ableton Live nicht gefunden (C:\\ProgramData\\Ableton\\Live …) – der Live-Set-Export braucht die Vorlagen einer installierten Live-Version"); return false; }
    auto set = loadGzXml (res.getChildFile ("Builtin/Templates/DefaultLiveSet.als"));
    auto simplerDoc = loadGzXml (res.getChildFile ("Core Library/Defaults/Instruments/Simpler.adv"));
    // clip and sample-part structures from a demo set of the Core Library
    std::unique_ptr<Xml> midiClipT, audioClipT, partT;
    for (const auto& f : res.getChildFile ("Core Library").findChildFiles (juce::File::findFiles, true, "*.als"))
    {
        if (midiClipT != nullptr && audioClipT != nullptr && partT != nullptr) break;
        if (f.getSize() > 8 * 1024 * 1024) continue;
        auto demo = loadGzXml (f);
        if (demo == nullptr) continue;
        if (midiClipT == nullptr) if (auto* c = findFirstWithin (demo.get(), "ArrangerAutomation", "MidiClip")) midiClipT = std::make_unique<Xml> (*c);
        if (audioClipT == nullptr) if (auto* c = findFirstWithin (demo.get(), "ArrangerAutomation", "AudioClip")) audioClipT = std::make_unique<Xml> (*c);
        if (partT == nullptr) if (auto* c = findFirstWithin (demo.get(), "OriginalSimpler", "MultiSamplePart")) partT = std::make_unique<Xml> (*c);
    }
    auto* liveSet = set != nullptr ? set->getChildByName ("LiveSet") : nullptr;
    auto* simplerT = simplerDoc != nullptr ? simplerDoc->getChildByName ("OriginalSimpler") : nullptr;
    auto* tracks = liveSet != nullptr ? liveSet->getChildByName ("Tracks") : nullptr;
    if (tracks == nullptr || simplerT == nullptr || midiClipT == nullptr || partT == nullptr)
    { log = U ("Vorlagen der Live-Installation unvollständig (") + res.getFullPathName() + ")"; return false; }

    std::unique_ptr<Xml> midiTrackT, audioTrackT;
    for (auto* t : tracks->getChildIterator())
    {
        if (midiTrackT == nullptr && t->hasTagName ("MidiTrack")) midiTrackT = std::make_unique<Xml> (*t);
        if (audioTrackT == nullptr && t->hasTagName ("AudioTrack")) audioTrackT = std::make_unique<Xml> (*t);
    }
    if (midiTrackT == nullptr) { log = U ("Vorlage ohne MIDI-Spur"); return false; }
    for (int i = tracks->getNumChildElements(); --i >= 0;)
        if (tracks->getChildElement (i)->hasTagName ("MidiTrack") || tracks->getChildElement (i)->hasTagName ("AudioTrack"))
            tracks->removeChildElement (tracks->getChildElement (i), true);
    int insertAt = 0;   // new tracks before the return tracks
    int nextPointee = liveSet->getChildByName ("NextPointeeId") != nullptr ? liveSet->getChildByName ("NextPointeeId")->getIntAttribute ("Value") + 100 : 50000;
    int nextTrackId = 1000;

    // ---- notes (same data as the DAWproject export: one track per sample)
    const bool ym = song.format == Format::YM;
    const Sample ymVoice = ymSquare(), ymNoiseVoice = ymNoise();
    MidiExportOptions o;
    o.perSample = true;
    o.bendRange = 12;
    std::vector<int> groups;
    o.trackGroups = &groups;
    const auto mf = exportMidi (song, o);
    const double ppq = (double) kExportPpq;
    std::vector<std::pair<double, double>> tempos;
    double endBeat = 4.0;
    for (int t = 0; t < mf.getNumTracks(); ++t)
        for (auto* ev : *mf.getTrack (t))
        {
            endBeat = juce::jmax (endBeat, ev->message.getTimeStamp() / ppq);
            if (t == 0 && ev->message.isTempoMetaEvent())
            {
                const double beat = ev->message.getTimeStamp() / ppq, bpm = 60.0 / ev->message.getTempoSecondsPerQuarterNote();
                if (! tempos.empty() && std::abs (tempos.back().first - beat) < 1e-9) tempos.back().second = bpm;
                else if (tempos.empty() || std::abs (tempos.back().second - bpm) > 1e-6) tempos.push_back ({ beat, bpm });
            }
        }
    if (tempos.empty()) tempos.push_back ({ 0.0, 125.0 });
    endBeat = std::ceil (endBeat / 4.0) * 4.0;

    const auto sampleDir = projectDir.getChildFile ("Samples");
    sampleDir.createDirectory();
    int midiTracks = 0, audioTracks = 0, noteCount = 0;

    for (int t = 1; t < mf.getNumTracks(); ++t)
    {
        auto seq = *mf.getTrack (t);
        seq.updateMatchedPairs();
        juce::String name;
        bool anyNote = false;
        for (auto* ev : seq) { if (ev->message.isTrackNameEvent() && name.isEmpty()) name = ev->message.getTextFromTextMetaEvent(); anyNote |= ev->message.isNoteOn(); }
        if (! anyNote) continue;
        const Sample* sp = nullptr;
        int si = t - 1;
        if (ym) sp = name.contains ("Rauschen") ? &ymNoiseVoice : &ymVoice;
        else
        {
            if (t - 1 >= (int) groups.size()) continue;
            si = groups[(size_t) t - 1];
            if (si < 0 || si >= song.numSamples()) continue;
            sp = &song.samples[(size_t) si];
        }
        if (sp->pcm.empty()) continue;
        const int colour = kLiveColours[si % 8];

        // sample file
        const auto wav = sampleDir.getChildFile (safeFileName (juce::String (t).paddedLeft ('0', 2) + " " + name) + ".wav");
        if (! writeSampleWav (*sp, wav)) continue;
        const double rate = (double) juce::roundToInt (sp->c5Rate());

        auto track = std::make_unique<Xml> (*midiTrackT);
        track->setAttribute ("Id", nextTrackId++);
        renumber (*track, nextPointee);
        setTrackName (*track, name, colour);

        // Simpler with the sample, pitch bend range ±12 like the MIDI export
        auto simpler = std::make_unique<Xml> (*simplerT);
        simpler->setAttribute ("Id", 0);
        renumber (*simpler, nextPointee);
        setValue (child (simpler.get(), { "Globals", "PitchBendRange" }), "12");
        if (auto* parts = child (simpler.get(), { "Player", "MultiSampleMap", "SampleParts" }))
        {
            auto part = std::make_unique<Xml> (*partT);
            part->setAttribute ("Id", 0);
            renumber (*part, nextPointee);
            setValue (part->getChildByName ("Name"), name);
            if (auto* kr = part->getChildByName ("KeyRange")) { setValue (kr->getChildByName ("Min"), "0"); setValue (kr->getChildByName ("Max"), "127"); }
            setValue (part->getChildByName ("RootKey"), "60");
            setValue (part->getChildByName ("Detune"), "0");
            setValue (part->getChildByName ("Volume"), "1");
            const int len = (int) sp->pcm.size();
            setValue (part->getChildByName ("SampleStart"), "0");
            setValue (part->getChildByName ("SampleEnd"), juce::String (len - 1));
            if (auto* sl = part->getChildByName ("SustainLoop"))
            {
                setValue (sl->getChildByName ("Start"), juce::String (sp->hasLoop() ? sp->loopStart : 0));
                setValue (sl->getChildByName ("End"), juce::String (sp->hasLoop() ? sp->loopStart + sp->loopLength : len - 1));
                setValue (sl->getChildByName ("Mode"), sp->hasLoop() ? "1" : "0");
            }
            if (auto* rl = part->getChildByName ("ReleaseLoop")) { setValue (rl->getChildByName ("Start"), "0"); setValue (rl->getChildByName ("End"), juce::String (len - 1)); }
            setFileRef (part->getChildByName ("SampleRef"), wav, len, rate);
            if (auto* wp = part->getChildByName ("SampleWarpProperties"))
            {
                setValue (wp->getChildByName ("IsWarped"), "false");
                if (auto* wm = wp->getChildByName ("WarpMarkers")) wm->deleteAllChildElements();
            }
            if (auto* sl = part->getChildByName ("SlicePoints")) sl->deleteAllChildElements();
            parts->deleteAllChildElements();
            parts->addChildElement (part.release());
        }
        if (auto* devices = child (track.get(), { "DeviceChain", "DeviceChain", "Devices" }))
            devices->addChildElement (simpler.release());

        // mixer: panning of the first note, YM note tracks muted (the rendered voices sound)
        double firstPan = 0.0;
        for (auto* ev : seq) if (ev->message.isController() && ev->message.getControllerNumber() == 10) { firstPan = ev->message.getControllerValue() / 63.5 - 1.0; break; }
        if (auto* mx = mixerOf (*track))
        {
            setValue (child (mx, { "Pan", "Manual" }), juce::jlimit (-1.0, 1.0, firstPan));
            if (ym) setValue (child (mx, { "Speaker", "Manual" }), "false");
        }

        // arrangement clip: notes per key + pitch bend envelope
        auto clip = std::make_unique<Xml> (*midiClipT);
        setClipRange (*clip, endBeat, name, colour);
        std::map<int, std::vector<std::tuple<double, double, int>>> byKey;
        juce::XmlElement bendEnv ("Events");
        int bendEvents = 0, noteId = 1;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto& m = seq.getEventPointer (i)->message;
            const double beat = seq.getEventTime (i) / ppq;
            if (m.isNoteOn())
            {
                const double off = seq.getTimeOfMatchingKeyUp (i) / ppq;
                byKey[m.getNoteNumber()].push_back ({ beat, juce::jmax (1.0 / 64.0, (off > beat ? off : endBeat) - beat), m.getVelocity() });
                ++noteCount;
            }
            else if (m.isPitchWheel())
            {
                if (bendEvents == 0)
                {
                    auto* e0 = bendEnv.createNewChildElement ("FloatEvent");
                    e0->setAttribute ("Id", 1); e0->setAttribute ("Time", "-63072000"); e0->setAttribute ("Value", "0");
                }
                auto* e = bendEnv.createNewChildElement ("FloatEvent");
                e->setAttribute ("Id", bendEvents + 2);
                e->setAttribute ("Time", juce::String (beat, 6));
                e->setAttribute ("Value", juce::String (m.getPitchWheelValue() - 8192));
                ++bendEvents;
            }
        }
        if (auto* keyTracks = child (clip.get(), { "Notes", "KeyTracks" }))
        {
            keyTracks->deleteAllChildElements();
            int kid = 0;
            for (auto& [key, notes] : byKey)
            {
                auto* kt = keyTracks->createNewChildElement ("KeyTrack");
                kt->setAttribute ("Id", kid++);
                auto* ns = kt->createNewChildElement ("Notes");
                for (auto& [time, dur, vel] : notes)
                {
                    auto* n = ns->createNewChildElement ("MidiNoteEvent");
                    n->setAttribute ("Time", juce::String (time, 6));
                    n->setAttribute ("Duration", juce::String (dur, 6));
                    n->setAttribute ("Velocity", juce::String (vel));
                    n->setAttribute ("OffVelocity", "64");
                    n->setAttribute ("NoteId", noteId++);
                }
                kt->createNewChildElement ("MidiKey")->setAttribute ("Value", key);
            }
        }
        if (auto* evl = child (clip.get(), { "Notes", "PerNoteEventStore", "EventLists" })) evl->deleteAllChildElements();
        if (auto* gen = findFirst (child (clip.get(), { "Notes" }), "NoteIdGenerator")) setValue (gen->getChildByName ("NextId"), juce::String (noteId));
        if (bendEvents > 0)
            if (auto* envs = child (clip.get(), { "Envelopes", "Envelopes" }))
                if (auto* pbTarget = child (track.get(), { "DeviceChain", "MainSequencer", "MidiControllers", "ControllerTargets.0" }))
                {
                    auto* ce = envs->createNewChildElement ("ClipEnvelope");
                    ce->setAttribute ("Id", 0);
                    ce->createNewChildElement ("EnvelopeTarget")->createNewChildElement ("PointeeId")->setAttribute ("Value", pbTarget->getIntAttribute ("Id"));
                    auto* au = ce->createNewChildElement ("Automation");
                    au->addChildElement (new Xml (bendEnv));
                    auto* tv = au->createNewChildElement ("AutomationTransformViewState");
                    tv->createNewChildElement ("IsTransformPending")->setAttribute ("Value", "false");
                    tv->createNewChildElement ("TimeAndValueTransforms");
                    ce->createNewChildElement ("LoopSlot")->createNewChildElement ("Value");
                    auto* sp2 = ce->createNewChildElement ("ScrollerTimePreserver");
                    sp2->createNewChildElement ("LeftTime")->setAttribute ("Value", "0");
                    sp2->createNewChildElement ("RightTime")->setAttribute ("Value", "0");
                }
        if (auto* evs = child (track.get(), { "DeviceChain", "MainSequencer", "ClipTimeable", "ArrangerAutomation", "Events" }))
            evs->addChildElement (clip.release());
        tracks->insertChildElement (track.release(), insertAt++);
        ++midiTracks;
    }

    // YM/COSO: the rendered voices as audio tracks
    if (ym && audioTrackT != nullptr && audioClipT != nullptr)
    {
        const double sr = 48000.0, bpm = tempos.front().second;
        for (int v = 0; v < 3; ++v)
        {
            const juce::String name = "Stimme " + juce::String::charToString ((juce::juce_wchar) ('A' + v)) + " (Audio)";
            const auto wav = sampleDir.getChildFile ("YM " + name.replace ("(", "").replace (")", "").trim() + ".wav");
            const double secs = renderYmVoice (song, v, wav, sr);
            if (secs <= 0.0) continue;
            const double beats = secs * bpm / 60.0;
            auto track = std::make_unique<Xml> (*audioTrackT);
            track->setAttribute ("Id", nextTrackId++);
            renumber (*track, nextPointee);
            setTrackName (*track, name, kLiveColours[v]);
            auto clip = std::make_unique<Xml> (*audioClipT);
            setClipRange (*clip, beats, name, kLiveColours[v]);
            setFileRef (clip->getChildByName ("SampleRef"), wav, (juce::int64) std::llround (secs * sr), sr);
            setValue (clip->getChildByName ("IsWarped"), "true");
            setValue (clip->getChildByName ("WarpMode"), "3");   // Re-Pitch: 1:1 at the song tempo, no stretching artefacts
            if (auto* wm = clip->getChildByName ("WarpMarkers"))
            {
                wm->deleteAllChildElements();
                auto* a = wm->createNewChildElement ("WarpMarker");
                a->setAttribute ("Id", 0); a->setAttribute ("SecTime", "0"); a->setAttribute ("BeatTime", "0");
                auto* b = wm->createNewChildElement ("WarpMarker");
                b->setAttribute ("Id", 1); b->setAttribute ("SecTime", juce::String (secs, 6)); b->setAttribute ("BeatTime", juce::String (beats, 6));
            }
            if (auto* evs = child (track.get(), { "DeviceChain", "MainSequencer", "Sample", "ArrangerAutomation", "Events" }))
                evs->addChildElement (clip.release());
            else if (auto* evs2 = findFirstWithin (track.get(), "ArrangerAutomation", "Events"))
                evs2->addChildElement (clip.release());
            tracks->insertChildElement (track.release(), insertAt++);
            ++audioTracks;
        }
    }
    if (midiTracks + audioTracks == 0) { log = U ("Keine Spuren mit Noten gefunden"); return false; }

    // tempo (+ tempo changes as automation on the main track)
    if (auto* main = liveSet->getChildByName ("MainTrack"))
        if (auto* tempo = child (main, { "DeviceChain", "Mixer", "Tempo" }))
        {
            setValue (tempo->getChildByName ("Manual"), std::round (tempos.front().second * 100.0) / 100.0);
            const int tid = tempo->getChildByName ("AutomationTarget") != nullptr ? tempo->getChildByName ("AutomationTarget")->getIntAttribute ("Id") : -1;
            if (auto* envs = child (main, { "AutomationEnvelopes", "Envelopes" }))
                for (auto* env : envs->getChildIterator())
                    if (auto* pid = child (env, { "EnvelopeTarget", "PointeeId" }); pid != nullptr && pid->getIntAttribute ("Value") == tid)
                        if (auto* evs = child (env, { "Automation", "Events" }))
                        {
                            evs->deleteAllChildElements();
                            int id = 1;
                            auto add = [&] (const juce::String& time, double bpm) {
                                auto* e = evs->createNewChildElement ("FloatEvent");
                                e->setAttribute ("Id", id++); e->setAttribute ("Time", time); e->setAttribute ("Value", juce::String (bpm, 6));
                            };
                            add ("-63072000", tempos.front().second);
                            for (size_t k = 1; k < tempos.size(); ++k) add (juce::String (tempos[k].first, 6), tempos[k].second);
                        }
        }
    setValue (liveSet->getChildByName ("NextPointeeId"), juce::String (nextPointee + 10));

    // write gzip'ed XML
    out.deleteFile();
    bool ok = false;
    {
        juce::FileOutputStream fos (out);
        if (fos.openedOk())
        {
            juce::GZIPCompressorOutputStream gz (fos, 6, juce::GZIPCompressorOutputStream::windowBitsGZIP);
            juce::XmlElement::TextFormat fmt;
            fmt.addDefaultHeader = true;
            set->writeTo (gz, fmt);
            gz.flush();
            ok = true;
        }
    }
    log << midiTracks << U (" MIDI-Spuren mit Simpler") << (audioTracks > 0 ? ", " + juce::String (audioTracks) + U (" Audiospuren (YM-Stimmen)") : juce::String())
        << ", " << noteCount << " Noten, Tempo " << juce::roundToInt (tempos.front().second) << " BPM → " << out.getFullPathName();
    if (! ok) log = "Datei kann nicht geschrieben werden: " + out.getFullPathName();
    return ok;
}
} // namespace sw
