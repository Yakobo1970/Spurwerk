// OpenMPT-style pattern editor: order list on top, channel headers with mute/VU, note grid.
#pragma once
#include "Look.h"
#include "Song.h"

class SpurwerkEditor;

namespace sw::ui
{
class OrderList : public juce::Component
{
public:
    explicit OrderList (SpurwerkEditor& e) : ed (e) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    int scroll = 0;
    void ensureVisible (int order);
private:
    SpurwerkEditor& ed;
    static constexpr int kCell = 30;
    int orderAt (int x) const { return scroll + (x - 4) / kCell; }
};

class PatternView : public juce::Component, private juce::ScrollBar::Listener
{
public:
    explicit PatternView (SpurwerkEditor& e);
    ~PatternView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool keyStateChanged (bool isKeyDown) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void focusGained (FocusChangeType) override { repaint(); }
    void focusLost (FocusChangeType) override { repaint(); }

    void refresh();                 // called by the editor timer
    void songChanged();             // new song loaded
    int shownOrder() const;         // order whose pattern is displayed
    int shownPattern() const;
    void setCursorOrder (int o);
    int cursorRow = 0, cursorChannel = 0, cursorSub = 0;
    void undo();
    void redo();
    void showMenu();                // context menu (right click / menu key)
    void transpose (int semis);

private:
    SpurwerkEditor& ed;
    juce::ScrollBar vbar { true }, hbar { false };
    int firstRow = 0, firstChannel = 0;
    int lastPlayOrder = -1, lastPlayRow = -1;
    float lastLevels[kMaxChannels] {};
    float charW = 8.0f;
    static constexpr int kRowH = 18, kHeaderH = 42, kRowNumW = 34;
    // selection (rows x channels)
    bool selecting = false, hasSel = false;
    int selRow0 = 0, selRow1 = 0, selCh0 = 0, selCh1 = 0, anchorRow = 0, anchorCh = 0;
    // clipboard
    std::vector<Cell> clip; int clipRows = 0, clipChannels = 0;
    // undo
    struct Snapshot { std::vector<int> orders; std::vector<Pattern> patterns; int row = 0, channel = 0; };
    std::vector<Snapshot> undoStack, redoStack;
    int previewKey = 0;

    void scrollBarMoved (juce::ScrollBar*, double) override;
    int channelWidth() const;
    int subCount() const;
    int visibleRows() const;
    int visibleChannels() const;
    juce::Rectangle<int> gridArea() const;
    bool cellAt (juce::Point<int> p, int& row, int& ch, int& sub) const;
    void moveCursor (int dRow, int dSub, bool extendSel);
    void ensureCursorVisible();
    void pushUndo();
    Cell* cursorCell();
    Pattern* pattern();
    void advance();
    bool enterNote (int note);
    bool enterHex (int digit);
    void clearSelectionCells();
    void copySel (bool cut);
    void paste();
    void insertRow (bool remove);
    int noteForKey (juce::juce_wchar c) const;
    void edited();
    void drawCell (juce::Graphics&, const Cell&, int x, int y, bool ym, const Theme&) const;
    juce::String fxText (const Cell& c, bool ym) const;
};
} // namespace sw::ui
