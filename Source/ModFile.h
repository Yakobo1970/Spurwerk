#pragma once
#include "Song.h"

namespace sw
{
// Loads any supported file (MOD / YM). Returns nullptr and fills `error` on failure.
std::unique_ptr<Song> loadSongFile (const juce::File& file, juce::String& error);
std::unique_ptr<Song> loadSongData (const juce::MemoryBlock& data, const juce::String& fileName, juce::String& error, int subSong = 0);

std::unique_ptr<Song> loadMod (const juce::MemoryBlock& data, juce::String& error);
bool saveMod (const Song& song, juce::MemoryBlock& out, juce::String& error);

juce::String supportedWildcards();   // "*.mod;*.ym;..."
bool isSupportedFile (const juce::File& f);
// true if the data carries the signature of a supported format (or is packed); false = only the file name looked like a module
bool hasModuleSignature (const juce::MemoryBlock& data);
// name of a known but unsupported music/packer format ("Future Composer 1.4" ...), empty if unknown
juce::String unsupportedFormatName (const juce::MemoryBlock& data);
} // namespace sw
