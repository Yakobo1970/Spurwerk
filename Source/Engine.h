// Spurwerk playback: ProTracker replayer, YM2149 emulation and a MIDI driven sampler.
#pragma once
#include "Song.h"
#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>

namespace sw
{
// ============================================================================ ProTracker style replayer
class ModEngine
{
public:
    struct Channel
    {
        int sample = -1;          // 0-based
        double pos = 0.0;
        bool active = false;
        int period = 0, outPeriod = 0, finetune = 0;
        int volume = 0, outVolume = 0;
        int portaTarget = 0, portaSpeed = 0, slideMem = 0;
        int vibPos = 0, vibSpeed = 0, vibDepth = 0, vibWave = 0;
        int tremPos = 0, tremSpeed = 0, tremDepth = 0, tremWave = 0;
        int offsetMem = 0, loopRow = 0, loopCount = 0;
        bool glissando = false;
        float pan = 0.0f;         // -1..1
        Cell cell;
        int delayedNote = 0, delayedInstr = 0;
        // XM / S3M state
        int instr = 0, lastNote = 0;          // current instrument (1-based) and note
        int memPortaUp = 0, memPortaDown = 0, memVol = 0, memFineUp = 0, memFineDown = 0, memFineVolUp = 0, memFineVolDown = 0;
        int memGlobal = 0, memPan = 0, memRetrig = 0, memXUp = 0, memXDown = 0, memTremor = 0, s3mMem = 0;
        bool keyOn = true;
        int envVolTick = 0, envPanTick = 0, fadeVol = 65536;
        int avPos = 0, avSweep = 0, retrigCount = 0, tremorCount = 0;
        float mixVol = 0.0f, mixPan = 0.0f;   // final volume 0..64 (envelopes, fadeout, global volume) and panning
        // IT state
        int chanVol = 64, nna = 0, envPitchTick = 0, offsetHigh = 0, owner = -1;
        bool fading = false;
        std::array<int, 27> itMem {};
        // analysis (MIDI export): set when a note (re)starts
        bool triggered = false;
        int trigNote = 0, trigInstr = 0;
        bool cutFlag = false;
        float level = 0.0f;       // peak meter
        // Future Composer macro player
        struct Fc
        {
            bool on = false, freqStop = false, volStop = false, vibUp = true, bendStep = false, volBendStep = false, portaStep = false;
            int block = 0, index = 0, freqPos = 0, volPos = 0, freqWait = 0, volWait = 0, volSpeed = 1, volCount = 1;
            int pitch = 0, volume = 0, vibSpeed = 0, vibDepth = 0, vibDelay = 0, vibValue = 0;
            int bendSpeed = 0, bendRemain = 0, volBendSpeed = 0, volBendRemain = 0, periodAdd = 0, porta = 0;
        } fc;
    };

    void setSong (const Song* s);
    void reset (int order = 0, int row = 0);
    void setLoopPattern (bool b) { loopPattern = b; }
    void prepare (double sr) { sampleRate = sr; }

    // mixes n frames (adds to L/R)
    void render (float* L, float* R, int n, juce::uint32 muteMask, float gain);
    // one tracker tick without audio (MIDI export); advances sample positions by one tick
    void analysisTick();

    int order = 0, row = 0, tick = 0, speed = 6, tempo = 125;
    int songLoops = 0;                // incremented when the song wraps
    bool stopped = false;             // F00 or empty song
    double tickSeconds() const
    {
        if (song != nullptr && song->tempoMode == 2)   // OpenMPT modern tempo: tempo = BPM, independent of the speed
            return 60.0 / juce::jmax (1.0, (tempo + (tempo == song->initialTempo ? song->tempoFraction : 0.0)) * song->rowsPerBeat * juce::jmax (1, speed));
        return 2.5 / juce::jmax (1, tempo);
    }
    std::array<Channel, kMaxChannels> ch;
    std::array<Channel, kMaxChannels> bg;   // IT background voices (new note action continue / note off / fade)
    int stereoSeparation = 60;
    bool interpolate = true;
    int globalVol = 64;

    // period helpers (depend on the song's period mode)
    int notePeriod (int note, int sample, int finetune) const;
    double periodRate (int period) const;                 // playback rate in Hz
    double periodSemis (int basePeriod, int period) const; // semitones of `period` above `basePeriod`

private:
    const Song* song = nullptr;
    double sampleRate = 48000.0, tickRemain = 0.0;
    bool loopPattern = false;
    int patternDelay = 0, patternDelayCount = 0;
    int nextOrder = -1, nextRow = -1;
    bool breakFlag = false, jumpFlag = false;
    std::vector<juce::uint8> visited;   // per order*64+row, song end detection

    void processRow();
    void processTick();
    void advanceRow();
    void trigger (Channel& c, int note);
    void fcStart (Channel& c, int instrument, int index);
    void fcTick (Channel& c);
    void fcSetSample (Channel& c, int sample, bool restart);
    void keyOff (Channel& c);
    void postTick();
    void mixChannel (Channel& c);
    void spawnBackground (Channel& c);
    void pastNotes (int owner, int action);
    Cell convertS3m (const Cell& in, Channel& c) const;
    void applyMemory (Cell& cell, Channel& c) const;
    int clampPeriod (int p) const;
    int periodShift (int period, double semis) const;
    int unit() const;
};

// ============================================================================ YM2149 emulation + YM frame player
class YmEngine
{
public:
    void setData (const YmData* d);
    void prepare (double sr);
    void reset (int frame = 0);
    void render (float* L, float* R, int n, juce::uint32 muteMask, float gain);
    int frame = 0, songLoops = 0;
    float level[3] {};
    int stereoSeparation = 60;

private:
    const YmData* ym = nullptr;
    double sampleRate = 48000.0, frameRemain = 0.0;
    double stepsPerSample = 5.0, stepAcc = 0.0;
    juce::uint8 reg[16] {};
    int toneCnt[3] {}, toneOut[3] {};
    int noiseCnt = 0, noisePre = 0;
    juce::uint32 lfsr = 1;
    int noiseOut = 0;
    int envCnt = 0, envPos = 0, envShape = 0;
    float vol32[32] {};
    juce::uint8 envTable[16][96] {};
    // special effects
    struct Fx { int type = -1, voice = 0; double phase = 0.0, freq = 0.0; };
    Fx slot[2];
    struct Drum { bool on = false; double pos = 0.0, step = 0.0; int index = 0; };
    Drum drum[3];
    double dcL = 0.0, dcR = 0.0;
    void writeFrame (int f);
    void startEffects (int f);
};

// ============================================================================ MIDI sampler (Sampler-Modus + preview)
class Sampler
{
public:
    void prepare (double sr) { sampleRate = sr; }
    void setSong (const Song* s) { song = s; allOff(); }
    void handle (const juce::MidiMessage& m);
    void preview (int instr, int note, bool on);   // instr 1-based, uses channel 16 slot
    void render (float* L, float* R, int n, float gain);
    void allOff();
    int program[16] {};   // instrument index (0-based) per MIDI channel

    Sampler() { for (int i = 0; i < 16; ++i) program[i] = i; }

private:
    struct Voice { bool on = false, release = false; int ch = 0, note = 0, sample = 0; double pos = 0.0; float gain = 0.0f, env = 1.0f; };
    std::array<Voice, 48> voices;
    float cc7[16], cc11[16], bend[16];
    int bendRange[16], rpnMsb[16], rpnLsb[16];
    int offsetLo[16], offsetHi[16];
    float pan[16];                    // CC10, -1..1   // CC110/CC111: sample start offset in units of 256 frames (tracker 9xx)
    const Song* song = nullptr;
    double sampleRate = 48000.0;
    bool init = false;
    juce::uint32 noiseState = 12345;
    void ensureInit();
    void noteOn (int ch, int note, int vel, int sample);
    void noteOff (int ch, int note);
};
} // namespace sw
