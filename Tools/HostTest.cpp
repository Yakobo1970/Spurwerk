// Loads the built Spurwerk.vst3 like a DAW: state with a module, host start/stop sync, sampler via MIDI, state round trip.
// Usage: HostTest <Spurwerk.vst3> <module file>
#include <juce_audio_processors/juce_audio_processors.h>
#include <iostream>

struct FakePlayHead : juce::AudioPlayHead
{
    double ppq = 0, bpm = 125;
    bool playing = false;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying (playing);
        p.setBpm (bpm);
        p.setPpqPosition (ppq);
        return p;
    }
};

static int failures = 0;
#define CHECK(cond, msg) do { if (! (cond)) { ++failures; std::cout << "FAIL: " << msg << "\n"; } else std::cout << "ok:   " << msg << "\n"; } while (0)

static float render (juce::AudioPluginInstance& inst, double seconds, const juce::MidiBuffer* first = nullptr)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    double sum = 0; long long n = 0;
    for (int i = 0; i < (int) (seconds * 48000 / 512); ++i)
    {
        midi.clear();
        if (i == 0 && first != nullptr) midi = *first;
        inst.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < 512; ++k) { const float v = buf.getSample (c, k); sum += v * v; ++n; }
    }
    return (float) std::sqrt (sum / juce::jmax (1LL, n));
}

// frequency estimate from rising zero crossings (channel 0)
static double measureHz (juce::AudioPluginInstance& inst, int note)
{
    juce::MidiBuffer m;
    m.addEvent (juce::MidiMessage::allNotesOff (1), 0);
    m.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) 110), 1);
    juce::AudioBuffer<float> buf (2, 512);
    int crossings = 0, total = 0;
    float last = 0.0f;
    for (int i = 0; i < 40; ++i)
    {
        juce::MidiBuffer mm;
        if (i == 0) mm = m;
        inst.processBlock (buf, mm);
        if (i < 4) continue;   // skip attack
        if (note == 60 && std::getenv ("HOSTTEST_DUMP") != nullptr)
            juce::File (juce::String (std::getenv ("HOSTTEST_DUMP"))).appendData (buf.getReadPointer (0), 512 * sizeof (float));
        for (int k = 0; k < 512; ++k)
        {
            const float v = buf.getSample (0, k);
            if (last <= 0.0f && v > 0.0f) ++crossings;
            last = v;
            ++total;
        }
    }
    juce::MidiBuffer off;
    off.addEvent (juce::MidiMessage::allNotesOff (1), 0);
    inst.processBlock (buf, off);
    return crossings * 48000.0 / juce::jmax (1, total);
}

static int playerTest (const juce::String& path)
{
    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> descs;
    vst3.findAllTypesForFile (descs, path);
    if (descs.isEmpty()) { std::cout << "FAIL: plugin not found\n"; return 1; }
    juce::String err;
    auto inst = vst3.createInstanceFromDescription (*descs[0], 48000.0, 512, err);
    if (inst == nullptr) { std::cout << "FAIL: " << err << "\n"; return 1; }
    std::cout << "loaded " << inst->getName() << " (" << descs[0]->category << ")\n";
    inst->setPlayConfigDetails (0, 2, 48000.0, 512);
    inst->prepareToPlay (48000.0, 512);
    juce::MidiBuffer note;
    note.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
    CHECK (render (*inst, 0.3, &note) < 1e-5f, "silent without a sample");

    // state with a looped sine: 32 frames per cycle at c5 = 8363 Hz -> 261.3 Hz on MIDI 60
    juce::XmlElement x ("SpurwerkPlayer");
    x.setAttribute ("name", "01 Testsinus");
    x.setAttribute ("c5", 8363.0);
    x.setAttribute ("loopStart", 0);
    x.setAttribute ("loopLength", 8000);
    x.setAttribute ("volume", 64);
    juce::MemoryOutputStream pcm;
    for (int i = 0; i < 8000; ++i) pcm.writeShort ((short) (20000.0 * std::sin (i * juce::MathConstants<double>::twoPi / 32.0)));
    x.createNewChildElement ("pcm")->addTextElement (pcm.getMemoryBlock().toBase64Encoding());
    juce::MemoryBlock raw, stData;
    juce::AudioProcessor::copyXmlToBinary (x, raw);
    juce::XmlElement host ("VST3PluginState");
    host.createNewChildElement ("IComponent")->addTextElement (raw.toBase64Encoding());
    juce::AudioProcessor::copyXmlToBinary (host, stData);
    inst->setStateInformation (stData.getData(), (int) stData.getSize());

    for (int n : { 24, 60, 96 })
    {
        juce::MidiBuffer mb;
        mb.addEvent (juce::MidiMessage::allNotesOff (1), 0);
        mb.addEvent (juce::MidiMessage::noteOn (1, n, (juce::uint8) 110), 1);
        const float r = render (*inst, 0.3, &mb);
        CHECK (r > 0.01f, "MIDI note " + juce::String (n) + " plays (rms " + juce::String (r, 4) + ")");
    }
    const double expect60 = 8363.0 / 32.0;
    const double hz60 = measureHz (*inst, 60), hz72 = measureHz (*inst, 72), hz48 = measureHz (*inst, 48);
    CHECK (std::abs (hz60 - expect60) < expect60 * 0.02, "MIDI 60 = root (" + juce::String (hz60, 1) + " Hz, expected " + juce::String (expect60, 1) + ")");
    CHECK (std::abs (hz72 / hz60 - 2.0) < 0.04, "MIDI 72 = one octave up (" + juce::String (hz72, 1) + " Hz)");
    CHECK (std::abs (hz60 / hz48 - 2.0) < 0.04, "MIDI 48 = one octave down (" + juce::String (hz48, 1) + " Hz)");
    {
        juce::MidiBuffer bend;
        bend.addEvent (juce::MidiMessage::pitchWheel (1, 16383), 0);
        render (*inst, 0.05, &bend);
        const double hzBend = measureHz (*inst, 60);
        CHECK (std::abs (hzBend / hz60 - 2.0) < 0.06, "pitchbend +max = +12 semitones (" + juce::String (hzBend, 1) + " Hz)");
        juce::MidiBuffer centre;
        centre.addEvent (juce::MidiMessage::pitchWheel (1, 8192), 0);
        render (*inst, 0.05, &centre);
    }
    juce::MidiBuffer pc;
    pc.addEvent (juce::MidiMessage::programChange (1, 5), 0);
    pc.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 1);
    CHECK (render (*inst, 0.3, &pc) > 0.01f, "program change is ignored (still plays)");
    juce::MidiBuffer ch10;
    ch10.addEvent (juce::MidiMessage::allNotesOff (1), 0);
    ch10.addEvent (juce::MidiMessage::noteOn (10, 60, (juce::uint8) 110), 1);
    CHECK (render (*inst, 0.3, &ch10) > 0.01f, "any MIDI channel plays");

    juce::MemoryBlock saved;
    inst->getStateInformation (saved);
    auto inst2 = vst3.createInstanceFromDescription (*descs[0], 48000.0, 512, err);
    inst2->setPlayConfigDetails (0, 2, 48000.0, 512);
    inst2->prepareToPlay (48000.0, 512);
    inst2->setStateInformation (saved.getData(), (int) saved.getSize());
    CHECK (std::abs (measureHz (*inst2, 60) - hz60) < 1.0, "state round trip keeps the sample");
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (inst->createEditorIfNeeded());
        CHECK (ed != nullptr && ed->getWidth() >= 600, "editor opens");
    }
    {
        // sample offset (CC110/111, tracker 9xx): first 4096 frames silent, then a sine
        juce::XmlElement y ("SpurwerkPlayer");
        y.setAttribute ("name", "02 Offsettest");
        y.setAttribute ("c5", 8363.0);
        y.setAttribute ("volume", 64);
        juce::MemoryOutputStream pcm2;
        for (int i = 0; i < 8192; ++i) pcm2.writeShort ((short) (i < 4096 ? 0 : 20000.0 * std::sin (i * juce::MathConstants<double>::twoPi / 32.0)));
        y.createNewChildElement ("pcm")->addTextElement (pcm2.getMemoryBlock().toBase64Encoding());
        juce::MemoryBlock raw2, st2;
        juce::AudioProcessor::copyXmlToBinary (y, raw2);
        juce::XmlElement host2 ("VST3PluginState");
        host2.createNewChildElement ("IComponent")->addTextElement (raw2.toBase64Encoding());
        juce::AudioProcessor::copyXmlToBinary (host2, st2);
        inst2->setStateInformation (st2.getData(), (int) st2.getSize());
        juce::MidiBuffer plain;
        plain.addEvent (juce::MidiMessage::allNotesOff (1), 0);
        plain.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 1);
        const float without = render (*inst2, 0.2, &plain);
        juce::MidiBuffer withOff;
        withOff.addEvent (juce::MidiMessage::allNotesOff (1), 0);
        withOff.addEvent (juce::MidiMessage::controllerEvent (1, 111, 0), 1);
        withOff.addEvent (juce::MidiMessage::controllerEvent (1, 110, 16), 1);   // 16 * 256 = 4096 frames
        withOff.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 2);
        const float with = render (*inst2, 0.2, &withOff);
        CHECK (without < 1e-4f && with > 0.05f, "sample offset CC110/111 (" + juce::String (without, 5) + " -> " + juce::String (with, 4) + ")");
        juce::MidiBuffer panRight;
        panRight.addEvent (juce::MidiMessage::allNotesOff (1), 0);
        panRight.addEvent (juce::MidiMessage::controllerEvent (1, 10, 127), 1);
        panRight.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 2);
        juce::AudioBuffer<float> b2 (2, 512);
        double l = 0, r = 0;
        for (int i = 0; i < 20; ++i)
        {
            juce::MidiBuffer m;
            if (i == 0) m = panRight;
            inst2->processBlock (b2, m);
            l += b2.getRMSLevel (0, 0, 512); r += b2.getRMSLevel (1, 0, 512);
        }
        CHECK (r > 0.01 && l < r * 0.05, "CC10 pan right (L " + juce::String (l, 4) + ", R " + juce::String (r, 4) + ")");
    }
    inst->releaseResources();
    inst2->releaseResources();
    std::cout << (failures == 0 ? "ALL OK\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}

int main (int argc, char** argv)
{
    if (argc < 3) { std::cout << "usage: HostTest <Spurwerk.vst3> <module> | HostTest player <Spurwerk Player.vst3>\n"; return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    if (juce::String (argv[1]) == "player") return playerTest (juce::String::fromUTF8 (argv[2]));
    if (juce::String (argv[1]) == "preset" && argc >= 4)
    {
        // HostTest preset <Spurwerk Player.vst3> <file.vstpreset>: loads the "Comp" chunk like a DAW importing a DAWproject
        juce::MemoryBlock pf;
        juce::File (juce::String::fromUTF8 (argv[3])).loadFileAsData (pf);
        const auto* d = static_cast<const char*> (pf.getData());
        CHECK (pf.getSize() > 48 && std::memcmp (d, "VST3", 4) == 0, "preset header VST3");
        const juce::String cid (d + 8, 32);
        CHECK (cid == "ABCDEF019182FAEB59616B6F5370506C", "class ID " + cid);
        juce::int64 listOff = 0;
        std::memcpy (&listOff, d + 40, 8);
        CHECK (listOff > 48 && listOff + 12 <= (juce::int64) pf.getSize() && std::memcmp (d + listOff, "List", 4) == 0, "chunk list");
        juce::int64 off = 0, size = 0;
        std::memcpy (&off, d + listOff + 12, 8);
        std::memcpy (&size, d + listOff + 20, 8);
        CHECK (std::memcmp (d + listOff + 8, "Comp", 4) == 0 && off + size <= (juce::int64) pf.getSize(), "Comp chunk " + juce::String (size) + " bytes");
        juce::VST3PluginFormat vst3;
        juce::OwnedArray<juce::PluginDescription> descs;
        vst3.findAllTypesForFile (descs, juce::String::fromUTF8 (argv[2]));
        juce::String err;
        auto inst = descs.isEmpty() ? nullptr : vst3.createInstanceFromDescription (*descs[0], 48000.0, 512, err);
        if (inst == nullptr) { std::cout << "FAIL: plugin\n"; return 1; }
        inst->setPlayConfigDetails (0, 2, 48000.0, 512);
        inst->prepareToPlay (48000.0, 512);
        juce::MemoryBlock comp (d + off, (size_t) size), stData;
        juce::XmlElement host ("VST3PluginState");
        host.createNewChildElement ("IComponent")->addTextElement (comp.toBase64Encoding());
        juce::AudioProcessor::copyXmlToBinary (host, stData);
        inst->setStateInformation (stData.getData(), (int) stData.getSize());
        juce::MidiBuffer mb;
        mb.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 0);
        const float r = render (*inst, 0.4, &mb);
        CHECK (r > 0.005f, "preset sample plays on MIDI 60 (rms " + juce::String (r, 4) + ")");
        inst->releaseResources();
        std::cout << (failures == 0 ? "ALL OK\n" : "FAILURES\n");
        return failures == 0 ? 0 : 1;
    }
    juce::VST3PluginFormat vst3;
    juce::OwnedArray<juce::PluginDescription> descs;
    vst3.findAllTypesForFile (descs, juce::String::fromUTF8 (argv[1]));
    if (descs.isEmpty()) { std::cout << "FAIL: plugin not found\n"; return 1; }
    juce::String err;
    auto inst = vst3.createInstanceFromDescription (*descs[0], 48000.0, 512, err);
    if (inst == nullptr) { std::cout << "FAIL: " << err << "\n"; return 1; }
    std::cout << "loaded " << inst->getName() << " (" << descs[0]->category << ")\n";
    FakePlayHead ph;
    inst->setPlayHead (&ph);
    inst->setPlayConfigDetails (0, 2, 48000.0, 512);
    inst->prepareToPlay (48000.0, 512);

    // state with a module (same layout as SpurwerkProcessor::getStateInformation)
    juce::MemoryBlock mod;
    juce::File (juce::String::fromUTF8 (argv[2])).loadFileAsData (mod);
    juce::ValueTree st ("SpurwerkState");
    st.setProperty ("song", mod.toBase64Encoding(), nullptr);
    st.setProperty ("name", juce::File (juce::String::fromUTF8 (argv[2])).getFileName(), nullptr);
    st.setProperty ("sync", 1, nullptr);
    juce::MemoryBlock raw, stData;
    { juce::MemoryOutputStream os (raw, false); st.writeToStream (os); }
    juce::XmlElement host ("VST3PluginState");     // the VST3 host wraps the component state like this
    host.createNewChildElement ("IComponent")->addTextElement (raw.toBase64Encoding());
    juce::AudioProcessor::copyXmlToBinary (host, stData);
    inst->setStateInformation (stData.getData(), (int) stData.getSize());

    CHECK (render (*inst, 0.5) < 1e-5f, "silent while host is stopped");
    ph.playing = true;
    const float playing = render (*inst, 4.0);
    CHECK (playing > 0.01f, "plays when host starts (rms " + juce::String (playing, 4) + ")");
    ph.playing = false;
    render (*inst, 0.1);
    CHECK (render (*inst, 0.5) < 1e-4f, "silent after host stops");

    // sampler mode: MIDI note on channel 1 plays sample 1 (or program change to a used sample)
    juce::MidiBuffer mb;
    mb.addEvent (juce::MidiMessage::programChange (1, 0), 0);
    mb.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 1);
    float best = 0.0f;
    for (int prog = 0; prog < 31 && best < 0.005f; ++prog)
    {
        juce::MidiBuffer m2;
        m2.addEvent (juce::MidiMessage::allNotesOff (1), 0);
        m2.addEvent (juce::MidiMessage::programChange (1, prog), 1);
        m2.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 110), 2);
        best = juce::jmax (best, render (*inst, 0.3, &m2));
    }
    CHECK (best > 0.005f, "sampler mode: MIDI note plays a sample (rms " + juce::String (best, 4) + ")");
    juce::MidiBuffer off;
    off.addEvent (juce::MidiMessage::allNotesOff (1), 0);
    render (*inst, 0.2, &off);

    // state round trip into a second instance
    juce::MemoryBlock saved;
    inst->getStateInformation (saved);
    auto inst2 = vst3.createInstanceFromDescription (*descs[0], 48000.0, 512, err);
    FakePlayHead ph2;
    inst2->setPlayHead (&ph2);
    inst2->setPlayConfigDetails (0, 2, 48000.0, 512);
    inst2->prepareToPlay (48000.0, 512);
    inst2->setStateInformation (saved.getData(), (int) saved.getSize());
    ph2.playing = true;
    const float again = render (*inst2, 4.0);
    CHECK (std::abs (again - playing) < playing * 0.05f + 1e-4f, "state round trip plays the same (" + juce::String (again, 4) + " vs " + juce::String (playing, 4) + ")");
    CHECK (saved.getSize() > mod.getSize() / 2, "state contains the module (" + juce::String ((int) saved.getSize()) + " bytes)");

    // editor opens
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (inst->createEditorIfNeeded());
        CHECK (ed != nullptr && ed->getWidth() > 800, "editor opens");
    }
    inst->releaseResources();
    inst2->releaseResources();
    std::cout << (failures == 0 ? "ALL OK\n" : "FAILURES\n");
    return failures == 0 ? 0 : 1;
}
