#include "PlayerProcessor.h"
#include "PlayerEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>

using namespace sw;

PlayerProcessor::PlayerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    addParameter (gain = new juce::AudioParameterFloat (juce::ParameterID ("gain", 1), U ("Lautstärke"), 0.0f, 2.0f, 1.0f));
    song.samples.resize (1);
    for (auto& p : sampler.program) p = 0;   // every MIDI channel plays the one sample
    sampler.setSong (&song);
}

bool PlayerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void PlayerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const juce::ScopedLock sl (lock);
    sampler.prepare (sampleRate);
    scratch.setSize (2, juce::jmax (512, samplesPerBlock));
}

void PlayerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();
    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked()) { midi.clear(); return; }
    if (scratch.getNumSamples() < n) scratch.setSize (2, n, false, false, true);
    scratch.clear();
    // sample-accurate enough: split the block at MIDI events
    int pos = 0;
    const float g = gain->get() * 1.6f;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, n, meta.samplePosition);
        if (at > pos) { sampler.render (scratch.getWritePointer (0, pos), scratch.getWritePointer (1, pos), at - pos, g); pos = at; }
        const auto m = meta.getMessage();
        if (! m.isProgramChange()) sampler.handle (m);
    }
    if (pos < n) sampler.render (scratch.getWritePointer (0, pos), scratch.getWritePointer (1, pos), n - pos, g);
    midi.clear();
    if (buffer.getNumChannels() >= 2)
    {
        buffer.copyFrom (0, 0, scratch, 0, 0, n);
        buffer.copyFrom (1, 0, scratch, 1, 0, n);
    }
    else if (buffer.getNumChannels() == 1)
    {
        buffer.copyFrom (0, 0, scratch, 0, 0, n);
        buffer.applyGain (0, 0, n, 0.5f);
        buffer.addFrom (0, 0, scratch, 1, 0, n, 0.5f);
    }
}

void PlayerProcessor::setSample (Sample s)
{
    const juce::ScopedLock sl (lock);
    song.samples.assign (1, std::move (s));
    sampler.setSong (&song);
    ++ver;
}

void PlayerProcessor::preview (bool on)
{
    const juce::ScopedLock sl (lock);
    sampler.preview (1, 60, on);
}

bool PlayerProcessor::loadAudioFile (const juce::File& f, juce::String& error)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr) { error = U ("Format nicht lesbar: ") + f.getFileName(); return false; }
    const int len = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 600.0));
    juce::AudioBuffer<float> buf ((int) reader->numChannels, juce::jmax (1, len));
    reader->read (&buf, 0, len, 0, true, true);
    Sample s;
    s.pcm.resize ((size_t) len);
    for (int i = 0; i < len; ++i)
    {
        float v = 0.0f;
        for (int c = 0; c < buf.getNumChannels(); ++c) v += buf.getSample (c, i);
        s.pcm[(size_t) i] = v / (float) juce::jmax (1, buf.getNumChannels());
    }
    // root key and loop from the WAV "smpl" chunk (Spurwerk writes root C-5 = MIDI 60)
    const auto& md = reader->metadataValues;
    const int unity = md.getValue ("MidiUnityNote", "60").getIntValue();
    s.c5Speed = reader->sampleRate * std::pow (2.0, (60 - juce::jlimit (0, 127, unity)) / 12.0);
    if (md.getValue ("NumSampleLoops", "0").getIntValue() > 0)
    {
        const int a = md.getValue ("Loop0Start", "0").getIntValue();
        const int b = md.getValue ("Loop0End", "0").getIntValue();
        if (b > a && a >= 0 && b < len) { s.loopStart = a; s.loopLength = b - a + 1; }
    }
    s.volume = 64;
    s.name = f.getFileNameWithoutExtension();
    setSample (std::move (s));
    sampleName = f.getFileNameWithoutExtension();
    sourceTitle = f.getFileName();
    shareFolder.clear();
    shareIndex = -1;
    midiData.reset();
    ++ver;
    return true;
}

bool PlayerProcessor::loadShared (const SharedModule& m, const SharedSample& sh, juce::String& error)
{
    if (! loadAudioFile (sh.wav, error)) return false;
    {
        const juce::ScopedLock sl (lock);
        auto& s = song.samples[0];
        s.c5Speed = sh.c5Rate;   // exact rate (the WAV header holds it rounded)
        const int n = (int) s.pcm.size();
        if (sh.loopLength > 2 && sh.loopStart + sh.loopLength <= n) { s.loopStart = sh.loopStart; s.loopLength = sh.loopLength; }
        s.volume = juce::jlimit (0, 64, sh.volume);
        s.name = sh.name;
    }
    sampleName = sh.name;
    sourceTitle = m.title;
    shareFolder = m.folder.getFileName();
    shareIndex = sh.index;
    midiData.reset();
    if (sh.midi.existsAsFile()) sh.midi.loadFileAsData (midiData);
    ++ver;
    return true;
}

void PlayerProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    const juce::ScopedLock sl (lock);
    dest = makePlayerState (song.samples.empty() ? Sample() : song.samples[0], sampleName, sourceTitle, shareFolder, shareIndex, gain->get(), midiData);
}

void PlayerProcessor::setStateInformation (const void* data, int size)
{
    auto x = getXmlFromBinary (data, size);
    if (x == nullptr || ! x->hasTagName ("SpurwerkPlayer")) return;
    *gain = (float) x->getDoubleAttribute ("gain", 1.0);
    sampleName = x->getStringAttribute ("name");
    sourceTitle = x->getStringAttribute ("source");
    shareFolder = x->getStringAttribute ("shareFolder");
    shareIndex = x->getIntAttribute ("shareIndex", -1);
    Sample s;
    if (auto* p = x->getChildByName ("pcm"))
    {
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (p->getAllSubText()))
        {
            juce::MemoryInputStream in (mb, false);
            s.pcm.resize (mb.getSize() / 2);
            for (auto& v : s.pcm) v = (float) in.readShort() / 32767.0f;
        }
    }
    s.c5Speed = x->getDoubleAttribute ("c5", 8363.0);
    s.loopStart = x->getIntAttribute ("loopStart");
    s.loopLength = x->getIntAttribute ("loopLength");
    s.volume = x->getIntAttribute ("volume", 64);
    s.name = sampleName;
    setSample (std::move (s));
    midiData.reset();
    if (auto* m = x->getChildByName ("midi")) midiData.fromBase64Encoding (m->getAllSubText());
    ++ver;
}

#ifndef SW_NO_PLUGIN_FACTORY
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PlayerProcessor(); }
#endif

juce::AudioProcessorEditor* PlayerProcessor::createEditor() { return new PlayerEditor (*this); }
