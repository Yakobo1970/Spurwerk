// DAWproject export (.dawproject, the open project format of Bitwig and PreSonus/Fender Studio Pro).
// One instrument track per sample with the "Spurwerk Player" VST3 already loaded (sample embedded in its
// plugin state), the notes of that sample, pitch bend + CC11 automation and the tempo. The WAVs are added too.
#pragma once
#include "Song.h"

namespace sw
{
bool exportDawProject (const Song& song, const juce::File& out, juce::String& log);

// YM/COSO helpers (also used by the Ableton Live Set export)
Sample ymSquare();                 // looped square, MIDI 60 = middle C
Sample ymNoise();                  // looped 17-bit LFSR noise
double renderYmVoice (const Song& song, int voice, const juce::File& file, double sr);   // one voice as stereo WAV, returns seconds

// Ableton Live Set (.als), built from the templates of the installed Live (DefaultLiveSet, Simpler, demo clips):
// one MIDI track with Simpler (sample loaded) per sample, notes + pitch bend in the arrangement, tempo; YM/COSO: rendered
// voices as audio tracks plus muted note tracks. WAVs go to "<name> Samples" next to the set.
bool exportAbletonLiveSet (const Song& song, const juce::File& out, juce::String& log);
juce::File findAbletonResources();   // newest "C:\ProgramData\Ableton\Live 1x ...\Resources", or {} if Live is missing
} // namespace sw
