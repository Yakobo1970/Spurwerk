// Spurwerk Player – a tiny sampler: one sample over all 128 keys (root C-5 = MIDI 60), loop as in the tracker.
// Samples come from a running Spurwerk (shared folder, no drag & drop needed) or from a WAV/AIFF/FLAC file.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "../Source/Song.h"
#include "../Source/Engine.h"
#include "../Source/SampleShare.h"

class PlayerProcessor : public juce::AudioProcessor
{
public:
    PlayerProcessor();
    ~PlayerProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Spurwerk Player"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- the sample (UI thread)
    bool loadAudioFile (const juce::File& f, juce::String& error);
    bool loadShared (const sw::SharedModule& m, const sw::SharedSample& s, juce::String& error);
    const sw::Sample* sample() const { return song.samples.empty() || song.samples[0].pcm.empty() ? nullptr : &song.samples[0]; }
    juce::String sampleName, sourceTitle;   // "05 Kick", module title or file name
    juce::String shareFolder;               // folder name in the share root ("" = loaded from a file)
    int shareIndex = -1;
    juce::MemoryBlock midiData;             // notes of this sample (from Spurwerk), may be empty
    juce::uint32 version() const { return ver.load(); }
    void preview (bool on);

    juce::AudioParameterFloat* gain = nullptr;
    juce::CriticalSection lock;

private:
    sw::Song song;
    sw::Sampler sampler;
    std::atomic<juce::uint32> ver { 1 };
    juce::AudioBuffer<float> scratch;
    void setSample (sw::Sample s);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PlayerProcessor)
};
