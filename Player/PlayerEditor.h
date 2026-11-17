#pragma once
#include "PlayerProcessor.h"
#include "../Source/Look.h"

class PlayerEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget,
                     public juce::DragAndDropContainer, private juce::Timer
{
public:
    explicit PlayerEditor (PlayerProcessor& p);
    ~PlayerEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int, int) override;
    void refreshModules (bool keepSelection);   // re-reads the shared folder

private:
    PlayerProcessor& proc;
    sw::ui::Look look;
    juce::ComboBox moduleBox, sampleBox;
    juce::TextButton refreshBtn { juce::String::fromUTF8 ("↻") }, fileBtn { juce::String::fromUTF8 ("Datei …") };
    juce::Slider gainSlider;
    std::unique_ptr<juce::SliderParameterAttachment> gainAttach;
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<sw::SharedModule> modules;
    juce::Rectangle<int> waveArea, midiArea, infoArea;
    std::vector<std::pair<float, float>> peaks;
    juce::uint32 shownVersion = 0;
    juce::String status;
    bool midiPressed = false, midiDragged = false, wavePressed = false;
    int timerTicks = 0;
    bool updating = false;
    void moduleChosen();
    void sampleChosen();
    void rebuildPeaks();
    void timerCallback() override;
};
