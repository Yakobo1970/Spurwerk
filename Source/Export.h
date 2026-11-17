#pragma once
#include "Song.h"
#include <juce_audio_basics/juce_audio_basics.h>

namespace sw
{
struct MidiExportOptions
{
    bool perInstrument = true;   // false: one track per tracker channel (program changes), true: one track/channel per instrument
    int bendRange = 12;           // semitones (RPN 0 is written)
    double maxMinutes = 20.0;
    int onlySample = -1;
    bool perSample = false;       // one track per sample, track name = sampleTitle()
    std::vector<int>* trackGroups = nullptr;   // out: sample/instrument/channel of every exported track (MIDI track i+1)          // >= 0: only notes that play this sample (0-based), one track
};

constexpr int kExportPpq = 960;

juce::MidiFile exportMidi (const Song& song, const MidiExportOptions& opt, juce::String* summary = nullptr);
bool writeMidiFile (const juce::MidiFile& mf, const juce::File& file);

// sample export: one WAV (with smpl loop chunk + root key) per used sample, SFZ per sample,
// a kit SFZ and a DecentSampler preset (sample i on key 36+i). Returns the written WAV files.
juce::Array<juce::File> exportSamples (const Song& song, const juce::File& folder, juce::String& log);
bool writeSampleWav (const Sample& s, const juce::File& file);
juce::String safeFileName (const juce::String& s);
// "05 Kick" – sample number + sample name, or + module title when the sample has no name
juce::String sampleTitle (const Song& song, int index);
// all samples as WAV (file name = sampleTitle) + one MIDI file with one track per sample (track name = sampleTitle)
bool exportSamplesWavMidi (const Song& song, const juce::File& folder, juce::String& log);

// renders the whole song (one pass) to a stereo WAV
bool renderSongWav (const Song& song, const juce::File& file, double sampleRate, double maxMinutes, juce::String& log);
} // namespace sw
