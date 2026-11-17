#pragma once
#include "Song.h"

namespace sw
{
// LHA -lh5- (also -lh4-/-lh6-/-lh7-, -lh0-) depacker for packed YM files. Returns false on error.
bool depackLha (const juce::MemoryBlock& in, juce::MemoryBlock& out, juce::String& error);

// removes CR before LF when that makes the size match `expected` (text-mode transfer damage)
bool repairCrLf (juce::MemoryBlock& data, size_t expected);

std::unique_ptr<Song> loadYm (const juce::MemoryBlock& data, juce::String& error);

// YM tone period -> note (0 if silent/invalid) and exact semitone offset
double ymPeriodToMidi (int period, int clock);

// YM channel state of one frame (used by pattern conversion and MIDI export)
struct YmVoice
{
    bool audible = false, tone = false, noise = false, env = false;
    int period = 0, volume = 0;   // volume 0..15
    double midi = 0.0;            // exact pitch (MIDI note number)
};
YmVoice ymVoice (const YmData& ym, int frame, int ch);

// effect column codes used in converted YM patterns
enum YmFx { ymFxNone = 0, ymFxNoise = 1, ymFxEnv = 2, ymFxSpecial = 3 };
} // namespace sw
