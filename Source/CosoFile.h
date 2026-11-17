#pragma once
#include "Song.h"

namespace sw
{
// Jochen Hippel COSO (Atari ST): rendered to YM2149 register frames, shown/played like a YM song.
bool isCoso (const juce::MemoryBlock& data);
std::unique_ptr<Song> loadCoso (const juce::MemoryBlock& data, int subSong, juce::String& error);
} // namespace sw
