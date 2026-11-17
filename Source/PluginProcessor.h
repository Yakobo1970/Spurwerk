#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Song.h"
#include "Engine.h"
#include "ModArchive.h"

class SpurwerkProcessor : public juce::AudioProcessor
{
public:
    SpurwerkProcessor();
    ~SpurwerkProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Spurwerk"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- song access (UI thread). Hold `lock` while changing song data.
    juce::CriticalSection lock;
    sw::Song& song() { return *current; }
    const sw::Song& song() const { return *current; }
    bool loadFile (const juce::File& f, juce::String& error);
    bool loadData (const juce::MemoryBlock& data, const juce::String& name, juce::String& error, int subSong = 0);
    bool selectSubSong (int subSong, juce::String& error);   // COSO: re-render another sub song
    void newSong (int channels = 4);
    bool saveModFile (const juce::File& f, juce::String& error);
    void songStructureChanged();
    void shareSamples();           // publish samples + MIDI for the Spurwerk Player (background thread)   // after channels/patterns/samples changed (engines re-bound)
    juce::uint32 songVersion() const { return version.load(); }
    void touch() { ++version; modified = true; }
    bool modified = false;

    // ---- transport
    enum class Play { Stop, Song, Pattern };
    void play (Play mode, int order, int row);
    void stop();
    Play playState() const { return playing.load(); }
    int posOrder() const { return pOrder.load(); }
    int posRow() const { return pRow.load(); }
    int posSpeed() const { return pSpeed.load(); }
    int posTempo() const { return pTempo.load(); }
    double playSeconds() const { return pSeconds.load(); }
    float channelLevel (int ch) const { return ch >= 0 && ch < sw::kMaxChannels ? levels[(size_t) ch].load() : 0.0f; }
    void previewNote (int instr, int note, bool on);

    std::atomic<juce::uint32> muteMask { 0 };
    std::atomic<int> cursorOrder { 0 };
    std::atomic<float> masterGain { 0.8f };
    std::atomic<bool> interpolate { true };
    std::atomic<bool> samplerOn { true };   // MIDI input plays the module's samples
    std::atomic<int> syncMode { 1 };        // 0 = aus, 1 = Start/Stopp mit DAW, 2 = Start/Stopp + Tempo

    juce::SharedResourcePointer<sw::archive::Archive> archive;   // keeps a running folder scan alive while the editor is closed
    juce::ValueTree uiState { "UI" };       // theme, light, loupe, keyboard layout, folder ...
    juce::MemoryBlock originalFile;         // raw file for read-only formats (YM, COSO, XM, S3M, IT, MTM, OKT) to restore the state

private:
    std::unique_ptr<sw::Song> current;
    sw::ModEngine mod;
    sw::YmEngine ym;
    sw::Sampler sampler;
    double sr = 48000.0;
    std::atomic<Play> playing { Play::Stop };
    std::atomic<int> pOrder { 0 }, pRow { 0 }, pSpeed { 6 }, pTempo { 125 };
    std::atomic<double> pSeconds { 0.0 };
    std::array<std::atomic<float>, sw::kMaxChannels> levels {};
    std::atomic<juce::uint32> version { 1 };
    bool hostWasPlaying = false;
    juce::AudioBuffer<float> scratch;
    void bindEngines();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpurwerkProcessor)
};
