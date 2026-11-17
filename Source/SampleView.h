// Sample editor: waveform with loop markers, name/volume/finetune/loop, WAV import/export.
#pragma once
#include "Look.h"
#include "Song.h"

class SpurwerkEditor;

namespace sw::ui
{
// Overview: every sample as a small waveform tile, two per row.
// Click = preview, drag or hold = WAV to the DAW, "MIDI" badge = notes of this sample, double click = edit.
class SampleGrid : public juce::Component, private juce::Timer
{
public:
    explicit SampleGrid (SpurwerkEditor& e);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void refresh (int width);            // rebuild peaks + height
    std::function<void (int)> onEdit;    // sample index (0-based)

private:
    SpurwerkEditor& ed;
    std::vector<std::vector<std::pair<float, float>>> peaks;
    int pressed = -1;
    bool pressedMidi = false, dragDone = false;
    juce::uint32 lastSongVersion = 0;
    static constexpr int kTileH = 104, kGap = 8, kPeakBins = 300;
    juce::Rectangle<int> tile (int i) const;
    juce::Rectangle<int> midiBadge (int i) const;
    int tileAt (juce::Point<int> p) const;
    void startDrag (int i, bool midi);
    void timerCallback() override;
};

class SampleView : public juce::Component
{
public:
    explicit SampleView (SpurwerkEditor& e);
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void update();                       // instrument changed / song loaded
    void setGridMode (bool g);
    bool loadWavInto (const juce::File& f, juce::String& error);   // replaces the current sample

private:
    SpurwerkEditor& ed;
    juce::TextEditor name;
    juce::Slider volume, finetune, loopStart, loopLength;
    juce::ToggleButton loopOn { "Schleife" };
    juce::TextButton loadBtn { "WAV laden ..." }, saveBtn { "Als WAV speichern ..." }, playBtn { juce::String::fromUTF8 ("▶ Vorhören") },
        clearBtn { juce::String::fromUTF8 ("Löschen") }, normBtn { "Normalisieren" }, revBtn { "Umkehren" };
    juce::Rectangle<int> wave, infoArea;
    juce::TextButton modeBtn, saveAllBtn { juce::String::fromUTF8 ("Alle Samples als WAV & MIDI speichern …") };
    juce::Viewport viewport;
    SampleGrid grid;
    bool gridMode = true;
    std::unique_ptr<juce::FileChooser> chooser;
    int dragging = 0;   // 1 = loop start, 2 = loop end
    bool updating = false;
    Sample* sample();
    void changed();
    int frameAt (int x) const;
    int xForFrame (int f) const;
};
} // namespace sw::ui
