// Spurwerk – saving any loaded song as ProTracker MOD or OctaMED (MMD1).
// Tracker formats are converted cell by cell (notes, samples/instruments, effects mapped to the target's commands);
// YM/COSO and Future Composer (no pattern cells that a sample tracker could play) are written from the notes as played.
#pragma once
#include "Song.h"

namespace sw
{
enum class SaveTarget { MOD, MED };

// converted copy ready for the writer; `notes` collects what could not be taken over
std::unique_ptr<Song> convertSong (const Song& src, SaveTarget target, juce::StringArray& notes);
bool saveMed (const Song& song, juce::MemoryBlock& out, juce::String& error);   // OctaMED MMD1
// convert + write; report = short German summary (incl. losses)
bool saveSongAs (const Song& src, SaveTarget target, const juce::File& file, juce::String& report);
juce::String saveTargetName (SaveTarget t);
} // namespace sw
