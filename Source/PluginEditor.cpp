#include "PluginEditor.h"
#include "ModFile.h"
#include "DawProject.h"
#include "Convert.h"
#include "Export.h"

using namespace sw;
using namespace sw::ui;

namespace
{
constexpr int kW = 1320, kH = 840;
constexpr int kSideW = 250;

juce::File tempExportDir (const juce::String& sub)
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Spurwerk").getChildFile (sub);
    d.createDirectory();
    return d;
}
} // namespace

// ============================================================================================ FileList
FileList::FileList (SpurwerkEditor& e) : ed (e)
{
    list.setRowHeight (20);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (list);
    addAndMakeVisible (upBtn);
    addAndMakeVisible (folderBtn);
    upBtn.setTooltip (U ("Übergeordneter Ordner"));
    folderBtn.setTooltip (U ("Ordner mit Modulen wählen – der Dialog zeigt Ordner und Module; ein gewähltes Modul wird gleich geladen"));
    upBtn.onClick = [this] { if (dir.getParentDirectory() != dir) setFolder (dir.getParentDirectory()); };
    folderBtn.onClick = [this] {
        // JUCE's own browser instead of the Windows folder picker: that one hides all files
        chooser = std::make_unique<juce::FileChooser> (U ("Ordner oder Modul wählen"), dir, supportedWildcards(), false);
        juce::Component::SafePointer<FileList> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr) return;
            const auto r = fc.getResult();
            if (r.isDirectory()) safe->setFolder (r);
            else if (r.existsAsFile())
            {
                safe->setFolder (r.getParentDirectory());
                safe->ed.openFile (r);
            }
        });
    };
}

void FileList::setFolder (const juce::File& f)
{
    if (! f.isDirectory()) return;
    dir = f;
    entries.clear();
    auto subs = dir.findChildFiles (juce::File::findDirectories, false);
    subs.sort();
    entries.addArray (subs);
    juce::Array<juce::File> mods;
    for (const auto& c : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFiles))
        if (isSupportedFile (c.getFile())) mods.add (c.getFile());
    mods.sort();
    entries.addArray (mods);
    list.updateContent();
    list.repaint();
    ed.proc.uiState.setProperty ("folder", dir.getFullPathName(), nullptr);
    repaint();
}

void FileList::selectFile (const juce::File& f)
{
    if (f.getParentDirectory() != dir) setFolder (f.getParentDirectory());
    const int i = entries.indexOf (f);
    if (i >= 0) { list.selectRow (i); list.scrollToEnsureRowIsOnscreen (i); }
}

void FileList::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (48).withTrimmedTop (22);
    upBtn.setBounds (top.removeFromRight (30));
    top.removeFromRight (4);
    folderBtn.setBounds (top);
    r.removeFromTop (4);
    list.setBounds (r);
}

void FileList::paint (juce::Graphics& g)
{
    const auto& th = ed.theme();
    g.setColour (th.dim);
    g.setFont (font (12.0f, true));
    g.drawText ("DATEIEN", 2, 0, getWidth(), 20, juce::Justification::centredLeft);
    g.setFont (font (11.5f));
    g.drawText (dir.getFileName(), 70, 0, getWidth() - 72, 20, juce::Justification::centredRight);
}

void FileList::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= entries.size()) return;
    const auto& th = ed.theme();
    const auto& f = entries.getReference (row);
    if (selected) { g.setColour (th.accent.withAlpha (0.3f)); g.fillRoundedRectangle (0, 0, (float) w, (float) h, 4.0f); }
    const bool isDir = f.isDirectory();
    const bool current = f.getFullPathName() == ed.proc.song().sourcePath;
    g.setColour (isDir ? th.dim : (current ? th.accent : th.text));
    g.setFont (font (13.0f, current));
    g.drawText ((isDir ? juce::String::fromUTF8 ("▸ ") : juce::String()) + f.getFileName(), 6, 0, w - 8, h, juce::Justification::centredLeft);
}

void FileList::open (int row)
{
    if (row < 0 || row >= entries.size()) return;
    const auto f = entries[row];
    if (f.isDirectory()) setFolder (f);
    else ed.openFile (f);
}

void FileList::listBoxItemDoubleClicked (int row, const juce::MouseEvent&) { open (row); }
void FileList::returnKeyPressed (int row) { open (row); }

// ============================================================================================ InstrumentList
InstrumentList::InstrumentList (SpurwerkEditor& e) : ed (e)
{
    list.setRowHeight (19);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (list);
}

int InstrumentList::getNumRows() { return ed.proc.song().hasSamples() ? ed.proc.song().numSamples() : 0; }

void InstrumentList::update()
{
    list.updateContent();
    list.selectRow (ed.instrument - 1, true, true);
    list.repaint();
    repaint();
}

void InstrumentList::paint (juce::Graphics& g)
{
    const auto& th = ed.theme();
    g.setColour (th.dim);
    g.setFont (font (12.0f, true));
    g.drawText ("SAMPLES / INSTRUMENTE", 2, 0, getWidth(), 20, juce::Justification::centredLeft);
    if (! ed.proc.song().hasSamples())
    {
        g.setFont (font (12.5f));
        g.drawFittedText (U ("YM-Song: drei Tonstimmen + Rauschen des YM2149, keine Samples."), getLocalBounds().withTrimmedTop (26).reduced (6, 0),
                          juce::Justification::topLeft, 3);
    }
}

void InstrumentList::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    const auto& s = ed.proc.song();
    if (row < 0 || row >= s.numSamples()) return;
    const auto& th = ed.theme();
    const auto& smp = s.samples[(size_t) row];
    if (selected) { g.setColour (th.accent.withAlpha (0.3f)); g.fillRoundedRectangle (0, 0, (float) w, (float) h, 4.0f); }
    g.setColour (th.track[(size_t) (row % 8)].withAlpha (smp.pcm.empty() ? 0.35f : 1.0f));
    g.setFont (mono (12.5f));
    g.drawText (hex2 (row + 1), 4, 0, 24, h, juce::Justification::centredLeft);
    g.setColour (smp.pcm.empty() ? th.dim : th.text);
    g.setFont (font (12.5f));
    g.drawText (smp.name, 30, 0, w - 34, h, juce::Justification::centredLeft);
}

void InstrumentList::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { ed.showLoupeMenu(); return; }
    ed.selectInstrument (row + 1);
    ed.proc.previewNote (row + 1, 60, true);
    juce::Component::SafePointer<SpurwerkEditor> safe (&ed);
    juce::Timer::callAfterDelay (700, [safe] { if (safe != nullptr) safe->proc.previewNote (0, 0, false); });
}

void InstrumentList::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    ed.selectInstrument (row + 1);
    ed.showTab (2);
}

// ============================================================================================ GeneralTab
GeneralTab::GeneralTab (SpurwerkEditor& e) : ed (e)
{
    title.setInputRestrictions (20);
    title.setFont (font (15.0f));
    title.onTextChange = [this] {
        if (updating) return;
        { const juce::ScopedLock sl (ed.proc.lock); ed.proc.song().title = title.getText(); }
        ed.songEdited();
    };
    comment.setMultiLine (true);
    comment.setReadOnly (true);
    comment.setFont (font (13.0f));
    for (int c : { 4, 6, 8, 10, 12, 16, 24, 32 }) channels.addItem (juce::String (c) + U (" Kanäle"), c);
    channels.onChange = [this] {
        if (updating) return;
        const int n = channels.getSelectedId();
        auto& s = ed.proc.song();
        if (n <= 0 || s.readOnly() || n == s.channels) return;
        {
            const juce::ScopedLock sl (ed.proc.lock);
            for (auto& p : s.patterns)
            {
                Pattern np;
                np.resize (p.rows, n);
                for (int r = 0; r < p.rows; ++r)
                    for (int c = 0; c < juce::jmin (n, p.channels); ++c) np.at (r, c) = p.at (r, c);
                p = np;
            }
            s.channels = n;
            s.formatName = n == 4 ? U ("ProTracker MOD (M.K., 4 Kanäle)") : "ProTracker MOD (" + juce::String (n) + U (" Kanäle)");
        }
        ed.proc.songStructureChanged();
        ed.songLoaded();
    };
    subSong.onChange = [this] {
        if (updating) return;
        juce::String err;
        if (ed.proc.selectSubSong (subSong.getSelectedId() - 1, err)) { ed.songLoaded(); ed.setStatus ("Teilsong " + juce::String (subSong.getSelectedId())); }
        else ed.setStatus (err);
    };
    syncMode.addItem ("Aus (nur eigener Start/Stopp)", 1);
    syncMode.addItem ("Start/Stopp mit der DAW", 2);
    syncMode.addItem ("Start/Stopp + Tempo der DAW", 3);
    syncMode.onChange = [this] { if (! updating) ed.proc.syncMode = syncMode.getSelectedId() - 1; };
    keyboard.addItem ("QWERTZ (deutsch)", 1);
    keyboard.addItem ("QWERTY (englisch)", 2);
    keyboard.onChange = [this] { if (! updating) ed.proc.uiState.setProperty ("keys", keyboard.getSelectedId() - 1, nullptr); };
    for (int i = 0; i < (int) themes().size(); ++i) themeBox.addItem (themes()[(size_t) i].name, i + 1);
    themeBox.onChange = [this] { if (! updating) { ed.proc.uiState.setProperty ("theme", themeBox.getSelectedId() - 1, nullptr); ed.applyTheme(); } };
    light.onClick = [this] { if (! updating) { ed.proc.uiState.setProperty ("light", light.getToggleState(), nullptr); ed.applyTheme(); } };
    for (auto* s : { &speed, &tempo, &separation, &volume, &restart })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
        addAndMakeVisible (s);
    }
    speed.setRange (1, 31, 1);
    tempo.setRange (32, 255, 1);
    separation.setRange (0, 100, 1);
    separation.setTextValueSuffix (" %");
    volume.setRange (0, 150, 1);
    volume.setTextValueSuffix (" %");
    restart.setRange (0, 127, 1);
    speed.onValueChange = [this] { if (! updating) { ed.proc.song().initialSpeed = (int) speed.getValue(); ed.songEdited(); } };
    tempo.onValueChange = [this] { if (! updating) { ed.proc.song().initialTempo = (int) tempo.getValue(); ed.songEdited(); } };
    restart.onValueChange = [this] { if (! updating) { ed.proc.song().restartPos = (int) restart.getValue(); ed.songEdited(); } };
    separation.onValueChange = [this] { if (! updating) ed.proc.song().stereoSeparation = (int) separation.getValue(); };
    volume.onValueChange = [this] { if (! updating) ed.proc.masterGain = (float) volume.getValue() / 100.0f; };
    interp.onClick = [this] { ed.proc.interpolate = interp.getToggleState(); };
    sampler.onClick = [this] { ed.proc.samplerOn = sampler.getToggleState(); };
    addChildComponent (subSong);
    for (juce::Component* c : { (juce::Component*) &title, (juce::Component*) &comment, (juce::Component*) &channels, (juce::Component*) &syncMode,
                                (juce::Component*) &keyboard, (juce::Component*) &themeBox, (juce::Component*) &interp, (juce::Component*) &sampler,
                                (juce::Component*) &light })
        addAndMakeVisible (c);
    sampler.setTooltip (U ("MIDI an Spurwerk: Kanal N spielt anfangs Sample N, Program Change wählt das Sample, CC7/CC11/Pitchbend wirken – passend zum MIDI-Export"));
}

void GeneralTab::update()
{
    updating = true;
    auto& s = ed.proc.song();
    title.setText (s.title, false);
    title.setReadOnly (s.readOnly());
    channels.setSelectedId (s.channels, juce::dontSendNotification);
    channels.setEnabled (! s.readOnly());
    channels.setVisible (s.subSongs <= 1);
    subSong.setVisible (s.subSongs > 1);
    subSong.clear (juce::dontSendNotification);
    for (int i = 0; i < s.subSongs; ++i) subSong.addItem ("Teilsong " + juce::String (i + 1) + " von " + juce::String (s.subSongs), i + 1);
    subSong.setSelectedId (s.subSong + 1, juce::dontSendNotification);
    speed.setValue (s.initialSpeed, juce::dontSendNotification);
    tempo.setValue (s.initialTempo, juce::dontSendNotification);
    restart.setValue (s.restartPos, juce::dontSendNotification);
    for (auto* c : { &speed, &tempo, &restart }) c->setEnabled (! s.readOnly());
    separation.setValue (s.stereoSeparation, juce::dontSendNotification);
    volume.setValue (ed.proc.masterGain.load() * 100.0, juce::dontSendNotification);
    interp.setToggleState (ed.proc.interpolate.load(), juce::dontSendNotification);
    sampler.setToggleState (ed.proc.samplerOn.load(), juce::dontSendNotification);
    syncMode.setSelectedId (ed.proc.syncMode.load() + 1, juce::dontSendNotification);
    keyboard.setSelectedId (ed.qwertz() ? 1 : 2, juce::dontSendNotification);
    themeBox.setSelectedId ((int) ed.proc.uiState.getProperty ("theme", 0) + 1, juce::dontSendNotification);
    light.setToggleState ((bool) ed.proc.uiState.getProperty ("light", false), juce::dontSendNotification);
    juce::String c;
    c << "Format: " << s.formatName << "\n";
    if (s.format == Format::YM)
    {
        c << "Autor: " << s.ym.author << "\n";
        if (s.ym.comment.isNotEmpty()) c << "Kommentar: " << s.ym.comment << "\n";
        c << "Frames: " << s.ym.frames << " bei " << s.ym.rate << " Hz (" << juce::String (s.ym.frames / (double) s.ym.rate / 60.0, 1).replaceCharacter ('.', ',')
          << " Min.), Loop ab Frame " << s.ym.loopFrame << "\nTakt YM2149: " << s.ym.clock << " Hz, Digidrums: " << (int) s.ym.drums.size() << "\n";
    }
    else
    {
        int used = 0, bytes = 0;
        for (const auto& smp : s.samples) if (! smp.pcm.empty()) { ++used; bytes += (int) smp.pcm.size(); }
        c << "Patterns: " << (int) s.patterns.size() << ", Positionen: " << (int) s.orders.size() << "\n"
          << "Samples: " << used << " von " << s.numSamples() << " belegt (" << bytes / 1024 << " KB)\n";
        if (! s.instruments.empty()) c << "Instrumente: " << (int) s.instruments.size() << U (" (mit Hüllkurven; links sind die Samples einzeln aufgelistet)\n");
        if (s.readOnly()) c << U ("Patterns nur lesend – Bearbeiten und Speichern derzeit nur für MOD; Export (MIDI, Samples, WAV) steht voll zur Verfügung.\n");
    }
    if (s.sourcePath.isNotEmpty()) c << "Datei: " << s.sourcePath << "\n";
    comment.setText (c, false);
    updating = false;
    repaint();
}

void GeneralTab::layoutLabel (juce::Graphics& g, juce::Component& c, const char* text)
{
    g.drawText (U (text), c.getX() - 170, c.getY(), 162, c.getHeight(), juce::Justification::centredRight);
}

void GeneralTab::paint (juce::Graphics& g)
{
    const auto& th = ed.theme();
    g.fillAll (th.bg);
    g.setColour (th.dim);
    g.setFont (font (13.0f));
    layoutLabel (g, title, "Titel");
    layoutLabel (g, channels, ed.proc.song().subSongs > 1 ? "Teilsong" : "Kanäle");
    layoutLabel (g, speed, "Start-Speed (Ticks/Zeile)");
    layoutLabel (g, tempo, "Start-Tempo (BPM)");
    layoutLabel (g, restart, "Neustart-Position");
    layoutLabel (g, separation, "Stereo-Trennung");
    layoutLabel (g, volume, "Gesamtlautstärke");
    layoutLabel (g, syncMode, "DAW-Synchronisation");
    layoutLabel (g, keyboard, "Tastatur");
    layoutLabel (g, themeBox, "Farbthema");
    g.setColour (th.accent);
    g.setFont (font (13.0f, true));
    g.drawText ("SONG", 20, 10, 300, 20, juce::Justification::centredLeft);
    g.drawText ("WIEDERGABE", 20, speed.getY() - 30, 300, 20, juce::Justification::centredLeft);
    g.drawText ("PLUGIN", 20, separation.getY() - 30, 300, 20, juce::Justification::centredLeft);
    g.drawText ("INFO", comment.getX(), 10, 300, 20, juce::Justification::centredLeft);
}

void GeneralTab::resized()
{
    auto r = getLocalBounds().reduced (20);
    auto left = r.removeFromLeft (juce::jmin (620, r.getWidth() / 2 + 120)).withTrimmedLeft (180);
    r.removeFromLeft (20);
    left.removeFromTop (24);
    auto row = [&left] (int h = 26) { auto x = left.removeFromTop (h); left.removeFromTop (8); return x; };
    title.setBounds (row (28).withWidth (280));
    channels.setBounds (row().withWidth (180));
    subSong.setBounds (channels.getBounds().withWidth (220));
    left.removeFromTop (30);
    speed.setBounds (row());
    tempo.setBounds (row());
    restart.setBounds (row());
    interp.setBounds (row());
    left.removeFromTop (30);
    separation.setBounds (row());
    volume.setBounds (row());
    syncMode.setBounds (row().withWidth (300));
    sampler.setBounds (row().withX (left.getX() - 170).withWidth (left.getWidth() + 170));
    keyboard.setBounds (row().withWidth (200));
    auto th = row();
    themeBox.setBounds (th.removeFromLeft (160));
    th.removeFromLeft (12);
    light.setBounds (th.removeFromLeft (160));
    r.removeFromTop (24);
    comment.setBounds (r.withHeight (juce::jmin (r.getHeight(), 260)));
}

// ============================================================================================ ExportTab
ExportTab::ExportTab (SpurwerkEditor& e) : ed (e)
{
    // default: one track per instrument – samples that shared a tracker channel end up on their own DAW tracks
    midiMode.addItem ("Eine Spur je Instrument/Sample (Spurname = Samplename)", 2);
    midiMode.addItem ("Eine Spur je Tracker-Kanal (mit Program Change)", 1);
    midiMode.setSelectedId ((int) ed.proc.uiState.getProperty ("midiMode", 2) == 1 ? 1 : 2, juce::dontSendNotification);
    midiMode.onChange = [this] { ed.proc.uiState.setProperty ("midiMode", midiMode.getSelectedId(), nullptr); };
    for (int r : { 2, 12, 24, 48 }) bendRange.addItem ("Pitchbend " + juce::String::charToString (0xb1) + juce::String (r) + " HT", r);
    bendRange.setSelectedId (12);
    for (juce::Component* c : { (juce::Component*) &midiMode, (juce::Component*) &bendRange, (juce::Component*) &saveMidi,
                                (juce::Component*) &saveSamples, (juce::Component*) &saveWav, (juce::Component*) &saveDaw, (juce::Component*) &saveAls, (juce::Component*) &summary })
        addAndMakeVisible (c);
    summary.setFont (font (13.0f));
    saveDaw.setVisible (false);   // triggered by a click on the DAWproject field (DAWs do not accept a dropped project)
    saveDaw.setTooltip (U ("Projektdatei für Fender Studio Pro, Studio One, Bitwig, Cubase: je Sample eine Instrumentenspur mit Spurwerk Player, "
                           "Sample und Noten – Datei in der DAW öffnen bzw. importieren"));
    saveDaw.onClick = [this] {
        const auto& s = ed.proc.song();
        auto start = juce::File (ed.proc.uiState.getProperty ("exportDir", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getFullPathName()).toString())
                         .getChildFile (safeFileName (s.title.isNotEmpty() ? s.title : juce::String ("spurwerk")) + ".dawproject");
        chooser = std::make_unique<juce::FileChooser> ("DAWproject speichern", start, "*.dawproject", false);
        juce::Component::SafePointer<ExportTab> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            const auto f = fc.getResult().withFileExtension ("dawproject");
            if (safe->ed.writeBlocked (f)) return;
            juce::String log;
            bool ok;
            { const juce::ScopedLock sl (safe->ed.proc.lock); ok = exportDawProject (safe->ed.proc.song(), f, log); }
            safe->ed.proc.uiState.setProperty ("exportDir", f.getParentDirectory().getFullPathName(), nullptr);
            safe->lastInfo = "DAWproject: " + log + (ok ? U (" → ") + f.getFullPathName() : juce::String());
            safe->summary.setText (safe->lastInfo, juce::dontSendNotification);
            safe->ed.setStatus (ok ? juce::String ("DAWproject gespeichert") : "Fehler: " + log);
        });
    };
    saveAls.setTooltip (U ("Ableton Live Set (.als): je Sample eine MIDI-Spur mit Simpler (Sample geladen), Noten und Pitchbend im Arrangement, Tempo. "
                            "YM/COSO: die drei Stimmen als Audiospuren. Braucht ein installiertes Ableton Live 12 (Vorlagen). Spurwerk legt einen Ordner mit dem Songnamen an: darin das Set und der Unterordner „Samples“ mit den WAVs."));
    saveAls.onClick = [this] {
        const auto& s = ed.proc.song();
        auto start = juce::File (ed.proc.uiState.getProperty ("exportDir", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getFullPathName()).toString())
                         .getChildFile (safeFileName (s.title.isNotEmpty() ? s.title : juce::String ("spurwerk")) + ".als");
        chooser = std::make_unique<juce::FileChooser> ("Ableton Live Set speichern", start, "*.als", false);
        juce::Component::SafePointer<ExportTab> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            const auto f = fc.getResult().withFileExtension ("als");
            if (safe->ed.writeBlocked (f)) return;
            juce::String log;
            bool ok;
            { const juce::ScopedLock sl (safe->ed.proc.lock); ok = exportAbletonLiveSet (safe->ed.proc.song(), f, log); }
            safe->ed.proc.uiState.setProperty ("exportDir", f.getParentDirectory().getFullPathName(), nullptr);
            safe->lastInfo = "Live Set: " + log;
            safe->summary.setText (safe->lastInfo, juce::dontSendNotification);
            safe->ed.setStatus (ok ? juce::String ("Ableton Live Set gespeichert") : "Fehler: " + log);
        });
    };
    saveMidi.onClick = [this] {
        const auto& s = ed.proc.song();
        auto start = juce::File (ed.proc.uiState.getProperty ("exportDir", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getFullPathName()).toString())
                         .getChildFile (safeFileName (s.title.isNotEmpty() ? s.title : juce::String ("spurwerk")) + ".mid");
        chooser = std::make_unique<juce::FileChooser> ("MIDI speichern", start, "*.mid");
        juce::Component::SafePointer<ExportTab> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            auto f = fc.getResult().withFileExtension ("mid");
            if (safe->ed.writeBlocked (f)) return;
            const auto tmp = safe->makeTempMidi();
            tmp.copyFileTo (f);
            safe->ed.proc.uiState.setProperty ("exportDir", f.getParentDirectory().getFullPathName(), nullptr);
            safe->ed.setStatus ("MIDI gespeichert: " + f.getFullPathName());
        });
    };
    saveSamples.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> (U ("Zielordner für die Samples"),
            juce::File (ed.proc.uiState.getProperty ("exportDir", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getFullPathName()).toString()),
            "*", false);
        juce::Component::SafePointer<ExportTab> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            const auto picked = fc.getResult().isDirectory() ? fc.getResult() : fc.getResult().getParentDirectory();
            if (safe->ed.writeBlocked (picked.getChildFile ("x"))) return;
            const auto& s = safe->ed.proc.song();
            const auto dir = picked.getChildFile (safeFileName (s.title.isNotEmpty() ? s.title : juce::String ("Spurwerk")) + "_Samples");
            juce::String log;
            {
                const juce::ScopedLock sl (safe->ed.proc.lock);
                exportSamples (s, dir, log);
            }
            safe->ed.proc.uiState.setProperty ("exportDir", picked.getFullPathName(), nullptr);
            safe->lastInfo = log + " in " + dir.getFullPathName();
            safe->summary.setText (safe->lastInfo, juce::dontSendNotification);
            safe->ed.setStatus ("Samples exportiert");
        });
    };
    saveWav.onClick = [this] {
        const auto& s = ed.proc.song();
        auto start = juce::File (ed.proc.uiState.getProperty ("exportDir", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getFullPathName()).toString())
                         .getChildFile (safeFileName (s.title.isNotEmpty() ? s.title : juce::String ("spurwerk")) + ".wav");
        chooser = std::make_unique<juce::FileChooser> ("Song als WAV", start, "*.wav");
        juce::Component::SafePointer<ExportTab> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            if (safe->ed.writeBlocked (fc.getResult())) return;
            juce::String log;
            renderSongWav (safe->ed.proc.song(), fc.getResult().withFileExtension ("wav"), 44100.0, 20.0, log);
            safe->summary.setText ("WAV: " + log, juce::dontSendNotification);
            safe->ed.setStatus ("WAV gespeichert");
        });
    };
}

juce::File ExportTab::makeTempMidi()
{
    MidiExportOptions o;
    o.perInstrument = midiMode.getSelectedId() == 2;
    o.bendRange = bendRange.getSelectedId();
    juce::String sum;
    juce::MidiFile mf;
    {
        const juce::ScopedLock sl (ed.proc.lock);
        mf = exportMidi (ed.proc.song(), o, &sum);
    }
    const auto& s = ed.proc.song();
    auto f = tempExportDir ("midi").getChildFile (safeFileName (s.title.isNotEmpty() ? s.title : juce::String ("spurwerk")) + ".mid");
    writeMidiFile (mf, f);
    lastInfo = "MIDI: " + sum;
    summary.setText (lastInfo, juce::dontSendNotification);
    return f;
}

juce::File ExportTab::makeTempDawProject()
{
    const auto& s = ed.proc.song();
    auto f = tempExportDir ("dawproject").getChildFile (safeFileName (s.title.isNotEmpty() ? s.title : juce::String ("spurwerk")) + ".dawproject");
    juce::String log;
    bool ok;
    { const juce::ScopedLock sl (ed.proc.lock); ok = exportDawProject (s, f, log); }
    lastInfo = "DAWproject: " + log;
    summary.setText (lastInfo, juce::dontSendNotification);
    return ok ? f : juce::File();
}

void ExportTab::update()
{
    const bool ym = ed.proc.song().format == Format::YM;
    midiMode.setEnabled (! ym);
    saveSamples.setEnabled (! ym);
    saveDaw.setEnabled (true);   // YM/COSO: rendered voices + note tracks
    repaint();
}

void ExportTab::doDrag (int which)
{
    juce::StringArray files;
    if (which == 0) files.add (makeTempMidi().getFullPathName());
    else if (which == 3)
    {
        if (! ed.proc.song().hasSamples()) return;
        const auto f = makeTempDawProject();
        if (f.existsAsFile()) files.add (f.getFullPathName());
    }
    else if (which == 1)
    {
        if (! ed.proc.song().hasSamples()) return;
        const auto dir = tempExportDir ("samples").getChildFile (safeFileName (ed.proc.song().title.isNotEmpty() ? ed.proc.song().title : juce::String ("Spurwerk")));
        dir.deleteRecursively();
        juce::String log;
        juce::Array<juce::File> wavs;
        {
            const juce::ScopedLock sl (ed.proc.lock);
            wavs = exportSamples (ed.proc.song(), dir, log);
        }
        for (auto& w : wavs) files.add (w.getFullPathName());
        summary.setText (log, juce::dontSendNotification);
    }
    else
    {
        auto f = tempExportDir ("audio").getChildFile (safeFileName (ed.proc.song().title.isNotEmpty() ? ed.proc.song().title : juce::String ("spurwerk")) + ".wav");
        juce::String log;
        renderSongWav (ed.proc.song(), f, 44100.0, 20.0, log);
        summary.setText ("WAV: " + log, juce::dontSendNotification);
        files.add (f.getFullPathName());
    }
    if (! files.isEmpty()) juce::DragAndDropContainer::performExternalDragDropOfFiles (files, false, this);
}

void ExportTab::mouseDrag (const juce::MouseEvent& e)
{
    if (e.getDistanceFromDragStart() < 6 || e.mouseWasDraggedSinceMouseDown() == false) return;
    static juce::uint32 lastDrag = 0;
    if (juce::Time::getMillisecondCounter() - lastDrag < 800) return;
    lastDrag = juce::Time::getMillisecondCounter();
    const auto p = e.getMouseDownPosition();
    if (midiDrag.contains (p)) doDrag (0);
    else if (sampleDrag.contains (p)) doDrag (1);
    else if (wavDrag.contains (p)) doDrag (2);
}

void ExportTab::mouseUp (const juce::MouseEvent& e)
{
    auto clicked = [&] (juce::Rectangle<int> r) { return r.contains (e.getMouseDownPosition()) && r.contains (e.getPosition()) && ! e.mouseWasDraggedSinceMouseDown(); };
    if (clicked (saveBox) && ! ed.proc.song().readOnly()) ed.saveSong (false);
    else if (clicked (saveAsBox)) ed.saveConverted();
    else if (clicked (dawDrag) && saveDaw.isEnabled()) saveDaw.triggerClick();
    else if (clicked (alsBox)) saveAls.triggerClick();
    else if (clicked (midiSaveBox)) saveMidi.triggerClick();
}

void ExportTab::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    const bool click = (dawDrag.contains (p) && saveDaw.isEnabled()) || alsBox.contains (p) || midiSaveBox.contains (p) || saveAsBox.contains (p)
                    || (saveBox.contains (p) && ! ed.proc.song().readOnly());
    const bool drag = midiDrag.contains (p) || wavDrag.contains (p) || (sampleDrag.contains (p) && ed.proc.song().hasSamples());
    setMouseCursor (click ? juce::MouseCursor::PointingHandCursor : drag ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
}

namespace
{
// four-way "move" arrow: a plus with arrow heads (drag fields)
void drawDragSymbol (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    const auto cx = r.getCentreX(), cy = r.getCentreY(), h = juce::jmin (r.getWidth(), r.getHeight()) * 0.5f, head = h * 0.42f, t = juce::jmax (2.0f, h * 0.16f);
    juce::Path p;
    p.addRectangle (cx - t / 2, cy - h + head * 0.8f, t, 2 * (h - head * 0.8f));
    p.addRectangle (cx - h + head * 0.8f, cy - t / 2, 2 * (h - head * 0.8f), t);
    p.addTriangle (cx, cy - h, cx - head, cy - h + head, cx + head, cy - h + head);
    p.addTriangle (cx, cy + h, cx - head, cy + h - head, cx + head, cy + h - head);
    p.addTriangle (cx - h, cy, cx - h + head, cy - head, cx - h + head, cy + head);
    p.addTriangle (cx + h, cy, cx + h - head, cy - head, cx + h - head, cy + head);
    g.setColour (c);
    g.fillPath (p);
}

// project exchange symbol (own design, no third-party logo): filled tile, two opposite arrows cut out
void drawDawProjectLogo (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, juce::Colour cut)
{
    const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.92f;
    auto t = juce::Rectangle<float> (s, s).withCentre (r.getCentre());
    g.setColour (fill);
    g.fillRoundedRectangle (t, s * 0.18f);
    const float u = s / 120.0f, x = t.getX(), y = t.getY();
    juce::Path a;
    a.addRoundedRectangle (x + 20 * u, y + 38 * u, 62 * u, 12 * u, 3 * u);
    a.addTriangle (x + 80 * u, y + 24 * u, x + 104 * u, y + 44 * u, x + 80 * u, y + 64 * u);
    a.addRoundedRectangle (x + 38 * u, y + 70 * u, 62 * u, 12 * u, 3 * u);
    a.addTriangle (x + 40 * u, y + 56 * u, x + 16 * u, y + 76 * u, x + 40 * u, y + 96 * u);
    g.setColour (cut);
    g.fillPath (a);
}

// live set symbol (own design, no third-party logo): two filled squares, one bar upright, one across
void drawAbletonLogo (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour fill, juce::Colour cut)
{
    const float side = juce::jmin (r.getHeight() * 0.62f, (r.getWidth() - 8.0f) / 2.0f), gap = side * 0.14f;
    const float x0 = r.getCentreX() - side - gap / 2, y0 = r.getCentreY() - side / 2;
    const float bar = side * 0.18f, len = side * 0.72f;
    g.setColour (fill);
    g.fillRoundedRectangle (x0, y0, side, side, side * 0.14f);
    g.fillRoundedRectangle (x0 + side + gap, y0, side, side, side * 0.14f);
    g.setColour (cut);
    g.fillRoundedRectangle (x0 + (side - bar) / 2, y0 + (side - len) / 2, bar, len, bar * 0.2f);
    g.fillRoundedRectangle (x0 + side + gap + (side - len) / 2, y0 + (side - bar) / 2, len, bar, bar * 0.2f);
}
} // namespace

void ExportTab::paint (juce::Graphics& g)
{
    const auto& th = ed.theme();
    g.fillAll (th.bg);
    const bool ym = ed.proc.song().format == Format::YM;
    auto box = [&] (juce::Rectangle<int> r, const juce::String& title, juce::Colour c, bool enabled) {
        g.setColour (th.panel);
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
        g.setColour (enabled ? c : th.dim);
        g.drawRoundedRectangle (r.toFloat().reduced (1.0f), 8.0f, 2.0f);
        float dash[] = { 6.0f, 4.0f };
        juce::Path p;
        p.addRoundedRectangle (r.toFloat().reduced (8.0f), 6.0f);
        juce::Path d;
        juce::PathStrokeType (1.2f).createDashedStroke (d, p, dash, 2);
        g.setColour ((enabled ? c : th.dim).withAlpha (0.5f));
        g.fillPath (d);
        g.setColour (enabled ? th.text : th.dim);
        g.setFont (font (14.0f, true));
        g.drawFittedText (title, r.reduced (14), juce::Justification::centred, 3);
    };
    auto heading = [&] (juce::Rectangle<int> box, const juce::String& t, const juce::String& sub) {
        g.setColour (th.accent);
        g.setFont (font (13.0f, true));
        g.drawText (t, box.getX(), box.getY() - 26, 300, 20, juce::Justification::centredLeft);
        if (sub.isNotEmpty())
        {
            const int tw = (int) std::ceil (juce::GlyphArrangement::getStringWidth (font (13.0f, true), t)) + 12;
            g.setColour (th.dim);
            g.setFont (font (12.0f));
            g.drawText (sub, box.getX() + tw, box.getY() - 26, box.getWidth() - tw, 20, juce::Justification::centredLeft);
        }
    };
    // drag field: symbol left, text right
    auto dragBox = [&] (juce::Rectangle<int> r, const juce::String& title, juce::Colour c, bool enabled) {
        box (r, {}, c, enabled);
        auto in = r.reduced (18, 14);
        drawDragSymbol (g, in.removeFromLeft (in.getHeight()).toFloat(), (enabled ? c : th.dim));
        g.setColour (enabled ? th.text : th.dim);
        g.setFont (font (14.0f, true));
        g.drawFittedText (title, in.withTrimmedLeft (12), juce::Justification::centredLeft, 3);
    };
    auto clickBox = [&] (juce::Rectangle<int> r, const juce::String& title, const juce::String& note, juce::Colour c, bool enabled,
                         const std::function<void (juce::Rectangle<float>)>& logo) {
        box (r, {}, c, enabled);
        auto in = r.reduced (18, 12);
        if (logo) logo (in.removeFromLeft (juce::roundToInt (in.getHeight() * 1.25f)).toFloat());
        in.removeFromLeft (10);
        g.setColour (enabled ? th.text : th.dim);
        g.setFont (font (14.0f, true));
        g.drawText (title, in.removeFromTop (note.isNotEmpty() ? 24 : in.getHeight()), note.isNotEmpty() ? juce::Justification::bottomLeft : juce::Justification::centredLeft);
        if (note.isNotEmpty())
        {
            g.setColour (th.dim);
            g.setFont (font (12.0f));
            g.drawFittedText (note, in, juce::Justification::topLeft, 3, 0.9f);
        }
    };
    g.setColour (th.accent);
    g.setFont (font (13.0f, true));
    g.drawText ("MIDI-EXPORT", 20, 12, 300, 20, juce::Justification::centredLeft);
    dragBox (midiDrag, U ("Alle MIDI-Spuren von hier\nin die DAW ziehen"), th.track[0], true);
    clickBox (midiSaveBox, U ("MIDI speichern …"), U ("Eine MIDI-Datei mit allen Spuren"), th.track[0], true, [&] (juce::Rectangle<float> rr) {
        // save symbol: arrow down into a tray
        const auto c = th.track[0];
        const float h = rr.getHeight() * 0.8f, cx = rr.getCentreX(), top = rr.getCentreY() - h / 2, t = juce::jmax (2.0f, h * 0.1f);
        juce::Path pth;
        pth.addRectangle (cx - t / 2, top, t, h * 0.5f);
        pth.addTriangle (cx, top + h * 0.72f, cx - h * 0.25f, top + h * 0.42f, cx + h * 0.25f, top + h * 0.42f);
        pth.addRectangle (cx - h * 0.45f, top + h - t, h * 0.9f, t);
        pth.addRectangle (cx - h * 0.45f, top + h * 0.62f, t, h * 0.38f);
        pth.addRectangle (cx + h * 0.45f - t, top + h * 0.62f, t, h * 0.38f);
        g.setColour (c);
        g.fillPath (pth);
    });
    g.setColour (th.accent);
    g.setFont (font (13.0f, true));
    g.drawText ("SAMPLE-EXPORT", sampleDrag.getX(), saveSamples.getY() - 24, 300, 20, juce::Justification::centredLeft);
    g.drawText ("AUDIO", wavDrag.getX(), saveSamples.getY() - 24, 300, 20, juce::Justification::centredLeft);
    dragBox (sampleDrag, U ("Alle Samples ziehen\n(WAV mit Grundton C-5 + Schleife)"), th.track[3], ! ym);
    dragBox (wavDrag, U ("Song als WAV ziehen"), th.track[5], true);
    heading (dawDrag, "PROJEKT-EXPORT", U ("in der DAW als neuen Song öffnen"));
    clickBox (dawDrag, U ("DAWproject speichern …"), U ("Studio Pro / Studio One, Bitwig, Cubase"), th.track[6], saveDaw.isEnabled(),
              [&] (juce::Rectangle<float> rr) { drawDawProjectLogo (g, rr, saveDaw.isEnabled() ? th.track[6] : th.dim, th.panel); });
    clickBox (alsBox, U ("Ableton Live Set speichern …"), U ("Live 12 (installiert)"), th.track[4], true,
              [&] (juce::Rectangle<float> rr) { drawAbletonLogo (g, rr, th.track[4], th.panel); });
    heading (saveBox, "MODUL SPEICHERN", U ("Strg+S / Strg+Umschalt+S"));
    {
        const auto& song = ed.proc.song();
        const juce::File cur (song.sourcePath);
        const bool canSave = ! song.readOnly();
        const auto disk = [&] (juce::Rectangle<float> rr, juce::Colour c) {
            const float h = rr.getHeight() * 0.72f;
            auto b = juce::Rectangle<float> (h, h).withCentre (rr.getCentre());
            g.setColour (c);
            g.fillRoundedRectangle (b, h * 0.08f);
            g.setColour (th.panel);
            g.fillRect (b.getX() + h * 0.22f, b.getY(), h * 0.5f, h * 0.3f);
            g.fillRoundedRectangle (b.getX() + h * 0.15f, b.getY() + h * 0.48f, h * 0.7f, h * 0.42f, h * 0.04f);
        };
        clickBox (saveBox, U ("Speichern"), canSave ? (cur.existsAsFile() && cur.hasFileExtension ("mod") ? cur.getFileName() : U ("als ProTracker MOD"))
                                                      : U ("nur für MOD-Songs – sonst „Speichern als …“"),
                  th.track[2], canSave, [&] (juce::Rectangle<float> rr) { disk (rr, canSave ? th.track[2] : th.dim); });
        clickBox (saveAsBox, U ("Speichern als … (MOD / OctaMED)"), U ("wandelt jedes Format um, auch YM, COSO und Future Composer"),
                  th.track[7], true, [&] (juce::Rectangle<float> rr) { disk (rr, th.track[7]); });
    }

    g.setColour (th.dim);
    g.setFont (font (13.0f));
    juce::String help;
    if (ym)
        help = U ("YM: Jede der drei Stimmen wird eine MIDI-Spur (Kanal 1–3), reines Rauschen landet auf Kanal 10 (Hi-Hat/Snare). "
                  "Feine Tonhöhen über Pitchbend, Lautstärke über Velocity und CC11. Mit aktivem Sampler-Modus spielt Spurwerk "
                  "diese MIDI-Spuren selbst als YM-Rechteckstimmen.\n\nDAWproject (auch COSO): die drei YM-Stimmen werden exakt als Audiospuren gerendert (mit Hüllkurven, Rauschen und Digidrums); "
                  "dazu kommen stummgeschaltete Notenspuren mit Spurwerk Player (Rechteck bzw. Rauschen) zum Bearbeiten oder Austauschen des Klangs.");
    else
        help = U ("„Je Kanal“: jede Tracker-Spur wird eine MIDI-Spur, Instrumentwechsel als Program Change – ideal mit Spurwerks eigenem "
                  "Sampler-Modus (eine zweite Spurwerk-Instanz mit demselben Modul empfängt die MIDI-Spuren). "
                  "„Je Instrument“: Instrument N spielt auf eigenem MIDI-Kanal – so lässt sich jedes Sample in einen beliebigen Sampler laden "
                  "(Samples-Export: Grundton C-5 = MIDI 60). Portamento/Vibrato/Arpeggio → Pitchbend, Lautstärkeverlauf → CC11, Panning → CC10, "
                  "Sample-Offset → CC110/111. Echos und Akkorde desselben Samples bekommen eigene Spuren „(Stimme 2)“.\n\n"
                  "Sample-Export schreibt pro Sample eine WAV (16 Bit, Rate = Amiga-Tonhöhe auf C-5, Schleife im smpl-Chunk) und eine SFZ-Datei, "
                  "dazu ein Kit (Sample N auf Taste 36+N) als SFZ und DecentSampler-Preset.\n\n"
                  "DAWproject: eine Projektdatei, die Fender Studio Pro, Studio One, Bitwig und Cubase als neuen Song öffnen bzw. importieren (Ableton Live kennt das Format nicht). Jedes Sample wird "
                  "eine eigene Instrumentenspur (Name = Samplename) mit dem Spurwerk Player, in dem das Sample schon geladen ist, dazu die Noten, "
                  "Pitchbend und Lautstärkeverlauf (CC11) und das Tempo. Die WAV-Dateien liegen zusätzlich im Ordner „samples“ der Datei.\n\nAbleton Live Set: Live kennt kein DAWproject – „Ableton Live Set …“ speichert ein .als mit je einer MIDI-Spur + Simpler pro Sample (braucht ein installiertes Live 12).");
    g.drawFittedText (help, helpArea (), juce::Justification::topLeft, 12);
}

juce::Rectangle<int> ExportTab::helpArea() const
{
    return { 20, saveAsBox.getBottom() + 18, getWidth() - 40, juce::jmax (40, getHeight() - saveAsBox.getBottom() - 70) };
}

void ExportTab::resized()
{
    auto r = getLocalBounds().reduced (20);
    r.removeFromTop (26);
    auto row = r.removeFromTop (28);
    midiMode.setBounds (row.removeFromLeft (380));
    row.removeFromLeft (10);
    bendRange.setBounds (row.removeFromLeft (160));
    saveMidi.setBounds ({});   // triggered by the big "MIDI speichern" field
    const int w = juce::jmin (r.getWidth(), 760), half = w / 2 - 6, boxH = 78;
    auto pair = [&] (juce::Rectangle<int>& a, juce::Rectangle<int>& b) {
        auto top = r.removeFromTop (boxH).withWidth (w);
        a = top.removeFromLeft (half);
        top.removeFromLeft (12);
        b = top;
    };
    r.removeFromTop (12);
    pair (midiDrag, midiSaveBox);
    r.removeFromTop (40);   // headings of the sample / audio row
    auto row2 = r.removeFromTop (28);
    saveSamples.setBounds (row2.withWidth (220));
    r.removeFromTop (8);
    pair (sampleDrag, wavDrag);
    saveWav.setBounds (wavDrag.getX(), saveSamples.getY(), 160, 28);
    r.removeFromTop (34);
    pair (dawDrag, alsBox);
    r.removeFromTop (34);
    pair (saveBox, saveAsBox);
    saveAls.setBounds ({});
    summary.setBounds (getLocalBounds().removeFromBottom (40).reduced (20, 6));
}

// ============================================================================================ HelpPanel
HelpPanel::HelpPanel (SpurwerkEditor& e) : ed (e)
{
    text.setMultiLine (true);
    text.setReadOnly (true);
    text.setScrollbarsShown (true);
    text.setFont (font (14.0f));
    guide = U (
        "SPURWERK – Tracker als VST3-Plugin und Standalone\n\n"
        "Aufbau wie ein klassischer PC-Tracker: links die Samples und Instrumente, rechts die Reiter Allgemein, Patterns, Samples, Export und Archiv. Module findest du im Reiter Archiv; einzelne Dateien öffnest du mit „Öffnen“ (Strg+O) oder ziehst sie ins Fenster, Ordner landen im Archiv.\n\n"
        "FORMATE\n"
        "• MOD: ProTracker/NoiseTracker (M.K., 4–32 Kanäle wie 6CHN/8CHN/xxCH) und alte Soundtracker-Module mit 15 Samples – laden, bearbeiten, speichern.\n"
        "• YM (Atari ST, YM2149): YM2/YM3/YM5/YM6, LHA-gepackt oder ungepackt, mit SID-Stimmen, Digidrums und Sync-Buzzer. "
        "YM-Songs sind Registeraufnahmen; Spurwerk zeigt sie als Pattern (1 Zeile = 1 Frame, Stimmen A–C, Rauschen N, Hüllkurve H) – nur lesend.\n"
        "• COSO (.cos, Jochen Hippel, Atari ST): wird mit einem nachgebauten Hippel-Player abgespielt und wie ein YM-Song angezeigt und exportiert; "
        "Teilsongs im Reiter Allgemein wählen. Abgeglichen mit echten YM-Aufnahmen (97–100 % gleiche Noten). Samples der DIGI-Varianten werden nicht gespielt.\n"
        "• XM (FastTracker 2): bis 32 Kanäle, Instrumente mit Lautstärke- und Panning-Hüllkurven, Fadeout und Auto-Vibrato, "
        "Lautstärkespalte (v Lautstärke, d/c Slide, b/a fein, u/h Vibrato, p Panning, l/r Pan-Slide, g Tone-Porta), lineare und Amiga-Frequenzen, "
        "Ping-Pong-Schleifen, Effekte 0–F sowie G/H globale Lautstärke, K Key-Off, L Hüllkurvenposition, P Pan-Slide, R Multi-Retrigger, T Tremor, X Extra-Fein-Porta.\n"
        "• S3M (Scream Tracker 3): bis 32 Kanäle (AdLib-Kanäle werden ausgelassen), 8- und 16-Bit-Samples, Panning, Effekte A–X in der Original-Schreibweise.\n"
        "• OKT (Oktalyzer, Amiga): 4 bis 8 Stimmen – je Amiga-Kanal wahlweise zwei per Software gemischte Stimmen. Die Oktalyzer-Effekte werden in MOD-Effekte umgesetzt, "
        "Notenslides und Arpeggios erscheinen als u/d/h/l bzw. a/b/c.\n"
        "• IT (Impulse Tracker, auch von OpenMPT gespeichert): Sample- und Instrument-Modus, gepackte Samples (IT 2.14/2.15), Lautstärke-, Panning- und Pitch-Hüllkurven "
        "mit Sustain-Schleifen, Fadeout, New Note Action (ausklingende Noten laufen auf Hintergrundstimmen weiter), Kanal- und Sample-Lautstärken, "
        "Lautstärkespalte inkl. e/f Pitch-Slide, Effekte A–Z in der Original-Schreibweise (M/N Kanallautstärke, P Pan-Slide, S7x, SAx). "
        "Resonanzfilter, Panbrello und MIDI-Makros werden nicht nachgebildet; über 32 Kanäle hinaus wird abgeschnitten.\n"
        "• MTM (MultiTracker): bis 32 Kanäle mit eigenem Panning, MOD-Effekte.\n"
        "• 669 (Composer 669 / UNIS 669): 8 Kanäle, Tempo und Pattern-Länge je Pattern. FAR (Farandole Composer): 16 Kanäle, 8/16-Bit-Samples. "
        "MDL (Digitrakker 2/3): bis 32 Kanäle, Instrumente mit Hüllkurven, gepackte Samples, zwei Effektspalten. Die Effekte dieser drei Formate werden "
        "beim Laden in MOD/XM-Effekte umgesetzt (zweite MDL-Effektspalte wenn möglich in die Lautstärkespalte).\n"
        "• MPTM (OpenMPT): wie IT, dazu bis 1024 Zeilen je Pattern und die Tempo-Modi von OpenMPT (modernes Tempo wird umgerechnet); über 32 Kanäle hinaus wird abgeschnitten.\n"
        "• OctaMED / MED (MMD0–MMD3, auch mit Endung .mod): bis 32 Spuren, Blöcke unterschiedlicher Länge, Abschnitte und Abspielfolgen (MMD2/3), "
        "klassisches Tempo und BPM-Modus, Spurlautstärken und -panning, Anmerkung als Song-Nachricht, Instrumentnamen. Synth- und Hybrid-Instrumente "
        "sowie mehroktavige IFF-Samples bleiben stumm.\n"
        "• TCB Tracker (Atari ST, „AN COOL“): 4 Kanäle, 16 Samples, Pitch-Bends aus der Sondertabelle. Future Composer 1.0–1.4 (SMOD/FC14, Amiga): "
        "die Instrumente sind kleine Programme aus Lautstärke- und Frequenzfolgen (Wellenformwechsel, Arpeggio, Vibrato, Pitch-/Lautstärke-Slides); Spurwerk spielt sie "
        "mit einem eigenen Interpreter nach, die eingebauten Wellenformen von FC 1.3 sind enthalten.\n"
        "• Gepackte Module: PowerPacker (PP20), ZIP (.mdz/.s3z/.xmz/.itz/.zip) und GZIP werden beim Laden automatisch entpackt.\n"
        "Alle Formate außer MOD sind in dieser Version nur lesend: Abspielen und Export (MIDI, Samples, WAV) gehen vollständig, Bearbeiten und Speichern nur bei MOD.\n"
        "• Beschädigte Dateien: Viele YM- und MOD-Dateien aus alten Sammlungen wurden einmal im Textmodus übertragen (jedes LF wurde CR LF). "
        "Spurwerk erkennt das an der Dateigröße und repariert es beim Laden automatisch.\n\n"
        "ABSPIELEN\n"
        "F5 / „▶ Song“ spielt den Song ab Anfang, F7 ab der gewählten Position, F6 / „▶ Pattern“ wiederholt das aktuelle Pattern, F8 oder Esc stoppt. "
        "„Folgen“ lässt die Ansicht mitlaufen. Im Plugin startet und stoppt Spurwerk mit der DAW (Reiter Allgemein: DAW-Synchronisation, "
        "wahlweise auch mit DAW-Tempo).\n\n"
        "PATTERN-EDITOR\n"
        "Leertaste schaltet die Aufnahme ein (roter Cursor). Noten wie bei FastTracker/OpenMPT: untere Buchstabenreihe Y S X D C V G B H N J M = eine Oktave, "
        "obere Reihe Q 2 W 3 E R 5 T 6 Z 7 U = die nächste (QWERTY wählbar). Oktave mit +/- im Kopf oder Ziffernblock * und /, Schrittweite mit „Schritt“. "
        "In den Spalten Instrument und Effekt Hex-Ziffern 0–9 A–F eingeben. Entf löscht, Einfg fügt eine Zeile ein, Rück entfernt eine Zeile. "
        "Umschalt + Pfeile oder Ziehen markiert, Strg+C/X/V kopiert, Strg+Z/Y macht rückgängig, Alt+Hoch/Runter transponiert. "
        "Tab springt zum nächsten Kanal, Bild-auf/ab um 16 Zeilen, Ziffernblock +/- wechselt die Position.\n"
        "Kanalkopf anklicken = stumm, Strg+Klick oder Doppelklick = solo. Positionsliste oben: Klick wählt, Doppelklick spielt ab dort, "
        "Strg+Mausrad ändert die Pattern-Nummer; die Knöpfe daneben fügen Positionen und Patterns ein oder löschen sie.\n"
        "MOD-Effekte: 0 Arpeggio, 1/2 Portamento, 3 Tone-Porta, 4 Vibrato, 5/6 Kombi, 7 Tremolo, 8 Panning, 9 Sample-Offset, A Lautstärke-Slide, "
        "B Sprung, C Lautstärke, D Pattern-Break, E Spezial (E1/E2 Fein-Porta, E6 Schleife, E9 Retrigger, EA/EB Fein-Lautstärke, EC Cut, ED Delay, EE Pattern-Delay), "
        "F Speed (<20h) / Tempo.\n\n"
        "SAMPLES\n"
        "Der Reiter zeigt zuerst alle Samples als Kacheln mit Wellenbild, je zwei nebeneinander. Klick = vorhören, Ziehen oder kurz gedrückt halten = das Sample als WAV "
        "(Grundton C-5 = MIDI 60, Schleife im smpl-Chunk) direkt in den Sampler oder auf eine Spur der DAW ziehen. Die Plakette „MIDI“ einer Kachel zieht die Noten genau "
        "dieses Samples als MIDI-Datei heraus – Sample und Noten zusammen ergeben die fertige Spur. Doppelklick oder „Bearbeiten“ öffnet den Sample-Editor.\n"
        "Instrument links auswählen (Klick spielt es an), Doppelklick öffnet den Sample-Reiter. WAV/AIFF/FLAC laden (wird auf Mono, Originaltonhöhe auf C-6 umgerechnet), "
        "Name, Lautstärke, Feinstimmung und Schleife einstellen, Schleifenpunkte im Wellenbild ziehen, als WAV speichern.\n\n"
        "EXPORT\n"
        "MIDI: als Datei speichern oder vom Feld aus alle MIDI-Spuren in die DAW ziehen. „Je Kanal“ (Program Change pro Instrumentwechsel) oder „je Instrument“ "
        "(Instrument N auf eigenem MIDI-Kanal). Portamento, Vibrato und Arpeggio werden zu Pitchbend, Lautstärkeverläufe zu CC11, "
        "das Panning des Tracker-Kanals zu CC10, der Sample-Offset (9xx) zu CC110/CC111. Spielt dasselbe Sample kurz nacheinander auf zwei Kanälen "
        "(Echo, Akkord), bekommt es eine zweite Spur „(Stimme 2)“ – so behält jedes Echo seine eigene Lautstärke und sein Panning.\n"
        "Samples: WAV mit Grundton C-5 (MIDI 60) und Schleife, je eine SFZ-Datei, dazu ein Kit als SFZ und DecentSampler-Preset. "
        "Auch hier: Feld „Alle Samples ziehen“ direkt in den Sampler oder Browser der DAW ziehen.\n"
        "DAWproject: Klick auf das DAWproject-Feld speichert eine Projektdatei für Fender Studio Pro/Studio One, Bitwig und Cubase. "
        "Jedes Sample wird eine Instrumentenspur mit dem Spurwerk Player (Sample bereits geladen), dazu Noten, Pitchbend, Lautstärkeverlauf und Tempo; "
        "die WAVs liegen zusätzlich in der Datei. Die Datei wird in der DAW als neuer Song geöffnet bzw. importiert – in einen bestehenden Song "
        "lässt sie sich nicht hineinziehen (dafür MIDI-Spuren und Samples ziehen). Ableton Live kennt DAWproject nicht.\n"
        "Ableton Live Set: „Ableton Live Set …“ speichert ein .als-Set für Live 12 – je Sample eine MIDI-Spur mit Simpler (Sample geladen, Schleife, Pitchbend ±12), "
        "Noten und Pitchbend im Arrangement, Panning und Tempo; YM/COSO zusätzlich die drei Stimmen als Audiospuren. Spurwerk legt dafür einen Ordner mit dem Songnamen an (Set + Unterordner „Samples“). "
        "Der Export nutzt die Vorlagen der installierten Live-Version; Lautstärkeverläufe (CC11) übernimmt Simpler nicht.\n"
        "Modul speichern (Export-Reiter, Strg+S / Strg+Umschalt+S): „Speichern“ schreibt MOD-Songs zurück; „Speichern als …“ wandelt jeden Song in ProTracker MOD oder OctaMED (MMD1) um – Tracker-Formate zellgenau (Noten, Samples, umsetzbare Effekte), YM/COSO/Future Composer aus den gespielten Noten. Was das Ziel nicht kennt, zeigt Spurwerk danach an.\n"
        "Audio: der ganze Song (ein Durchlauf) als WAV.\n\n"
        "SPURWERK PLAYER (eigenes kleines VST3)\n"
        "Für DAWs, die nur ein Plugin-Fenster gleichzeitig zeigen (z. B. Fender Studio Pro), und für alle, die ohne Einstellen loslegen wollen: "
        "„Spurwerk Player“ auf eine Instrumentenspur legen, oben das in Spurwerk geladene Modul und das Sample wählen – fertig. "
        "Spurwerk legt dafür die Samples jedes geladenen Moduls automatisch bereit, Ziehen ist nicht nötig. Der Player spielt das Sample auf allen 128 Tasten "
        "(C-5 = MIDI 60 = Originaltonhöhe, wie im Tracker), mit Schleife, Velocity, Pitchbend ±12 Halbtöne, CC7/CC11. Das Feld „MIDI dieses Samples“ "
        "zieht die passenden Noten auf die Spur. Das Sample wird im DAW-Projekt gespeichert. Alternativ eine WAV/AIFF/FLAC-Datei auf den Player ziehen.\n\n"
        "SAMPLER-MODUS\n"
        "Spurwerk ist zugleich ein Sampler: MIDI an das Plugin spielt die Samples des geladenen Moduls. Kanal N startet mit Sample N, "
        "Program Change wählt ein anderes Sample, Pitchbend (Bereich per RPN), CC7 und CC11 wirken. So passt es direkt zum MIDI-Export: "
        "Modul laden, MIDI exportieren, die MIDI-Spuren an Spurwerk schicken – eigene Noten, gleicher Klang. "
        "Bei YM-Songs spielt der Sampler-Modus eine Rechteckstimme wie der YM2149 (Kanal 10 = Rauschen).\n\n"
        "ARCHIV\n"
        "Der Reiter Archiv ist ein Katalog aller Module auf deinen Laufwerken. „Ordner hinzufügen …“ (oder einen Ordner ins Fenster ziehen) durchsucht den Ordner "
        "samt Unterordnern im Hintergrund; „Einlesen“ prüft später alle Archiv-Ordner erneut und analysiert nur neue oder geänderte Dateien. "
        "Für jedes Modul werden Titel, Format, Kanäle, Länge, Notenzahl, Samplenamen und die Song-Nachricht gespeichert, dazu ein Notenverlauf "
        "(Spalte „Verlauf“: Notendichte über die Spielzeit, gestapelt nach Samples).\n"
        "Dubletten werden schon beim Einlesen erkannt: „identisch“ (gleiche Datei), „gleiche Noten“ (gleiche Musik, z. B. umbenannt oder mit anderen Samplenamen) "
        "und „ähnlich“ (gleiche Melodien, auch transponiert oder in anderem Tempo; die Schwelle steht unter „Mehr ▾“). Filter „Nur Dubletten“ zeigt die Gruppen "
        "nebeneinander; unten links erscheint der Notenverlauf des gewählten Moduls und darunter der seiner ähnlichsten Datei. „Kein Duplikat“ trennt eine falsche Zuordnung.\n"
        "Bewertung (Sterne anklicken), Favorit (Herz) und eine eigene Notiz werden nur in der Datenbank gespeichert – Spurwerk verändert die Moduldateien dabei nie. "
        "Rechts unten stehen alle Infos samt Samplenamen zum Markieren und Kopieren (viele Musiker haben Grüße, Titel und Credits in die Samplenamen geschrieben); "
        "Im Infotext lässt sich Text mit der Maus markieren; „Text kopieren“, Strg+C und Strg+Einfg kopieren die Markierung – ohne Markierung den ganzen Infotext. Das Suchfeld durchsucht Titel, Datei, Künstler, Samplenamen, Nachricht und Notiz. "
        "Beim Tippen schlägt Spurwerk bekannte Einträge der Spalte vor, die den Text enthalten (z. B. „Mart“ → „Martin Galway“, „Bang Martinson“): "
        "Pfeil hoch/runter wählt, Enter übernimmt, Weitertippen ignoriert die Liste, Esc schließt sie. "
        "Strg+Z (oder „Mehr ▾ → Rückgängig“) nimmt die letzten Datenbank-Änderungen schrittweise zurück – Titel, Künstler, Komponist, Arrangeur, Bewertung, Favorit, Notiz, Sammelpool; "
        "eine Änderung an mehreren Zeilen ist ein Schritt. Gelöschte Dateien kommen so nicht zurück (dafür der Papierkorb).\n"
        "Die Tabelle arbeitet wie eine Tabellenkalkulation: In Titel, Künstler, Komponist und Arrangeur ist die aktive Zelle immer zur Eingabe bereit, ihr Inhalt ist markiert "
        "(Klick genügt, Einfügen mit Strg+V oder Umschalt+Einfg). Solange nichts geändert ist, wandern die Pfeiltasten und Tab in die Nachbarzelle (Umschalt+Hoch/Runter markiert Zeilen mit). "
        "Nach einer Änderung bewegen links/rechts nur die Schreibmarke im Feld, bis Enter speichert (eine Zeile tiefer) oder Esc den alten Inhalt zurückholt. "
        "Sind mehrere Zeilen markiert, steht die Eingabe in der zuletzt markierten Zeile und Enter setzt den Wert nach Rückfrage für alle. "
        "Zum Abspielen: Doppelklick auf eine nicht bearbeitbare Spalte (z. B. Datei) oder „Öffnen & spielen“. Gespeichert wird nur in der Datenbank, die Moduldatei bleibt unverändert; "
        "eigene Titel bleiben auch nach erneutem Einlesen.\n"
        "Tonart, Tongeschlecht und Skala bestimmt Spurwerk aus den Noten (Krumhansl-Kessler; Schlagzeug-Samples mit höchstens zwei Tonhöhen zählen nicht mit, „?“ = unsicher). "
        "Grundlage sind die Notennamen im Tracker – ist ein Sample nicht auf C gestimmt, verschiebt sich die erkannte Tonart entsprechend.\n"
        "SCHREIBSCHUTZ: Unter „Mehr ▾ → Schreibschutz“ lassen sich Ordner schützen (z. B. Spiele-Ordner). Spurwerk speichert, überschreibt und löscht dort nichts – "
        "auch Exporte und „Speichern als …“ in diese Ordner sind gesperrt. Einträge der Datenbank (Bewertung, Titel, Komponist, Notiz …) bleiben bearbeitbar. "
        "Geschützte Dateien erscheinen in leicht rötlicher Schrift.\n"
        "SAMMELPOOL: Fehlerhafte, fehlende und unlesbare Dateien (z. B. Spiel-Archive *.mod/*.stk oder unbekannte Amiga-Formate) sowie alles, was du ausschließt, "
        "landen im Sammelpool – aus der normalen Ansicht und der Dublettensuche heraus, sichtbar über den Filter „Sammelpool“. Ausgeschlossene Dateien werden beim Einlesen "
        "übersprungen, solange sie sich nicht ändern; „Wieder aufnehmen“ holt sie zurück. Die Dateien selbst bleiben dabei unverändert.\n"
        "Rechtsklick auf eine Zeile: öffnen, Bewertung, Favorit, Dubletten, „Ausschließen“ bzw. „Wieder aufnehmen“ und – rot – "
        "„Datei von der Festplatte löschen …“ (nach Rückfrage in den Papierkorb; auf Netzlaufwerken ohne Papierkorb endgültig).\n"
        "Doppelklick oder Enter lädt das Modul und spielt es ab. Die Datenbank liegt unter %APPDATA%\\Spurwerk\\archiv.swdb.\n\n"
        "LUPE\n"
        "Rechtsklick (außerhalb des Pattern-Rasters, dort im Kontextmenü unter „Lupe …“) schaltet eine Lupe ein, die der Maus folgt; Vergrößerung und Größe im selben Menü.");
    info = U (
        "SPURWERK " JucePlugin_VersionString " – Tracker für MOD, XM, S3M, IT, MPTM, MTM, 669, FAR, MDL, OctaMED, Future Composer, TCB, OKT und Atari-YM als VST3 und Standalone\n"
        "von Un(d)AbtanzBAR\n\n"
        "LINKS\n"
        "Website: www.undabtanzbar.de\n"
        "GitHub: github.com/Yakobo1970/Spurwerk\n\n"
        "ENTWICKLUNG\n"
        "Idee, Konzept und Regie: Kay 'Yakobo' Fiedler.\n"
        "Umgesetzt mit Claude (Anthropic) als KI-Programmierassistent für C++/JUCE-Code, Abspiel-Engines, Formate und Tests.\n\n"
        "TECHNIK\n"
        "JUCE (VST3 und Standalone). Eigener Replayer für ProTracker, FastTracker 2, Scream Tracker 3, Impulse Tracker, MultiTracker, Composer 669, Farandole, Digitrakker, OctaMED, Future Composer (Makro-Interpreter), TCB Tracker und Oktalyzer (Amiga- und lineare Perioden, Hüllkurven, IT- und MDL-Sample-Entpacker, PowerPacker-Entpacker), eigene YM2149-Emulation "
        "mit Hüllkurven, Rauschgenerator, SID-Stimmen, Digidrums und Sync-Buzzer, eigener LHA-Entpacker (-lh5-) für YM-Dateien. "
        "Bedienkonzept angelehnt an klassische PC-Tracker wie OpenMPT/ModPlug Tracker und FastTracker – keine Verbindung zu diesen Projekten. "
        "Das YM-Format stammt von Arnaud Carré (Leonard/Oxygene). Die Formatbeschreibungen für 669, FAR und MDL folgen den Ladern von OpenMPT (BSD-Lizenz).\n\n"
        "UNTERSTÜTZEN\n"
        "Spurwerk ist kostenlos. Wer die Entwicklung unterstützen möchte, kann über den Knopf unten eine kleine Spende per PayPal schicken – vielen Dank!");
    for (auto* b : { &tabGuide, &tabInfo })
    {
        b->setRadioGroupId (303);
        b->setClickingTogglesState (true);
        addAndMakeVisible (b);
    }
    tabGuide.onClick = [this] { showPage (0); };
    tabInfo.onClick = [this] { showPage (1); };
    donate.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffd6336c));
    donate.setTooltip (kDonateUrl);
    website.setTooltip (kWebsiteUrl);
    github.setTooltip (kGithubUrl);
    donate.onClick = [] { juce::URL (kDonateUrl).launchInDefaultBrowser(); };
    website.onClick = [] { juce::URL (kWebsiteUrl).launchInDefaultBrowser(); };
    github.onClick = [] { juce::URL (kGithubUrl).launchInDefaultBrowser(); };
    for (auto* b : { &donate, &website, &github }) addAndMakeVisible (b);
    addAndMakeVisible (text);
    addAndMakeVisible (close);
    close.onClick = [this] { ed.showHelp (false); };
    showPage (0);
}

void HelpPanel::showPage (int page)
{
    (page == 0 ? tabGuide : tabInfo).setToggleState (true, juce::dontSendNotification);
    text.setText (page == 0 ? guide : info, false);
    text.moveCaretToTop (false);
}

void HelpPanel::paint (juce::Graphics& g)
{
    const auto& th = ed.theme();
    g.fillAll (th.bg.withAlpha (0.7f));
    auto box = getLocalBounds().withSizeKeepingCentre (820, 680).toFloat();
    g.setColour (th.panel);
    g.fillRoundedRectangle (box, 12.0f);
    g.setColour (th.accent);
    g.drawRoundedRectangle (box, 12.0f, 1.5f);
}

void HelpPanel::resized()
{
    auto box = getLocalBounds().withSizeKeepingCentre (820, 680).reduced (18);
    auto tabs = box.removeFromTop (30);
    tabGuide.setBounds (tabs.removeFromLeft (120));
    tabs.removeFromLeft (6);
    tabInfo.setBounds (tabs.removeFromLeft (120));
    box.removeFromTop (10);
    auto bottom = box.removeFromBottom (32);
    close.setBounds (bottom.removeFromRight (110));
    donate.setBounds (bottom.removeFromLeft (190));
    bottom.removeFromLeft (8);
    website.setBounds (bottom.removeFromLeft (170));
    bottom.removeFromLeft (8);
    github.setBounds (bottom.removeFromLeft (90));
    box.removeFromBottom (10);
    text.setBounds (box);
}

// ============================================================================================ Editor
SpurwerkEditor::SpurwerkEditor (SpurwerkProcessor& p) : AudioProcessorEditor (p), proc (p)
{
    setLookAndFeel (&look);
    files = std::make_unique<FileList> (*this);
    instruments = std::make_unique<InstrumentList> (*this);
    orders = std::make_unique<OrderList> (*this);
    patterns = std::make_unique<PatternView> (*this);
    samples = std::make_unique<SampleView> (*this);
    general = std::make_unique<GeneralTab> (*this);
    exporter = std::make_unique<ExportTab> (*this);
    archiveTab = std::make_unique<ArchiveTab> (*this);
    addChildComponent (files.get());
    addAndMakeVisible (instruments.get());
    for (juce::Component* c : { (juce::Component*) orders.get(), (juce::Component*) patterns.get(), (juce::Component*) samples.get(),
                                (juce::Component*) general.get(), (juce::Component*) exporter.get(), (juce::Component*) archiveTab.get() })
        addChildComponent (c);

    for (auto* b : { &newBtn, &openBtn, &playBtn, &playPatBtn, &stopBtn, &recBtn, &followBtn,
                     &octDown, &octUp, &stepDown, &stepUp, &helpBtn })
        addAndMakeVisible (b);
    int ai = 0;
    for (auto* b : { &posIns, &posDel, &patNew, &patDup })
    {
        addChildComponent (b);
        b->onClick = [this, ai] { orderEdit (ai); };
        ++ai;
    }
    posIns.setTooltip (U ("Position hinter der aktuellen einfügen (gleiches Pattern)"));
    posDel.setTooltip (U ("Aktuelle Position aus der Liste entfernen"));
    patNew.setTooltip (U ("Neues leeres Pattern anlegen und hinter der aktuellen Position einfügen"));
    patDup.setTooltip (U ("Aktuelles Pattern kopieren und die Kopie hinter der aktuellen Position einfügen"));
    tabBtns[4].setTooltip (U ("Modul-Archiv: Ordner durchsuchen, Dubletten finden, bewerten, Samplenamen lesen"));
    for (int i = 0; i < 5; ++i)
    {
        addAndMakeVisible (tabBtns[i]);
        tabBtns[i].setWantsKeyboardFocus (false);   // keys never toggle a tab button behind the scenes
        tabBtns[i].onClick = [this, i] { showTab (i); };   // the active tab is set by showTab alone (no radio group)
    }
    // order list edit buttons live in the pattern tab header (drawn by orders component area)
    newBtn.onClick = [this] { askNewSong(); };
    openBtn.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> (U ("Modul öffnen"), files->folder(), supportedWildcards());
        juce::Component::SafePointer<SpurwerkEditor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
            if (safe != nullptr && fc.getResult().existsAsFile()) safe->openFile (fc.getResult());
        });
    };
    saveBtn.onClick = [this] { saveSong (false); };
    saveAsBtn.onClick = [this] { saveSong (true); };
    playBtn.onClick = [this] { playSong (false); };
    playPatBtn.onClick = [this] { playPattern(); };
    stopBtn.onClick = [this] { stopPlay(); };
    recBtn.setClickingTogglesState (true);
    recBtn.onClick = [this] { record = recBtn.getToggleState() && ! proc.song().readOnly(); updateHeader(); patterns->repaint(); patterns->grabKeyboardFocus(); };
    followBtn.setClickingTogglesState (true);
    followBtn.setToggleState (true, juce::dontSendNotification);
    followBtn.onClick = [this] { follow = followBtn.getToggleState(); };
    octDown.onClick = [this] { octave = juce::jmax (0, octave - 1); updateHeader(); };
    octUp.onClick = [this] { octave = juce::jmin (8, octave + 1); updateHeader(); };
    stepDown.onClick = [this] { editStep = juce::jmax (0, editStep - 1); updateHeader(); };
    stepUp.onClick = [this] { editStep = juce::jmin (16, editStep + 1); updateHeader(); };
    helpBtn.onClick = [this] { showHelp (helpPanel == nullptr); };
    playBtn.setTooltip ("Song ab Anfang spielen (F5), F7 = ab gewählter Position");
    playPatBtn.setTooltip ("Aktuelles Pattern wiederholen (F6)");
    stopBtn.setTooltip ("Stopp (F8 / Esc)");
    recBtn.setTooltip ("Aufnahme/Editiermodus (Leertaste)");
    followBtn.setTooltip ("Ansicht folgt der Wiedergabe");
    helpBtn.setTooltip ("Anleitung und Info");
    saveBtn.setTooltip ("Als ProTracker-MOD speichern (Strg+S)");
    recBtn.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffe03e4e));

    loupe = std::make_unique<Loupe>();
    addChildComponent (*loupe);
    catcher = std::make_unique<RightClickCatcher>();
    catcher->onRightClick = [this] { showLoupeMenu(); };
    catcher->passThrough = [this] (juce::Point<int> p) {
        if (helpPanel != nullptr) return true;
        if (tab == 1 && patterns->isVisible() && patterns->getBounds().contains (p)) return true;   // pattern grid has its own menu
        if (tab == 4 && archiveTab->isVisible() && archiveTab->tableBounds().contains (p)) return true;   // archive table has its own menu (incl. Lupe)
        return false;
    };
    addAndMakeVisible (*catcher);

    const auto folder = juce::File (proc.uiState.getProperty ("folder", juce::File::getSpecialLocation (juce::File::userMusicDirectory).getFullPathName()).toString());
    files->setFolder (folder.isDirectory() ? folder : juce::File::getSpecialLocation (juce::File::userMusicDirectory));

    applyTheme();
    setResizable (true, true);
    setResizeLimits (1060, 660, 2600, 1800);
    setSize ((int) proc.uiState.getProperty ("w", kW), (int) proc.uiState.getProperty ("h", kH));
    setWantsKeyboardFocus (true);
    showTab (1);
    songLoaded();
    startTimerHz (30);
}

SpurwerkEditor::~SpurwerkEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void SpurwerkEditor::applyTheme()
{
    const int ti = juce::jlimit (0, (int) themes().size() - 1, (int) proc.uiState.getProperty ("theme", 0));
    const bool lightOn = proc.uiState.getProperty ("light", false);
    look.setTheme (lightOn ? lightVariant (themes()[(size_t) ti]) : themes()[(size_t) ti]);
    sendLookAndFeelChange();
    if (archiveTab != nullptr) archiveTab->update();
    repaint();
}

void SpurwerkEditor::songLoaded()
{
    lastVersion = proc.songVersion();
    instrument = juce::jlimit (1, juce::jmax (1, proc.song().numSamples()), instrument);
    if (proc.song().readOnly()) { record = false; recBtn.setToggleState (false, juce::dontSendNotification); }
    recBtn.setEnabled (! proc.song().readOnly());
    patterns->songChanged();
    orders->scroll = 0;
    orders->repaint();
    instruments->update();
    samples->update();
    general->update();
    exporter->update();
    if (proc.song().sourcePath.isNotEmpty()) files->selectFile (juce::File (proc.song().sourcePath));
    files->repaint();
    updateHeader();
    for (auto* b : { &posIns, &posDel, &patNew, &patDup }) b->setVisible (tab == 1 && ! proc.song().readOnly());
    repaint();
}

void SpurwerkEditor::songEdited()
{
    shareDue = juce::Time::getMillisecondCounter() + 2000;   // re-publish for the Spurwerk Player once editing pauses
    proc.touch();
    lastVersion = proc.songVersion();
    instruments->update();
    orders->repaint();
    patterns->repaint();
    updateHeader();
}

void SpurwerkEditor::selectInstrument (int i)
{
    instrument = juce::jlimit (1, juce::jmax (1, proc.song().numSamples()), i);
    instruments->update();
    samples->update();
    updateHeader();
}

void SpurwerkEditor::showTab (int t)
{
    tab = juce::jlimit (0, 4, t);
    for (int i = 0; i < 5; ++i) tabBtns[i].setToggleState (i == tab, juce::dontSendNotification);
    general->setVisible (tab == 0);
    orders->setVisible (tab == 1);
    patterns->setVisible (tab == 1);
    for (auto* b : { &posIns, &posDel, &patNew, &patDup }) b->setVisible (tab == 1 && ! proc.song().readOnly());
    samples->setVisible (tab == 2);
    exporter->setVisible (tab == 3);
    archiveTab->setVisible (tab == 4);
    if (tab == 0) general->update();
    if (tab == 2) samples->update();
    if (tab == 3) exporter->update();
    if (tab == 4) archiveTab->update();
    if (tab == 1 && isShowing()) patterns->grabKeyboardFocus();
    resized();
}

void SpurwerkEditor::openFile (const juce::File& f)
{
    juce::String err;
    if (! proc.loadFile (f, err))
    {
        setStatus (U ("Laden fehlgeschlagen: ") + f.getFileName() + " – " + err);
        return;
    }
    songLoaded();
    setStatus (f.getFileName() + " geladen (" + proc.song().formatName + ")");
}

void SpurwerkEditor::playSong (bool fromCursor)
{
    proc.play (SpurwerkProcessor::Play::Song, fromCursor ? proc.cursorOrder.load() : 0, 0);
    patterns->grabKeyboardFocus();
}

void SpurwerkEditor::playPattern()
{
    proc.play (SpurwerkProcessor::Play::Pattern, proc.cursorOrder.load(), 0);
}

void SpurwerkEditor::stopPlay()
{
    if (proc.playState() != SpurwerkProcessor::Play::Stop && follow)
    {
        proc.cursorOrder = proc.posOrder();   // keep the view where playback stopped
        patterns->cursorRow = proc.posRow();
    }
    proc.stop();
    patterns->setCursorOrder (proc.cursorOrder.load());
}

void SpurwerkEditor::askNewSong()
{
    juce::PopupMenu m;
    m.addSectionHeader ("Neues Modul");
    for (int c : { 4, 6, 8, 16 }) m.addItem (c, juce::String (c) + U (" Kanäle"));
    juce::Component::SafePointer<SpurwerkEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&newBtn), [safe] (int r) {
        if (safe == nullptr || r == 0) return;
        safe->proc.newSong (r);
        safe->instrument = 1;
        safe->songLoaded();
        safe->setStatus ("Neues Modul mit " + juce::String (r) + U (" Kanälen"));
    });
}

bool SpurwerkEditor::writeBlocked (const juce::File& target)
{
    if (! proc.archive->isProtected (target.getFullPathName())) return false;
    setStatus (U ("Schreibgeschützter Ordner – nichts gespeichert: ") + target.getParentDirectory().getFullPathName());
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, U ("Schreibgeschützter Ordner"),
        target.getFullPathName() + U ("\n\nliegt in einem schreibgeschützten Ordner (Archiv → Mehr ▾ → Schreibschutz). Spurwerk schreibt, überschreibt und löscht dort nichts. "
                                      "Bitte einen anderen Speicherort wählen."));
    return true;
}

void SpurwerkEditor::saveConverted()
{
    auto& s = proc.song();
    auto* aw = new juce::AlertWindow (U ("Als Modul speichern"),
        U ("In welchem System soll der Song gespeichert werden?\n\n")
        + U ("ProTracker MOD: bis 32 Kanäle, 31 Samples, Noten C-1..B-3, 64 Zeilen je Pattern – läuft fast überall.\n")
        + U ("OctaMED (MMD1): bis 32 Spuren, 63 Samples, größerer Tonumfang, Patterns beliebiger Länge.\n\n")
        + U ("Was das Ziel nicht kennt (Hüllkurven, zweite Effekte, YM-Klang, FC-Makros), geht verloren – Spurwerk zeigt danach, was nicht übernommen wurde."),
        juce::MessageBoxIconType::QuestionIcon, this);
    aw->addComboBox ("target", { U ("ProTracker MOD (.mod)"), U ("OctaMED MMD1 (.med)") });
    if (auto* cb = aw->getComboBoxComponent ("target")) cb->setSelectedItemIndex (s.channels > 8 || s.format == Format::MED ? 1 : 0, juce::dontSendNotification);
    aw->addButton ("Weiter", 1, juce::KeyPress (juce::KeyPress::returnKey));
    aw->addButton ("Abbrechen", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<SpurwerkEditor> safe (this);
    aw->enterModalState (true, juce::ModalCallbackFunction::create ([safe, aw] (int r) {
        if (safe == nullptr || r != 1) return;
        const auto target = aw->getComboBoxComponent ("target")->getSelectedItemIndex() == 1 ? SaveTarget::MED : SaveTarget::MOD;
        const juce::String ext = target == SaveTarget::MED ? "med" : "mod";
        auto& song = safe->proc.song();
        const juce::File current (song.sourcePath);
        auto start = (current.existsAsFile() ? current.getParentDirectory() : safe->files->folder())
                         .getChildFile (safeFileName (song.title.isNotEmpty() ? song.title : juce::String ("spurwerk")) + "." + ext);
        safe->chooser = std::make_unique<juce::FileChooser> (saveTargetName (target) + " speichern", start, "*." + ext);
        safe->chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe, target, ext] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            const auto f = fc.getResult().withFileExtension (ext);
            if (safe->writeBlocked (f)) return;
            juce::String report;
            bool ok;
            { const juce::ScopedLock sl (safe->proc.lock); ok = saveSongAs (safe->proc.song(), target, f, report); }
            safe->setStatus (ok ? U ("Gespeichert: ") + f.getFileName() : "Fehler: " + report);
            if (ok && report.contains (U ("nicht übernommen")))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, U ("Gespeichert: ") + f.getFileName(), report);
            safe->exporter->update();
        });
    }), true);
}

void SpurwerkEditor::saveSong (bool askName)
{
    auto& s = proc.song();
    if (s.readOnly())
    {
        saveConverted();   // other formats: choose MOD or OctaMED
        return;
    }
    const juce::File current (s.sourcePath);
    if (! askName && current.existsAsFile() && current.hasFileExtension ("mod"))
    {
        if (writeBlocked (current)) return;
        juce::String err;
        setStatus (proc.saveModFile (current, err) ? "Gespeichert: " + current.getFileName() : "Fehler: " + err);
        files->setFolder (files->folder());
        return;
    }
    auto start = (current.existsAsFile() ? current.getParentDirectory() : files->folder())
                     .getChildFile (safeFileName (s.title.isNotEmpty() ? s.title : juce::String ("spurwerk")) + ".mod");
    chooser = std::make_unique<juce::FileChooser> ("Als MOD speichern", start, "*.mod");
    juce::Component::SafePointer<SpurwerkEditor> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe] (const juce::FileChooser& fc) {
        if (safe == nullptr || fc.getResult() == juce::File()) return;
        juce::String err;
        const auto f = fc.getResult().withFileExtension ("mod");
        if (safe->writeBlocked (f)) return;
        safe->setStatus (safe->proc.saveModFile (f, err) ? "Gespeichert: " + f.getFileName() : "Fehler: " + err);
        safe->files->setFolder (f.getParentDirectory());
        safe->updateHeader();
    });
}

void SpurwerkEditor::orderEdit (int action)
{
    auto& s = proc.song();
    if (s.readOnly()) return;
    const int o = juce::jlimit (0, juce::jmax (0, (int) s.orders.size() - 1), proc.cursorOrder.load());
    {
        const juce::ScopedLock sl (proc.lock);
        if (action == 0 && s.orders.size() < 128)
            s.orders.insert (s.orders.begin() + juce::jmin ((int) s.orders.size(), o + 1), s.orders.empty() ? 0 : s.orders[(size_t) o]);
        else if (action == 1 && s.orders.size() > 1)
            s.orders.erase (s.orders.begin() + o);
        else if ((action == 2 || action == 3) && s.patterns.size() < 128 && s.orders.size() < 128)
        {
            Pattern np;
            np.resize (64, s.channels);
            if (action == 3 && ! s.orders.empty())
            {
                const int src = s.orders[(size_t) o];
                if (src >= 0 && src < (int) s.patterns.size()) np = s.patterns[(size_t) src];
            }
            s.patterns.push_back (np);
            s.orders.insert (s.orders.begin() + juce::jmin ((int) s.orders.size(), o + 1), (int) s.patterns.size() - 1);
        }
        else return;
        if (s.restartPos >= (int) s.orders.size()) s.restartPos = 0;
    }
    proc.songStructureChanged();
    patterns->setCursorOrder (action == 1 ? juce::jmin (o, (int) s.orders.size() - 1) : o + 1);
    songEdited();
}

void SpurwerkEditor::updateHeader()
{
    recBtn.setToggleState (record, juce::dontSendNotification);
    repaint (headerArea);
    repaint (infoArea);
}

void SpurwerkEditor::showHelp (bool show)
{
    if (show)
    {
        helpPanel = std::make_unique<HelpPanel> (*this);
        addAndMakeVisible (*helpPanel);
        helpPanel->setBounds (getLocalBounds());
    }
    else helpPanel.reset();
    if (catcher) catcher->toFront (false);
    if (loupe) loupe->toFront (false);
}

void SpurwerkEditor::timerCallback()
{
    ++timerTicks;
    for (int i = 0; i < 5; ++i)   // exactly one tab button lit: the visible tab
        if (tabBtns[i].getToggleState() != (i == tab)) tabBtns[i].setToggleState (i == tab, juce::dontSendNotification);
    if (proc.songVersion() != lastVersion) songLoaded();   // state restored by the host
    patterns->refresh();
    if (proc.playState() != SpurwerkProcessor::Play::Stop && timerTicks % 3 == 0) repaint (infoArea);
    if (status.isNotEmpty() && juce::Time::getMillisecondCounter() - statusTime > 8000) { status.clear(); repaint (infoArea); }
    if (timerTicks % 2 == 0) updateLoupe();
    if (shareDue != 0 && juce::Time::getMillisecondCounter() > shareDue) { shareDue = 0; proc.shareSamples(); }
    if (timerTicks % 30 == 0)
    {
        proc.uiState.setProperty ("w", getWidth(), nullptr);
        proc.uiState.setProperty ("h", getHeight(), nullptr);
    }
}

void SpurwerkEditor::paint (juce::Graphics& g)
{
    const auto& th = theme();
    g.fillAll (th.bg);
    g.setColour (th.panel);
    g.fillRect (headerArea);
    g.fillRect (0, headerArea.getBottom(), kSideW + 8, getHeight());
    // logo
    {
        auto logo = headerArea.withWidth (172).reduced (12, 8);
        g.setColour (th.accent);
        for (int i = 0; i < 4; ++i)
        {
            g.setColour (th.track[(size_t) i]);
            g.fillRoundedRectangle ((float) logo.getX() + i * 6.0f, (float) logo.getY() + 4 + (i % 2) * 6, 4.0f, 18.0f - (i % 2) * 6, 1.5f);
        }
        g.setColour (th.text);
        g.setFont (font (18.0f, true));
        g.drawText ("SPURWERK", logo.withTrimmedLeft (28).withTrimmedBottom (9), juce::Justification::centredLeft, false);
        g.setColour (th.dim);
        g.setFont (font (10.5f));
        g.drawText ("Version " SW_VERSION, logo.withTrimmedLeft (29).removeFromBottom (11), juce::Justification::centredLeft, false);
    }
    g.setColour (th.dim);
    g.setFont (font (12.0f));
    g.drawText ("Oktave", octArea.withWidth (50), juce::Justification::centredLeft);
    g.drawText ("Schritt", stepArea.withWidth (50), juce::Justification::centredLeft);
    g.setColour (th.text);
    g.setFont (mono (15.0f, true));
    g.drawText (juce::String (octave), octDown.getRight(), octDown.getY(), octUp.getX() - octDown.getRight(), octDown.getHeight(), juce::Justification::centred);
    g.drawText (juce::String (editStep), stepDown.getRight(), stepDown.getY(), stepUp.getX() - stepDown.getRight(), stepDown.getHeight(), juce::Justification::centred);

    // info bar
    const auto& s = proc.song();
    g.setColour (th.panel2);
    g.fillRect (infoArea);
    auto ia = infoArea.reduced (10, 0);
    g.setFont (font (13.5f, true));
    g.setColour (th.text);
    const juce::String title = s.title.isNotEmpty() ? s.title : juce::String ("(ohne Titel)");
    g.drawText (title + (proc.modified ? " *" : ""), ia.removeFromLeft (230), juce::Justification::centredLeft);
    g.setFont (font (12.5f));
    g.setColour (th.dim);
    g.drawText (s.formatName, ia.removeFromLeft (360), juce::Justification::centredLeft);
    const bool playing = proc.playState() != SpurwerkProcessor::Play::Stop;
    const int o = playing ? proc.posOrder() : proc.cursorOrder.load();
    juce::String pos;
    const int pi = (o >= 0 && o < (int) s.orders.size()) ? s.orders[(size_t) o] : 0;
    pos << "Position " << juce::String (o).paddedLeft ('0', 2) << "/" << juce::String (juce::jmax (0, (int) s.orders.size() - 1)).paddedLeft ('0', 2)
        << U ("  ·  Pattern ") << juce::String (pi).paddedLeft ('0', 2)
        << U ("  ·  Zeile ") << juce::String (playing ? proc.posRow() : patterns->cursorRow).paddedLeft ('0', 2);
    if (s.format != Format::YM)
        pos << U ("  ·  Speed ") << (playing ? proc.posSpeed() : s.initialSpeed) << U ("  ·  Tempo ") << (playing ? proc.posTempo() : s.initialTempo);
    pos << U ("  ·  Instr. ") << hex2 (instrument);
    if (playing)
    {
        const int secs = (int) proc.playSeconds();
        pos << U ("  ·  ") << secs / 60 << ":" << juce::String (secs % 60).paddedLeft ('0', 2);
    }
    g.setColour (playing ? th.accent : th.text);
    g.drawText (pos, ia, juce::Justification::centredLeft);
    if (status.isNotEmpty())
    {
        g.setColour (th.accent);
        g.drawText (status, statusArea, juce::Justification::centredRight);
    }
    // pattern tab: order list label + buttons area background
    if (tab == 1)
    {
        g.setColour (th.panel);
        g.fillRect (orders->getBounds().withX (kSideW + 8).withRight (getWidth()));
        g.setColour (th.dim);
        g.setFont (font (11.5f, true));
        g.drawText ("POSITIONEN", kSideW + 16, orders->getY(), 80, orders->getHeight(), juce::Justification::centredLeft);
    }
}

void SpurwerkEditor::resized()
{
    auto r = getLocalBounds();
    headerArea = r.removeFromTop (46);
    infoArea = r.removeFromTop (26);
    statusArea = infoArea.withTrimmedLeft (infoArea.getWidth() / 2).reduced (10, 0);

    auto h = headerArea.reduced (8, 8);
    h.removeFromLeft (166);
    auto place = [&h] (juce::Component& c, int w, int gap = 4) { c.setBounds (h.removeFromLeft (w)); h.removeFromLeft (gap); };
    place (newBtn, 50); place (openBtn, 64, 18);   // saving moved to the Export tab
    place (playBtn, 80); place (playPatBtn, 92); place (stopBtn, 36); place (followBtn, 70, 18);
    octArea = h.withWidth (130);
    h.removeFromLeft (52);
    place (octDown, 26, 0); h.removeFromLeft (28); place (octUp, 26, 18);
    stepArea = h.withWidth (130);
    h.removeFromLeft (54);
    place (stepDown, 26, 0); h.removeFromLeft (28); place (stepUp, 26, 18);
    place (recBtn, 90);
    helpBtn.setBounds (headerArea.reduced (8, 8).removeFromRight (34));

    auto side = r.removeFromLeft (kSideW).reduced (8, 8);
    files->setVisible (false);   // file browsing: Archiv tab and the "Öffnen" button (FileList keeps the last folder)
    instruments->setBounds (side);
    r.removeFromLeft (8);

    auto tabs = r.removeFromTop (34).reduced (0, 4);
    for (int i = 0; i < 5; ++i)
    {
        tabBtns[i].setBounds (tabs.removeFromLeft (110));
        tabs.removeFromLeft (4);
    }
    general->setBounds (r);
    samples->setBounds (r);
    exporter->setBounds (r);
    archiveTab->setBounds (r);
    auto pr = r;
    auto ol = pr.removeFromTop (36);
    ol.removeFromLeft (90);
    auto ob = ol.removeFromRight (390).reduced (4, 5);
    for (auto* b : { &posIns, &posDel, &patNew, &patDup })
    {
        b->setBounds (ob.removeFromLeft (b == &patDup ? 130 : b == &patNew ? 112 : 60));
        ob.removeFromLeft (4);
    }
    orders->setBounds (ol);
    patterns->setBounds (pr);
    if (helpPanel) helpPanel->setBounds (getLocalBounds());
    if (catcher) { catcher->setBounds (getLocalBounds()); catcher->toFront (false); }
    if (loupe) loupe->toFront (false);
}

bool SpurwerkEditor::keyPressed (const juce::KeyPress& k)
{
    const int code = k.getKeyCode();
    if (k.getModifiers().isCommandDown())
    {
        const auto lc = juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) code);
        if (lc == 's') { if (k.getModifiers().isShiftDown()) saveConverted(); else saveSong (false); return true; }
        if (lc == 'o') { openBtn.triggerClick(); return true; }
    }
    if (code == juce::KeyPress::F5Key) { playSong (false); return true; }
    if (code == juce::KeyPress::F6Key) { playPattern(); return true; }
    if (code == juce::KeyPress::F7Key) { playSong (true); return true; }
    if (code == juce::KeyPress::F8Key) { stopPlay(); return true; }
    if (code == juce::KeyPress::escapeKey && helpPanel != nullptr) { showHelp (false); return true; }
    if (code == juce::KeyPress::F1Key) { showHelp (helpPanel == nullptr); return true; }
    return false;
}

bool SpurwerkEditor::isInterestedInFileDrag (const juce::StringArray& fs)
{
    for (auto& f : fs)
    {
        const juce::File file (f);
        if (isSupportedFile (file) || file.hasFileExtension ("wav;aif;aiff;flac") || file.isDirectory()) return true;
    }
    return false;
}

void SpurwerkEditor::filesDropped (const juce::StringArray& fs, int, int)
{
    for (auto& f : fs)
    {
        const juce::File file (f);
        if (file.isDirectory())   // folder: into the archive
        {
            showTab (4);
            archiveTab->addFolder (file);
            continue;
        }
        if (file.hasFileExtension ("wav;aif;aiff;flac"))
        {
            showTab (2);
            juce::String err;
            if (! samples->loadWavInto (file, err)) setStatus (err);
            return;
        }
        if (isSupportedFile (file)) { openFile (file); return; }
    }
}

void SpurwerkEditor::showLoupeMenu()
{
    auto ui = proc.uiState;
    const bool on = ui.getProperty ("loupeOn", false);
    const int zoom = ui.getProperty ("loupeZoom", 250), size = ui.getProperty ("loupeSize", 1);
    juce::PopupMenu m;
    m.addSectionHeader ("Lupe");
    m.addItem (1, on ? "Lupe ausschalten" : "Lupe einschalten");
    m.addSeparator();
    m.addSectionHeader (U ("Vergrößerung"));
    for (int i = 0; i < (int) std::size (LoupeSettings::kZooms); ++i)
    {
        const int z = LoupeSettings::kZooms[i];
        m.addItem (10 + i, juce::String (z / 100.0, z % 100 == 0 ? 0 : 1).replaceCharacter ('.', ',') + U ("×"), true, zoom == z);
    }
    m.addSeparator();
    m.addSectionHeader (U ("Lupengröße"));
    const char* sizes[] = { "klein", "mittel", "groß" };
    for (int i = 0; i < 3; ++i) m.addItem (20 + i, U (sizes[i]), true, size == i);
    juce::Component::SafePointer<SpurwerkEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withMousePosition(), [safe] (int r) {
        if (safe == nullptr || r == 0) return;
        auto ui2 = safe->proc.uiState;
        if (r == 1) ui2.setProperty ("loupeOn", ! (bool) ui2.getProperty ("loupeOn", false), nullptr);
        else if (r >= 10 && r < 20) { ui2.setProperty ("loupeZoom", LoupeSettings::kZooms[r - 10], nullptr); ui2.setProperty ("loupeOn", true, nullptr); }
        else if (r >= 20) { ui2.setProperty ("loupeSize", r - 20, nullptr); ui2.setProperty ("loupeOn", true, nullptr); }
        safe->updateLoupe();
    });
}

void SpurwerkEditor::updateLoupe()
{
    if (loupe == nullptr) return;
    auto ui = proc.uiState;
    const auto mouse = getMouseXYRelative();
    if (! (bool) ui.getProperty ("loupeOn", false) || ! getLocalBounds().contains (mouse) || ! isShowing())
    {
        loupe->setVisible (false);
        return;
    }
    const float zoom = (float) (int) ui.getProperty ("loupeZoom", 250) / 100.0f;
    const auto size = LoupeSettings::windowSize ((int) ui.getProperty ("loupeSize", 1));
    const int gap = 48;
    const juce::Point<int> candidates[] = { { mouse.x + gap, mouse.y + gap }, { mouse.x - gap - size.x, mouse.y + gap },
                                            { mouse.x + gap, mouse.y - gap - size.y }, { mouse.x - gap - size.x, mouse.y - gap - size.y } };
    auto pos = candidates[0];
    for (auto c : candidates)
        if (getLocalBounds().contains (juce::Rectangle<int> (c.x, c.y, size.x, size.y))) { pos = c; break; }
    loupe->setBounds (juce::jlimit (0, juce::jmax (0, getWidth() - size.x), pos.x),
                      juce::jlimit (0, juce::jmax (0, getHeight() - size.y), pos.y), size.x, size.y);
    const auto area = juce::Rectangle<float> ((float) size.x / zoom, (float) size.y / zoom).withCentre (mouse.toFloat()).getSmallestIntegerContainer();
    const float display = (float) juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds())->scale;
    loupe->setVisible (false);
    auto img = createComponentSnapshot (area, true, zoom * display);
    loupe->frame = theme().accent;
    loupe->setImage (img);
    loupe->setVisible (true);
}
