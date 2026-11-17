// Spurwerk – "Archiv" tab: module catalogue with folder scan, search, ratings, duplicate groups,
// note overview of the selected module (and its closest duplicate) and copyable sample-name text.
#pragma once
#include "ModArchive.h"
#include "Look.h"

class SpurwerkEditor;

namespace sw::ui
{
// piano-roll overview of a whole module (all samples, coloured), optional second roll for the closest match
class NoteOverview : public juce::Component, private juce::Timer
{
public:
    explicit NoteOverview (SpurwerkEditor& e);
    ~NoteOverview() override;
    void show (const juce::String& path, const juce::String& matchPath, const juce::String& matchLabel);
    void paint (juce::Graphics&) override;
    void waitUntilReady();   // tools: blocks until the background analysis is shown
private:
    struct Roll { juce::String path; archive::Analysis a; bool ready = false; };
    SpurwerkEditor& ed;
    Roll main, match;
    juce::String matchLabel;
    std::atomic<int> generation { 0 };
    juce::ThreadPool pool { 1 };
    juce::CriticalSection lock;
    std::unique_ptr<Roll> doneMain, doneMatch;
    int doneGeneration = -1;
    void timerCallback() override;
    void paintRoll (juce::Graphics&, juce::Rectangle<int>, const Roll&, const juce::String& label);
};

class ArchiveTab : public juce::Component, private juce::TableListBoxModel, private juce::ChangeListener, private juce::Timer,
                   private juce::KeyListener
{
public:
    explicit ArchiveTab (SpurwerkEditor& e);
    ~ArchiveTab() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void update();
    void addFolder (const juce::File& dir);   // also used for folders dropped onto the window
    void prepareShot (int selectRow, const juce::String& searchText, int filterId);   // UiShot: synchronous refresh
    juce::String copyTest (int from, int to, bool viaButton);
    juce::String cursorTest();   // UiShot: arrow keys, typing, Enter – returns a log   // UiShot: marks info text [from,to) (or nothing), copies, returns the clipboard
    juce::Rectangle<int> tableBounds() const { return table.getBounds() + getPosition(); }   // in parent coordinates (right click)

    enum Col { cFav = 1, cRating, cTitle, cFormat, cChannels, cLength, cNotes, cSamples, cArtist, cDup, cProfile, cFile, cFolder, cSize, cComposer, cArranger, cKey, cMode, cScale };

private:
    SpurwerkEditor& ed;
    juce::SharedResourcePointer<archive::Archive> arch;
    juce::TextButton addBtn { juce::String::fromUTF8 ("Ordner hinzufügen …") }, scanBtn { "Einlesen" }, stopBtn { "Stopp" },
        moreBtn { juce::String::fromUTF8 ("Mehr ▾") }, openBtn { juce::String::fromUTF8 ("▶ Öffnen & spielen") },
        notDupBtn { "Kein Duplikat" }, revealBtn { "Im Explorer zeigen" }, copyBtn { "Text kopieren" };
    juce::TextEditor search, info, comment;
    // editing in the table (title, artist, composer, arranger): click on a selected cell, F2 or just type;
    // Enter writes into all selected rows (with a question for more than one), Esc cancels
    // spreadsheet-like editing: the active cell of an editable column always has an open editor with its text selected.
    // Unchanged: arrow keys / Tab move to the neighbour cell (Shift+up/down extends the selection). Changed: only the
    // caret moves (left/right) until Enter writes – into all selected rows, with a question for more than one.
    std::unique_ptr<juce::TextEditor> cellEditor;
    int editId = 0, editCol = 0, curCol = cTitle, cursorRow = -1, anchorRow = -1;
    bool programmaticSelection = false, syncForTest = false;
    juce::String editOriginal;   // text when the editor opened: changed = current text differs
    bool isModified() const { return cellEditor != nullptr && cellEditor->getText() != editOriginal; }
    static bool isEditableCol (int col) { return col == cTitle || col == cArtist || col == cComposer || col == cArranger; }
    int columnStep (int col, int delta) const;
    void scrollColumnIntoView (int col);
    int cursorRowOrLast() const;
    void openAt (int row, int col);                   // cursor to this cell (editor if the column is editable)
    void navigate (int dRow, int dCol, bool extend);   // from the keyboard (unchanged cell)
    void selectRowsProgrammatically (int from, int to);
    void later (std::function<void()> fn);             // next message loop turn (never inside an editor callback)
    void finishEdit (bool commit, bool fromFocusLoss);
    void commitAndMoveOn (int dRow, int dCol);         // Enter / Tab after a change
    bool editorKey (const juce::KeyPress& k);
    void doUndo();   // Strg+Z / Mehr ▾: last database change
    // suggestions while typing: known values of the column that contain the text (word starts first, then the most used)
    struct SuggestBox;
    std::unique_ptr<SuggestBox> suggest;
    void updateSuggestions();
    void hideSuggestions();
    void acceptSuggestion (int index);   // into the field and written like Enter
public:
    juce::String suggestTest();          // UiShot
private:
    bool keyPressed (const juce::KeyPress&) override;
    bool keyPressed (const juce::KeyPress&, juce::Component*) override;   // info text: Ctrl+C / Ctrl+Einfg
    void copyInfo();   // marked text of the info box, or all of it
    juce::ComboBox filter;
    juce::TableListBox table { "Archiv", this };
    NoteOverview overview;
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<int> rows;
    juce::uint32 shownVersion = 0;
    int selectedId = 0, commentId = 0;
    juce::String status;
    bool rebuildPending = true;

    // TableListBoxModel
    int getNumRows() override { return (int) rows.size(); }
    void paintRowBackground (juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell (juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    void cellClicked (int row, int col, const juce::MouseEvent&) override;
    void cellDoubleClicked (int row, int col, const juce::MouseEvent&) override;
    void returnKeyPressed (int row) override;
    void selectedRowsChanged (int lastRow) override;
    void sortOrderChanged (int col, bool forwards) override;
    juce::String getCellTooltip (int row, int col) override;
    void deleteKeyPressed (int) override {}

    void changeListenerCallback (juce::ChangeBroadcaster*) override { rebuildPending = true; }
    void timerCallback() override;
    void rebuildRows();
    void showSelected();
    void openAndPlay (int id);
    void rowMenu (int row);
    void moreMenu();
    void confirmDelete (const std::vector<int>& ids);
    void saveView();
    void storeComment();
    archive::Entry* entryAt (int row) const;
    juce::String dupText (const archive::Entry& e) const;
};
} // namespace sw::ui
