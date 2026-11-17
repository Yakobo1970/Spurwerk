// Renders the Spurwerk editor into PNG files (UI checks without a DAW).
// Usage: UiShot <out.png> <module|-> <tab 0-4> [theme 0-5] [light 0/1] [playSeconds] [help 0/1] [instrument]
#include "../Source/PluginProcessor.h"
#include "../Source/PluginEditor.h"
#include "../Source/ModFile.h"
#include "../Player/PlayerProcessor.h"
#include "../Player/PlayerEditor.h"

class UiShotApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "UiShot"; }
    const juce::String getApplicationVersion() override { return "1"; }

    void initialise (const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();
        if (args.size() > 0 && args[0] == "keytest") { keyTest(); quit(); return; }
        if (args.size() > 1 && args[0] == "player") { playerShot (args); quit(); return; }
        if (args.size() < 3) { quit(); return; }
        const juce::File out (args[0]);
        SpurwerkProcessor proc;
        proc.setPlayConfigDetails (0, 2, 48000.0, 512);
        proc.prepareToPlay (48000.0, 512);
        juce::String err;
        if (args[1] != "-" && ! proc.loadFile (juce::File (args[1]), err))
            juce::File (out.getFullPathName() + ".err.txt").replaceWithText (err);
        proc.uiState.setProperty ("theme", args.size() > 3 ? args[3].getIntValue() : 0, nullptr);
        proc.uiState.setProperty ("light", args.size() > 4 && args[4] == "1", nullptr);
        proc.uiState.setProperty ("folder", juce::File (args[1]).getParentDirectory().getFullPathName(), nullptr);
        const double playSecs = args.size() > 5 ? args[5].getDoubleValue() : 0.0;
        {
            SpurwerkEditor ed (proc);
            ed.setSize (1320, 840);
            ed.showTab (args[2].getIntValue());
            if (args.size() > 7 && args[2].getIntValue() != 4) ed.selectInstrument (args[7].getIntValue());
            if (args[2].getIntValue() == 4)   // archive: [7] = row to select, [8] = search text, [9] = filter id
                ed.archive().prepareShot (args.size() > 7 ? args[7].getIntValue() : -1, args.size() > 8 ? args[8].unquoted() : juce::String(),
                                          args.size() > 9 ? args[9].getIntValue() : 0);
            if (args[2].getIntValue() == 4 && args.size() > 10 && args[10] == "suggesttest")
                juce::File (out.getFullPathName() + ".suggest.txt").replaceWithText (ed.archive().suggestTest());
            if (args[2].getIntValue() == 4 && args.size() > 10 && args[10] == "cursortest")
                juce::File (out.getFullPathName() + ".cursor.txt").replaceWithText (ed.archive().cursorTest());
            if (args[2].getIntValue() == 4 && args.size() > 10 && args[10] == "copytest")
            {
                juce::String log;
                log << "marked[0,12) ctrl+ins: [" << ed.archive().copyTest (0, 12, false) << "]" << juce::newLine;
                log << "marked[0,12) button:   [" << ed.archive().copyTest (0, 12, true) << "]" << juce::newLine;
                const auto all = ed.archive().copyTest (0, 0, true);
                log << "nothing marked button: " << all.length() << " chars, starts [" << all.substring (0, 20) << "]" << juce::newLine;
                const auto all2 = ed.archive().copyTest (0, 0, false);
                log << "nothing marked ctrl+ins: " << all2.length() << " chars" << juce::newLine;
                juce::File (out.getFullPathName() + ".copy.txt").replaceWithText (log);
            }
            if (playSecs > 0.0)
            {
                proc.play (SpurwerkProcessor::Play::Song, 0, 0);
                juce::AudioBuffer<float> buf (2, 512);
                juce::MidiBuffer mb;
                for (int i = 0; i < (int) (playSecs * 48000.0 / 512.0); ++i) proc.processBlock (buf, mb);
                ed.patternView().refresh();
            }
            if (args.size() > 6 && args[6] == "1") ed.showHelp (true);
            ed.setStatus ({});   // clean screenshots (README, websites)
            auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            out.deleteFile();
            juce::FileOutputStream os (out);
            juce::PNGImageFormat().writeImageToStream (img, os);
        }
        quit();
    }
    void shutdown() override {}

    // UiShot player <out.png> [module|wav]: Spurwerk Player editor; a module is published first and its first sample chosen
    void playerShot (const juce::StringArray& args)
    {
        const juce::File out (args[1]);
        PlayerProcessor proc;
        proc.setPlayConfigDetails (0, 2, 48000.0, 512);
        proc.prepareToPlay (48000.0, 512);
        juce::String err;
        if (args.size() > 2)
        {
            const juce::File f (args[2]);
            if (f.hasFileExtension ("wav;aif;aiff;flac")) proc.loadAudioFile (f, err);
            else if (auto song = sw::loadSongFile (f, err))
            {
                sw::publishSongSamples (*song);
                for (const auto& m : sw::listSharedModules())
                    if (m.title == song->title && ! m.samples.empty()) { proc.loadShared (m, m.samples[juce::jmin (1, (int) m.samples.size() - 1)], err); break; }
            }
        }
        {
            PlayerEditor ed (proc);
            ed.refreshModules (true);
            auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
            out.deleteFile();
            juce::FileOutputStream os (out);
            juce::PNGImageFormat().writeImageToStream (img, os);
        }
    }

    // simulates typing in the pattern editor: notes, hex effect, undo, copy/paste, save/load round trip
    void keyTest()
    {
        SpurwerkProcessor proc;
        proc.setPlayConfigDetails (0, 2, 48000.0, 512);
        proc.prepareToPlay (48000.0, 512);
        juce::String log;
        int fails = 0;
        auto check = [&] (bool ok, const juce::String& what) { log << (ok ? "ok:   " : "FAIL: ") << what << juce::newLine; if (! ok) ++fails; };
        {
            SpurwerkEditor ed (proc);
            ed.setSize (1320, 840);
            ed.showTab (1);
            auto& pv = ed.patternView();
            auto& s = proc.song();
            s.samples[0].pcm.assign (2000, 0.5f);   // something to preview
            auto press = [&] (int code, juce::juce_wchar ch, juce::ModifierKeys m = {}) { pv.keyPressed (juce::KeyPress (code, m, ch)); };
            press (juce::KeyPress::spaceKey, ' ');                          // record on
            check (ed.record, "space toggles record");
            ed.instrument = 3;
            press ('Q', 'q');                                               // C-5 (octave 4 -> upper row = 5)
            check (s.patterns[0].at (0, 0).note == 61 && s.patterns[0].at (0, 0).instr == 3, "Q enters C-5 with instrument 03");
            check (pv.cursorRow == 1, "cursor advances by edit step");
            press ('Y', 'y');                                               // C-4 on QWERTZ
            check (s.patterns[0].at (1, 0).note == 49, "Y enters C-4 (QWERTZ)");
            pv.cursorRow = 0; pv.cursorSub = 3;
            press ('C', 'c'); press ('2', '2'); press ('0', '0');           // effect C20
            check (s.patterns[0].at (0, 0).fx == 0xC && s.patterns[0].at (0, 0).param == 0x20, "hex entry C20 in effect column");
            for (int i = 0; i < 3; ++i) press ('Z', 'z', juce::ModifierKeys::ctrlModifier);   // one undo step per digit
            check (s.patterns[0].at (0, 0).fx == 0 && s.patterns[0].at (0, 0).param == 0 && s.patterns[0].at (0, 0).note == 61, "3x Ctrl+Z undoes the effect digits");
            press ('Y', 'y', juce::ModifierKeys::ctrlModifier);
            check (s.patterns[0].at (0, 0).fx == 0xC, "Ctrl+Y redoes");
            pv.cursorRow = 0; pv.cursorSub = 0; pv.cursorChannel = 0;
            press ('C', 'c', juce::ModifierKeys::ctrlModifier);
            pv.cursorChannel = 2;
            press ('V', 'v', juce::ModifierKeys::ctrlModifier);
            check (s.patterns[0].at (0, 2).note == 61, "copy/paste cell to channel 3");
            pv.cursorChannel = 0; pv.cursorRow = 0;
            pv.transpose (12);
            check (s.patterns[0].at (0, 0).note == 73, "transpose +12");
            ed.record = false;
        }
        juce::String err;
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spurwerk_keytest.mod");
        check (proc.saveModFile (f, err), "save as MOD");
        SpurwerkProcessor p2;
        check (p2.loadFile (f, err) && p2.song().patterns[0].at (0, 0).note == 73 && p2.song().patterns[0].at (0, 2).note == 61, "reload keeps edits");
        log << (fails == 0 ? "ALL OK" : "FAILURES") << juce::newLine;
        juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("keytest.txt").replaceWithText (log);
        setApplicationReturnValue (fails == 0 ? 0 : 1);
    }
};

START_JUCE_APPLICATION (UiShotApp)
