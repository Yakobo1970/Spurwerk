// Spurwerk – format independent song model (patterns, orders, samples) plus YM register data.
#pragma once
#include <juce_core/juce_core.h>
#include <vector>
#include <array>

namespace sw
{
// UTF-8 literal -> juce::String (umlauts)
inline juce::String U (const char* s) { return juce::String::fromUTF8 (s); }

// Note numbers: 0 = empty, 1 = C-0 ... 120 = B-9 (OpenMPT naming). C-5 (61) = Amiga period 428 = MIDI 60.
constexpr int kNoteNone = 0;
constexpr int kNoteOff = 254;   // key off "===" (XM, YM)
constexpr int kNoteCut = 253;   // note cut "^^^" (S3M, IT)
constexpr int kNoteFade = 252;  // note fade "~~~" (IT)
constexpr int kVolNone = 255;
constexpr int kMaxChannels = 32;

// Volume column commands (XM style); vcmd 0 = plain volume in `vol` (or kVolNone)
enum VolCmd : juce::uint8 { vcNone = 0, vcSlideDown, vcSlideUp, vcFineDown, vcFineUp, vcVibSpeed, vcVibDepth, vcPan, vcPanLeft, vcPanRight, vcPorta,
                         vcPitchDown, vcPitchUp, vcPanFine,   // IT: pitch slide e/f, panning 0..64
                         vcOffsetHigh,
                         vcFcIndex };   // Future Composer: raw note index (0..127) in `vol`, interpreted by the FC macro player                       // MDL: sample offset high byte (x 65536 frames)

struct Cell
{
    juce::uint8 note = 0, instr = 0, vol = kVolNone, fx = 0, param = 0, vcmd = 0;
    bool isEmpty() const noexcept { return note == 0 && instr == 0 && vol == kVolNone && fx == 0 && param == 0 && vcmd == 0; }
    bool operator== (const Cell& o) const noexcept { return note == o.note && instr == o.instr && vol == o.vol && fx == o.fx && param == o.param && vcmd == o.vcmd; }
};

// Effect numbers: MOD/XM/OKT cells use XM numbering (0-9, A=10 ... Z=35) plus these internal codes;
// S3M cells keep their native letters (1 = A ... 26 = Z) and are translated during playback.
enum : juce::uint8 { fxXmG = 16, fxXmH = 17, fxXmK = 20, fxXmL = 21, fxXmP = 25, fxXmR = 27, fxXmT = 29, fxXmX = 33,
                     fxSpeed = 36, fxTempo = 37, fxFineVib = 38,
                     fxOktSlideUp = 40, fxOktSlideDown = 41, fxOktUpOnce = 42, fxOktDownOnce = 43,
                     fxOktArp1 = 44, fxOktArp2 = 45, fxOktArp3 = 46,
                     fxChanVol = 47, fxChanVolSlide = 48, fxItPanSlide = 49, fxOffsetHigh = 50, fxPastNote = 51, fxTempoSlide = 52,
                     fxMedTempo = 53, fxFcPorta = 54 };   // OctaMED tempo (raw MED value, converted by Song::medTempo)

struct Pattern
{
    int rows = 64, channels = 4;
    std::vector<Cell> cells;
    void resize (int r, int c) { rows = r; channels = c; cells.assign ((size_t) (r * c), {}); }
    Cell& at (int row, int ch) { return cells[(size_t) (row * channels + ch)]; }
    const Cell& at (int row, int ch) const { return cells[(size_t) (row * channels + ch)]; }
};

struct Sample
{
    juce::String name;
    std::vector<float> pcm;       // mono, -1..1
    int loopStart = 0, loopLength = 0;   // in frames; loopLength > 2 => looping
    int volume = 64;              // 0..64
    int finetune = 0;             // -8..7 (ProTracker)
    double c5Speed = 0.0;         // Hz at C-5 (S3M/XM); 0 = derive from ProTracker finetune
    int pan = -1;                 // 0..255 default panning (XM), -1 = keep channel panning
    float globalGain = 1.0f;      // IT sample global volume
    bool hasLoop() const noexcept { return loopLength > 2 && loopStart + loopLength <= (int) pcm.size(); }
    // playback rate at C-5 (period 428, finetune applied)
    double c5Rate() const noexcept { return c5Speed > 0.0 ? c5Speed : 7093789.2 / 856.0 * std::pow (2.0, finetune / 96.0); }
};

// XM instrument (keymap + envelopes); MOD/S3M/OKT have none: instrument number = sample number
struct Envelope
{
    std::vector<std::pair<int, int>> points;   // tick, value 0..64
    bool on = false, sustain = false, loop = false;
    int sus = 0, susEnd = 0, loopStart = 0, loopEnd = 0;   // susEnd > sus: IT sustain loop
};

struct Instrument
{
    juce::String name;
    std::array<short, 120> map {};    // note-1 -> sample index (0-based), -1 = none
    std::array<juce::uint8, 120> noteMap {};   // note-1 -> played note (IT keyboard transpose), 1-based
    Envelope vol, pan, pitch;         // pitch: IT only, value 32 = no change, 1 unit = 1/2 semitone
    int fadeout = 0;                  // per tick, of 65536
    int panDefault = -1;              // 0..255 (IT), -1 = none
    int nna = 0;                      // IT new note action: 0 cut, 1 continue, 2 note off, 3 fade
    float globalGain = 1.0f;          // IT instrument global volume
    int vibType = 0, vibSweep = 0, vibDepth = 0, vibRate = 0;
    Instrument() { map.fill (-1); for (int i = 0; i < 120; ++i) noteMap[(size_t) i] = (juce::uint8) (i + 1); }
};

enum class Format { MOD, YM, XM, S3M, OKT, IT, MTM, F669, FAR, MDL, MED, TCB, FC };

// Future Composer instrument macros: 64-byte volume blocks (speed, freq sequence, vibrato speed/depth/delay, volume bytes)
// and 64-byte frequency sequences (pitch bytes + commands E0..EA); interpreted per tick by the engine
struct FcData
{
    bool fc14 = false;
    int numInstruments = 0;
    std::vector<juce::uint8> freq, vol;
};

// YM2149 register dump (Atari ST); converted into read-only patterns for display/export
struct YmData
{
    juce::String version;            // "YM3!", "YM5!", ...
    int frames = 0, loopFrame = 0;
    int clock = 2000000, rate = 50;
    std::vector<std::array<juce::uint8, 16>> regs;
    std::vector<std::vector<juce::uint8>> drums;  // digidrum samples (unsigned 8 bit)
    bool drums4bit = false;
    juce::String author, comment;
};

struct Song
{
    Format format = Format::MOD;
    juce::String title, formatName, sourcePath;
    juce::String artist, message;    // composer (MDL) and song message/comment text stored in the module
    int channels = 4;
    int initialSpeed = 6, initialTempo = 125;
    int restartPos = 0;
    int stereoSeparation = 60;   // percent, Amiga hard panning = 100
    bool fifteenSamples = false;
    int subSongs = 1, subSong = 0;   // COSO
    int periodMode = 0;              // 0 = ProTracker periods, 1 = Amiga periods x4 (S3M, XM Amiga), 2 = XM linear
    int globalVolume = 64;           // initial (XM/S3M), 0..64
    std::vector<float> channelPanInit;   // -1..1 per channel; empty = Amiga LRRL
    std::vector<int> channelVolInit;     // 0..64 per channel (IT); empty = 64
    int medLinesPerBeat = 0;             // OctaMED BPM mode (0 = classic MED tempo)
    bool med8Ch = false, medMixing = false;   // OctaMED 5-8 channel mode, software mixing (flags2 0x80)
    // OctaMED tempo -> tracker tempo (2.5 / tempo seconds per tick), following OpenMPT's MMDTempoToBPM
    int medTempo (int t) const
    {
        double v;
        if (medLinesPerBeat > 0 && ! med8Ch) v = t < 7 ? 111.5 : t * medLinesPerBeat / 4.0;   // BPM mode (ticks per row still count)
        else if (med8Ch && t > 0)
        {
            static const int tempos[10] = { 179, 164, 152, 141, 131, 123, 116, 110, 104, 99 };   // 8-channel mode: tempo 1..10
            v = tempos[juce::jmin (t, 10) - 1];
        }
        else if (! medMixing && t > 0 && t <= 10) v = 6.0 * 1773447.0 / 14500.0 / t;   // SoundTracker-compatible tempo
        else if (medMixing && t < 8) v = 157.86;
        else v = t / 0.264;                                                        // classic: MED tempo 33 = 125
        return juce::jlimit (1, 2000, juce::roundToInt (v));
    }
    // OpenMPT tempo mode (MPTM): 2 = modern, the tempo is the real BPM and a row lasts 60 / (bpm * rowsPerBeat) s whatever the speed
    int tempoMode = 0, rowsPerBeat = 4;
    double tempoFraction = 0.0;          // fractional part of the initial tempo (MPTM "DTFR")
    std::vector<Instrument> instruments; // XM only
    std::vector<int> orders;
    std::vector<Pattern> patterns;
    std::vector<Sample> samples;  // index 0 = instrument 1
    YmData ym;
    FcData fc;

    bool readOnly() const noexcept { return format != Format::MOD; }   // pattern editing / saving only for MOD
    bool hasSamples() const noexcept { return format != Format::YM; }
    bool hasVolumeColumn() const noexcept { return format == Format::XM || format == Format::S3M || format == Format::IT
                                                       || format == Format::F669 || format == Format::FAR || format == Format::MDL; }
    bool s3mEffects() const noexcept { return format == Format::S3M || format == Format::IT; }   // cells keep letters A..Z
    int numSamples() const noexcept { return (int) samples.size(); }
    int numInstruments() const noexcept { return instruments.empty() ? (int) samples.size() : (int) instruments.size(); }
    juce::String instrumentName (int instr) const   // 1-based
    {
        if (instruments.empty()) return sampleName (instr);
        if (instr < 1 || instr > (int) instruments.size()) return {};
        return instruments[(size_t) instr - 1].name;
    }
    // sample (0-based) an instrument plays for a note, -1 = none
    int sampleFor (int instr, int note) const
    {
        if (instruments.empty()) return (instr >= 1 && instr <= (int) samples.size()) ? instr - 1 : -1;
        if (instr < 1 || instr > (int) instruments.size() || note < 1 || note > 120) return -1;
        const int s = instruments[(size_t) instr - 1].map[(size_t) note - 1];
        return s < (int) samples.size() ? s : -1;
    }
    // note an instrument actually plays (IT keyboard table may transpose)
    int noteFor (int instr, int note) const
    {
        if (instruments.empty() || instr < 1 || instr > (int) instruments.size() || note < 1 || note > 120) return note;
        return instruments[(size_t) instr - 1].noteMap[(size_t) note - 1];
    }
    // Future Composer note index (0..127) -> note (the original period table wraps)
    static int fcNoteForIndex (int idx) noexcept
    {
        idx &= 0x7F;
        if (idx < 48) return idx + 37;
        if (idx < 60 || idx >= 120) return 84;
        return idx - 35;
    }
    int initialChannelVolume (int ch) const noexcept { return ch < (int) channelVolInit.size() ? channelVolInit[(size_t) ch] : 64; }
    float initialPan (int ch) const noexcept { return ch < (int) channelPanInit.size() ? channelPanInit[(size_t) ch] : (float) channelPan (ch); }
    juce::String sampleName (int instr) const   // instr 1-based
    {
        if (instr < 1 || instr > (int) samples.size()) return {};
        return samples[(size_t) instr - 1].name;
    }
    static int channelPan (int ch) noexcept { return ((ch & 3) == 0 || (ch & 3) == 3) ? -1 : 1; }  // Amiga LRRL
};

juce::String noteName (int note);   // "C-5", "===" ...
juce::String effectText (const Song& s, const Cell& c);   // "A0F" etc. in the native notation of the format
juce::String volumeText (const Cell& c);                  // "v40", "d05", "..."
int periodForNote (int note, int finetune);   // Amiga period (extended range C-3..B-7)
int noteForPeriod (int period);               // nearest note, finetune 0

std::unique_ptr<Song> newEmptySong (int channels = 4);
} // namespace sw
