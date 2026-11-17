#include "SampleView.h"
#include "PluginEditor.h"
#include "Export.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace sw::ui
{
// ============================================================================================ SampleGrid
namespace
{
juce::File dragDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Spurwerk").getChildFile ("drag");
    d.createDirectory();
    return d;
}
} // namespace

SampleGrid::SampleGrid (SpurwerkEditor& e) : ed (e) {}

juce::Rectangle<int> SampleGrid::tile (int i) const
{
    const int w = (getWidth() - kGap) / 2;
    return { (i % 2) * (w + kGap), (i / 2) * (kTileH + kGap), w, kTileH };
}

juce::Rectangle<int> SampleGrid::midiBadge (int i) const
{
    const auto t = tile (i);
    return { t.getRight() - 54, t.getY() + 6, 46, 18 };
}

int SampleGrid::tileAt (juce::Point<int> p) const
{
    const int n = ed.proc.song().hasSamples() ? ed.proc.song().numSamples() : 0;
    for (int i = 0; i < n; ++i)
        if (tile (i).contains (p)) return i;
    return -1;
}

void SampleGrid::refresh (int width)
{
    const auto& song = ed.proc.song();
    const int n = song.hasSamples() ? song.numSamples() : 0;
    {
        const juce::ScopedLock sl (ed.proc.lock);
        peaks.assign ((size_t) n, {});
        for (int i = 0; i < n; ++i)
        {
            const auto& pcm = song.samples[(size_t) i].pcm;
            if (pcm.empty()) continue;
            auto& pk = peaks[(size_t) i];
            pk.resize (kPeakBins);
            const size_t len = pcm.size();
            for (int b = 0; b < kPeakBins; ++b)
            {
                const size_t a = len * (size_t) b / kPeakBins, z = juce::jmax (a + 1, len * (size_t) (b + 1) / kPeakBins);
                float lo = 1.0f, hi = -1.0f;
                for (size_t k = a; k < z && k < len; ++k) { lo = juce::jmin (lo, pcm[k]); hi = juce::jmax (hi, pcm[k]); }
                pk[(size_t) b] = { lo, hi };
            }
        }
    }
    const int rows = (n + 1) / 2;
    setSize (juce::jmax (100, width), juce::jmax (60, rows * (kTileH + kGap)));
    repaint();
}

void SampleGrid::paint (juce::Graphics& g)
{
    const auto& th = ed.theme();
    const auto& song = ed.proc.song();
    if (! song.hasSamples())
    {
        g.setColour (th.dim);
        g.setFont (font (15.0f));
        g.drawText (U ("YM-Songs haben keine Samples."), getLocalBounds(), juce::Justification::centred);
        return;
    }
    const auto clip = g.getClipBounds();
    for (int i = 0; i < (int) peaks.size() && i < song.numSamples(); ++i)
    {
        const auto t = tile (i);
        if (! t.intersects (clip)) continue;
        const auto& smp = song.samples[(size_t) i];
        const bool sel = i + 1 == ed.instrument;
        const auto col = th.track[(size_t) (i % 8)];
        g.setColour (th.panel);
        g.fillRoundedRectangle (t.toFloat(), 7.0f);
        g.setColour (sel ? th.accent : th.panel2);
        g.drawRoundedRectangle (t.toFloat().reduced (0.75f), 7.0f, sel ? 2.0f : 1.0f);

        auto head = t.withHeight (30).reduced (10, 0);
        g.setFont (font (13.0f, true));
        g.setColour (col);
        g.drawText (hex2 (i + 1), head.removeFromLeft (24), juce::Justification::centredLeft);
        juce::String info;
        if (! smp.pcm.empty())
        {
            info << juce::String ((double) smp.pcm.size() / smp.c5Rate(), 2).replaceCharacter ('.', ',') << " s";
            if (smp.hasLoop()) info << U (" · Loop");
        }
        head.removeFromRight (56);
        g.setFont (font (12.0f));
        g.setColour (th.dim);
        g.drawText (info, head.removeFromRight (90), juce::Justification::centredRight);
        g.setColour (th.text);
        g.setFont (font (13.0f));
        g.drawText (smp.name.isNotEmpty() ? smp.name : juce::String ("(ohne Namen)"), head, juce::Justification::centredLeft, true);

        const auto mb = midiBadge (i);
        const bool hasData = ! smp.pcm.empty();
        g.setColour (hasData ? col.withAlpha (0.25f) : th.panel2);
        g.fillRoundedRectangle (mb.toFloat(), 4.0f);
        g.setColour (hasData ? col : th.dim);
        g.setFont (font (11.0f, true));
        g.drawText ("MIDI", mb, juce::Justification::centred);

        const auto w = t.withTrimmedTop (30).reduced (10, 8);
        g.setColour (th.bg.withAlpha (0.6f));
        g.fillRoundedRectangle (w.toFloat(), 4.0f);
        const auto& pk = peaks[(size_t) i];
        if (pk.empty())
        {
            g.setColour (th.dim);
            g.setFont (font (12.0f));
            g.drawText ("leer", w, juce::Justification::centred);
            continue;
        }
        if (smp.hasLoop())
        {
            const double len = (double) smp.pcm.size();
            const int x0 = w.getX() + (int) (smp.loopStart / len * w.getWidth());
            const int x1 = w.getX() + (int) ((smp.loopStart + smp.loopLength) / len * w.getWidth());
            g.setColour (th.accent.withAlpha (0.12f));
            g.fillRect (x0, w.getY(), juce::jmax (1, x1 - x0), w.getHeight());
        }
        const float mid = (float) w.getCentreY(), half = (float) w.getHeight() * 0.48f;
        g.setColour (col);
        for (int x = 0; x < w.getWidth(); ++x)
        {
            const auto& p = pk[(size_t) juce::jmin (kPeakBins - 1, x * kPeakBins / juce::jmax (1, w.getWidth()))];
            g.drawVerticalLine (w.getX() + x, mid - p.second * half, mid - p.first * half + 1.0f);
        }
    }
}

void SampleGrid::mouseDown (const juce::MouseEvent& e)
{
    pressed = tileAt (e.getPosition());
    dragDone = false;
    if (pressed < 0) return;
    pressedMidi = midiBadge (pressed).contains (e.getPosition());
    ed.selectInstrument (pressed + 1);
    if (! pressedMidi) startTimer (450);   // hold = drag the WAV
}

void SampleGrid::mouseDrag (const juce::MouseEvent& e)
{
    if (pressed >= 0 && ! dragDone && e.getDistanceFromDragStart() > 6) startDrag (pressed, pressedMidi);
}

void SampleGrid::timerCallback()
{
    stopTimer();
    if (pressed >= 0 && ! dragDone && juce::ModifierKeys::currentModifiers.isLeftButtonDown()) startDrag (pressed, false);
}

void SampleGrid::mouseUp (const juce::MouseEvent&)
{
    stopTimer();
    if (pressed >= 0 && ! dragDone)
    {
        if (pressedMidi) ed.setStatus (U ("MIDI-Plakette in eine DAW-Spur ziehen: Noten dieses Samples (Grundton C-5 = MIDI 60)"));
        else
        {
            ed.proc.previewNote (pressed + 1, 60, true);
            juce::Component::SafePointer<SpurwerkEditor> safe (&ed);
            juce::Timer::callAfterDelay (1500, [safe] { if (safe != nullptr) safe->proc.previewNote (0, 0, false); });
        }
    }
    pressed = -1;
}

void SampleGrid::mouseDoubleClick (const juce::MouseEvent& e)
{
    stopTimer();
    const int i = tileAt (e.getPosition());
    if (i >= 0 && onEdit) onEdit (i);
}

void SampleGrid::startDrag (int i, bool midi)
{
    dragDone = true;
    stopTimer();
    const auto& song = ed.proc.song();
    if (i < 0 || i >= song.numSamples() || song.samples[(size_t) i].pcm.empty()) return;
    const auto& smp = song.samples[(size_t) i];
    const auto base = safeFileName (sampleTitle (song, i));
    juce::File f;
    if (midi)
    {
        MidiExportOptions o;
        o.onlySample = i;
        juce::MidiFile mf;
        juce::String sum;
        { const juce::ScopedLock sl (ed.proc.lock); mf = exportMidi (song, o, &sum); }
        if (sum.startsWith ("0 ")) { ed.setStatus (U ("Dieses Sample wird im Song nicht gespielt – kein MIDI")); return; }
        f = dragDir().getChildFile (base + ".mid");
        if (! writeMidiFile (mf, f)) return;
        ed.setStatus ("MIDI: " + sum);
    }
    else
    {
        f = dragDir().getChildFile (base + ".wav");
        bool ok;
        { const juce::ScopedLock sl (ed.proc.lock); ok = writeSampleWav (smp, f); }
        if (! ok) return;
        ed.setStatus (U ("WAV in die DAW ziehen (Grundton C-5 = MIDI 60, Schleife im smpl-Chunk)"));
    }
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, &ed);
}

// ============================================================================================ SampleView
SampleView::SampleView (SpurwerkEditor& e) : ed (e), grid (e)
{
    name.setInputRestrictions (22);
    name.setFont (font (14.0f));
    name.onTextChange = [this] {
        if (updating) return;
        if (auto* s = sample()) { s->name = name.getText(); changed(); }
    };
    for (auto* s : { &volume, &finetune, &loopStart, &loopLength })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 64, 22);
        addAndMakeVisible (s);
    }
    volume.setRange (0, 64, 1);
    finetune.setRange (-8, 7, 1);
    volume.onValueChange = [this] { if (! updating) if (auto* s = sample()) { s->volume = (int) volume.getValue(); changed(); } };
    finetune.onValueChange = [this] { if (! updating) if (auto* s = sample()) { s->finetune = (int) finetune.getValue(); changed(); } };
    auto loopChange = [this] {
        if (updating) return;
        auto* s = sample();
        if (s == nullptr) return;
        const juce::ScopedLock sl (ed.proc.lock);
        const int len = (int) s->pcm.size();
        if (loopOn.getToggleState() && len > 4)
        {
            s->loopStart = juce::jlimit (0, len - 4, (int) loopStart.getValue()) & ~1;
            s->loopLength = juce::jlimit (4, len - s->loopStart, (int) loopLength.getValue()) & ~1;
        }
        else s->loopLength = 0;
        changed();
    };
    loopStart.onValueChange = loopChange;
    loopLength.onValueChange = loopChange;
    loopOn.onClick = [this, loopChange] {
        if (auto* s = sample())
            if (loopOn.getToggleState() && s->loopLength <= 2)
            {
                updating = true;
                loopStart.setValue (0, juce::dontSendNotification);
                loopLength.setValue ((int) s->pcm.size() & ~1, juce::dontSendNotification);
                updating = false;
            }
        loopChange();
    };
    loadBtn.onClick = [this] {
        chooser = std::make_unique<juce::FileChooser> ("Sample laden", juce::File(), "*.wav;*.aif;*.aiff;*.flac");
        juce::Component::SafePointer<SampleView> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            juce::String err;
            if (! safe->loadWavInto (fc.getResult(), err)) safe->ed.setStatus (err);
        });
    };
    saveBtn.onClick = [this] {
        auto* s = sample();
        if (s == nullptr || s->pcm.empty()) return;
        chooser = std::make_unique<juce::FileChooser> ("Sample speichern",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (safeFileName (s->name.isNotEmpty() ? s->name : juce::String ("sample")) + ".wav"), "*.wav");
        juce::Component::SafePointer<SampleView> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            if (safe->ed.writeBlocked (fc.getResult())) return;
            if (auto* smp = safe->sample())
                safe->ed.setStatus (writeSampleWav (*smp, fc.getResult().withFileExtension ("wav")) ? juce::String ("WAV gespeichert (Grundton C-5, Schleife im smpl-Chunk)") : juce::String ("Fehler beim Speichern"));
        });
    };
    playBtn.onClick = [this] {
        ed.proc.previewNote (ed.instrument, 60, true);
        juce::Component::SafePointer<SampleView> safe (this);
        juce::Timer::callAfterDelay (1500, [safe] { if (safe != nullptr) safe->ed.proc.previewNote (0, 0, false); });
    };
    clearBtn.onClick = [this] {
        if (auto* s = sample())
        {
            { const juce::ScopedLock sl (ed.proc.lock); *s = Sample(); }
            changed(); update();
        }
    };
    normBtn.onClick = [this] {
        if (auto* s = sample())
        {
            float peak = 0.0f;
            for (auto v : s->pcm) peak = juce::jmax (peak, std::abs (v));
            if (peak > 0.0001f) { const juce::ScopedLock sl (ed.proc.lock); for (auto& v : s->pcm) v = juce::jlimit (-1.0f, 0.992f, v / peak); }
            changed();
        }
    };
    revBtn.onClick = [this] {
        if (auto* s = sample()) { { const juce::ScopedLock sl (ed.proc.lock); std::reverse (s->pcm.begin(), s->pcm.end()); } changed(); }
    };
    for (juce::Component* c : { (juce::Component*) &name, (juce::Component*) &loopOn, (juce::Component*) &loadBtn, (juce::Component*) &saveBtn,
                                (juce::Component*) &playBtn, (juce::Component*) &clearBtn, (juce::Component*) &normBtn, (juce::Component*) &revBtn })
        addAndMakeVisible (c);
    viewport.setViewedComponent (&grid, false);
    viewport.setScrollBarsShown (true, false);
    viewport.setScrollBarThickness (10);
    addChildComponent (viewport);
    addAndMakeVisible (modeBtn);
    modeBtn.onClick = [this] { setGridMode (! gridMode); };
    addChildComponent (saveAllBtn);
    saveAllBtn.setTooltip (U ("Jedes Sample als WAV (Name = Nummer + Samplename, ohne Namen Nummer + Modulname) und eine MIDI-Datei "
                              "mit einer Spur je Sample (Spurname = Samplename) in einen Ordner speichern"));
    saveAllBtn.onClick = [this] {
        const auto& song = ed.proc.song();
        if (! song.hasSamples()) return;
        const auto start = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                               .getChildFile (safeFileName (song.title.isNotEmpty() ? song.title : juce::String ("Spurwerk")));
        chooser = std::make_unique<juce::FileChooser> (U ("Ordner für Samples + MIDI wählen"), start.isDirectory() ? start : start.getParentDirectory(), "*", false);
        juce::Component::SafePointer<SampleView> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
                                  | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File()) return;
            const auto dir = fc.getResult().isDirectory() ? fc.getResult() : fc.getResult().getParentDirectory();
            if (safe->ed.writeBlocked (dir.getChildFile ("x"))) return;
            juce::String log;
            bool ok;
            { const juce::ScopedLock sl (safe->ed.proc.lock); ok = exportSamplesWavMidi (safe->ed.proc.song(), dir, log); }
            safe->ed.setStatus ((ok ? juce::String() : juce::String ("Fehler: ")) + log + U (" → ") + dir.getFullPathName());
        });
    };
    grid.onEdit = [this] (int i) { ed.selectInstrument (i + 1); setGridMode (false); };
    setGridMode (true);
    loadBtn.setTooltip (U ("WAV/AIFF/FLAC in dieses Sample laden (wird auf Mono, 16574 Hz = Originaltonhöhe bei C-6 umgerechnet)"));
    playBtn.setTooltip (U ("Spielt das Sample 1,5 s lang auf C-5"));
}

Sample* SampleView::sample()
{
    auto& s = ed.proc.song();
    if (! s.hasSamples() || ed.instrument < 1 || ed.instrument > s.numSamples()) return nullptr;
    return &s.samples[(size_t) ed.instrument - 1];
}

void SampleView::setGridMode (bool g)
{
    gridMode = g;
    for (juce::Component* c : { (juce::Component*) &name, (juce::Component*) &volume, (juce::Component*) &finetune, (juce::Component*) &loopStart,
                                (juce::Component*) &loopLength, (juce::Component*) &loopOn, (juce::Component*) &loadBtn, (juce::Component*) &saveBtn,
                                (juce::Component*) &playBtn, (juce::Component*) &clearBtn, (juce::Component*) &normBtn, (juce::Component*) &revBtn })
        c->setVisible (! g);
    viewport.setVisible (g);
    saveAllBtn.setVisible (g);
    modeBtn.setButtonText (g ? U ("Bearbeiten ▸") : U ("◂ Übersicht"));
    modeBtn.setTooltip (g ? U ("Gewähltes Sample im Editor öffnen (oder Doppelklick auf eine Kachel)") : U ("Alle Samples als Kacheln"));
    resized();
    update();
}

void SampleView::changed()
{
    ed.songEdited();
    repaint();
}

void SampleView::update()
{
    updating = true;
    auto* s = sample();
    const bool ok = s != nullptr;
    for (juce::Component* c : { (juce::Component*) &name, (juce::Component*) &volume, (juce::Component*) &finetune, (juce::Component*) &loopStart,
                                (juce::Component*) &loopLength, (juce::Component*) &loopOn, (juce::Component*) &loadBtn, (juce::Component*) &saveBtn,
                                (juce::Component*) &playBtn, (juce::Component*) &clearBtn, (juce::Component*) &normBtn, (juce::Component*) &revBtn })
        c->setEnabled (ok);
    if (ok)
    {
        name.setText (s->name, false);
        volume.setValue (s->volume, juce::dontSendNotification);
        finetune.setValue (s->finetune, juce::dontSendNotification);
        const int len = juce::jmax (4, (int) s->pcm.size());
        loopStart.setRange (0, len, 2);
        loopLength.setRange (0, len, 2);
        loopStart.setValue (s->loopStart, juce::dontSendNotification);
        loopLength.setValue (s->loopLength, juce::dontSendNotification);
        loopOn.setToggleState (s->hasLoop(), juce::dontSendNotification);
        loopStart.setEnabled (s->hasLoop());
        loopLength.setEnabled (s->hasLoop());
    }
    else name.setText ({}, false);
    updating = false;
    if (gridMode && viewport.getWidth() > 0) grid.refresh (viewport.getWidth() - viewport.getScrollBarThickness() - 2);
    repaint();
}

bool SampleView::loadWavInto (const juce::File& f, juce::String& error)
{
    auto* s = sample();
    if (s == nullptr) { error = U ("Kein Sample gewählt (YM-Songs haben keine Samples)"); return false; }
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (f));
    if (reader == nullptr) { error = "Format nicht lesbar"; return false; }
    const int len = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) reader->sampleRate * 60);
    juce::AudioBuffer<float> buf ((int) reader->numChannels, len);
    reader->read (&buf, 0, len, 0, true, true);
    std::vector<float> mono ((size_t) len);
    for (int i = 0; i < len; ++i)
    {
        float v = 0.0f;
        for (int c = 0; c < buf.getNumChannels(); ++c) v += buf.getSample (c, i);
        mono[(size_t) i] = v / (float) juce::jmax (1, buf.getNumChannels());
    }
    // resample: C-6 (2x C-5 rate) = original pitch
    const double target = 2.0 * 7093789.2 / 856.0;
    const double ratio = reader->sampleRate / target;
    const int outLen = juce::jmin (131070, (int) (len / ratio)) & ~1;
    std::vector<float> out ((size_t) outLen);
    juce::LagrangeInterpolator interp;
    interp.process (ratio, mono.data(), out.data(), outLen);
    {
        const juce::ScopedLock sl (ed.proc.lock);
        s->pcm = std::move (out);
        s->loopStart = s->loopLength = 0;
        s->finetune = 0;
        s->volume = 64;
        if (s->name.isEmpty()) s->name = f.getFileNameWithoutExtension().substring (0, 22);
    }
    changed();
    update();
    ed.setStatus ("Sample geladen: " + f.getFileName() + U (" – Originaltonhöhe auf C-6"));
    return true;
}

void SampleView::resized()
{
    auto r = getLocalBounds().reduced (14);
    if (gridMode)
    {
        auto top = r.removeFromTop (28);
        modeBtn.setBounds (top.removeFromRight (140));
        top.removeFromRight (8);
        saveAllBtn.setBounds (top.removeFromRight (300));
        r.removeFromTop (10);
        viewport.setBounds (r);
        grid.refresh (r.getWidth() - viewport.getScrollBarThickness() - 2);
        return;
    }
    auto row1 = r.removeFromTop (28);
    row1.removeFromLeft (90);
    name.setBounds (row1.removeFromLeft (260));
    row1.removeFromLeft (10);
    for (auto* b : { &loadBtn, &saveBtn, &playBtn, &clearBtn })
    {
        b->setBounds (row1.removeFromLeft (b == &saveBtn ? 170 : 120));
        row1.removeFromLeft (6);
    }
    r.removeFromTop (10);
    auto row2 = r.removeFromTop (26);
    row2.removeFromLeft (90);
    volume.setBounds (row2.removeFromLeft (220));
    row2.removeFromLeft (90);
    finetune.setBounds (row2.removeFromLeft (200));
    row2.removeFromLeft (16);
    normBtn.setBounds (row2.removeFromLeft (120));
    row2.removeFromLeft (6);
    revBtn.setBounds (row2.removeFromLeft (100));
    modeBtn.setBounds (row2.removeFromRight (130));
    r.removeFromTop (8);
    auto row3 = r.removeFromTop (26);
    loopOn.setBounds (row3.removeFromLeft (90));
    row3.removeFromLeft (70);
    loopStart.setBounds (row3.removeFromLeft (240));
    row3.removeFromLeft (80);
    loopLength.setBounds (row3.removeFromLeft (240));
    r.removeFromTop (10);
    infoArea = r.removeFromBottom (22);
    wave = r;
}

int SampleView::xForFrame (int f) const
{
    const auto& s = const_cast<SampleView*> (this)->sample();
    const int len = s ? juce::jmax (1, (int) s->pcm.size()) : 1;
    return wave.getX() + (int) ((double) f / len * wave.getWidth());
}

int SampleView::frameAt (int x) const
{
    const auto& s = const_cast<SampleView*> (this)->sample();
    const int len = s ? (int) s->pcm.size() : 0;
    return juce::jlimit (0, len, (int) ((double) (x - wave.getX()) / juce::jmax (1, wave.getWidth()) * len));
}

void SampleView::paint (juce::Graphics& g)
{
    const auto& th = ed.theme();
    g.fillAll (th.bg);
    if (gridMode)
    {
        g.setColour (th.dim);
        g.setFont (font (13.0f));
        const auto& s = ed.proc.song();
        g.drawFittedText (juce::String (s.hasSamples() ? s.numSamples() : 0)
                              + U (" Samples  ·  Klick = vorhören  ·  ziehen/halten = WAV in die DAW  ·  „MIDI“ ziehen = Noten des Samples  ·  Doppelklick = bearbeiten"),
                          getLocalBounds().reduced (14).removeFromTop (28).withTrimmedRight (460), juce::Justification::centredLeft, 2, 0.85f);
        return;
    }
    g.setColour (th.dim);
    g.setFont (font (12.5f));
    const auto lbl = [&] (juce::Component& c, const juce::String& t, int w) {
        g.drawText (t, c.getX() - w, c.getY(), w - 6, c.getHeight(), juce::Justification::centredRight);
    };
    lbl (name, "Sample " + hex2 (ed.instrument), 90);
    lbl (volume, U ("Lautstärke"), 90);
    lbl (finetune, "Feinstimmung", 90);
    lbl (loopStart, "Start", 70);
    lbl (loopLength, U ("Länge"), 80);

    g.setColour (th.panel);
    g.fillRoundedRectangle (wave.toFloat(), 6.0f);
    auto* s = sample();
    if (! ed.proc.song().hasSamples())
    {
        g.setColour (th.dim);
        g.setFont (font (15.0f));
        g.drawFittedText (U ("YM-Songs bestehen aus Registerdaten des Soundchips YM2149 – es gibt keine Samples.\n"
                             "Digidrums werden beim Abspielen mitgespielt."), wave.reduced (20), juce::Justification::centred, 3);
        return;
    }
    if (s == nullptr || s->pcm.empty())
    {
        g.setColour (th.dim);
        g.setFont (font (15.0f));
        g.drawText (U ("Leeres Sample – WAV laden oder Datei hierher ziehen"), wave, juce::Justification::centred);
        return;
    }
    const int len = (int) s->pcm.size();
    if (s->hasLoop())
    {
        const int x0 = xForFrame (s->loopStart), x1 = xForFrame (s->loopStart + s->loopLength);
        g.setColour (th.track[3].withAlpha (0.12f));
        g.fillRect (x0, wave.getY(), x1 - x0, wave.getHeight());
        g.setColour (th.track[3]);
        g.fillRect (x0 - 1, wave.getY(), 3, wave.getHeight());
        g.fillRect (x1 - 1, wave.getY(), 3, wave.getHeight());
    }
    const float cy = (float) wave.getCentreY(), hh = wave.getHeight() * 0.46f;
    g.setColour (th.panel2);
    g.drawHorizontalLine ((int) cy, (float) wave.getX(), (float) wave.getRight());
    const auto col = th.track[(size_t) ((ed.instrument - 1) % 8)];
    g.setColour (col);
    for (int x = 0; x < wave.getWidth(); ++x)
    {
        const int a = (int) ((double) x / wave.getWidth() * len), b = juce::jmax (a + 1, (int) ((double) (x + 1) / wave.getWidth() * len));
        float mn = 1.0f, mx = -1.0f;
        for (int i = a; i < b && i < len; ++i) { mn = juce::jmin (mn, s->pcm[(size_t) i]); mx = juce::jmax (mx, s->pcm[(size_t) i]); }
        if (mn > mx) continue;
        g.drawVerticalLine (wave.getX() + x, cy - mx * hh, cy - mn * hh + 1.0f);
    }
    g.setColour (th.dim);
    g.setFont (font (12.5f));
    juce::String info;
    info << U ("Länge ") << len << " Frames (" << juce::String (len / s->c5Rate(), 2).replaceCharacter ('.', ',') << " s auf C-5)  ·  Rate C-5: "
         << juce::roundToInt (s->c5Rate()) << " Hz";
    if (s->hasLoop()) info << "  ·  Schleife " << s->loopStart << " – " << (s->loopStart + s->loopLength);
    info << U ("  ·  Schleifenpunkte im Bild mit der Maus ziehen");
    g.drawText (info, infoArea, juce::Justification::centredLeft);
}

void SampleView::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) { ed.showLoupeMenu(); return; }
    auto* s = sample();
    dragging = 0;
    if (s == nullptr || ! s->hasLoop() || ! wave.contains (e.getPosition())) return;
    const int x0 = xForFrame (s->loopStart), x1 = xForFrame (s->loopStart + s->loopLength);
    if (std::abs (e.x - x0) < 8) dragging = 1;
    else if (std::abs (e.x - x1) < 8) dragging = 2;
}

void SampleView::mouseDrag (const juce::MouseEvent& e)
{
    auto* s = sample();
    if (s == nullptr || dragging == 0) return;
    const int f = frameAt (e.x) & ~1;
    {
        const juce::ScopedLock sl (ed.proc.lock);
        const int end = s->loopStart + s->loopLength;
        if (dragging == 1) { s->loopStart = juce::jlimit (0, end - 4, f); s->loopLength = end - s->loopStart; }
        else s->loopLength = juce::jlimit (4, (int) s->pcm.size() - s->loopStart, f - s->loopStart);
    }
    updating = true;
    loopStart.setValue (s->loopStart, juce::dontSendNotification);
    loopLength.setValue (s->loopLength, juce::dontSendNotification);
    updating = false;
    repaint();
}

void SampleView::mouseUp (const juce::MouseEvent&)
{
    if (dragging != 0) { dragging = 0; changed(); }
}
} // namespace sw::ui
