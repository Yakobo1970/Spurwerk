// Spurwerk – loaders for FastTracker 2 (.xm), Scream Tracker 3 (.s3m), Impulse Tracker (.it), MultiTracker (.mtm), Composer 669 (.669), Farandole (.far), Digitrakker (.mdl) and Oktalyzer (.okt)
#pragma once
#include "Song.h"

namespace sw
{
bool isXm (const juce::MemoryBlock& d);
bool isS3m (const juce::MemoryBlock& d);
bool isOkt (const juce::MemoryBlock& d);
bool isIt (const juce::MemoryBlock& d);
bool isMtm (const juce::MemoryBlock& d);
std::unique_ptr<Song> loadXm (const juce::MemoryBlock& d, juce::String& error);
std::unique_ptr<Song> loadS3m (const juce::MemoryBlock& d, juce::String& error);
std::unique_ptr<Song> loadOkt (const juce::MemoryBlock& d, juce::String& error);
std::unique_ptr<Song> loadIt (const juce::MemoryBlock& d, juce::String& error);
std::unique_ptr<Song> loadMtm (const juce::MemoryBlock& d, juce::String& error);
bool is669 (const juce::MemoryBlock& d);
bool isFar (const juce::MemoryBlock& d);
bool isMdl (const juce::MemoryBlock& d);
bool isMed (const juce::MemoryBlock& d);
std::unique_ptr<Song> load669 (const juce::MemoryBlock& d, juce::String& error);
std::unique_ptr<Song> loadFar (const juce::MemoryBlock& d, juce::String& error);
std::unique_ptr<Song> loadMdl (const juce::MemoryBlock& d, juce::String& error);
std::unique_ptr<Song> loadMed (const juce::MemoryBlock& d, juce::String& error);   // MedFormat.cpp
bool isTcb (const juce::MemoryBlock& d);   // AmigaFormats.cpp
bool isFc (const juce::MemoryBlock& d);
std::unique_ptr<Song> loadTcb (const juce::MemoryBlock& d, juce::String& error);
std::unique_ptr<Song> loadFc (const juce::MemoryBlock& d, juce::String& error);
} // namespace sw
