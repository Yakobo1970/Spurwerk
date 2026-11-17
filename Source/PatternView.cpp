#include "PatternView.h"
#include "PluginEditor.h"
#include "YmFile.h"
#include "TrackerFormats.h"

namespace sw::ui
{
namespace
{
const int kSubX_Mod[6] = { 0, 4, 5, 7, 8, 9 }, kSubW_Mod[6] = { 3, 1, 1, 1, 1, 1 };
const int kSubX_Ym[8] = { 0, 4, 5, 7, 8, 10, 11, 12 }, kSubW_Ym[8] = { 3, 1, 1, 1, 1, 1, 1, 1 };
const int kSubX_Vc[8] = { 0, 4, 5, 8, 9, 11, 12, 13 }, kSubW_Vc[8] = { 3, 1, 1, 1, 1, 1, 1, 1 };
} // namespace

// ============================================================================================ OrderList
void OrderList::ensureVisible (int order)
{
    const int visible = juce::jmax (1, (getWidth() - 8) / kCell);
    if (order < scroll) scroll = order;
    else if (order >= scroll + visible) scroll = order - visible + 1;
}

void OrderList::paint (juce::Graphics& g)
{
    const auto& th = ed.theme();
    g.fillAll (th.panel);
    const auto& s = ed.proc.song();
    const int cur = ed.patterns->shownOrder();
    const bool playing = ed.proc.playState() != SpurwerkProcessor::Play::Stop;
    const int playO = ed.proc.posOrder();
    g.setFont (mono (13.0f));
    for (int i = scroll; i < (int) s.orders.size(); ++i)
    {
        const int x = 4 + (i - scroll) * kCell;
        if (x > getWidth()) break;
        juce::Rectangle<int> r (x, 4, kCell - 3, getHeight() - 8);
        g.setColour (i == cur ? th.accent : th.panel2);
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        if (playing && i == playO)
        {
            g.setColour (th.track[3]);
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 4.0f, 2.0f);
        }
        g.setColour (i == cur ? th.bg : th.text);
        g.drawText (juce::String (s.orders[(size_t) i]).paddedLeft ('0', 2), r, juce::Justification::centred);
    }
    if (s.orders.empty())
    {
        g.setColour (th.dim);
        g.drawText ("(leer)", getLocalBounds().reduced (6, 0), juce::Justification::centredLeft);
    }
}

void OrderList::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { ed.showLoupeMenu(); return; }
    const int o = orderAt (e.x);
    if (o >= 0 && o < (int) ed.proc.song().orders.size())
    {
        ed.patterns->setCursorOrder (o);
        if (e.getNumberOfClicks() > 1) ed.playSong (true);
    }
}

void OrderList::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    auto& s = ed.proc.song();
    const int o = orderAt (e.x);
    if (e.mods.isCtrlDown() && ! s.readOnly() && o >= 0 && o < (int) s.orders.size())
    {
        // ctrl + wheel changes the pattern number of this order entry
        const int d = w.deltaY > 0 ? 1 : -1;
        {
            const juce::ScopedLock sl (ed.proc.lock);
            s.orders[(size_t) o] = juce::jlimit (0, juce::jmin (127, (int) s.patterns.size() - 1), s.orders[(size_t) o] + d);
        }
        ed.songEdited();
        return;
    }
    scroll = juce::jlimit (0, juce::jmax (0, (int) s.orders.size() - 1), scroll + (w.deltaY > 0 ? -2 : 2));
    repaint();
}

// ============================================================================================ PatternView
PatternView::PatternView (SpurwerkEditor& e) : ed (e)
{
    setWantsKeyboardFocus (true);
    setMouseClickGrabsKeyboardFocus (true);
    for (auto* b : { &vbar, &hbar })
    {
        b->addListener (this);
        b->setAutoHide (false);
        addAndMakeVisible (b);
    }
    charW = juce::GlyphArrangement::getStringWidth (mono (14.0f), "0000000000") / 10.0f;
}

PatternView::~PatternView()
{
    vbar.removeListener (this);
    hbar.removeListener (this);
}

int PatternView::subCount() const { const auto& s = ed.proc.song(); return s.format == Format::YM || s.hasVolumeColumn() ? 8 : 6; }
int PatternView::channelWidth() const
{
    const auto& s = ed.proc.song();
    return (int) std::ceil (charW * (s.format == Format::YM ? 14.0f : s.hasVolumeColumn() ? 15.5f : 11.5f));
}

juce::Rectangle<int> PatternView::gridArea() const
{
    return getLocalBounds().withTrimmedTop (kHeaderH).withTrimmedRight (12).withTrimmedBottom (12).withTrimmedLeft (kRowNumW);
}

int PatternView::visibleRows() const { return juce::jmax (1, gridArea().getHeight() / kRowH); }
int PatternView::visibleChannels() const { return juce::jmax (1, gridArea().getWidth() / juce::jmax (1, channelWidth())); }

int PatternView::shownOrder() const
{
    const bool playing = ed.proc.playState() != SpurwerkProcessor::Play::Stop;
    if (playing && ed.follow) return ed.proc.posOrder();
    return ed.proc.cursorOrder.load();
}

int PatternView::shownPattern() const
{
    const auto& s = ed.proc.song();
    const int o = shownOrder();
    if (o < 0 || o >= (int) s.orders.size()) return -1;
    const int p = s.orders[(size_t) o];
    return (p >= 0 && p < (int) s.patterns.size()) ? p : -1;
}

Pattern* PatternView::pattern()
{
    const int p = shownPattern();
    return p < 0 ? nullptr : &ed.proc.song().patterns[(size_t) p];
}

Cell* PatternView::cursorCell()
{
    auto* p = pattern();
    if (p == nullptr || cursorRow >= p->rows || cursorChannel >= p->channels) return nullptr;
    return &p->at (cursorRow, cursorChannel);
}

void PatternView::setCursorOrder (int o)
{
    ed.proc.cursorOrder = juce::jlimit (0, juce::jmax (0, (int) ed.proc.song().orders.size() - 1), o);
    if (ed.orders) { ed.orders->ensureVisible (ed.proc.cursorOrder.load()); ed.orders->repaint(); }
    if (auto* p = pattern()) cursorRow = juce::jmin (cursorRow, p->rows - 1);
    ensureCursorVisible();
    repaint();
    ed.updateHeader();
}

void PatternView::songChanged()
{
    cursorRow = cursorChannel = cursorSub = 0;
    firstRow = firstChannel = 0;
    hasSel = false;
    undoStack.clear();
    redoStack.clear();
    ed.proc.cursorOrder = 0;
    resized();
    repaint();
}

void PatternView::resized()
{
    auto r = getLocalBounds();
    vbar.setBounds (r.getRight() - 12, kHeaderH, 12, r.getHeight() - kHeaderH - 12);
    hbar.setBounds (kRowNumW, r.getBottom() - 12, r.getWidth() - kRowNumW - 12, 12);
    ensureCursorVisible();
}

void PatternView::scrollBarMoved (juce::ScrollBar* sb, double start)
{
    if (sb == &vbar) firstRow = (int) start;
    else firstChannel = (int) start;
    repaint();
}

void PatternView::ensureCursorVisible()
{
    const auto* p = const_cast<PatternView*> (this)->pattern();
    const int rows = p ? p->rows : 64;
    const int vis = visibleRows();
    const bool playing = ed.proc.playState() != SpurwerkProcessor::Play::Stop && ed.follow;
    const int focusRow = playing ? ed.proc.posRow() : cursorRow;
    if (playing) firstRow = focusRow - vis / 2;
    else if (focusRow < firstRow + 2) firstRow = focusRow - 2;
    else if (focusRow >= firstRow + vis - 2) firstRow = focusRow - vis + 3;
    firstRow = juce::jlimit (-vis / 2, juce::jmax (0, rows - vis / 2), firstRow);
    const int chs = ed.proc.song().channels;
    const int vc = visibleChannels();
    if (cursorChannel < firstChannel) firstChannel = cursorChannel;
    else if (cursorChannel >= firstChannel + vc) firstChannel = cursorChannel - vc + 1;
    firstChannel = juce::jlimit (0, juce::jmax (0, chs - vc), firstChannel);
    vbar.setRangeLimits (-vis / 2, rows + vis / 2, juce::dontSendNotification);
    vbar.setCurrentRange (firstRow, vis, juce::dontSendNotification);
    hbar.setRangeLimits (0, juce::jmax (chs, 1), juce::dontSendNotification);
    hbar.setCurrentRange (firstChannel, juce::jmin (vc, chs), juce::dontSendNotification);
    hbar.setVisible (chs > vc);
}

void PatternView::refresh()
{
    const bool playing = ed.proc.playState() != SpurwerkProcessor::Play::Stop;
    const int o = ed.proc.posOrder(), r = ed.proc.posRow();
    bool dirty = false;
    if (playing && (o != lastPlayOrder || r != lastPlayRow))
    {
        lastPlayOrder = o; lastPlayRow = r;
        if (ed.follow)
        {
            if (ed.orders) { ed.orders->ensureVisible (o); }
            ensureCursorVisible();
        }
        if (ed.orders) ed.orders->repaint();
        dirty = true;
    }
    else if (! playing && lastPlayOrder >= 0) { lastPlayOrder = lastPlayRow = -1; dirty = true; }
    for (int c = 0; c < juce::jmin (ed.proc.song().channels, kMaxChannels); ++c)
    {
        const float l = ed.proc.channelLevel (c);
        if (std::abs (l - lastLevels[c]) > 0.02f) { lastLevels[c] = l; dirty = true; }
    }
    if (dirty) repaint();
}

juce::String PatternView::fxText (const Cell& c, bool ym) const
{
    if (ym)
    {
        switch (c.fx)
        {
            case ymFxNoise: return "N" + hex2 (c.param);
            case ymFxEnv: return "H" + hex2 (c.param);
            case ymFxSpecial:
            {
                static const char* t[4] = { "SID", "DRM", "SIN", "BUZ" };
                return t[c.param & 3];
            }
            default: return "...";
        }
    }
    const auto& s = ed.proc.song();
    if (s.format != Format::MOD) return effectText (s, c);
    if (c.fx == 0 && c.param == 0) return "...";
    return juce::String::toHexString (c.fx & 15).toUpperCase() + hex2 (c.param);
}

void PatternView::drawCell (juce::Graphics& g, const Cell& c, int x, int y, bool ym, const Theme& th) const
{
    const auto draw = [&] (const juce::String& t, int charPos, juce::Colour col) {
        g.setColour (col);
        g.drawSingleLineText (t, x + (int) (charPos * charW), y + kRowH - 5);
    };
    const auto empty = th.dim.withAlpha (0.45f);
    if (c.note == kNoteOff) draw ("===", 0, th.track[0]);
    else if (c.note == kNoteCut) draw ("^^^", 0, th.track[0]);
    else draw (noteName (c.note), 0, c.note ? th.text : empty);
    if (c.instr) draw (hex2 (c.instr).substring (0, 2).paddedLeft ('0', 2), 4, th.track[(size_t) ((c.instr - 1) % 8)]);
    else draw ("..", 4, empty);
    if (ym)
    {
        if (c.vol != kVolNone) draw (c.vol >= 16 ? juce::String ("Hk") : hex2 (c.vol), 7, th.track[3]);
        else draw ("..", 7, empty);
        const auto f = fxText (c, true);
        draw (f, 10, c.fx ? (c.fx == ymFxNoise ? th.track[5] : c.fx == ymFxEnv ? th.track[1] : th.track[6]) : empty);
    }
    else if (ed.proc.song().hasVolumeColumn())
    {
        const bool hasVol = c.vcmd != 0 || c.vol != kVolNone;
        draw (volumeText (c), 7, hasVol ? (c.vcmd == 0 ? th.track[3] : th.track[5]) : empty);
        const auto f = fxText (c, false);
        draw (f, 11, (c.fx || c.param) ? th.accent : empty);
    }
    else
    {
        const auto f = fxText (c, false);
        draw (f, 7, (c.fx || c.param) ? th.accent : empty);
    }
}

void PatternView::paint (juce::Graphics& g)
{
    const auto& th = ed.theme();
    g.fillAll (th.bg);
    const auto& s = ed.proc.song();
    const bool ym = s.format == Format::YM;
    const int pi = shownPattern();
    const auto grid = gridArea();
    const int cw = channelWidth();
    const int vc = visibleChannels();
    const bool playing = ed.proc.playState() != SpurwerkProcessor::Play::Stop;
    const bool showPlayRow = playing && ed.proc.posOrder() == shownOrder();
    const int playRow = ed.proc.posRow();
    const auto mute = ed.proc.muteMask.load();

    // channel headers
    g.setColour (th.panel);
    g.fillRect (0, 0, getWidth(), kHeaderH);
    for (int i = 0; i < vc + 1; ++i)
    {
        const int ch = firstChannel + i;
        if (ch >= s.channels) break;
        const int x = grid.getX() + i * cw;
        if (x >= grid.getRight()) break;
        juce::Rectangle<int> hr (x + 2, 4, cw - 4, kHeaderH - 8);
        const bool muted = (mute >> ch) & 1u;
        g.setColour (muted ? th.panel2.darker (0.3f) : th.panel2);
        g.fillRoundedRectangle (hr.toFloat(), 4.0f);
        g.setColour (muted ? th.dim : th.track[(size_t) (ch % 8)]);
        g.setFont (font (12.5f, true));
        const juce::String name = ym ? juce::String ("Stimme ") + juce::String::charToString ((juce::juce_wchar) ('A' + ch)) : "Kanal " + juce::String (ch + 1);
        g.drawText (name + (muted ? " (stumm)" : ""), hr.withTrimmedLeft (6).withHeight (18), juce::Justification::centredLeft);
        // VU
        const float lvl = juce::jlimit (0.0f, 1.0f, ed.proc.channelLevel (ch) * 1.4f);
        auto vu = hr.reduced (6, 0).removeFromBottom (12).withTrimmedBottom (4);
        g.setColour (th.bg.withAlpha (0.6f));
        g.fillRoundedRectangle (vu.toFloat(), 2.0f);
        g.setColour (th.track[(size_t) (ch % 8)].withAlpha (muted ? 0.25f : 0.9f));
        g.fillRoundedRectangle (vu.withWidth ((int) (vu.getWidth() * lvl)).toFloat(), 2.0f);
    }

    if (pi < 0)
    {
        g.setColour (th.dim);
        g.setFont (font (15.0f));
        g.drawText ("Kein Pattern", grid, juce::Justification::centred);
        return;
    }
    const auto& pat = s.patterns[(size_t) pi];
    const int rows = visibleRows() + 1;
    g.setFont (mono (14.0f));
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (getLocalBounds().withTrimmedTop (kHeaderH).withTrimmedBottom (12).withTrimmedRight (12));
        for (int i = 0; i < rows; ++i)
        {
            const int r = firstRow + i;
            const int y = grid.getY() + i * kRowH;
            if (r < 0 || r >= pat.rows) continue;
            juce::Colour rowBg = th.bg;
            if (r % 16 == 0) rowBg = th.panel2;
            else if (r % 4 == 0) rowBg = th.panel;
            if (showPlayRow && r == playRow) rowBg = th.accent.withAlpha (0.28f);
            g.setColour (rowBg);
            g.fillRect (0, y, grid.getRight(), kRowH);
            if (! showPlayRow && r == cursorRow)
            {
                g.setColour (th.accent.withAlpha (hasKeyboardFocus (true) ? 0.14f : 0.08f));
                g.fillRect (0, y, grid.getRight(), kRowH);
            }
            g.setColour (r % 4 == 0 ? th.text : th.dim);
            g.drawSingleLineText (juce::String (r).paddedLeft ('0', 2), 6, y + kRowH - 5);
            for (int k = 0; k <= vc; ++k)
            {
                const int ch = firstChannel + k;
                if (ch >= pat.channels) break;
                const int x = grid.getX() + k * cw + 6;
                if (x >= grid.getRight()) break;
                if (hasSel && r >= selRow0 && r <= selRow1 && ch >= selCh0 && ch <= selCh1)
                {
                    g.setColour (th.accent.withAlpha (0.25f));
                    g.fillRect (x - 6, y, cw, kRowH);
                }
                if (r == cursorRow && ch == cursorChannel)
                {
                    const int* sx = ym ? kSubX_Ym : ed.proc.song().hasVolumeColumn() ? kSubX_Vc : kSubX_Mod;
                    const int* sw = ym ? kSubW_Ym : ed.proc.song().hasVolumeColumn() ? kSubW_Vc : kSubW_Mod;
                    const int sub = juce::jlimit (0, subCount() - 1, cursorSub);
                    juce::Rectangle<float> cr (x + sx[sub] * charW - 1.0f, (float) y + 1, sw[sub] * charW + 2.0f, (float) kRowH - 2);
                    g.setColour (ed.record ? th.track[0].withAlpha (0.55f) : th.accent.withAlpha (0.55f));
                    g.fillRoundedRectangle (cr, 2.0f);
                }
                drawCell (g, pat.at (r, ch), x, y, ym, th);
            }
        }
        // channel separators
        g.setColour (th.panel2);
        for (int k = 0; k <= vc && firstChannel + k <= pat.channels; ++k)
        {
            const int x = grid.getX() + k * cw;
            if (x > grid.getRight()) break;
            g.drawVerticalLine (x, (float) grid.getY(), (float) grid.getBottom() + 12);
        }
    }
    if (s.readOnly())
    {
        g.setColour (th.dim);
        g.setFont (font (11.5f));
        g.drawText (s.format == Format::YM || s.subSongs > 1 ? juce::String ("nur lesen (YM: 1 Zeile = 1 Frame)") : U ("nur lesen (Bearbeiten derzeit nur bei MOD)"), getLocalBounds().removeFromBottom (12).withTrimmedLeft (kRowNumW + 6), juce::Justification::centredLeft);
    }
}

bool PatternView::cellAt (juce::Point<int> p, int& row, int& ch, int& sub) const
{
    const auto grid = gridArea();
    if (! grid.contains (p)) return false;
    row = firstRow + (p.y - grid.getY()) / kRowH;
    const int cw = channelWidth();
    ch = firstChannel + (p.x - grid.getX()) / cw;
    const float cx = (float) ((p.x - grid.getX()) % cw - 6) / charW;
    const bool ym = ed.proc.song().format == Format::YM;
    const int* sx = ym ? kSubX_Ym : ed.proc.song().hasVolumeColumn() ? kSubX_Vc : kSubX_Mod;
    sub = 0;
    for (int i = subCount() - 1; i >= 0; --i)
        if (cx >= (float) sx[i] - 0.5f) { sub = i; break; }
    const auto* pat = const_cast<PatternView*> (this)->pattern();
    if (pat == nullptr) return false;
    row = juce::jlimit (0, pat->rows - 1, row);
    ch = juce::jlimit (0, pat->channels - 1, ch);
    return true;
}

void PatternView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (e.mods.isPopupMenu()) { showMenu(); return; }
    if (e.y < kHeaderH)
    {
        // header: click = mute, ctrl/double click = solo
        const int ch = firstChannel + (e.x - gridArea().getX()) / channelWidth();
        if (e.x < gridArea().getX() || ch >= ed.proc.song().channels) return;
        auto m = ed.proc.muteMask.load();
        if (e.mods.isCtrlDown() || e.mods.isShiftDown())
        {
            const juce::uint32 solo = ~(1u << ch);
            m = (m == solo) ? 0u : solo;
        }
        else m ^= (1u << ch);
        ed.proc.muteMask = m;
        repaint();
        return;
    }
    int r, c, s;
    if (cellAt (e.getPosition(), r, c, s))
    {
        cursorRow = r; cursorChannel = c; cursorSub = s;
        if (e.mods.isShiftDown())
        {
            hasSel = true;
            selRow0 = juce::jmin (anchorRow, r); selRow1 = juce::jmax (anchorRow, r);
            selCh0 = juce::jmin (anchorCh, c); selCh1 = juce::jmax (anchorCh, c);
        }
        else
        {
            hasSel = false;
            anchorRow = r; anchorCh = c;
            selRow0 = selRow1 = r; selCh0 = selCh1 = c;
        }
        selecting = true;
        repaint();
    }
}

void PatternView::mouseDrag (const juce::MouseEvent& e)
{
    if (! selecting) return;
    int r, c, s;
    if (cellAt (e.getPosition(), r, c, s))
    {
        hasSel = r != anchorRow || c != anchorCh || hasSel;
        selRow0 = juce::jmin (anchorRow, r); selRow1 = juce::jmax (anchorRow, r);
        selCh0 = juce::jmin (anchorCh, c); selCh1 = juce::jmax (anchorCh, c);
        cursorRow = r; cursorChannel = c;
        repaint();
    }
}

void PatternView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (e.y < kHeaderH)
    {
        const int ch = firstChannel + (e.x - gridArea().getX()) / channelWidth();
        if (ch < ed.proc.song().channels) { ed.proc.muteMask = ~(1u << ch); repaint(); }
    }
}

void PatternView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isShiftDown()) { firstChannel = juce::jmax (0, firstChannel + (w.deltaY > 0 ? -1 : 1)); }
    else firstRow += (w.deltaY > 0 ? -4 : 4);
    const auto* p = pattern();
    const int rows = p ? p->rows : 64;
    firstRow = juce::jlimit (-visibleRows() / 2, juce::jmax (0, rows - visibleRows() / 2), firstRow);
    firstChannel = juce::jlimit (0, juce::jmax (0, ed.proc.song().channels - visibleChannels()), firstChannel);
    vbar.setCurrentRange (firstRow, visibleRows(), juce::dontSendNotification);
    hbar.setCurrentRange (firstChannel, visibleChannels(), juce::dontSendNotification);
    repaint();
}

void PatternView::moveCursor (int dRow, int dSub, bool extendSel)
{
    auto* p = pattern();
    if (p == nullptr) return;
    const int prevRow = cursorRow, prevCh = cursorChannel;
    if (dRow != 0)
    {
        cursorRow += dRow;
        if (cursorRow < 0)
        {
            if (ed.proc.cursorOrder.load() > 0 && dRow == -1 && ! extendSel)
            {
                setCursorOrder (ed.proc.cursorOrder.load() - 1);
                if (auto* np = pattern()) cursorRow = np->rows - 1;
            }
            else cursorRow = 0;
        }
        else if (cursorRow >= p->rows)
        {
            if (ed.proc.cursorOrder.load() + 1 < (int) ed.proc.song().orders.size() && dRow == 1 && ! extendSel)
            {
                setCursorOrder (ed.proc.cursorOrder.load() + 1);
                cursorRow = 0;
            }
            else cursorRow = p->rows - 1;
        }
    }
    if (dSub != 0)
    {
        cursorSub += dSub;
        if (cursorSub < 0) { if (cursorChannel > 0) { --cursorChannel; cursorSub = subCount() - 1; } else cursorSub = 0; }
        else if (cursorSub >= subCount()) { if (cursorChannel + 1 < p->channels) { ++cursorChannel; cursorSub = 0; } else cursorSub = subCount() - 1; }
    }
    if (extendSel)
    {
        if (! hasSel) { hasSel = true; selRow0 = selRow1 = prevRow; selCh0 = selCh1 = prevCh; }
        // anchor = the corner opposite to the previous cursor
        const int aRow = (prevRow == selRow0) ? selRow1 : selRow0;
        const int aCh = (prevCh == selCh0) ? selCh1 : selCh0;
        selRow0 = juce::jmin (aRow, cursorRow); selRow1 = juce::jmax (aRow, cursorRow);
        selCh0 = juce::jmin (aCh, cursorChannel); selCh1 = juce::jmax (aCh, cursorChannel);
    }
    else hasSel = false;
    ensureCursorVisible();
    repaint();
}

void PatternView::pushUndo()
{
    Snapshot s;
    s.orders = ed.proc.song().orders;
    s.patterns = ed.proc.song().patterns;
    s.row = cursorRow; s.channel = cursorChannel;
    undoStack.push_back (std::move (s));
    if (undoStack.size() > 60) undoStack.erase (undoStack.begin());
    redoStack.clear();
}

void PatternView::undo()
{
    if (undoStack.empty()) { ed.setStatus (U ("Nichts rückgängig zu machen")); return; }
    auto& song = ed.proc.song();
    Snapshot cur { song.orders, song.patterns, cursorRow, cursorChannel };
    {
        const juce::ScopedLock sl (ed.proc.lock);
        song.orders = undoStack.back().orders;
        song.patterns = undoStack.back().patterns;
    }
    cursorRow = undoStack.back().row; cursorChannel = undoStack.back().channel;
    redoStack.push_back (std::move (cur));
    undoStack.pop_back();
    ed.proc.songStructureChanged();
    ed.songEdited();
    ed.setStatus (U ("Rückgängig"));
}

void PatternView::redo()
{
    if (redoStack.empty()) return;
    auto& song = ed.proc.song();
    Snapshot cur { song.orders, song.patterns, cursorRow, cursorChannel };
    {
        const juce::ScopedLock sl (ed.proc.lock);
        song.orders = redoStack.back().orders;
        song.patterns = redoStack.back().patterns;
    }
    undoStack.push_back (std::move (cur));
    redoStack.pop_back();
    ed.proc.songStructureChanged();
    ed.songEdited();
    ed.setStatus ("Wiederhergestellt");
}

void PatternView::edited()
{
    ed.songEdited();
    repaint();
}

void PatternView::advance()
{
    if (ed.editStep > 0) moveCursor (ed.editStep, 0, false);
}

int PatternView::noteForKey (juce::juce_wchar c) const
{
    c = juce::CharacterFunctions::toLowerCase (c);
    // lower row (octave), upper row (octave + 1) – FastTracker/OpenMPT layout
    const juce::String lower = ed.qwertz() ? "ysxdcvgbhnjm,l.;-" : "zsxdcvgbhnjm,l.;/";
    const juce::String upper = ed.qwertz() ? "q2w3er5t6z7ui9o0p" : "q2w3er5t6y7ui9o0p";
    int idx = upper.indexOfChar (c);
    if (idx >= 0) return 1 + (ed.octave + 1) * 12 + idx;
    idx = lower.indexOfChar (c);
    if (idx >= 0) return 1 + ed.octave * 12 + idx;
    return 0;
}

bool PatternView::enterNote (int note)
{
    auto& song = ed.proc.song();
    note = juce::jlimit (1, 120, note);
    ed.proc.previewNote (ed.instrument, note - 1, true);
    if (! ed.record || song.readOnly()) return true;
    if (song.format == Format::MOD && (note < 37 || note > 96))
    {
        ed.setStatus (U ("MOD: Noten nur von C-3 bis B-7 (Amiga-Bereich)"));
        return true;
    }
    pushUndo();
    if (auto* c = cursorCell())
    {
        const juce::ScopedLock sl (ed.proc.lock);
        c->note = (juce::uint8) note;
        c->instr = (juce::uint8) ed.instrument;
    }
    advance();
    edited();
    return true;
}

bool PatternView::enterHex (int digit)
{
    auto& song = ed.proc.song();
    if (! ed.record || song.readOnly() || cursorSub == 0) return false;
    auto* c = cursorCell();
    if (c == nullptr) return false;
    pushUndo();
    {
        const juce::ScopedLock sl (ed.proc.lock);
        switch (cursorSub)
        {
            case 1: c->instr = (juce::uint8) juce::jlimit (0, 31, (digit << 4) | (c->instr & 15)); break;
            case 2: c->instr = (juce::uint8) juce::jlimit (0, 31, (c->instr & 0xf0) | digit); break;
            case 3: c->fx = (juce::uint8) digit; break;
            case 4: c->param = (juce::uint8) ((digit << 4) | (c->param & 15)); break;
            case 5: c->param = (juce::uint8) ((c->param & 0xf0) | digit); break;
            default: break;
        }
    }
    if (cursorSub == 1 || cursorSub == 4) moveCursor (0, 1, false);
    else if (cursorSub == 3) moveCursor (0, 1, false);
    else { cursorSub = cursorSub == 2 ? 1 : 4; advance(); }
    edited();
    return true;
}

void PatternView::clearSelectionCells()
{
    auto* p = pattern();
    if (p == nullptr || ed.proc.song().readOnly()) return;
    pushUndo();
    const juce::ScopedLock sl (ed.proc.lock);
    if (hasSel)
    {
        for (int r = selRow0; r <= selRow1; ++r)
            for (int c = selCh0; c <= selCh1; ++c) p->at (r, c) = Cell();
    }
    else if (auto* c = cursorCell())
    {
        if (cursorSub == 0) { c->note = 0; c->instr = 0; }
        else if (cursorSub <= 2) c->instr = 0;
        else { c->fx = 0; c->param = 0; }
    }
}

void PatternView::copySel (bool cut)
{
    auto* p = pattern();
    if (p == nullptr) return;
    int r0 = cursorRow, r1 = cursorRow, c0 = cursorChannel, c1 = cursorChannel;
    if (hasSel) { r0 = selRow0; r1 = selRow1; c0 = selCh0; c1 = selCh1; }
    clipRows = r1 - r0 + 1; clipChannels = c1 - c0 + 1;
    clip.assign ((size_t) (clipRows * clipChannels), Cell());
    for (int r = 0; r < clipRows; ++r)
        for (int c = 0; c < clipChannels; ++c) clip[(size_t) (r * clipChannels + c)] = p->at (r0 + r, c0 + c);
    ed.setStatus (juce::String (clipRows) + " Zeilen x " + juce::String (clipChannels) + (cut ? U (" Kanäle ausgeschnitten") : U (" Kanäle kopiert")));
    if (cut) { clearSelectionCells(); edited(); }
}

void PatternView::paste()
{
    auto* p = pattern();
    if (p == nullptr || clip.empty() || ed.proc.song().readOnly()) return;
    pushUndo();
    {
        const juce::ScopedLock sl (ed.proc.lock);
        for (int r = 0; r < clipRows; ++r)
            for (int c = 0; c < clipChannels; ++c)
            {
                const int rr = cursorRow + r, cc = cursorChannel + c;
                if (rr < p->rows && cc < p->channels) p->at (rr, cc) = clip[(size_t) (r * clipChannels + c)];
            }
    }
    edited();
}

void PatternView::insertRow (bool remove)
{
    auto* p = pattern();
    if (p == nullptr || ed.proc.song().readOnly()) return;
    pushUndo();
    const int c0 = hasSel ? selCh0 : cursorChannel, c1 = hasSel ? selCh1 : cursorChannel;
    {
        const juce::ScopedLock sl (ed.proc.lock);
        for (int c = c0; c <= c1; ++c)
        {
            if (remove)
            {
                for (int r = cursorRow; r < p->rows - 1; ++r) p->at (r, c) = p->at (r + 1, c);
                p->at (p->rows - 1, c) = Cell();
            }
            else
            {
                for (int r = p->rows - 1; r > cursorRow; --r) p->at (r, c) = p->at (r - 1, c);
                p->at (cursorRow, c) = Cell();
            }
        }
    }
    edited();
}

void PatternView::transpose (int semis)
{
    auto* p = pattern();
    if (p == nullptr || ed.proc.song().readOnly()) return;
    pushUndo();
    int r0 = cursorRow, r1 = cursorRow, c0 = cursorChannel, c1 = cursorChannel;
    if (hasSel) { r0 = selRow0; r1 = selRow1; c0 = selCh0; c1 = selCh1; }
    {
        const juce::ScopedLock sl (ed.proc.lock);
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
            {
                auto& cell = p->at (r, c);
                if (cell.note > 0 && cell.note <= 120) cell.note = (juce::uint8) juce::jlimit (37, 96, cell.note + semis);
            }
    }
    edited();
}

bool PatternView::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    const int code = k.getKeyCode();
    const auto ch = k.getTextCharacter();
    auto& song = ed.proc.song();
    const bool shift = mods.isShiftDown();

    if (code == juce::KeyPress::upKey) { moveCursor (-1, 0, shift); return true; }
    if (code == juce::KeyPress::downKey) { moveCursor (1, 0, shift); return true; }
    if (code == juce::KeyPress::leftKey) { if (shift) { if (cursorChannel > 0) { cursorSub = 0; moveCursor (0, 0, false); } } moveCursor (0, -1, shift); return true; }
    if (code == juce::KeyPress::rightKey) { moveCursor (0, 1, shift); return true; }
    if (code == juce::KeyPress::pageUpKey) { moveCursor (-16, 0, shift); return true; }
    if (code == juce::KeyPress::pageDownKey) { moveCursor (16, 0, shift); return true; }
    if (code == juce::KeyPress::homeKey) { moveCursor (-cursorRow, 0, shift); return true; }
    if (code == juce::KeyPress::endKey) { if (auto* p = pattern()) moveCursor (p->rows - 1 - cursorRow, 0, shift); return true; }
    if (code == juce::KeyPress::tabKey)
    {
        const int n = song.channels;
        cursorChannel = (cursorChannel + (shift ? n - 1 : 1)) % juce::jmax (1, n);
        cursorSub = 0;
        hasSel = false;
        ensureCursorVisible();
        repaint();
        return true;
    }
    if (code == juce::KeyPress::F5Key) { ed.playSong (false); return true; }
    if (code == juce::KeyPress::F6Key) { ed.playPattern(); return true; }
    if (code == juce::KeyPress::F7Key) { ed.playSong (true); return true; }
    if (code == juce::KeyPress::F8Key || code == juce::KeyPress::escapeKey) { ed.stopPlay(); return true; }
    if (code == juce::KeyPress::spaceKey && ! mods.isAnyModifierKeyDown())
    {
        ed.record = ! ed.record && ! song.readOnly();
        ed.updateHeader();
        repaint();
        return true;
    }
    if (mods.isCommandDown())
    {
        const auto lc = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) code);
        if (lc == 'z') { undo(); return true; }
        if (lc == 'y') { redo(); return true; }
        if (lc == 'c') { copySel (false); return true; }
        if (lc == 'x') { copySel (true); return true; }
        if (lc == 'v') { paste(); return true; }
        if (lc == 'a')
        {
            if (auto* p = pattern()) { hasSel = true; selRow0 = 0; selRow1 = p->rows - 1; selCh0 = 0; selCh1 = p->channels - 1; repaint(); }
            return true;
        }
        if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey) return false;
    }
    if (mods.isAltDown())
    {
        if (code == juce::KeyPress::upKey) { transpose (1); return true; }
        if (code == juce::KeyPress::downKey) { transpose (-1); return true; }
    }
    if (code == juce::KeyPress::deleteKey) { clearSelectionCells(); if (! hasSel) advance(); edited(); return true; }
    if (code == juce::KeyPress::insertKey) { insertRow (false); return true; }
    if (code == juce::KeyPress::backspaceKey) { insertRow (true); return true; }
    if (code == juce::KeyPress::numberPadAdd) { setCursorOrder (ed.proc.cursorOrder.load() + 1); return true; }
    if (code == juce::KeyPress::numberPadSubtract) { setCursorOrder (ed.proc.cursorOrder.load() - 1); return true; }
    if (code == juce::KeyPress::numberPadMultiply) { ed.octave = juce::jmin (8, ed.octave + 1); ed.updateHeader(); return true; }
    if (code == juce::KeyPress::numberPadDivide) { ed.octave = juce::jmax (0, ed.octave - 1); ed.updateHeader(); return true; }

    if (mods.isCommandDown() || mods.isAltDown()) return false;
    if (cursorSub == 0)
    {
        if (const int n = noteForKey (ch); n > 0)
        {
            previewKey = code;
            return enterNote (n);
        }
        return false;
    }
    const int digit = juce::CharacterFunctions::getHexDigitValue (ch);
    if (digit >= 0) return enterHex (digit);
    return false;
}

bool PatternView::keyStateChanged (bool isKeyDown)
{
    if (! isKeyDown && previewKey != 0 && ! juce::KeyPress::isKeyCurrentlyDown (previewKey))
    {
        ed.proc.previewNote (ed.instrument, 0, false);
        previewKey = 0;
    }
    return false;
}

void PatternView::showMenu()
{
    const bool ro = ed.proc.song().readOnly();
    juce::PopupMenu m;
    m.addSectionHeader ("Pattern");
    m.addItem (1, U ("Rückgängig\tStrg+Z"), ! undoStack.empty() && ! ro);
    m.addItem (2, "Wiederholen\tStrg+Y", ! redoStack.empty() && ! ro);
    m.addSeparator();
    m.addItem (3, "Ausschneiden\tStrg+X", ! ro);
    m.addItem (4, "Kopieren\tStrg+C");
    m.addItem (5, U ("Einfügen\tStrg+V"), ! clip.empty() && ! ro);
    m.addItem (6, U ("Alles auswählen\tStrg+A"));
    m.addSeparator();
    m.addItem (7, "Transponieren +1\tAlt+Hoch", ! ro);
    m.addItem (8, "Transponieren -1\tAlt+Runter", ! ro);
    m.addItem (9, "Transponieren +12", ! ro);
    m.addItem (10, "Transponieren -12", ! ro);
    m.addSeparator();
    m.addItem (11, U ("Zeile einfügen\tEinfg"), ! ro);
    m.addItem (12, U ("Zeile löschen\tRück"), ! ro);
    m.addSeparator();
    m.addItem (20, "Lupe ...");
    juce::Component::SafePointer<PatternView> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe] (int r) {
        if (safe == nullptr || r == 0) return;
        auto& v = *safe;
        switch (r)
        {
            case 1: v.undo(); break;
            case 2: v.redo(); break;
            case 3: v.copySel (true); break;
            case 4: v.copySel (false); break;
            case 5: v.paste(); break;
            case 6: if (auto* p = v.pattern()) { v.hasSel = true; v.selRow0 = 0; v.selRow1 = p->rows - 1; v.selCh0 = 0; v.selCh1 = p->channels - 1; v.repaint(); } break;
            case 7: v.transpose (1); break;
            case 8: v.transpose (-1); break;
            case 9: v.transpose (12); break;
            case 10: v.transpose (-12); break;
            case 11: v.insertRow (false); break;
            case 12: v.insertRow (true); break;
            case 20: v.ed.showLoupeMenu(); break;
            default: break;
        }
    });
}
} // namespace sw::ui
