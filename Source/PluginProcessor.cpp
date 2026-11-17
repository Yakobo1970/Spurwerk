#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ModFile.h"
#include "SampleShare.h"

using namespace sw;

SpurwerkProcessor::SpurwerkProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    current = newEmptySong (4);
    bindEngines();
}

bool SpurwerkProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void SpurwerkProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const juce::ScopedLock sl (lock);
    sr = sampleRate;
    mod.prepare (sr);
    ym.prepare (sr);
    sampler.prepare (sr);
    scratch.setSize (2, juce::jmax (512, samplesPerBlock));
}

void SpurwerkProcessor::bindEngines()
{
    // caller holds the lock (or no audio is running yet)
    mod.setSong (current.get());
    ym.setData (current->format == Format::YM ? &current->ym : nullptr);
    sampler.setSong (current.get());
    mod.stopped = true;
    playing = Play::Stop;
}

void SpurwerkProcessor::songStructureChanged()
{
    const juce::ScopedLock sl (lock);
    const auto state = playing.load();
    const int o = mod.order, r = mod.row;
    mod.setSong (current.get());
    sampler.setSong (current.get());
    if (state != Play::Stop && current->format != Format::YM)
    {
        mod.setLoopPattern (state == Play::Pattern);
        mod.reset (juce::jlimit (0, juce::jmax (0, (int) current->orders.size() - 1), o), r);
    }
    else mod.stopped = true;
    touch();
}

bool SpurwerkProcessor::loadData (const juce::MemoryBlock& data, const juce::String& name, juce::String& error, int subSong)
{
    auto s = loadSongData (data, name, error, subSong);
    if (s == nullptr) return false;
    {
        const juce::ScopedLock sl (lock);
        current = std::move (s);
        originalFile = current->format != Format::MOD ? data : juce::MemoryBlock();
        bindEngines();
    }
    modified = false;
    ++version;
    return true;
}

bool SpurwerkProcessor::loadFile (const juce::File& f, juce::String& error)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb)) { error = "Datei kann nicht gelesen werden"; return false; }
    if (! loadData (mb, f.getFileName(), error)) return false;
    current->sourcePath = f.getFullPathName();
    uiState.setProperty ("folder", f.getParentDirectory().getFullPathName(), nullptr);
    shareSamples();
    return true;
}

bool SpurwerkProcessor::selectSubSong (int subSong, juce::String& error)
{
    const auto data = originalFile;
    const auto path = current->sourcePath;
    const int sep = current->stereoSeparation;
    if (data.getSize() == 0 || ! loadData (data, juce::File (path).getFileName(), error, subSong)) return false;
    current->sourcePath = path;
    current->stereoSeparation = sep;
    shareSamples();
    return true;
}

void SpurwerkProcessor::newSong (int channels)
{
    {
        const juce::ScopedLock sl (lock);
        current = newEmptySong (channels);
        originalFile.reset();
        bindEngines();
    }
    modified = false;
    ++version;
}

bool SpurwerkProcessor::saveModFile (const juce::File& f, juce::String& error)
{
    juce::MemoryBlock mb;
    {
        const juce::ScopedLock sl (lock);
        if (! saveMod (*current, mb, error)) return false;
    }
    f.deleteFile();
    if (! f.replaceWithData (mb.getData(), mb.getSize())) { error = "Datei kann nicht geschrieben werden"; return false; }
    current->sourcePath = f.getFullPathName();
    shareSamples();
    modified = false;
    ++version;
    return true;
}

void SpurwerkProcessor::shareSamples()
{
    const juce::ScopedLock sl (lock);
    publishSongSamplesAsync (*current);
}

void SpurwerkProcessor::play (Play mode, int order, int row)
{
    const juce::ScopedLock sl (lock);
    if (current->orders.empty()) return;
    order = juce::jlimit (0, (int) current->orders.size() - 1, order);
    if (current->format == Format::YM)
    {
        int frame = 0;
        for (int i = 0; i < order; ++i)
        {
            const int p = current->orders[(size_t) i];
            frame += (p >= 0 && p < (int) current->patterns.size()) ? current->patterns[(size_t) p].rows : 64;
        }
        ym.reset (frame + row);
    }
    else
    {
        mod.interpolate = interpolate.load();
        mod.setLoopPattern (mode == Play::Pattern);
        mod.reset (order, row);
    }
    pSeconds = 0.0;
    playing = mode;
}

void SpurwerkProcessor::stop()
{
    const juce::ScopedLock sl (lock);
    playing = Play::Stop;
    mod.stopped = true;
    for (auto& l : levels) l = 0.0f;
}

void SpurwerkProcessor::previewNote (int instr, int note, bool on)
{
    const juce::ScopedLock sl (lock);
    sampler.preview (instr, note, on);
}

void SpurwerkProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    buffer.clear();
    const juce::ScopedTryLock sl (lock);
    if (! sl.isLocked()) return;

    // host transport sync
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            const bool hostPlaying = pos->getIsPlaying();
            const int mode = syncMode.load();
            if (mode > 0 && hostPlaying != hostWasPlaying)
            {
                if (hostPlaying) play (Play::Song, cursorOrder.load(), 0);
                else { playing = Play::Stop; mod.stopped = true; }
            }
            if (mode == 2 && hostPlaying && current->format != Format::YM)
                if (auto bpm = pos->getBpm())
                    mod.tempo = current->tempoMode == 2 ? juce::jlimit (32, 2000, juce::roundToInt (*bpm * current->rowsPerBeat / 4.0))
                                                        : juce::jlimit (32, 255, juce::roundToInt (*bpm * mod.speed / 6.0));
            hostWasPlaying = hostPlaying;
        }
    }

    if (scratch.getNumSamples() < n) scratch.setSize (2, n, false, false, true);
    scratch.clear();
    float* L = scratch.getWritePointer (0);
    float* R = scratch.getWritePointer (1);
    const float gain = masterGain.load();
    const auto mute = muteMask.load();
    const auto state = playing.load();
    if (state != Play::Stop)
    {
        if (current->format == Format::YM)
        {
            ym.stereoSeparation = current->stereoSeparation;
            ym.render (L, R, n, mute, gain);
            const int f = juce::jmax (0, ym.frame - 1);
            pOrder = f / 64; pRow = f % 64;
            pSpeed = 1; pTempo = current->ym.rate;
            for (int c = 0; c < 3; ++c) levels[(size_t) c] = ym.level[c];
        }
        else
        {
            mod.interpolate = interpolate.load();
            mod.stereoSeparation = current->stereoSeparation;
            mod.render (L, R, n, mute, gain);
            pOrder = mod.order; pRow = mod.row; pSpeed = mod.speed; pTempo = mod.tempo;
            for (int c = 0; c < current->channels; ++c) levels[(size_t) c] = mod.ch[(size_t) c].level;
        }
        pSeconds = pSeconds.load() + n / sr;
    }

    // sampler mode: MIDI input plays the samples
    for (const auto meta : midi)
        if (samplerOn.load()) sampler.handle (meta.getMessage());
    midi.clear();
    sampler.render (L, R, n, gain);

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

void SpurwerkProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::ValueTree st ("SpurwerkState");
    st.setProperty ("version", 1, nullptr);
    {
        const juce::ScopedLock sl (lock);
        juce::MemoryBlock data;
        juce::String err;
        if (current->format != Format::MOD) data = originalFile;
        else saveMod (*current, data, err);
        st.setProperty ("song", data.toBase64Encoding(), nullptr);
        st.setProperty ("name", current->sourcePath.isNotEmpty() ? juce::File (current->sourcePath).getFileName() : juce::String ("song.mod"), nullptr);
        st.setProperty ("path", current->sourcePath, nullptr);
        st.setProperty ("separation", current->stereoSeparation, nullptr);
        st.setProperty ("subsong", current->subSong, nullptr);
    }
    st.setProperty ("mute", (int) muteMask.load(), nullptr);
    st.setProperty ("gain", masterGain.load(), nullptr);
    st.setProperty ("interp", interpolate.load(), nullptr);
    st.setProperty ("sampler", samplerOn.load(), nullptr);
    st.setProperty ("sync", syncMode.load(), nullptr);
    st.addChild (uiState.createCopy(), -1, nullptr);
    juce::MemoryOutputStream os (dest, false);
    st.writeToStream (os);
}

void SpurwerkProcessor::setStateInformation (const void* data, int size)
{
    auto st = juce::ValueTree::readFromData (data, (size_t) size);
    if (! st.hasType ("SpurwerkState")) return;
    juce::MemoryBlock mb;
    if (mb.fromBase64Encoding (st.getProperty ("song").toString()) && mb.getSize() > 0)
    {
        juce::String err;
        if (loadData (mb, st.getProperty ("name").toString(), err, (int) st.getProperty ("subsong", 0)))
        {
            current->sourcePath = st.getProperty ("path").toString();
            current->stereoSeparation = st.getProperty ("separation", 60);
            shareSamples();
        }
    }
    muteMask = (juce::uint32) (int) st.getProperty ("mute", 0);
    masterGain = (float) st.getProperty ("gain", 0.8f);
    interpolate = (bool) st.getProperty ("interp", true);
    samplerOn = (bool) st.getProperty ("sampler", true);
    syncMode = (int) st.getProperty ("sync", 1);
    auto ui = st.getChildWithName ("UI");
    if (ui.isValid()) uiState.copyPropertiesAndChildrenFrom (ui, nullptr);
    ++version;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SpurwerkProcessor(); }

juce::AudioProcessorEditor* SpurwerkProcessor::createEditor() { return new SpurwerkEditor (*this); }
