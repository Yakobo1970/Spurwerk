// Spurwerk <-> Spurwerk Player: every loaded module publishes its samples (WAV + MIDI of the
// notes each sample plays) into a shared temp folder, so the player can pick them without drag & drop
// (Fender Studio Pro shows only one plugin window at a time).
#pragma once
#include "Song.h"

namespace sw
{
struct SharedSample
{
    int index = 0;                 // 0-based sample number in the module
    juce::String name;             // "05 Kick" (sampleTitle)
    juce::File wav, midi;          // midi may not exist (sample not used in the song)
    double c5Rate = 8363.0;        // playback rate for MIDI note 60
    int loopStart = 0, loopLength = 0, volume = 64, frames = 0;
};

struct SharedModule
{
    juce::File folder;
    juce::String title, formatName, sourcePath;
    juce::Time time;
    std::vector<SharedSample> samples;
};

juce::File shareRoot();                                  // %TEMP%\Spurwerk\share
void publishSongSamples (const Song& song);              // blocking; call from a background thread
void publishSongSamplesAsync (const Song& song);         // copies the song and writes in the background
std::vector<SharedModule> listSharedModules();           // newest first

// Spurwerk Player state (what PlayerProcessor::getStateInformation writes) – also used for DAWproject export
juce::MemoryBlock makePlayerState (const Sample& s, const juce::String& name, const juce::String& source,
                                   const juce::String& shareFolder, int shareIndex, float gain, const juce::MemoryBlock& midi);
constexpr const char* kPlayerVst3Cid = "ABCDEF019182FAEB59616B6F5370506C";   // VST3 class ID of "Spurwerk Player" (Yako/SpPl)
} // namespace sw
