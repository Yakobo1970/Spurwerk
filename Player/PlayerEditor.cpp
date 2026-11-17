#include "PlayerEditor.h"
#include "../Source/Theme.h"

using namespace sw;
using namespace sw::ui;

namespace
{
constexpr int kW = 640, kH = 360;

juce::String secs (double s) { return juce::String (s, 2).replaceCharacter ('.', ','); }
} // namespace

PlayerEditor::PlayerEditor (PlayerProcessor& p) : AudioProcessorEditor (p), proc (p)
{
    look.setTheme (themes()[0]);
    setLookAndFeel (&look);
    for (auto* b : { &moduleBox, &sampleBox }) addAndMakeVisible (b);
    moduleBox.setTextWhenNothingSelected (U ("Modul aus Spurwerk wählen …"));
    moduleBox.setTextWhenNoChoicesAvailable (U ("Kein Spurwerk-Modul gefunden – Modul in Spurwerk laden"));
    sampleBox.setTextWhenNothingSelected (U ("Sample wählen …"));
    sampleBox.setTextWhenNoChoicesAvailable (U ("–"));
    moduleBox.onChange = [this] { if (! updating) moduleChosen(); };
    sampleBox.onChange = [this] { if (! updating) sampleChosen(); };
    addAndMakeVisible (refreshBtn);
    addAndMakeVisible (fileBtn);
    refreshBtn.setTooltip (U ("Liste der in Spurwerk geladenen Module neu einlesen"));
    fileBtn.setTooltip (U ("WAV/AIFF/FLAC laden (Grundton und Schleife aus dem smpl-Chunk, sonst C-5)"));
    refreshBtn.onClick = [this] { refreshModules (true); status = U ("Liste aktualisiert"); repaint(); };
    fileBtn.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Sample laden", juce::File(), "*.wav;*.aif;*.aiff;*.flac", false);
        juce::Component::SafePointer<PlayerEditor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || ! fc.getResult().existsAsFile()) return;
            juce::String err;
            safe->status = safe->proc.loadAudioFile (fc.getResult(), err) ? U ("Geladen: ") + fc.getResult().getFileName() : err;
            safe->refreshModules (true);
            safe->repaint();
        });
    };
    gainSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    gainSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
    addAndMakeVisible (gainSlider);
    gainAttach = std::make_unique<juce::SliderParameterAttachment> (*proc.gain, gainSlider);
    setSize (kW, kH);
    refreshModules (true);
    startTimerHz (10);
}

PlayerEditor::~PlayerEditor()
{
    setLookAndFeel (nullptr);
}

void PlayerEditor::refreshModules (bool keepSelection)
{
    updating = true;
    modules = listSharedModules();
    moduleBox.clear (juce::dontSendNotification);
    int sel = 0;
    for (int i = 0; i < (int) modules.size(); ++i)
    {
        const auto& m = modules[(size_t) i];
        moduleBox.addItem (m.title + "  (" + juce::String ((int) m.samples.size()) + " Samples)", i + 1);
        if (keepSelection && m.folder.getFileName() == proc.shareFolder) sel = i + 1;
    }
    moduleBox.setSelectedId (sel, juce::dontSendNotification);
    sampleBox.clear (juce::dontSendNotification);
    if (sel > 0)
    {
        const auto& m = modules[(size_t) sel - 1];
        for (int k = 0; k < (int) m.samples.size(); ++k)
        {
            sampleBox.addItem (m.samples[(size_t) k].name, k + 1);
            if (m.samples[(size_t) k].index == proc.shareIndex) sampleBox.setSelectedId (k + 1, juce::dontSendNotification);
        }
    }
    updating = false;
}

void PlayerEditor::moduleChosen()
{
    const int mi = moduleBox.getSelectedId() - 1;
    updating = true;
    sampleBox.clear (juce::dontSendNotification);
    if (mi >= 0 && mi < (int) modules.size())
        for (int k = 0; k < (int) modules[(size_t) mi].samples.size(); ++k) sampleBox.addItem (modules[(size_t) mi].samples[(size_t) k].name, k + 1);
    updating = false;
    if (sampleBox.getNumItems() > 0) sampleBox.setSelectedId (1);   // loads the first sample
}

void PlayerEditor::sampleChosen()
{
    const int mi = moduleBox.getSelectedId() - 1, si = sampleBox.getSelectedId() - 1;
    if (mi < 0 || mi >= (int) modules.size() || si < 0 || si >= (int) modules[(size_t) mi].samples.size()) return;
    juce::String err;
    const auto& m = modules[(size_t) mi];
    status = proc.loadShared (m, m.samples[(size_t) si], err) ? U ("Sample übernommen – es wird mit dem Projekt gespeichert") : err;
    repaint();
}

void PlayerEditor::rebuildPeaks()
{
    peaks.clear();
    const juce::ScopedLock sl (proc.lock);
    const auto* s = proc.sample();
    if (s == nullptr) return;
    const int bins = juce::jmax (1, waveArea.getWidth());
    peaks.resize ((size_t) bins);
    const size_t len = s->pcm.size();
    for (int b = 0; b < bins; ++b)
    {
        const size_t a = len * (size_t) b / (size_t) bins, z = juce::jmax (a + 1, len * (size_t) (b + 1) / (size_t) bins);
        float lo = 1.0f, hi = -1.0f;
        for (size_t k = a; k < z && k < len; ++k) { lo = juce::jmin (lo, s->pcm[k]); hi = juce::jmax (hi, s->pcm[k]); }
        peaks[(size_t) b] = { lo, hi };
    }
}

void PlayerEditor::timerCallback()
{
    ++timerTicks;
    if (proc.version() != shownVersion)
    {
        shownVersion = proc.version();
        rebuildPeaks();
        repaint();
    }
    if (timerTicks % 40 == 0 && ! moduleBox.isPopupActive() && ! sampleBox.isPopupActive())
    {
        const auto now = listSharedModules();
        bool changed = now.size() != modules.size();
        for (size_t i = 0; ! changed && i < now.size(); ++i) changed = now[i].folder != modules[i].folder || now[i].time != modules[i].time;
        if (changed) refreshModules (true);
    }
}

void PlayerEditor::resized()
{
    auto r = getLocalBounds().reduced (14);
    r.removeFromTop (34);
    auto row = r.removeFromTop (28);
    refreshBtn.setBounds (row.removeFromRight (34));
    row.removeFromRight (6);
    fileBtn.setBounds (row.removeFromRight (90));
    row.removeFromRight (10);
    moduleBox.setBounds (row.removeFromLeft (row.getWidth() / 2 - 5));
    row.removeFromLeft (10);
    sampleBox.setBounds (row);
    r.removeFromTop (10);
    auto bottom = r.removeFromBottom (44);
    gainSlider.setBounds (bottom.removeFromRight (220).withSizeKeepingCentre (220, 24));
    bottom.removeFromRight (90);
    midiArea = bottom.removeFromLeft (bottom.getWidth());
    r.removeFromBottom (8);
    infoArea = r.removeFromBottom (36);
    r.removeFromBottom (4);
    waveArea = r;
    rebuildPeaks();
}

void PlayerEditor::paint (juce::Graphics& g)
{
    const auto& th = look.theme();
    g.fillAll (th.bg);
    // title
    g.setColour (th.accent);
    g.setFont (font (18.0f, true));
    g.drawText ("SPURWERK PLAYER", 14, 10, 240, 26, juce::Justification::centredLeft);
    g.setColour (th.dim);
    g.setFont (font (12.5f));
    g.drawFittedText (U ("Ein Sample auf allen 128 Tasten · Grundton C-5 = MIDI 60 · Velocity, Pitchbend ±12"), 200, 10, getWidth() - 214, 26,
                      juce::Justification::centredRight, 1, 0.8f);

    // waveform
    g.setColour (th.panel);
    g.fillRoundedRectangle (waveArea.toFloat(), 7.0f);
    const auto w = waveArea.reduced (10, 8);
    const auto* s = proc.sample();
    if (s == nullptr)
    {
        g.setColour (th.dim);
        g.setFont (font (14.0f));
        g.drawFittedText (U ("Noch kein Sample.\nOben ein in Spurwerk geladenes Modul und ein Sample wählen,\noder eine WAV/AIFF/FLAC-Datei hierher ziehen."),
                          w, juce::Justification::centred, 3);
    }
    else
    {
        if (s->hasLoop())
        {
            const double len = (double) s->pcm.size();
            const int x0 = w.getX() + (int) (s->loopStart / len * w.getWidth());
            const int x1 = w.getX() + (int) ((s->loopStart + s->loopLength) / len * w.getWidth());
            g.setColour (th.accent.withAlpha (0.13f));
            g.fillRect (x0, w.getY(), juce::jmax (1, x1 - x0), w.getHeight());
        }
        const float mid = (float) w.getCentreY(), half = (float) w.getHeight() * 0.48f;
        g.setColour (th.track[0]);
        for (int x = 0; x < w.getWidth(); ++x)
        {
            const size_t b = (size_t) juce::jlimit (0, juce::jmax (0, (int) peaks.size() - 1), (x + 10) * (int) peaks.size() / juce::jmax (1, waveArea.getWidth()));
            if (peaks.empty()) break;
            g.drawVerticalLine (w.getX() + x, mid - peaks[b].second * half, mid - peaks[b].first * half + 1.0f);
        }
    }

    // info line
    g.setColour (th.text);
    g.setFont (font (14.0f, true));
    auto ia = infoArea;
    g.drawText (proc.sampleName.isNotEmpty() ? proc.sampleName : juce::String ("–"), ia.removeFromTop (18), juce::Justification::centredLeft, true);
    g.setColour (status.isNotEmpty() ? th.accent : th.dim);
    g.setFont (font (12.5f));
    juce::String info;
    if (status.isNotEmpty()) info = status;
    else if (s != nullptr)
        info << (proc.sourceTitle.isNotEmpty() ? proc.sourceTitle + U ("  ·  ") : juce::String()) << secs ((double) s->pcm.size() / s->c5Rate()) << U (" s auf C-5  ·  ")
             << (s->hasLoop() ? U ("Schleife") : U ("ohne Schleife")) << U ("  ·  Klick ins Wellenbild = vorhören");
    g.drawText (info, ia, juce::Justification::centredLeft, true);

    // MIDI drag area
    const bool hasMidi = proc.midiData.getSize() > 0;
    g.setColour (hasMidi ? th.track[2].withAlpha (midiPressed ? 0.35f : 0.18f) : th.panel);
    g.fillRoundedRectangle (midiArea.toFloat(), 7.0f);
    g.setColour (hasMidi ? th.track[2] : th.dim);
    juce::Path p;
    p.addRoundedRectangle (midiArea.toFloat().reduced (1.5f), 7.0f);
    float dash[] = { 6.0f, 4.0f };
    juce::PathStrokeType (1.5f).createDashedStroke (p, p, dash, 2);
    g.fillPath (p);
    g.setFont (font (13.0f, hasMidi));
    g.drawFittedText (hasMidi ? U ("MIDI dieses Samples ↗ auf die Spur ziehen") : U ("Kein MIDI (Sample stammt aus einer Datei)"),
                      midiArea.reduced (10, 2), juce::Justification::centred, 2);
    g.setColour (th.dim);
    g.drawText (U ("Lautstärke"), gainSlider.getX() - 86, gainSlider.getY(), 80, gainSlider.getHeight(), juce::Justification::centredRight);
}

void PlayerEditor::mouseDown (const juce::MouseEvent& e)
{
    midiPressed = midiArea.contains (e.getPosition()) && proc.midiData.getSize() > 0;
    midiDragged = false;
    wavePressed = waveArea.contains (e.getPosition()) && proc.sample() != nullptr;
    if (wavePressed) proc.preview (true);
    repaint (midiArea);
}

void PlayerEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (! midiPressed || midiDragged || e.getDistanceFromDragStart() < 6) return;
    midiDragged = true;
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Spurwerk").getChildFile ("player");
    dir.createDirectory();
    auto name = juce::File::createLegalFileName (proc.sampleName.isNotEmpty() ? proc.sampleName : juce::String ("Spurwerk Sample")).replaceCharacters (" ", "_");
    const auto f = dir.getChildFile (name + ".mid");
    if (f.replaceWithData (proc.midiData.getData(), proc.midiData.getSize()))
        juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, this);
    midiPressed = false;
    repaint (midiArea);
}

void PlayerEditor::mouseUp (const juce::MouseEvent&)
{
    if (wavePressed) proc.preview (false);
    if (midiPressed && ! midiDragged) { status = U ("Das Feld mit der Maus auf eine Spur der DAW ziehen"); repaint(); }
    midiPressed = wavePressed = false;
    repaint (midiArea);
}

bool PlayerEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac")) return true;
    return false;
}

void PlayerEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (const auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac"))
        {
            juce::String err;
            status = proc.loadAudioFile (juce::File (f), err) ? U ("Geladen: ") + juce::File (f).getFileName() : err;
            refreshModules (true);
            repaint();
            return;
        }
}
