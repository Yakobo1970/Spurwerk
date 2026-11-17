// Spurwerk – module archive: scanned folders, analysed entries (notes, sample names, profile), duplicate groups.
// One instance per process (juce::SharedResourcePointer), stored in %APPDATA%\Spurwerk\archiv.swdb (env SPURWERK_DB overrides).
// The archive never writes into module files: ratings, favourites and comments live in the database only.
#pragma once
#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Song.h"
#include <unordered_map>
#include <set>
#include <deque>
#include <thread>
#include <atomic>

namespace sw::archive
{
constexpr int kSketch = 64;      // bottom-k sketch of melodic n-grams
constexpr int kBins = 32;        // time bins of the note profile ("Verlauf")
constexpr int kLanes = 6;        // stacked lanes (largest samples first, rest in the last)

enum Flags : juce::uint32 { fMissing = 1, fError = 2, fNotModule = 4, fExcluded = 8 };   // fExcluded: put into the pool by the user   // fNotModule: no known format signature (only the file name looks like a module)
enum DupKind : juce::uint8 { dupNone = 0, dupSimilar = 1, dupSameNotes = 2, dupIdentical = 3 };

struct Note { float start = 0, len = 0; juce::uint8 pitch = 0; short track = 0; };

// result of analysing one module file
struct Analysis
{
    bool ok = false, notModule = false;
    juce::String error, title, artist, format, formatName, message, sampleText;
    int channels = 0, patterns = 0, orders = 0, samples = 0, instruments = 0, notes = 0;
    float seconds = 0;
    juce::uint64 fileHash = 0, noteHash = 0;
    std::vector<juce::uint32> sketch;
    int gramCount = 0;
    std::array<juce::uint8, kBins * kLanes> profile {};
    juce::String key, mode, scale;   // "a-Moll", "Moll", "Diatonisch · 7" (empty: no tonal notes)
    float keyConfidence = 0;
    std::vector<Note> noteList;      // only with keepNotes (detail view)
    std::vector<juce::String> trackNames;
};
Analysis analyseModule (const juce::File& f, bool keepNotes);
float sketchSimilarity (const std::vector<juce::uint32>& a, const std::vector<juce::uint32>& b);
juce::String shortFormat (const Song& s);

struct Entry
{
    int id = 0;
    juce::String path, name, folder;
    juce::int64 size = 0, mtime = 0;
    juce::uint32 flags = 0;
    juce::String title, artist, format, formatName, message, sampleText, comment, error;
    juce::String composer, arranger;   // entered by the user (database only)
    juce::uint32 edited = 0;           // 1 = title, 2 = artist set by the user: kept when the file is analysed again
    int channels = 0, patterns = 0, orders = 0, samples = 0, instruments = 0, notes = 0, rating = 0;
    bool favourite = false;
    float seconds = 0;
    juce::uint64 fileHash = 0, noteHash = 0;
    std::vector<juce::uint32> sketch;
    int gramCount = 0;
    std::array<juce::uint8, kBins * kLanes> profile {};
    juce::String key, mode, scale;
    float keyConfidence = 0;

    // duplicate state (runtime)
    int group = 0, groupSize = 0, bestMatch = 0;
    float bestScore = 0;
    juce::uint8 dupKind = dupNone;

    bool has (Flags f) const { return (flags & f) != 0; }
    // pool of excluded and faulty files: hidden in the normal views, not used for duplicates, skipped by scans while unchanged
    bool inPool() const { return (flags & (fMissing | fError | fNotModule | fExcluded)) != 0; }
    void fill (const Analysis& a);
};

struct DupLink { int other; float score; juce::uint8 kind; };
struct DupIndex;

class Archive : public juce::ChangeBroadcaster, private juce::Timer
{
public:
    Archive();
    ~Archive() override;

    const std::vector<std::unique_ptr<Entry>>& entries() const { return list; }
    Entry* byId (int id) const;
    Entry* byPath (const juce::String& path) const;
    juce::uint32 version() const { return changeCounter; }
    juce::StringArray folders;

    juce::String addFolder (const juce::File& dir);    // returns the archive folder that now covers dir
    int removeFolder (const juce::String& path);       // stops a scan, removes the folder and its entries
    int countInFolder (const juce::String& path) const;
    int removeMissing();

    // scanning (background threads; results arrive on the message thread)
    void startScan (bool fullRescan = false, const juce::StringArray& only = {});
    void stopScan();
    bool isScanning() const { return scanner != nullptr; }
    juce::String scanStatus() const;
    void pump();
    void waitForScan();             // tools: blocks and pumps until finished

    // edits (database only)
    // undo of database edits (title, artist, composer, arranger, rating, favourite, note, pool); several changes
    // inside one UndoGroup are undone together. Files on disk are never part of it (deleting cannot be undone here).
    struct UndoGroup
    {
        UndoGroup (Archive& a, const juce::String& label) : arch (a) { if (arch.undoDepth++ == 0) arch.openUndo (label); }
        ~UndoGroup() { if (--arch.undoDepth == 0) arch.closeUndo(); }
        Archive& arch;
    };
    bool canUndo() const { return ! undoStack.empty(); }
    juce::String undoLabel() const { return undoStack.empty() ? juce::String() : undoStack.back().label; }
    juce::String undo();   // restores the last step, returns its label (empty: nothing to undo)
    void setRating (Entry& e, int rating);
    void setFavourite (Entry& e, bool fav);
    void setComment (Entry& e, const juce::String& text);
    enum class Field { title, artist, composer, arranger };
    void setField (Entry& e, Field f, const juce::String& value);   // database only, never written into the module
    void removeEntries (const std::vector<int>& ids);
    // takes entries out of the database and remembers the files (path, size, date), so a later scan skips them
    // as long as they are unchanged – for files that only carry a module extension
    // write protection per folder (like Midi-Fundus): Spurwerk never writes, overwrites or deletes files there;
    // database-only data (ratings, titles, notes) stays editable
    juce::StringArray protectedFolders() const;
    void setProtected (const juce::String& folder, bool on);
    bool isProtected (const juce::String& path) const;
    void setExcluded (const std::vector<int>& ids, bool excluded);   // into / out of the pool (database only)
    int countPool() const;
    void markNotDuplicate (int idA, int idB);

    // duplicates
    std::vector<int> groupMembers (int group) const;
    std::vector<DupLink> linksOf (int id) const;
    float similarityThreshold() const { return threshold; }
    void setSimilarityThreshold (float t);
    int duplicateGroups() const;
    bool indexing() const { return index == nullptr; }
    void waitForIndex();

    // persistence
    juce::File databaseFile() const;
    void save();
    void saveSoon() { dirty = true; }
    bool load();
    juce::ValueTree view { "VIEW" };    // table layout, sort, filter (stored with the database)

private:
    struct Scanner;
    std::vector<std::unique_ptr<Entry>> list;
    std::unordered_map<juce::String, Entry*> pathIndex;
    std::unordered_map<int, Entry*> idIndex;
    std::unordered_map<juce::String, std::pair<juce::int64, juce::int64>> ignored;   // 0.8.x: removed files; turned into pool entries on load
    int nextId = 1;
    juce::uint32 changeCounter = 1;
    bool dirty = false;
    int saveCountdown = 0;
    float threshold = 0.45f;
    bool upgradeScan = false;
    struct UndoItem { int id = 0; juce::String title, artist, composer, arranger, comment; juce::uint32 edited = 0, flags = 0; int rating = 0; bool favourite = false; };
    struct UndoStep { juce::String label; std::vector<UndoItem> items; };
    std::vector<UndoStep> undoStack;
    int undoDepth = 0;
    bool undoOpen = false;
    void openUndo (const juce::String& label);
    void closeUndo();
    void remember (const Entry& e);   // before a change
    mutable juce::StringArray protectedCache;
    mutable int protectedCacheVersion = -1;

    std::unique_ptr<Scanner> scanner;
    juce::CriticalSection pendingLock;
    std::deque<std::unique_ptr<Entry>> pending;
    std::atomic<int> total { 0 }, done { 0 };
    juce::StringArray scannedFolders;
    std::set<juce::String> seenPaths;
    juce::uint32 scanStart = 0, lastScanSave = 0;

    std::unique_ptr<DupIndex> index, builtIndex;
    std::thread indexThread;
    std::atomic<bool> indexReady { false };
    juce::CriticalSection builtLock;
    std::set<juce::uint64> notDup;
    void startIndexBuild();
    bool installIndex();
    void finishGroups();

    void insertOrReplace (std::unique_ptr<Entry> e);
    void changed() { ++changeCounter; sendChangeMessage(); }
    void timerCallback() override;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Archive)
};

juce::String formatDuration (double seconds);
} // namespace sw::archive
