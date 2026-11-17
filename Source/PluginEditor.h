#pragma once
#include "PluginProcessor.h"
#include "Look.h"
#include "Loupe.h"
#include "PatternView.h"
#include "SampleView.h"
#include "ArchiveTab.h"

class SpurwerkEditor;

namespace sw::ui
{
// File browser list (left side) – shows supported modules of one folder.
class FileList : public juce::Component, private juce::ListBoxModel
{
public:
    explicit FileList (SpurwerkEditor& e);
    void setFolder (const juce::File& f);
    juce::File folder() const { return dir; }
    void resized() override;
    void paint (juce::Graphics&) override;
    void selectFile (const juce::File& f);
private:
    SpurwerkEditor& ed;
    juce::File dir;
    juce::Array<juce::File> entries;   // sub folders first, then files
    juce::ListBox list { "Dateien", this };
    juce::TextButton upBtn { juce::String::fromUTF8 ("↑") }, folderBtn { juce::String::fromUTF8 ("Ordner …") };
    std::unique_ptr<juce::FileChooser> chooser;
    int getNumRows() override { return entries.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int row) override;
    void open (int row);
};

// Instrument list (left side, below the files): selects the instrument used for note entry.
class InstrumentList : public juce::Component, private juce::ListBoxModel
{
public:
    explicit InstrumentList (SpurwerkEditor& e);
    void resized() override { list.setBounds (getLocalBounds().withTrimmedTop (22)); }
    void paint (juce::Graphics&) override;
    void update();
private:
    SpurwerkEditor& ed;
    juce::ListBox list { "Instrumente", this };
    int getNumRows() override;
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
};

class GeneralTab : public juce::Component
{
public:
    explicit GeneralTab (SpurwerkEditor& e);
    void paint (juce::Graphics&) override;
    void resized() override;
    void update();
private:
    SpurwerkEditor& ed;
    juce::TextEditor title, comment;
    juce::ComboBox channels, syncMode, keyboard, themeBox, subSong;
    juce::Slider speed, tempo, separation, volume, restart;
    juce::ToggleButton interp { "Interpolation (weicher Klang)" }, sampler { "Sampler-Modus: MIDI-Eingang spielt die Samples" },
                       light { "Helles Thema" };
    juce::Label info;
    bool updating = false;
    void layoutLabel (juce::Graphics&, juce::Component&, const char* text);
};

class ExportTab : public juce::Component
{
public:
    explicit ExportTab (SpurwerkEditor& e);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void update();
private:
    SpurwerkEditor& ed;
    juce::ComboBox midiMode, bendRange;
    juce::TextButton saveMidi { juce::String::fromUTF8 ("MIDI speichern …") }, saveSamples { juce::String::fromUTF8 ("Samples exportieren …") },
        saveWav { juce::String::fromUTF8 ("Song als WAV …") }, saveDaw { juce::String::fromUTF8 ("DAWproject speichern …") },
        saveAls { juce::String::fromUTF8 ("Ableton Live Set …") };
    juce::Label summary;
    juce::Rectangle<int> midiDrag, dawDrag, sampleDrag, wavDrag, midiSaveBox, alsBox, saveBox, saveAsBox;
    juce::File makeTempDawProject();
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String lastInfo;
    juce::File makeTempMidi();
    void doDrag (int which);
    juce::Rectangle<int> helpArea() const;
};

class HelpPanel : public juce::Component
{
public:
    explicit HelpPanel (SpurwerkEditor& e);
    void paint (juce::Graphics&) override;
    void resized() override;
    void showPage (int page);
    static constexpr const char* kWebsiteUrl = "https://www.undabtanzbar.de";
    static constexpr const char* kDonateUrl = "https://paypal.me/yakobo";
    static constexpr const char* kGithubUrl = "https://github.com/Yakobo1970/Spurwerk";
private:
    SpurwerkEditor& ed;
    juce::TextEditor text;
    juce::String guide, info;
    juce::TextButton tabGuide { "Anleitung" }, tabInfo { "Info" }, close { juce::String::fromUTF8 ("Schließen") },
        donate { "Spenden (PayPal)" }, website { "undabtanzbar.de" }, github { "GitHub" };
};
} // namespace sw::ui

class SpurwerkEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget,
                       public juce::DragAndDropContainer, private juce::Timer
{
public:
    explicit SpurwerkEditor (SpurwerkProcessor&);
    ~SpurwerkEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    SpurwerkProcessor& proc;
    const sw::Theme& theme() const { return look.theme(); }
    void applyTheme();

    // editing state
    int instrument = 1, octave = 4, editStep = 1;
    juce::uint32 shareDue = 0;
    bool record = false, follow = true;
    bool qwertz() const { return (int) proc.uiState.getProperty ("keys", 0) == 0; }

    void setStatus (const juce::String& s) { status = s; statusTime = juce::Time::getMillisecondCounter(); repaint (statusArea); }
    void songEdited();              // pattern/sample data changed by the UI
    void songLoaded();              // new song in the processor
    void selectInstrument (int i);
    void showTab (int t);
    int currentTab() const { return tab; }
    void openFile (const juce::File& f);
    void playSong (bool fromCursor);
    void playPattern();
    void stopPlay();
    void showLoupeMenu();
    void showHelp (bool show);
    void updateHeader();
    void saveSong (bool askName);   // MOD songs: overwrite / save as .mod
    void saveConverted();           // dialog: target system (MOD / OctaMED), converts any song
    bool writeBlocked (const juce::File& target);   // true (with a message) if target lies in a write-protected folder

    sw::ui::PatternView& patternView() { return *patterns; }
    sw::ui::FileList& fileList() { return *files; }
    sw::ui::ArchiveTab& archive() { return *archiveTab; }

private:
    sw::ui::Look look;
    juce::TooltipWindow tooltips { this, 600 };
    // header
    juce::TextButton newBtn { "Neu" }, openBtn { juce::String::fromUTF8 ("Öffnen") }, saveBtn { "Speichern" }, saveAsBtn { "Speichern unter" },
        playBtn { juce::String::fromUTF8 ("▶ Song") }, playPatBtn { juce::String::fromUTF8 ("▶ Pattern") }, stopBtn { juce::String::fromUTF8 ("■") },
        recBtn { "Aufnahme" }, followBtn { "Folgen" }, octDown { "-" }, octUp { "+" }, stepDown { "-" }, stepUp { "+" }, helpBtn { "?" },
        posIns { "+ Pos" }, posDel { juce::String::fromUTF8 ("− Pos") }, patNew { "Neues Pattern" }, patDup { "Pattern kopieren" };
    juce::TextButton tabBtns[5] { juce::TextButton ("Allgemein"), juce::TextButton ("Patterns"), juce::TextButton ("Samples"), juce::TextButton ("Export"), juce::TextButton ("Archiv") };
    juce::Rectangle<int> headerArea, infoArea, statusArea, octArea, stepArea;
    std::unique_ptr<sw::ui::FileList> files;
    std::unique_ptr<sw::ui::InstrumentList> instruments;
    std::unique_ptr<sw::ui::OrderList> orders;
    std::unique_ptr<sw::ui::PatternView> patterns;
    std::unique_ptr<sw::ui::SampleView> samples;
    std::unique_ptr<sw::ui::GeneralTab> general;
    std::unique_ptr<sw::ui::ExportTab> exporter;
    std::unique_ptr<sw::ui::ArchiveTab> archiveTab;
    std::unique_ptr<sw::ui::HelpPanel> helpPanel;
    std::unique_ptr<sw::ui::Loupe> loupe;
    std::unique_ptr<sw::ui::RightClickCatcher> catcher;
    std::unique_ptr<juce::FileChooser> chooser;
    int tab = 1;
    juce::String status;
    juce::uint32 statusTime = 0, lastVersion = 0;
    int timerTicks = 0;

    void timerCallback() override;
    void updateLoupe();
    void orderEdit (int action);
    void askNewSong();
    friend class sw::ui::OrderList;
    friend class sw::ui::PatternView;
};
