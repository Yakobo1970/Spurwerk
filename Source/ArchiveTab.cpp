#include "ArchiveTab.h"
#include "PluginEditor.h"

namespace sw::ui
{
using namespace sw::archive;

struct ArchiveTab::SuggestBox : public juce::Component
{
    explicit SuggestBox (ArchiveTab& o) : owner (o) { setAlwaysOnTop (true); setWantsKeyboardFocus (false); setMouseClickGrabsKeyboardFocus (false); }
    ArchiveTab& owner;
    juce::StringArray items;
    juce::String query;
    int selected = -1;
    static constexpr int rowH = 20;
    void paint (juce::Graphics& g) override
    {
        const auto& th = owner.ed.theme();
        g.setColour (th.panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.setColour (th.accent);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 4.0f, 1.0f);
        g.setFont (font (13.0f));
        for (int i = 0; i < items.size(); ++i)
        {
            auto r = juce::Rectangle<int> (2, 2 + i * rowH, getWidth() - 4, rowH);
            if (i == selected) { g.setColour (th.accent.withAlpha (0.4f)); g.fillRoundedRectangle (r.toFloat(), 3.0f); }
            // matched part in the accent colour
            const auto& t = items[i];
            const int at = t.indexOfIgnoreCase (query);
            juce::AttributedString as;
            as.setJustification (juce::Justification::centredLeft);
            auto add = [&] (const juce::String& part, juce::Colour c) { if (part.isNotEmpty()) as.append (part, font (13.0f, c == th.accent), c); };
            if (at >= 0 && query.isNotEmpty())
            {
                add (t.substring (0, at), th.text);
                add (t.substring (at, at + query.length()), th.accent);
                add (t.substring (at + query.length()), th.text);
            }
            else add (t, th.text);
            as.draw (g, r.reduced (6, 0).toFloat());
        }
    }
    void mouseMove (const juce::MouseEvent& e) override { const int i = (e.y - 2) / rowH; if (i != selected && i >= 0 && i < items.size()) { selected = i; repaint(); } }
    void mouseUp (const juce::MouseEvent& e) override
    {
        const int i = (e.y - 2) / rowH;
        if (i >= 0 && i < items.size()) { auto& o = owner; o.later ([&o, i] { o.acceptSuggestion (i); }); }
    }
};


// ============================================================================================ NoteOverview
NoteOverview::NoteOverview (SpurwerkEditor& e) : ed (e) { startTimer (60); }

NoteOverview::~NoteOverview()
{
    stopTimer();
    pool.removeAllJobs (true, 8000);
}

void NoteOverview::show (const juce::String& path, const juce::String& matchPath, const juce::String& label)
{
    if (path == main.path && matchPath == match.path) { matchLabel = label; repaint(); return; }
    const int gen = ++generation;
    main = {}; match = {};
    main.path = path;
    match.path = matchPath;
    matchLabel = label;
    repaint();
    if (path.isEmpty()) return;
    pool.removeAllJobs (false, 0);
    pool.addJob ([this, gen, path, matchPath] {
        if (gen != generation.load()) return;
        auto m = std::make_unique<Roll>();
        m->path = path;
        m->a = analyseModule (juce::File (path), true);
        m->ready = true;
        std::unique_ptr<Roll> x;
        if (matchPath.isNotEmpty() && gen == generation.load())
        {
            x = std::make_unique<Roll>();
            x->path = matchPath;
            x->a = analyseModule (juce::File (matchPath), true);
            x->ready = true;
        }
        const juce::ScopedLock sl (lock);
        doneMain = std::move (m);
        doneMatch = std::move (x);
        doneGeneration = gen;
    });
}

void NoteOverview::waitUntilReady()
{
    for (int i = 0; i < 600; ++i)
    {
        {
            const juce::ScopedLock sl (lock);
            if (doneGeneration == generation.load() && doneMain != nullptr) break;
        }
        if (main.path.isEmpty() || main.ready) return;
        juce::Thread::sleep (20);
    }
    timerCallback();
}

void NoteOverview::timerCallback()
{
    std::unique_ptr<Roll> m, x;
    {
        const juce::ScopedLock sl (lock);
        if (doneGeneration != generation.load() || doneMain == nullptr) return;
        m = std::move (doneMain);
        x = std::move (doneMatch);
    }
    main = std::move (*m);
    if (x != nullptr) match = std::move (*x);
    repaint();
}

void NoteOverview::paintRoll (juce::Graphics& g, juce::Rectangle<int> r, const Roll& roll, const juce::String& label)
{
    const auto& th = ed.theme();
    g.setColour (th.bg);
    g.fillRoundedRectangle (r.toFloat(), 5.0f);
    auto inner = r.reduced (6, 4);
    auto head = inner.removeFromTop (16);
    g.setColour (th.dim);
    g.setFont (font (12.0f));
    g.drawText (label, head, juce::Justification::centredLeft, true);
    if (roll.path.isEmpty()) return;
    if (! roll.ready) { g.drawText (juce::String::fromUTF8 ("Noten werden ermittelt …"), inner, juce::Justification::centred); return; }
    const auto& notes = roll.a.noteList;
    if (notes.empty()) { g.drawText (roll.a.ok ? juce::String ("Keine Noten") : roll.a.error, inner, juce::Justification::centred); return; }
    int lo = 127, hi = 0;
    float end = 0;
    for (auto& n : notes) { lo = juce::jmin (lo, (int) n.pitch); hi = juce::jmax (hi, (int) n.pitch); end = juce::jmax (end, n.start + n.len); }
    lo = juce::jmax (0, lo - 1); hi = juce::jmin (127, hi + 1);
    const float rowH = (float) inner.getHeight() / (float) (hi - lo + 1);
    const float scale = (float) inner.getWidth() / juce::jmax (1.0f, end);
    // octave lines
    g.setColour (th.panel2.withAlpha (0.8f));
    for (int p = lo; p <= hi; ++p)
        if (p % 12 == 0) g.fillRect ((float) inner.getX(), (float) inner.getBottom() - (float) (p - lo + 1) * rowH, (float) inner.getWidth(), 1.0f);
    for (auto& n : notes)
    {
        const float x = (float) inner.getX() + n.start * scale;
        const float w = juce::jmax (1.0f, n.len * scale);
        const float y = (float) inner.getBottom() - (float) (n.pitch - lo + 1) * rowH;
        g.setColour (th.track[(size_t) (n.track % 8)].withAlpha (0.85f));
        g.fillRect (x, y, w, juce::jmax (1.0f, rowH - (rowH > 3 ? 1.0f : 0.0f)));
    }
}

void NoteOverview::paint (juce::Graphics& g)
{
    auto r = getLocalBounds();
    const auto title = [] (const Roll& rl, const juce::String& prefix) {
        if (rl.path.isEmpty()) return prefix;
        auto s = prefix + juce::File (rl.path).getFileName();
        if (rl.ready && rl.a.ok) s << "  ·  " << rl.a.notes << " Noten, " << formatDuration (rl.a.seconds);
        return s;
    };
    if (main.path.isEmpty())
    {
        g.setColour (ed.theme().dim);
        g.setFont (font (13.0f));
        g.drawText (juce::String::fromUTF8 ("Modul in der Liste wählen – hier erscheint der Notenverlauf (Farben = Samples)"), r, juce::Justification::centred, true);
        return;
    }
    if (match.path.isNotEmpty())
    {
        auto top = r.removeFromTop (r.getHeight() / 2);
        paintRoll (g, top.withTrimmedBottom (3), main, title (main, {}));
        paintRoll (g, r.withTrimmedTop (3), match, title (match, matchLabel + ": "));
    }
    else paintRoll (g, r, main, title (main, {}));
}

// ============================================================================================ ArchiveTab
ArchiveTab::ArchiveTab (SpurwerkEditor& e) : ed (e), overview (e)
{
    for (auto* b : { &addBtn, &scanBtn, &stopBtn, &moreBtn, &openBtn, &notDupBtn, &revealBtn, &copyBtn }) addAndMakeVisible (b);
    addAndMakeVisible (search);
    addAndMakeVisible (filter);
    addAndMakeVisible (table);
    addAndMakeVisible (overview);
    addAndMakeVisible (info);
    addAndMakeVisible (comment);

    addBtn.setTooltip (juce::String::fromUTF8 ("Ordner (mit Unterordnern) ins Archiv aufnehmen und nach Modulen durchsuchen"));
    scanBtn.setTooltip (juce::String::fromUTF8 ("Alle Archiv-Ordner erneut durchsuchen (nur neue oder geänderte Dateien werden analysiert)"));
    stopBtn.setTooltip ("Einlesen abbrechen");
    openBtn.setTooltip (juce::String::fromUTF8 ("Markiertes Modul in Spurwerk laden und abspielen (auch Doppelklick oder Enter)"));
    notDupBtn.setTooltip (juce::String::fromUTF8 ("Markiertes Modul und seine ähnlichste Datei sind keine Dubletten – Verbindung trennen"));
    copyBtn.setTooltip (juce::String::fromUTF8 ("Markierten Text der Infobox kopieren – ohne Markierung den ganzen Text (wie Strg+C / Strg+Einfg)"));
    search.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Suchen: Titel, Datei, Künstler, Samplenamen, Notiz …"), ed.theme().dim);
    search.onTextChange = [this] { rebuildPending = true; };
    search.setEscapeAndReturnKeysConsumed (false);
    filter.addItem ("Alle Module", 1);
    filter.addItem ("Nur Dubletten", 2);
    filter.addItem ("Favoriten", 3);
    filter.addItem ("Bewertete", 4);
    filter.addItem (juce::String::fromUTF8 ("Sammelpool: ausgeschlossen / fehlerhaft"), 5);
    filter.setSelectedId (juce::jlimit (1, 5, (int) arch->view.getProperty ("filter", 1)), juce::dontSendNotification);
    filter.onChange = [this] {
        if (filter.getSelectedId() == 2) table.getHeader().setSortColumnId (cDup, true);   // groups side by side
        rebuildPending = true;
        saveView();
    };

    info.setMultiLine (true);
    info.setReadOnly (true);
    info.setScrollbarsShown (true);
    info.setCaretVisible (false);
    info.setWantsKeyboardFocus (true);   // read-only, but selectable and copyable with the keyboard
    info.addKeyListener (this);
    info.setFont (font (13.0f));
    comment.setTextToShowWhenEmpty (juce::String::fromUTF8 ("Eigene Notiz zum Modul (nur in der Datenbank) – Enter speichert"), ed.theme().dim);
    comment.setFont (font (13.0f));
    comment.onReturnKey = [this] { storeComment(); };
    comment.onFocusLost = [this] { storeComment(); };

    auto& h = table.getHeader();
    const int sortable = juce::TableHeaderComponent::defaultFlags;
    h.addColumn (juce::String::fromUTF8 ("★"), cFav, 26, 22, 40, sortable);
    h.addColumn ("Bewertung", cRating, 78, 60, 100, sortable);
    h.addColumn ("Titel", cTitle, 200, 60, 600, sortable);
    h.addColumn ("Format", cFormat, 52, 40, 90, sortable);
    h.addColumn (juce::String::fromUTF8 ("Kanäle"), cChannels, 50, 30, 80, sortable);
    h.addColumn (juce::String::fromUTF8 ("Länge"), cLength, 52, 40, 80, sortable);
    h.addColumn ("Noten", cNotes, 56, 40, 90, sortable);
    h.addColumn ("Samples", cSamples, 56, 40, 90, sortable);
    h.addColumn (juce::String::fromUTF8 ("Künstler"), cArtist, 110, 40, 300, sortable);
    h.addColumn ("Dublette", cDup, 120, 50, 260, sortable);
    h.addColumn ("Verlauf", cProfile, 90, 40, 220, sortable);
    h.addColumn ("Datei", cFile, 180, 60, 600, sortable);
    h.addColumn ("Ordner", cFolder, 240, 60, 900, sortable);
    h.addColumn (juce::String::fromUTF8 ("Größe"), cSize, 64, 40, 100, sortable);
    h.addColumn ("Komponist", cComposer, 120, 40, 300, sortable);
    h.addColumn ("Arrangeur", cArranger, 120, 40, 300, sortable);
    h.addColumn ("Tonart", cKey, 70, 40, 120, sortable);
    h.addColumn ("Tongeschlecht", cMode, 90, 40, 120, sortable);
    h.addColumn ("Skala", cScale, 110, 40, 200, sortable);
    const auto layout = arch->view.getProperty ("columns").toString();
    if (layout.isNotEmpty()) h.restoreFromString (layout);
    else h.setSortColumnId (cTitle, true);
    table.setMultipleSelectionEnabled (true);
    table.setRowHeight (22);
    table.setColour (juce::ListBox::backgroundColourId, ed.theme().panel);

    addBtn.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Ordner mit Modulen wählen"), juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        juce::Component::SafePointer<ArchiveTab> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe] (const juce::FileChooser& fc) {
            if (safe != nullptr && fc.getResult().isDirectory()) safe->addFolder (fc.getResult());
        });
    };
    scanBtn.onClick = [this] {
        if (arch->folders.isEmpty()) { addBtn.triggerClick(); return; }
        arch->startScan();
    };
    stopBtn.onClick = [this] { arch->stopScan(); };
    moreBtn.onClick = [this] { moreMenu(); };
    openBtn.onClick = [this] { openAndPlay (selectedId); };
    notDupBtn.onClick = [this] {
        if (auto* en = arch->byId (selectedId); en != nullptr && en->bestMatch != 0)
        {
            arch->markNotDuplicate (en->id, en->bestMatch);
            ed.setStatus (juce::String::fromUTF8 ("Als „kein Duplikat“ markiert"));
        }
    };
    revealBtn.onClick = [this] { if (auto* en = arch->byId (selectedId)) juce::File (en->path).revealToUser(); };
    copyBtn.onClick = [this] { copyInfo(); };

    arch->addChangeListener (this);
    startTimer (200);
}

ArchiveTab::~ArchiveTab()
{
    info.removeKeyListener (this);
    storeComment();
    saveView();
    arch->removeChangeListener (this);
}

void ArchiveTab::addFolder (const juce::File& dir)
{
    const auto covering = arch->addFolder (dir);
    arch->startScan (false, juce::StringArray (covering));
    ed.setStatus (juce::String::fromUTF8 ("Ordner wird eingelesen: ") + covering);
}

void ArchiveTab::prepareShot (int selectRow, const juce::String& searchText, int filterId)
{
    arch->waitForIndex();
    search.setText (searchText, false);
    if (filterId > 0) filter.setSelectedId (filterId, juce::dontSendNotification);
    rebuildRows();
    if (selectRow >= 0 && selectRow < (int) rows.size()) table.selectRow (selectRow);
    timerCallback();
    overview.waitUntilReady();
}

void ArchiveTab::update()
{
    table.setColour (juce::ListBox::backgroundColourId, ed.theme().panel);
    rebuildPending = true;
    repaint();
}

void ArchiveTab::saveView()
{
    arch->view.setProperty ("columns", table.getHeader().toString(), nullptr);
    arch->view.setProperty ("filter", filter.getSelectedId(), nullptr);
    arch->saveSoon();
}

void ArchiveTab::storeComment()
{
    if (commentId == 0) return;
    if (auto* en = arch->byId (commentId); en != nullptr && en->comment != comment.getText())
        arch->setComment (*en, comment.getText());
}

void ArchiveTab::timerCallback()
{
    if (arch->version() != shownVersion) { shownVersion = arch->version(); rebuildPending = true; }
    const auto st = arch->scanStatus();
    const bool scanning = arch->isScanning();
    stopBtn.setEnabled (scanning);
    scanBtn.setEnabled (! scanning);
    if (rebuildPending && isShowing())
    {
        rebuildPending = false;
        rebuildRows();
    }
    const int pool = arch->countPool();
    juce::String s;
    s << (int) arch->entries().size() - pool << " Module";
    if (filter.getSelectedId() != 5 && (int) rows.size() != (int) arch->entries().size() - pool) s << ", " << (int) rows.size() << " angezeigt";
    if (pool > 0) s << " · " << pool << juce::String::fromUTF8 (" im Sammelpool (Filter rechts)");
    if (arch->indexing()) s << juce::String::fromUTF8 (" · Dublettenindex wird aufgebaut …");
    else s << " · " << arch->duplicateGroups() << " Dubletten-Gruppen";
    if (st.isNotEmpty()) s << " · " << st;
    if (arch->folders.isEmpty()) s = juce::String::fromUTF8 ("Noch keine Ordner im Archiv – „Ordner hinzufügen …“ wählen oder einen Ordner ins Fenster ziehen");
    if (s != status) { status = s; repaint (0, 34, getWidth(), 22); }
}

archive::Entry* ArchiveTab::entryAt (int row) const
{
    return row >= 0 && row < (int) rows.size() ? arch->byId (rows[(size_t) row]) : nullptr;
}

juce::String ArchiveTab::dupText (const Entry& e) const
{
    if (e.group == 0) return {};
    juce::String s = juce::String (e.groupSize) + juce::String::fromUTF8 ("× ");
    switch (e.dupKind)
    {
        case dupIdentical: s << "identisch"; break;
        case dupSameNotes: s << "gleiche Noten"; break;
        case dupSimilar: s << juce::String::fromUTF8 ("ähnlich ") << juce::roundToInt (e.bestScore * 100.0f) << " %"; break;
        default: break;
    }
    return s;
}

void ArchiveTab::rebuildRows()
{
    const int keepId = selectedId;
    juce::StringArray words;
    words.addTokens (search.getText().toLowerCase(), " ", "\"");
    words.removeEmptyStrings();
    const int fl = filter.getSelectedId();
    rows.clear();
    for (auto& e : arch->entries())
    {
        if (fl == 2 && e->group == 0) continue;
        if (fl == 3 && ! e->favourite) continue;
        if (fl == 4 && e->rating == 0) continue;
        if ((fl == 5) != e->inPool()) continue;   // pool entries only in the pool view
        if (! words.isEmpty())
        {
            const auto hay = (e->title + "\n" + e->name + "\n" + e->artist + "\n" + e->composer + "\n" + e->arranger + "\n" + e->key + "\n" + e->scale + "\n" + e->format + "\n" + e->sampleText + "\n" + e->message + "\n" + e->comment + "\n" + e->folder).toLowerCase();
            bool ok = true;
            for (auto& w : words) ok &= hay.contains (w);
            if (! ok) continue;
        }
        rows.push_back (e->id);
    }
    const int col = table.getHeader().getSortColumnId();
    const bool fwd = table.getHeader().isSortedForwards();
    auto& a = *arch;
    auto cmpStr = [] (const juce::String& x, const juce::String& y) { return x.compareNatural (y); };
    std::stable_sort (rows.begin(), rows.end(), [&] (int ia, int ib) {
        const auto* x = a.byId (ia); const auto* y = a.byId (ib);
        int c = 0;
        switch (col)
        {
            case cFav: c = (int) x->favourite - (int) y->favourite; break;
            case cRating: c = x->rating - y->rating; break;
            case cFormat: c = cmpStr (x->format, y->format); break;
            case cChannels: c = x->channels - y->channels; break;
            case cLength: c = x->seconds < y->seconds ? -1 : x->seconds > y->seconds ? 1 : 0; break;
            case cNotes: case cProfile: c = x->notes - y->notes; break;
            case cSamples: c = x->samples - y->samples; break;
            case cArtist: c = cmpStr (x->artist, y->artist); break;
            case cDup:
                if ((x->group == 0) != (y->group == 0)) return x->group != 0;   // groups always first
                c = x->group - y->group;
                if (c == 0) return x->path.compareNatural (y->path) < 0;
                break;
            case cFile: c = cmpStr (x->name, y->name); break;
            case cFolder: c = cmpStr (x->folder, y->folder); if (c == 0) c = cmpStr (x->name, y->name); break;
            case cSize: c = x->size < y->size ? -1 : x->size > y->size ? 1 : 0; break;
            case cComposer: c = cmpStr (x->composer, y->composer); break;
            case cArranger: c = cmpStr (x->arranger, y->arranger); break;
            case cKey: c = cmpStr (x->key, y->key); break;
            case cMode: c = cmpStr (x->mode, y->mode); break;
            case cScale: c = cmpStr (x->scale, y->scale); break;
            default: c = cmpStr (x->title, y->title); break;
        }
        return fwd ? c < 0 : c > 0;
    });
    table.updateContent();
    // keep the selection on the same module
    int newRow = -1;
    for (int i = 0; i < (int) rows.size(); ++i) if (rows[(size_t) i] == keepId) { newRow = i; break; }
    if (newRow >= 0)
    {
        if (table.getSelectedRow() != newRow) table.selectRow (newRow, true, true);
    }
    else if (keepId != 0) { table.deselectAllRows(); selectedId = 0; showSelected(); }
    table.repaint();
    if (selectedId != 0) showSelected();
    if (cellEditor != nullptr)   // rows moved (scan, sorting): follow the edited entry or close
    {
        int r = -1;
        for (int i = 0; i < (int) rows.size(); ++i) if (rows[(size_t) i] == editId) { r = i; break; }
        if (r < 0) finishEdit (false, false);
        else
        {
            auto* content = table.getViewport()->getViewedComponent();
            cellEditor->setBounds (content->getLocalArea (&table, table.getCellPosition (editCol, r, true)).expanded (0, 1));
            cursorRow = r;
        }
    }
}

void ArchiveTab::selectedRowsChanged (int lastRow)
{
    if (lastRow >= 0)
    {
        cursorRow = lastRow;
        if (! programmaticSelection && table.getNumSelectedRows() == 1) anchorRow = lastRow;
        // keyboard / click in the table: the cursor cell gets its editor right away
        const bool tableInUse = table.hasKeyboardFocus (true) || (cellEditor != nullptr && cellEditor->hasKeyboardFocus (true));
        if (! programmaticSelection && tableInUse && isEditableCol (curCol))
            later ([this, lastRow] { if (cursorRow == lastRow && ! (cellEditor != nullptr && editId == (entryAt (lastRow) ? entryAt (lastRow)->id : -1) && editCol == curCol)) openAt (lastRow, curCol); });
    }
    table.repaint();
    auto* en = entryAt (lastRow);
    const int id = en != nullptr ? en->id : 0;
    if (id == selectedId) return;
    storeComment();
    selectedId = id;
    showSelected();
}

void ArchiveTab::showSelected()
{
    auto* en = arch->byId (selectedId);
    notDupBtn.setEnabled (en != nullptr && en->bestMatch != 0);
    for (auto* b : { &openBtn, &revealBtn, &copyBtn }) b->setEnabled (en != nullptr);
    if (en == nullptr)
    {
        info.setText ({}, false);
        comment.setText ({}, false);
        commentId = 0;
        overview.show ({}, {}, {});
        return;
    }
    juce::String t;
    t << "Titel: " << en->title << "\n";
    if (en->artist.isNotEmpty()) t << juce::String::fromUTF8 ("Künstler: ") << en->artist << "\n";
    if (en->composer.isNotEmpty()) t << "Komponist: " << en->composer << "\n";
    if (en->arranger.isNotEmpty()) t << "Arrangeur: " << en->arranger << "\n";
    if (en->key.isNotEmpty()) t << "Tonart: " << en->key << " (" << en->scale << ")\n";
    t << "Format: " << en->formatName << "\n";
    if (en->has (fNotModule)) t << juce::String::fromUTF8 ("Nicht lesbar: ") << en->error << "\n";
    else if (en->has (fError)) t << "Fehler: " << en->error << "\n";
    else
        t << juce::String::fromUTF8 ("Länge: ") << formatDuration (en->seconds) << ", " << en->notes << " Noten, " << en->patterns << " Patterns, "
          << en->samples << " Samples" << (en->instruments > 0 ? ", " + juce::String (en->instruments) + " Instrumente" : juce::String()) << "\n";
    t << "Datei: " << en->path << (en->has (fMissing) ? juce::String::fromUTF8 (" (fehlt)") : juce::String())
      << (arch->isProtected (en->path) ? juce::String::fromUTF8 (" (schreibgeschützter Ordner)") : juce::String()) << "\n";
    if (en->group != 0)
    {
        t << "\nDubletten (" << dupText (*en) << "):\n";
        auto links = arch->linksOf (en->id);
        std::sort (links.begin(), links.end(), [] (auto& x, auto& y) { return x.kind != y.kind ? x.kind > y.kind : x.score > y.score; });
        for (auto& l : links)
            if (auto* o = arch->byId (l.other))
                t << "  " << (l.kind == dupIdentical ? juce::String ("identisch") : l.kind == dupSameNotes ? juce::String ("gleiche Noten")
                                                     : juce::String::fromUTF8 ("ähnlich ") + juce::String (juce::roundToInt (l.score * 100.0f)) + " %")
                  << ": " << o->path << "\n";
        const int others = en->groupSize - 1 - (int) links.size();
        if (others > 0) t << juce::String::fromUTF8 ("  … und ") << others << " weitere in der Gruppe\n";
    }
    if (en->message.isNotEmpty()) t << "\n--- Nachricht ---\n" << en->message << "\n";
    if (en->sampleText.isNotEmpty()) t << "\n--- Samplenamen ---\n" << en->sampleText << "\n";
    if (info.getText() != t) { info.setText (t, false); info.moveCaretToTop (false); }
    if (commentId != en->id)
    {
        comment.setText (en->comment, false);
        commentId = en->id;
    }
    juce::String matchPath, label;
    if (auto* m = arch->byId (en->bestMatch))
    {
        matchPath = m->path;
        label = en->dupKind == dupIdentical ? juce::String ("Identische Datei") : en->dupKind == dupSameNotes ? juce::String ("Gleiche Noten")
              : juce::String::fromUTF8 ("Ähnlichste Datei (") + juce::String (juce::roundToInt (en->bestScore * 100.0f)) + " %)";
    }
    overview.show (en->path, matchPath, label);
}

void ArchiveTab::openAndPlay (int id)
{
    auto* en = arch->byId (id);
    if (en == nullptr) return;
    const juce::File f (en->path);
    if (! f.existsAsFile()) { ed.setStatus (juce::String::fromUTF8 ("Datei nicht gefunden: ") + en->path); return; }
    ed.openFile (f);
    ed.playSong (false);
    table.grabKeyboardFocus();
}

void ArchiveTab::paintRowBackground (juce::Graphics& g, int row, int, int, bool selected)
{
    const auto& th = ed.theme();
    if (selected) g.fillAll (th.accent.withAlpha (0.30f));
    else if (row % 2) g.fillAll (th.panel2.withAlpha (0.35f));
}

void ArchiveTab::paintCell (juce::Graphics& g, int row, int col, int w, int h, bool)
{
    auto* e = entryAt (row);
    if (e == nullptr) return;
    const auto& th = ed.theme();
    const bool off = e->inPool();
    const bool prot = arch->isProtected (e->path);
    const auto base = prot ? th.text.interpolatedWith (juce::Colour (0xffe5484d), 0.5f) : th.text;
    g.setColour (off ? base.interpolatedWith (th.bg, 0.4f) : base);
    g.setFont (font (13.0f));
    auto text = [&] (const juce::String& s, juce::Justification j = juce::Justification::centredLeft) { g.drawText (s, 4, 0, w - 8, h, j, true); };
    if (row == cursorRow && col == curCol && table.isRowSelected (row))   // cell cursor
    {
        const auto c = g.getCurrentFont();
        g.saveState();
        g.setColour (th.accent.withAlpha (table.hasKeyboardFocus (true) ? 1.0f : 0.5f));
        g.drawRoundedRectangle (1.0f, 1.0f, (float) w - 2.0f, (float) h - 2.0f, 3.0f, 1.6f);
        g.restoreState();
        g.setFont (c);
    }
    switch (col)
    {
        case cFav:
            g.setColour (e->favourite ? th.accent : th.dim.withAlpha (0.35f));
            g.drawText (juce::String::fromUTF8 (e->favourite ? "♥" : "♡"), 0, 0, w, h, juce::Justification::centred);
            break;
        case cRating:
        {
            const float sw = (float) (w - 6) / 5.0f;
            for (int i = 0; i < 5; ++i)
            {
                g.setColour (i < e->rating ? th.accent : th.dim.withAlpha (0.3f));
                g.drawText (juce::String::fromUTF8 (i < e->rating ? "★" : "☆"), juce::Rectangle<float> (3.0f + sw * (float) i, 0.0f, sw, (float) h).toNearestInt(), juce::Justification::centred);
            }
            break;
        }
        case cTitle: text (e->title); break;
        case cFormat: text (e->format); break;
        case cChannels: text (e->channels > 0 ? juce::String (e->channels) : juce::String(), juce::Justification::centredRight); break;
        case cLength: text (e->seconds > 0 ? formatDuration (e->seconds) : juce::String(), juce::Justification::centredRight); break;
        case cNotes: text (juce::String (e->notes), juce::Justification::centredRight); break;
        case cSamples: text (juce::String (e->samples), juce::Justification::centredRight); break;
        case cArtist: text (e->artist); break;
        case cComposer: text (e->composer); break;
        case cArranger: text (e->arranger); break;
        case cKey: text (e->key); break;
        case cMode: text (e->mode); break;
        case cScale: text (e->scale); break;
        case cDup:
            if (e->group != 0) g.setColour (e->dupKind == dupIdentical ? th.track[0] : e->dupKind == dupSameNotes ? th.track[1] : th.track[3]);
            text (off ? (e->has (fExcluded) ? juce::String ("ausgeschlossen") : e->has (fMissing) ? juce::String ("fehlt") : e->has (fNotModule) ? juce::String ("unbek. Format") : juce::String ("Fehler")) : dupText (*e));
            break;
        case cProfile:
        {
            const float bw = (float) (w - 4) / (float) kBins;
            for (int b = 0; b < kBins; ++b)
            {
                float y = (float) h - 3.0f;
                for (int l = 0; l < kLanes; ++l)
                {
                    const float v = e->profile[(size_t) (b * kLanes + l)] / 255.0f * (float) (h - 6);
                    if (v <= 0) continue;
                    g.setColour (th.track[(size_t) l].withAlpha (0.9f));
                    g.fillRect (2.0f + bw * (float) b, y - v, juce::jmax (1.0f, bw - 0.5f), v);
                    y -= v;
                }
            }
            break;
        }
        case cFile: text (e->name); break;
        case cFolder: text (e->folder); break;
        case cSize: text (juce::String (juce::jmax ((juce::int64) 1, e->size / 1024)) + " KB", juce::Justification::centredRight); break;
        default: break;
    }
}

juce::String ArchiveTab::getCellTooltip (int row, int col)
{
    auto* e = entryAt (row);
    if (e == nullptr) return {};
    if (col == cRating) return juce::String::fromUTF8 ("Klick auf einen Stern setzt die Bewertung, Klick auf den gesetzten Stern löscht sie");
    if (col == cFav) return juce::String::fromUTF8 ("Favorit an/aus");
    if (col == cProfile) return juce::String::fromUTF8 ("Notendichte über die Spielzeit, gestapelt nach Samples");
    if (col == cDup && e->group != 0) return juce::String::fromUTF8 ("Gruppe mit ") + juce::String (e->groupSize) + juce::String::fromUTF8 (" Dateien – Details unten rechts");
    if (col == cTitle && e->sampleText.isNotEmpty()) return e->sampleText.substring (0, 600);
    if (col == cFolder || col == cFile) return e->path;
    return {};
}

void ArchiveTab::cellClicked (int row, int col, const juce::MouseEvent& m)
{
    if (m.mods.isPopupMenu())
    {
        if (! table.isRowSelected (row)) table.selectRow (row);
        rowMenu (row);
        return;
    }
    auto* e = entryAt (row);
    if (e == nullptr) return;
    curCol = col;
    if (! m.mods.isShiftDown()) anchorRow = row;
    later ([this, row, col] { openAt (row, col); });   // click = cursor (and editor) in exactly this cell
    if (col == cFav) arch->setFavourite (*e, ! e->favourite);
    else if (col == cRating)
    {
        auto& h = table.getHeader();
        const auto colRect = h.getColumnPosition (h.getIndexOfColumnId (cRating, true));
        const auto x = h.getLocalPoint (nullptr, m.getScreenPosition()).x - colRect.getX();
        const int stars = juce::jlimit (0, 5, (int) ((float) (x - 3) / ((float) (colRect.getWidth() - 6) / 5.0f)) + 1);
        arch->setRating (*e, stars == e->rating ? 0 : stars);
    }
}

void ArchiveTab::cellDoubleClicked (int row, int col, const juce::MouseEvent&)
{
    if (col == cFav || col == cRating) return;
    if (auto* e = entryAt (row)) openAndPlay (e->id);
}

void ArchiveTab::returnKeyPressed (int row)
{
    if (auto* e = entryAt (row)) openAndPlay (e->id);
}

void ArchiveTab::sortOrderChanged (int, bool)
{
    rebuildPending = true;
    saveView();
}

void ArchiveTab::rowMenu (int row)
{
    auto* e = entryAt (row);
    if (e == nullptr) return;
    std::vector<int> ids;
    for (int i = 0; i < table.getNumSelectedRows(); ++i) if (auto* x = entryAt (table.getSelectedRow (i))) ids.push_back (x->id);
    juce::PopupMenu m, rate;
    m.addItem (1, juce::String::fromUTF8 ("In Spurwerk öffnen und spielen"));
    m.addItem (2, "Im Explorer zeigen");
    m.addItem (3, "Samplenamen kopieren", e->sampleText.isNotEmpty());
    m.addItem (4, "Dateipfad kopieren");
    m.addSeparator();
    for (int s = 0; s <= 5; ++s)
        rate.addItem (10 + s, s == 0 ? juce::String ("keine") : juce::String::repeatedString (juce::String::fromUTF8 ("★"), s), true, e->rating == s);
    m.addSubMenu ("Bewertung", rate);
    m.addItem (5, "Favorit", true, e->favourite);
    m.addSeparator();
    m.addItem (6, juce::String::fromUTF8 ("Kein Duplikat (mit der ähnlichsten Datei)"), e->bestMatch != 0);
    m.addItem (7, ids.size() > 1 ? juce::String::fromUTF8 ("Nur diese Dubletten-Gruppe zeigen") : juce::String::fromUTF8 ("Dubletten-Gruppe zeigen"), e->group != 0);
    m.addSeparator();
    int inPool = 0;
    for (int i : ids) if (auto* x = arch->byId (i); x != nullptr && x->has (fExcluded)) ++inPool;
    if (inPool < (int) ids.size())
        m.addItem (9, juce::String::fromUTF8 ("Ausschließen (in den Sammelpool)") + (ids.size() > 1 ? " – " + juce::String ((int) ids.size() - inPool) + " Dateien" : juce::String()));
    if (inPool > 0)
        m.addItem (8, juce::String::fromUTF8 ("Wieder aufnehmen (aus dem Sammelpool)") + (inPool > 1 ? " – " + juce::String (inPool) + " Dateien" : juce::String()));
    m.addSeparator();
    {
        int deletable = 0;
        for (int i : ids) if (auto* x = arch->byId (i); x != nullptr && ! arch->isProtected (x->path)) ++deletable;
        juce::PopupMenu::Item del (deletable == 0 ? juce::String::fromUTF8 ("Löschen gesperrt (schreibgeschützter Ordner)")
                                   : deletable == 1 ? juce::String::fromUTF8 ("Datei von der Festplatte löschen …")
                                                    : juce::String (deletable) + juce::String::fromUTF8 (" Dateien von der Festplatte löschen …"));
        del.itemID = 20;
        del.isEnabled = deletable > 0;
        del.colour = juce::Colour (0xffe5484d);
        m.addItem (del);
    }
    m.addSeparator();
    m.addItem (30, juce::String::fromUTF8 ("Lupe …"));
    juce::Component::SafePointer<ArchiveTab> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options(), [safe, id = e->id, ids] (int r) {
        if (safe == nullptr || r == 0) return;
        auto& a = *safe->arch;
        auto* en = a.byId (id);
        if (en == nullptr) return;
        if (r == 1) safe->openAndPlay (id);
        else if (r == 2) juce::File (en->path).revealToUser();
        else if (r == 3) juce::SystemClipboard::copyTextToClipboard (en->sampleText);
        else if (r == 4) juce::SystemClipboard::copyTextToClipboard (en->path);
        else if (r == 5) { archive::Archive::UndoGroup g (a, "Favorit"); const bool v = ! en->favourite; for (int i : ids) if (auto* x = a.byId (i)) a.setFavourite (*x, v); }
        else if (r == 6 && en->bestMatch != 0) a.markNotDuplicate (en->id, en->bestMatch);
        else if (r == 7)
        {
            safe->filter.setSelectedId (2, juce::dontSendNotification);
            safe->table.getHeader().setSortColumnId (cDup, true);
            safe->search.setText ({}, false);
            safe->rebuildPending = true;
        }
        else if (r == 8) a.setExcluded (ids, false);
        else if (r == 9) a.setExcluded (ids, true);   // files stay untouched; hidden, no duplicates, skipped while unchanged
        else if (r == 20) safe->confirmDelete (ids);
        else if (r == 30) safe->ed.showLoupeMenu();
        else if (r >= 10 && r <= 15) { archive::Archive::UndoGroup g (a, "Bewertung"); for (int i : ids) if (auto* x = a.byId (i)) a.setRating (*x, r - 10); }
    });
}

void ArchiveTab::confirmDelete (const std::vector<int>& ids)
{
    juce::StringArray names;
    std::vector<int> allowed;
    int blocked = 0;
    for (int i : ids)
        if (auto* x = arch->byId (i))
        {
            if (arch->isProtected (x->path)) { ++blocked; continue; }   // never delete in write-protected folders
            names.add (x->path);
            allowed.push_back (i);
        }
    if (names.isEmpty()) { ed.setStatus (juce::String::fromUTF8 ("Löschen gesperrt: schreibgeschützter Ordner")); return; }
    const auto ids2 = allowed;
    juce::String list;
    for (int i = 0; i < juce::jmin (8, names.size()); ++i) list << names[i] << "\n";
    if (names.size() > 8) list << juce::String::fromUTF8 ("… und ") << (names.size() - 8) << " weitere\n";
    juce::Component::SafePointer<ArchiveTab> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon,
        names.size() == 1 ? juce::String::fromUTF8 ("Datei löschen?") : juce::String (names.size()) + juce::String::fromUTF8 (" Dateien löschen?"),
        list + juce::String::fromUTF8 ("\nDie Datei(en) werden in den Papierkorb verschoben und aus dem Archiv entfernt. "
                                       "Auf Laufwerken ohne Papierkorb (z. B. Netzlaufwerke) ist das Löschen endgültig.")
            + (blocked > 0 ? juce::String::fromUTF8 ("\n\n") + juce::String (blocked) + juce::String::fromUTF8 (" Datei(en) in schreibgeschützten Ordnern bleiben unangetastet.") : juce::String()),
        juce::String::fromUTF8 ("Löschen"), "Abbrechen", nullptr,
        juce::ModalCallbackFunction::create ([safe, ids = ids2] (int ok) {
            if (! ok || safe == nullptr) return;
            auto& a = *safe->arch;
            std::vector<int> gone;
            int failed = 0;
            for (int i : ids)
                if (auto* x = a.byId (i))
                {
                    const juce::File f (x->path);
                    if (! f.existsAsFile() || f.moveToTrash()) gone.push_back (i);
                    else ++failed;
                }
            a.removeEntries (gone);
            safe->ed.setStatus (juce::String ((int) gone.size()) + juce::String::fromUTF8 (" Datei(en) gelöscht")
                                + (failed > 0 ? ", " + juce::String (failed) + juce::String::fromUTF8 (" ließen sich nicht löschen") : juce::String()));
        }));
}

juce::String ArchiveTab::copyTest (int from, int to, bool viaButton)
{
    juce::SystemClipboard::copyTextToClipboard ({});
    info.setHighlightedRegion ({ from, to });
    if (viaButton) copyBtn.onClick();   // what a click does (triggerClick would run later)
    else keyPressed (juce::KeyPress (juce::KeyPress::insertKey, juce::ModifierKeys::ctrlModifier, 0), &info);
    return juce::SystemClipboard::getTextFromClipboard();
}

juce::String ArchiveTab::cursorTest()
{
    syncForTest = true;
    juce::String log;
    auto edKey = [this] (int code, juce::ModifierKeys mods = {}, juce::juce_wchar ch = 0) { if (cellEditor != nullptr) cellEditor->keyPressed (juce::KeyPress (code, mods, ch)); };
    auto where = [this] {
        juce::String w;
        w << "editor " << (cellEditor != nullptr ? "open" : "closed") << " row " << cursorRow << " col " << table.getHeader().getColumnName (curCol);
        if (cellEditor != nullptr) w << " text '" << cellEditor->getText() << "' selected '" << cellEditor->getHighlightedText() << "'";
        return w;
    };
    // 1) click into Komponist of row 2: editor there immediately, content selected
    table.selectRow (2);
    curCol = cComposer;
    openAt (2, cComposer);
    log << "click row 2 Komponist: " << where() << juce::newLine;
    // 2) unchanged: down / right move to the neighbour cell
    edKey (juce::KeyPress::downKey);
    log << "down: " << where() << juce::newLine;
    edKey (juce::KeyPress::rightKey);
    log << "right: " << where() << juce::newLine;
    edKey (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier);
    log << "shift+up: " << where() << ", selected rows " << table.getNumSelectedRows() << juce::newLine;
    // 3) changed: arrows stay in the field, Enter writes
    table.selectRow (0);
    openAt (0, cComposer);
    cellEditor->insertTextAtCaret ("Testkomponist");
    edKey (juce::KeyPress::downKey);
    log << "after typing, down: " << where() << juce::newLine;
    edKey (juce::KeyPress::returnKey);
    if (auto* e0 = entryAt (0)) log << "Enter: row 0 composer '" << e0->composer << "', now " << where() << juce::newLine;
    // 4) range 1..3 by shift: cursor in the last marked row
    table.selectRow (1);
    anchorRow = 1;
    openAt (1, cArranger);
    edKey (juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier);
    edKey (juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier);
    log << "shift+down x2 from row 1: " << where() << ", selected rows " << table.getNumSelectedRows() << juce::newLine;
    // 5) several rows: type + Enter sets all of them
    if (cellEditor != nullptr) { cellEditor->selectAll(); cellEditor->insertTextAtCaret ("Jesper Kyd"); }
    edKey (juce::KeyPress::returnKey);
    juce::String all;
    for (int r = 1; r <= 3; ++r) if (auto* x = entryAt (r)) all << "'" << x->arranger << "' ";
    log << "Enter on rows 1..3: arranger " << all << ", now " << where() << juce::newLine;
    syncForTest = false;
    return log;
}

void ArchiveTab::doUndo()
{
    const bool editing = cellEditor != nullptr;
    const int row = cursorRowOrLast(), col = curCol;
    finishEdit (false, false);
    const auto label = arch->undo();
    ed.setStatus (label.isEmpty() ? juce::String::fromUTF8 ("Nichts rückgängig zu machen") : juce::String::fromUTF8 ("Rückgängig: ") + label);
    rebuildRows();
    if (editing && row >= 0) openAt (juce::jmin (row, getNumRows() - 1), col);
}

void ArchiveTab::copyInfo()
{
    const auto sel = info.getHighlightedText();
    const auto t = sel.isNotEmpty() ? sel : info.getText();
    if (t.isEmpty()) return;
    juce::SystemClipboard::copyTextToClipboard (t);
    ed.setStatus (sel.isNotEmpty() ? juce::String::fromUTF8 ("Markierter Text kopiert") : juce::String::fromUTF8 ("Ganzer Infotext kopiert"));
}

bool ArchiveTab::keyPressed (const juce::KeyPress& k, juce::Component* from)
{
    if (from != &info) return false;
    const int code = k.getKeyCode();
    if (k.getModifiers().isCommandDown() && (code == 'C' || code == 'c' || code == juce::KeyPress::insertKey)) { copyInfo(); return true; }
    if (k.getModifiers().isCommandDown() && (code == 'A' || code == 'a')) { info.selectAll(); return true; }
    return false;
}

int ArchiveTab::cursorRowOrLast() const
{
    if (cursorRow >= 0 && cursorRow < (int) rows.size()) return cursorRow;
    return table.getNumSelectedRows() > 0 ? table.getSelectedRow (table.getNumSelectedRows() - 1) : -1;
}

void ArchiveTab::later (std::function<void()> fn)
{
    if (syncForTest) { fn(); return; }
    juce::Component::SafePointer<ArchiveTab> safe (this);
    juce::MessageManager::callAsync ([safe, fn] { if (safe != nullptr) fn(); });
}

int ArchiveTab::columnStep (int col, int delta) const
{
    auto& h = table.getHeader();
    const int n = h.getNumColumns (true);
    if (n == 0) return col;
    const int idx = juce::jlimit (0, n - 1, juce::jmax (0, h.getIndexOfColumnId (col, true)) + delta);
    return h.getColumnIdOfIndex (idx, true);
}

void ArchiveTab::scrollColumnIntoView (int col)
{
    auto& h = table.getHeader();
    const int idx = h.getIndexOfColumnId (col, true);
    if (idx < 0) return;
    auto* vp = table.getViewport();
    const auto r = h.getColumnPosition (idx);
    const int x = vp->getViewPositionX(), w = vp->getViewWidth();
    if (r.getX() < x) vp->setViewPosition (r.getX(), vp->getViewPositionY());
    else if (r.getRight() > x + w) vp->setViewPosition (r.getRight() - w, vp->getViewPositionY());
}

void ArchiveTab::selectRowsProgrammatically (int from, int to)
{
    programmaticSelection = true;
    if (from == to) table.selectRow (to);
    else
    {
        table.selectRangeOfRows (from, to, true);
        table.selectRow (to, true, false);   // keep the range, but make `to` the last selected row
    }
    programmaticSelection = false;
    cursorRow = to;
    table.scrollToEnsureRowIsOnscreen (to);
}

void ArchiveTab::navigate (int dRow, int dCol, bool extend)
{
    const int n = getNumRows();
    if (n == 0) return;
    const int row = juce::jlimit (0, n - 1, cursorRowOrLast() + dRow);
    const int col = dCol != 0 ? columnStep (curCol, dCol) : curCol;
    finishEdit (false, false);   // unchanged cell: nothing to write
    if (dRow != 0)
    {
        if (extend) { if (anchorRow < 0) anchorRow = cursorRowOrLast(); selectRowsProgrammatically (anchorRow, row); }
        else { anchorRow = row; selectRowsProgrammatically (row, row); }
    }
    openAt (row, col);
}

bool ArchiveTab::keyPressed (const juce::KeyPress& k)
{
    // table has the focus (non-editable column or no editor open)
    if (cellEditor != nullptr) return false;
    const int code = k.getKeyCode();
    if (code == juce::KeyPress::leftKey) { navigate (0, -1, false); return true; }
    if (code == juce::KeyPress::rightKey) { navigate (0, 1, false); return true; }
    if (code == juce::KeyPress::tabKey) { navigate (0, k.getModifiers().isShiftDown() ? -1 : 1, false); return true; }
    if (code == juce::KeyPress::F2Key && table.getNumSelectedRows() > 0) { openAt (cursorRowOrLast(), isEditableCol (curCol) ? curCol : cTitle); return true; }
    if (k.getModifiers().isCommandDown() && (code == 'Z' || code == 'z')) { doUndo(); return true; }
    return false;
}

bool ArchiveTab::editorKey (const juce::KeyPress& k)
{
    const int code = k.getKeyCode();
    const bool shift = k.getModifiers().isShiftDown();
    if (suggest != nullptr && suggest->isVisible())
    {
        auto& sb = *suggest;
        if (code == juce::KeyPress::downKey) { sb.selected = juce::jmin (sb.items.size() - 1, sb.selected + 1); sb.repaint(); return true; }
        if (code == juce::KeyPress::upKey) { sb.selected = juce::jmax (-1, sb.selected - 1); sb.repaint(); return true; }
        if (code == juce::KeyPress::escapeKey) { hideSuggestions(); return true; }
        if ((code == juce::KeyPress::returnKey || code == juce::KeyPress::tabKey) && sb.selected >= 0)
        {
            const int i = sb.selected;
            later ([this, i] { acceptSuggestion (i); });
            return true;
        }
    }
    if (code == juce::KeyPress::returnKey)
    {
        if (isModified()) later ([this] { commitAndMoveOn (table.getNumSelectedRows() <= 1 ? 1 : 0, 0); });
        else later ([this] { navigate (1, 0, false); });
        return true;
    }
    if (code == juce::KeyPress::escapeKey)
    {
        if (isModified() && cellEditor != nullptr)
        {
            auto* e = arch->byId (editId);
            cellEditor->setText (e == nullptr ? juce::String() : editCol == cTitle ? e->title : editCol == cArtist ? e->artist : editCol == cComposer ? e->composer : e->arranger, false);
            cellEditor->selectAll();
            }
        return true;
    }
    if (code == juce::KeyPress::tabKey)
    {
        const int d = shift ? -1 : 1;
        if (isModified()) later ([this, d] { commitAndMoveOn (0, d); });
        else later ([this, d] { navigate (0, d, false); });
        return true;
    }
    if (! isModified() && k.getModifiers().isCommandDown() && (code == 'Z' || code == 'z')) { later ([this] { doUndo(); }); return true; }   // changed text: the field's own undo
    const bool up = code == juce::KeyPress::upKey, down = code == juce::KeyPress::downKey;
    const bool left = code == juce::KeyPress::leftKey, right = code == juce::KeyPress::rightKey;
    if (! isModified())
    {
        if (up || down) { const int d = up ? -1 : 1; later ([this, d, shift] { navigate (d, 0, shift); }); return true; }
        if ((left || right) && ! shift) { const int d = left ? -1 : 1; later ([this, d] { navigate (0, d, false); }); return true; }
        if (code == juce::KeyPress::pageUpKey || code == juce::KeyPress::pageDownKey)
        {
            const int d = (code == juce::KeyPress::pageUpKey ? -1 : 1) * juce::jmax (1, table.getHeight() / juce::jmax (1, table.getRowHeight()) - 1);
            later ([this, d, shift] { navigate (d, 0, shift); });
            return true;
        }
        return false;
    }
    if (up || down) return true;   // changed: the field keeps the cursor until Enter
    return false;
}

void ArchiveTab::hideSuggestions()
{
    if (suggest != nullptr) suggest->setVisible (false);
}

void ArchiveTab::updateSuggestions()
{
    if (cellEditor == nullptr || ! isModified()) { hideSuggestions(); return; }
    const auto q = cellEditor->getText().trim();
    if (q.isEmpty()) { hideSuggestions(); return; }
    std::map<juce::String, int> count;
    for (auto& e : arch->entries())
    {
        const auto& v = editCol == cTitle ? e->title : editCol == cArtist ? e->artist : editCol == cComposer ? e->composer : e->arranger;
        if (v.isNotEmpty() && v.containsIgnoreCase (q) && v != q) ++count[v];
    }
    std::vector<std::pair<juce::String, int>> list (count.begin(), count.end());
    std::sort (list.begin(), list.end(), [&q] (const auto& a, const auto& b) {
        const bool sa = a.first.startsWithIgnoreCase (q), sb = b.first.startsWithIgnoreCase (q);
        if (sa != sb) return sa;
        if (a.second != b.second) return a.second > b.second;
        return a.first.compareNatural (b.first) < 0;
    });
    if (list.empty()) { hideSuggestions(); return; }
    if (suggest == nullptr) { suggest = std::make_unique<SuggestBox> (*this); addChildComponent (*suggest); }
    suggest->items.clear();
    for (size_t i = 0; i < list.size() && i < 8; ++i) suggest->items.add (list[i].first);
    suggest->query = q;
    suggest->selected = -1;
    const auto er = getLocalArea (cellEditor->getParentComponent(), cellEditor->getBounds());
    const int h = suggest->items.size() * SuggestBox::rowH + 4, w = juce::jmax (er.getWidth(), 240);
    auto r = juce::Rectangle<int> (er.getX(), er.getBottom() + 1, w, h);
    if (r.getBottom() > getHeight()) r.setY (er.getY() - h - 1);   // no room below: above the field
    suggest->setBounds (r);
    suggest->setVisible (true);
    suggest->toFront (false);
    suggest->repaint();
}

void ArchiveTab::acceptSuggestion (int index)
{
    if (suggest == nullptr || cellEditor == nullptr || index < 0 || index >= suggest->items.size()) return;
    cellEditor->setText (suggest->items[index], false);
    hideSuggestions();
    commitAndMoveOn (table.getNumSelectedRows() <= 1 ? 1 : 0, 0);
}

juce::String ArchiveTab::suggestTest()
{
    syncForTest = true;
    juce::String log;
    // two artists that contain "Mart"
    if (getNumRows() >= 3)
    {
        if (auto* a = entryAt (1)) arch->setField (*a, archive::Archive::Field::artist, "Martin Galway");
        if (auto* b = entryAt (2)) arch->setField (*b, archive::Archive::Field::artist, "Bang Martinson");
    }
    table.selectRow (0);
    openAt (0, cArtist);
    if (cellEditor != nullptr) { cellEditor->setText ("Mart", false); updateSuggestions(); }
    log << "typed 'Mart': " << (suggest != nullptr && suggest->isVisible() ? suggest->items.joinIntoString (" | ") : juce::String ("(keine)")) << juce::newLine;
    if (cellEditor != nullptr) cellEditor->keyPressed (juce::KeyPress (juce::KeyPress::downKey));
    if (cellEditor != nullptr) cellEditor->keyPressed (juce::KeyPress (juce::KeyPress::downKey));
    log << "down x2 -> selected " << (suggest != nullptr ? suggest->selected : -9) << juce::newLine;
    if (cellEditor != nullptr) cellEditor->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    if (auto* e0 = entryAt (0)) log << "Enter: row 0 artist '" << e0->artist << "', cursor row " << cursorRow << juce::newLine;
    // Strg+Z: back to the old artist
    doUndo();
    if (auto* e0 = entryAt (0)) log << "Strg+Z: row 0 artist '" << e0->artist << "'" << juce::newLine;
    syncForTest = false;
    return log;
}

namespace
{
struct CellEditor : public juce::TextEditor
{
    std::function<bool (const juce::KeyPress&)> onKey;
    bool keyPressed (const juce::KeyPress& k) override { return (onKey && onKey (k)) || juce::TextEditor::keyPressed (k); }
};
} // namespace

void ArchiveTab::openAt (int row, int col)
{
    if (row < 0 || row >= getNumRows()) return;
    finishEdit (true, true);   // a changed single cell elsewhere is written, several rows need Enter
    cursorRow = row;
    curCol = col;
    table.scrollToEnsureRowIsOnscreen (row);
    scrollColumnIntoView (col);
    table.repaint();
    auto* e = entryAt (row);
    if (e == nullptr || ! isEditableCol (col) || ! table.getHeader().isColumnVisible (col)) { table.grabKeyboardFocus(); return; }
    editId = e->id;
    editCol = col;
    auto te = std::make_unique<CellEditor>();
    te->onKey = [this] (const juce::KeyPress& k) { return editorKey (k); };
    te->setFont (font (13.0f));
    te->setIndents (4, 3);
    te->setText (col == cTitle ? e->title : col == cArtist ? e->artist : col == cComposer ? e->composer : e->arranger, false);
    const auto& th = ed.theme();
    te->setColour (juce::TextEditor::outlineColourId, th.accent);
    te->setColour (juce::TextEditor::focusedOutlineColourId, th.accent);
    te->setColour (juce::TextEditor::backgroundColourId, th.bg);
    te->setColour (juce::TextEditor::highlightColourId, th.accent);
    te->setColour (juce::TextEditor::highlightedTextColourId, th.bg.getPerceivedBrightness() < 0.5f ? juce::Colours::black : juce::Colours::white);
    te->applyColourToAllText (th.text);
    te->onTextChange = [this] { updateSuggestions(); };
    te->onFocusLost = [this] { later ([this] {
        if (cellEditor == nullptr || cellEditor->hasKeyboardFocus (true)) return;
        if (suggest != nullptr && suggest->isVisible() && suggest->isMouseOverOrDragging()) return;   // click into the suggestions
        finishEdit (true, true); }); };
    auto* content = table.getViewport()->getViewedComponent();
    te->setAlwaysOnTop (true);
    content->addAndMakeVisible (*te);
    te->setBounds (content->getLocalArea (&table, table.getCellPosition (col, row, true)).expanded (0, 1));
    editOriginal = te->getText();
    cellEditor = std::move (te);
    cellEditor->grabKeyboardFocus();
    cellEditor->selectAll();
    later ([this] { if (cellEditor != nullptr && ! isModified()) { if (! cellEditor->hasKeyboardFocus (false)) cellEditor->grabKeyboardFocus(); cellEditor->selectAll(); } });
    const int n = table.getNumSelectedRows();
    if (n > 1) ed.setStatus (juce::String::fromUTF8 ("Eingabe + Enter setzt den Wert für alle ") + juce::String (n) + juce::String::fromUTF8 (" markierten Module"));
}

void ArchiveTab::commitAndMoveOn (int dRow, int dCol)
{
    if (cellEditor == nullptr) return;
    const int row = cursorRowOrLast();
    const int col = curCol;
    const bool single = table.getNumSelectedRows() <= 1;
    finishEdit (true, false);
    if (! single) return;   // several rows: the question decides, the cursor stays (reopened afterwards)
    const int nr = juce::jlimit (0, juce::jmax (0, getNumRows() - 1), row + dRow);
    if (dRow != 0) { anchorRow = nr; selectRowsProgrammatically (nr, nr); }
    openAt (nr, dCol != 0 ? columnStep (col, dCol) : col);
}

void ArchiveTab::finishEdit (bool commit, bool fromFocusLoss)
{
    hideSuggestions();
    if (cellEditor == nullptr) return;
    auto te = std::move (cellEditor);
    const auto value = te->getText().trim();
    const bool changed = te->getText() != editOriginal;   // cellEditor is already moved out here
    const int id = editId, col = editCol;
    te.reset();
    if (! commit || ! changed) return;
    static const auto fieldOf = [] (int c) {
        return c == cTitle ? archive::Archive::Field::title : c == cArtist ? archive::Archive::Field::artist
             : c == cComposer ? archive::Archive::Field::composer : archive::Archive::Field::arranger;
    };
    std::vector<int> ids;
    for (int i = 0; i < table.getNumSelectedRows(); ++i) if (auto* x = entryAt (table.getSelectedRow (i))) ids.push_back (x->id);
    if (std::find (ids.begin(), ids.end(), id) == ids.end()) ids = { id };
    auto current = [col] (const archive::Entry& x) { return col == cTitle ? x.title : col == cArtist ? x.artist : col == cComposer ? x.composer : x.arranger; };
    if (ids.size() <= 1)
    {
        if (auto* x = arch->byId (id); x != nullptr && current (*x) != value) arch->setField (*x, fieldOf (col), value);
        return;
    }
    if (fromFocusLoss) return;   // several rows: only Enter (with the question below) writes
    const auto colName = table.getHeader().getColumnName (col);
    juce::Component::SafePointer<ArchiveTab> safe (this);
    auto apply = [safe, ids, col, value] {
        if (safe == nullptr) return;
        {
            archive::Archive::UndoGroup g (*safe->arch, safe->table.getHeader().getColumnName (col) + juce::String::fromUTF8 (" für ") + juce::String ((int) ids.size()) + " Module");
            for (int i : ids) if (auto* x = safe->arch->byId (i)) safe->arch->setField (*x, fieldOf (col), value);
        }
        safe->ed.setStatus (juce::String ((int) ids.size()) + juce::String::fromUTF8 (" Module geändert"));
        safe->openAt (safe->cursorRowOrLast(), col);
    };
    if (syncForTest) { apply(); return; }
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, colName + juce::String::fromUTF8 (" für mehrere Module setzen"),
        colName + juce::String::fromUTF8 (" von ") + juce::String ((int) ids.size()) + juce::String::fromUTF8 (" markierten Modulen auf „") + value
            + juce::String::fromUTF8 ("“ setzen? (Nur in der Datenbank – die Dateien bleiben unverändert.)"),
        "Setzen", "Abbrechen", nullptr,
        juce::ModalCallbackFunction::create ([apply, safe, col] (int ok) {
            if (ok) apply();
            else if (safe != nullptr) safe->openAt (safe->cursorRowOrLast(), col);
        }));
}

void ArchiveTab::moreMenu()
{
    juce::PopupMenu m, folders, thr;
    for (int i = 0; i < arch->folders.size(); ++i)
        folders.addItem (100 + i, arch->folders[i] + "  (" + juce::String (arch->countInFolder (arch->folders[i])) + ")");
    if (arch->folders.isEmpty()) folders.addItem (99, "(keine)", false);
    m.addItem (40, arch->canUndo() ? juce::String::fromUTF8 ("Rückgängig: ") + arch->undoLabel() + "  (Strg+Z)" : juce::String::fromUTF8 ("Rückgängig (Strg+Z)"), arch->canUndo());
    m.addSeparator();
    m.addItem (1, juce::String::fromUTF8 ("Ordner hinzufügen …"));
    m.addSubMenu ("Ordner aus dem Archiv nehmen", folders, ! arch->folders.isEmpty());
    juce::PopupMenu prot;
    const auto protList = arch->protectedFolders();
    for (int i = 0; i < arch->folders.size(); ++i)
        prot.addItem (300 + i, arch->folders[i], true, protList.contains (arch->folders[i]));
    int extra = 0;
    for (int j = 0; j < protList.size(); ++j)
        if (! arch->folders.contains (protList[j])) { if (extra++ == 0) prot.addSeparator(); prot.addItem (400 + j, protList[j], true, true); }
    prot.addSeparator();
    prot.addItem (6, juce::String::fromUTF8 ("Anderen Ordner schützen …"));
    m.addSubMenu (juce::String::fromUTF8 ("Schreibschutz (Haken = geschützt)"), prot);
    m.addSeparator();
    m.addItem (2, "Alle Ordner einlesen (nur Neues)", ! arch->isScanning());
    m.addItem (3, juce::String::fromUTF8 ("Alle Dateien neu analysieren"), ! arch->isScanning());
    int missing = 0;
    for (auto& e : arch->entries()) if (e->has (fMissing)) ++missing;
    m.addItem (4, juce::String (missing) + juce::String::fromUTF8 (" fehlende Dateien aus dem Archiv entfernen"), missing > 0);
    m.addSeparator();
    const int cur = juce::roundToInt (arch->similarityThreshold() * 100.0f);
    for (int p : { 30, 40, 45, 50, 60, 70, 80 })
        thr.addItem (200 + p, juce::String (p) + " %" + (p == 45 ? juce::String (" (Standard)") : juce::String()), true, cur == p);
    m.addSubMenu (juce::String::fromUTF8 ("Ähnlichkeit für Dubletten"), thr);
    m.addItem (5, juce::String::fromUTF8 ("Datenbank im Explorer zeigen"));
    juce::Component::SafePointer<ArchiveTab> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&moreBtn), [safe] (int r) {
        if (safe == nullptr || r == 0) return;
        auto& a = *safe->arch;
        if (r == 40) safe->doUndo();
        else if (r == 1) safe->addBtn.triggerClick();
        else if (r == 2) a.startScan();
        else if (r == 3) a.startScan (true);
        else if (r == 4) a.removeMissing();
        else if (r == 5) a.databaseFile().revealToUser();
        else if (r >= 300 && r < 300 + a.folders.size()) { const auto f = a.folders[r - 300]; a.setProtected (f, ! a.protectedFolders().contains (f)); }
        else if (r >= 400 && r < 400 + a.protectedFolders().size()) a.setProtected (a.protectedFolders()[r - 400], false);
        else if (r == 6)
        {
            safe->chooser = std::make_unique<juce::FileChooser> (juce::String::fromUTF8 ("Ordner schreibschützen"), juce::File::getSpecialLocation (juce::File::userMusicDirectory));
            safe->chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe] (const juce::FileChooser& fc) {
                if (safe != nullptr && fc.getResult().isDirectory()) safe->arch->setProtected (fc.getResult().getFullPathName(), true);
            });
        }
        else if (r >= 230 && r <= 280) a.setSimilarityThreshold ((float) (r - 200) / 100.0f);
        else if (r >= 100 && r < 100 + a.folders.size())
        {
            const auto path = a.folders[r - 100];
            const int n = a.countInFolder (path);
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, juce::String::fromUTF8 ("Ordner aus dem Archiv nehmen"),
                path + "\n\n" + juce::String (n) + juce::String::fromUTF8 (" Einträge (mit Bewertungen und Notizen) werden aus der Datenbank entfernt. Die Dateien bleiben unverändert."),
                "Entfernen", "Abbrechen", nullptr,
                juce::ModalCallbackFunction::create ([safe, path] (int ok) { if (ok && safe != nullptr) safe->arch->removeFolder (path); }));
        }
    });
}

void ArchiveTab::paint (juce::Graphics& g)
{
    const auto& th = ed.theme();
    g.setColour (th.dim);
    g.setFont (font (12.5f));
    g.drawText (status, 4, 34, getWidth() - 8, 20, juce::Justification::centredLeft, true);
}

void ArchiveTab::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (30).reduced (0, 2);
    auto place = [&top] (juce::Component& c, int w) { c.setBounds (top.removeFromLeft (w)); top.removeFromLeft (4); };
    place (addBtn, 150);
    place (scanBtn, 80);
    place (stopBtn, 60);
    place (moreBtn, 70);
    top.removeFromLeft (8);
    filter.setBounds (top.removeFromRight (150));
    top.removeFromRight (6);
    search.setBounds (top);
    r.removeFromTop (24);   // status line
    auto bottom = r.removeFromBottom (juce::jmax (180, r.getHeight() * 40 / 100));
    table.setBounds (r.withTrimmedBottom (6));
    auto right = bottom.removeFromRight (juce::jlimit (260, 460, bottom.getWidth() * 42 / 100));
    overview.setBounds (bottom.withTrimmedRight (6));
    auto btns = right.removeFromTop (26);
    const int bw = (btns.getWidth() - 12) / 4;
    for (auto* b : { &openBtn, &notDupBtn, &copyBtn, &revealBtn }) { b->setBounds (btns.removeFromLeft (bw)); btns.removeFromLeft (4); }
    right.removeFromTop (4);
    comment.setBounds (right.removeFromBottom (26));
    right.removeFromBottom (4);
    info.setBounds (right);
}
} // namespace sw::ui
