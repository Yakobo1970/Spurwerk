#include "Engine.h"

namespace sw
{
namespace
{
const int kSine[32] = { 0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
                        255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24 };
constexpr double kPaulaClock = 7093789.2;

int waveValue (int wave, int pos)   // -255..255
{
    pos &= 63;
    switch (wave & 3)
    {
        case 1: { const int v = (pos & 31) * 8; return pos < 32 ? 255 - v : -v; }   // ramp down
        case 2: return pos < 32 ? 255 : -255;                                         // square
        default: { const int v = kSine[pos & 31]; return pos < 32 ? v : -v; }
    }
}

inline float samplePoint (const Sample& s, double pos, bool interp)
{
    const int n = (int) s.pcm.size();
    const int i = (int) pos;
    if (i >= n || i < 0) return 0.0f;
    const float a = s.pcm[(size_t) i];
    if (! interp) return a;
    int j = i + 1;
    if (j >= n) j = s.hasLoop() ? s.loopStart : i;
    else if (s.hasLoop() && j >= s.loopStart + s.loopLength) j = s.loopStart;
    const float frac = (float) (pos - i);
    return a + (s.pcm[(size_t) j] - a) * frac;
}

// advances a sample position, handles loops; returns false when the sample ended
inline bool advancePos (const Sample& s, double& pos, double inc)
{
    pos += inc;
    if (s.hasLoop())
    {
        const double end = s.loopStart + s.loopLength;
        if (pos >= end) pos = s.loopStart + std::fmod (pos - s.loopStart, (double) s.loopLength);
        return true;
    }
    return pos < (double) s.pcm.size();
}
} // namespace

// ============================================================================================ ModEngine
void ModEngine::setSong (const Song* s)
{
    song = s;
    reset (0, 0);
}

void ModEngine::reset (int o, int r)
{
    for (auto& c : ch) c = Channel();
    for (auto& c : bg) c = Channel();
    if (song != nullptr)
        for (int i = 0; i < song->channels && i < kMaxChannels; ++i)
        {
            ch[(size_t) i].pan = song->initialPan (i);
            ch[(size_t) i].chanVol = song->initialChannelVolume (i);
        }
    order = o; row = r; tick = 0;
    speed = song ? song->initialSpeed : 6;
    tempo = song ? song->initialTempo : 125;
    stereoSeparation = song ? song->stereoSeparation : 60;
    globalVol = song ? song->globalVolume : 64;
    tickRemain = 0.0;
    patternDelay = patternDelayCount = 0;
    nextOrder = nextRow = -1;
    breakFlag = jumpFlag = false;
    songLoops = 0;
    stopped = song == nullptr || song->orders.empty();
    visited.assign (song ? song->orders.size() * 256 : 0, 0);
    if (! stopped)
    {
        order = juce::jlimit (0, (int) song->orders.size() - 1, order);
        processRow();
        postTick();
    }
}

int ModEngine::unit() const { return song != nullptr && song->periodMode != 0 ? 4 : 1; }

int ModEngine::clampPeriod (int p) const
{
    if (song == nullptr) return p;
    if (song->periodMode == 1) return juce::jlimit (8, 200000, p);
    if (song->periodMode == 2) return juce::jlimit (1, 11520, p);
    const bool pt = song->format == Format::MOD && song->channels == 4;
    return pt ? juce::jlimit (113, 856, p) : juce::jlimit (28, 6848, p);
}

int ModEngine::notePeriod (int note, int sample, int finetune) const
{
    if (song == nullptr || song->periodMode == 0) return periodForNote (note, finetune);
    const double c5 = (sample >= 0 && sample < song->numSamples()) ? song->samples[(size_t) sample].c5Rate() : 8363.0;
    if (song->periodMode == 1) return juce::roundToInt (1712.0 * 8363.0 / juce::jmax (1.0, c5) * std::pow (2.0, -(note - 61) / 12.0));
    return juce::roundToInt (4608.0 - (note - 61) * 64.0 - 768.0 * std::log2 (juce::jmax (1.0, c5) / 8363.0));
}

double ModEngine::periodRate (int p) const
{
    if (p <= 0) return 0.0;
    if (song == nullptr || song->periodMode == 0) return kPaulaClock / (2.0 * p);
    if (song->periodMode == 1) return 8363.0 * 1712.0 / p;
    return 8363.0 * std::pow (2.0, (4608.0 - p) / 768.0);
}

double ModEngine::periodSemis (int base, int p) const
{
    if (base <= 0 || p <= 0) return 0.0;
    if (song != nullptr && song->periodMode == 2) return (base - p) / 64.0;
    return 12.0 * std::log2 ((double) base / (double) p);
}

int ModEngine::periodShift (int p, double semis) const
{
    if (song != nullptr && song->periodMode == 2) return juce::roundToInt (p - semis * 64.0);
    return juce::roundToInt (p * std::pow (2.0, -semis / 12.0));
}

void ModEngine::trigger (Channel& c, int note)
{
    if (song == nullptr) return;
    if (c.active && c.nna != 0 && c.owner < 0) spawnBackground (c);
    c.nna = 0;
    c.fading = false;
    c.envPitchTick = 0;
    if (! song->instruments.empty())
    {
        const int sn = song->sampleFor (c.instr, note);
        if (sn < 0) { c.active = false; return; }
        c.sample = sn;
        note = song->noteFor (c.instr, note);
        if (song->format == Format::IT) c.nna = song->instruments[(size_t) c.instr - 1].nna;
    }
    if (c.sample < 0 || c.sample >= song->numSamples()) return;
    c.period = notePeriod (note, c.sample, c.finetune);
    c.outPeriod = c.period;
    c.pos = 0.0;
    c.active = ! song->samples[(size_t) c.sample].pcm.empty();
    if (c.vibWave < 4) c.vibPos = 0;
    if (c.tremWave < 4) c.tremPos = 0;
    c.keyOn = true;
    c.envVolTick = c.envPanTick = 0;
    c.fadeVol = 65536;
    c.avPos = c.avSweep = 0;
    c.retrigCount = c.tremorCount = 0;
    c.lastNote = note;
    c.triggered = true;
    c.trigNote = note;
    c.trigInstr = song->instruments.empty() ? c.sample + 1 : c.instr;
}

void ModEngine::keyOff (Channel& c)
{
    c.keyOn = false;
    const bool hasIns = ! song->instruments.empty() && c.instr >= 1 && c.instr <= (int) song->instruments.size();
    const bool env = hasIns && song->instruments[(size_t) c.instr - 1].vol.on;
    if (song->format == Format::IT && hasIns)
    {
        // IT: note off releases the sustain; without envelope (or with a looping one) the fadeout starts
        const auto& ins = song->instruments[(size_t) c.instr - 1];
        if (! env || ins.vol.loop) c.fading = true;
        if (! env && ins.fadeout == 0) { c.volume = 0; c.cutFlag = true; if (c.owner >= 0) c.active = false; }
        return;
    }
    if (! env) { c.volume = 0; c.cutFlag = true; if (c.owner >= 0) c.active = false; }
}

// IT new note action: the old note keeps sounding on a background voice
void ModEngine::spawnBackground (Channel& c)
{
    int slot = -1;
    float quietest = 1.0e9f;
    for (int k = 0; k < kMaxChannels; ++k)
    {
        if (! bg[(size_t) k].active) { slot = k; break; }
        if (bg[(size_t) k].mixVol < quietest) { quietest = bg[(size_t) k].mixVol; slot = k; }
    }
    if (slot < 0) return;
    auto& b = bg[(size_t) slot];
    b = c;
    b.owner = (int) (&c - ch.data());
    b.period = b.outPeriod;
    b.cell = Cell();
    b.triggered = b.cutFlag = false;
    b.delayedNote = 0;
    if (c.nna == 2) keyOff (b);
    else if (c.nna == 3) b.fading = true;
}

// IT S70..S72: cut / note off / fade the background voices of a channel
void ModEngine::pastNotes (int owner, int action)
{
    for (auto& b : bg)
    {
        if (! b.active || b.owner != owner) continue;
        if (action == 0) b.active = false;
        else if (action == 1) keyOff (b);
        else b.fading = true;
    }
}

// Scream Tracker 3 effects -> XM numbering (with ST3's shared effect memory)
Cell ModEngine::convertS3m (const Cell& in, Channel& c) const
{
    Cell out = in;
    out.fx = 0; out.param = 0;
    const int cmd = in.fx;
    int p = in.param;
    const bool it = song->format == Format::IT;
    if (it)
    {
        // IT: own memory per effect; D/K/L share one, E/F share one
        int m = -1;
        switch (cmd)
        {
            case 4: case 11: case 12: m = 4; break;
            case 5: case 6: m = 5; break;
            case 9: case 10: case 14: case 16: case 17: case 18: case 19: case 20: case 23: m = cmd; break;
            default: break;
        }
        if (m >= 0) { if (p == 0) p = c.itMem[(size_t) m]; else c.itMem[(size_t) m] = p; }
    }
    else
        switch (cmd)
        {
            case 4: case 5: case 6: case 9: case 10: case 11: case 12: case 17: case 18: case 19:
                if (p == 0) p = c.s3mMem; else c.s3mMem = p;
                break;
            default: break;
        }
    const int hi = p >> 4, lo = p & 15;
    auto set = [&] (int f, int v) { out.fx = (juce::uint8) f; out.param = (juce::uint8) v; };
    auto volSlide = [&] (int f) {
        if (lo == 0xF && hi) set (0xE, 0xA0 | hi);
        else if (hi == 0xF && lo) set (0xE, 0xB0 | lo);
        else if (lo == 0) set (f, hi << 4);
        else set (f, lo);
    };
    switch (cmd)
    {
        case 1: if (p) set (fxSpeed, p); break;
        case 2: set (0xB, p); break;
        case 3: set (0xD, it ? (p < 160 ? ((p / 10) << 4) | (p % 10) : 0) : p); break;   // IT: row in hex
        case 4: volSlide (0xA); break;
        case 5: if (p >= 0xF0) set (0xE, 0x20 | lo); else if (p >= 0xE0) set (fxXmX, 0x20 | lo); else set (0x2, p); break;
        case 6: if (p >= 0xF0) set (0xE, 0x10 | lo); else if (p >= 0xE0) set (fxXmX, 0x10 | lo); else set (0x1, p); break;
        case 7: set (0x3, p); break;
        case 8: set (0x4, p); break;
        case 9: set (fxXmT, p); break;
        case 10: set (0x0, p); break;
        case 11: if (hi == 0xF || lo == 0xF) set (0x4, 0); else volSlide (0x6); break;
        case 12: if (hi == 0xF || lo == 0xF) set (0x3, 0); else volSlide (0x5); break;
        case 13: if (it) set (fxChanVol, p); break;
        case 14: if (it) set (fxChanVolSlide, p); break;
        case 15: set (0x9, p); break;
        case 16: if (it) set (fxItPanSlide, p); break;
        case 17: set (fxXmR, p); break;
        case 18: set (0x7, p); break;
        case 19:
            switch (hi)
            {
                case 0x1: set (0xE, 0x30 | lo); break;
                case 0x3: set (0xE, 0x40 | lo); break;
                case 0x4: set (0xE, 0x70 | lo); break;
                case 0x7: if (it && lo <= 2) set (fxPastNote, lo); break;
                case 0x8: set (0xE, 0x80 | lo); break;
                case 0xA: if (it) set (fxOffsetHigh, lo); break;
                case 0xB: set (0xE, 0x60 | lo); break;
                case 0xC: set (0xE, 0xC0 | lo); break;
                case 0xD: set (0xE, 0xD0 | lo); break;
                case 0xE: set (0xE, 0xE0 | lo); break;
                default: break;
            }
            break;
        case 20: if (p >= 0x20) set (fxTempo, p); else if (it && hi <= 1) set (fxTempoSlide, p); break;
        case 21: set (fxFineVib, p); break;
        case 22: set (fxXmG, it ? juce::jmin (64, (p + 1) / 2) : juce::jmin (64, p)); break;
        case 23: set (fxXmH, p); break;
        case 24: if (it) set (0x8, p); else if (p <= 0x80) set (0x8, juce::jmin (255, p * 2)); else if (p == 0xA4) set (0x8, 128); break;
        default: break;
    }
    return out;
}

// FastTracker 2 effect memory (parameter 00 = last value)
void ModEngine::applyMemory (Cell& cell, Channel& c) const
{
    auto mem = [] (juce::uint8& p, int& m) { if (p == 0) p = (juce::uint8) m; else m = p; };
    auto memLo = [] (juce::uint8& p, int& m) { if ((p & 15) == 0) p = (juce::uint8) ((p & 0xF0) | m); else m = p & 15; };
    switch (cell.fx)
    {
        case 0x1: mem (cell.param, c.memPortaUp); break;
        case 0x2: mem (cell.param, c.memPortaDown); break;
        case 0x5: case 0x6: case 0xA: mem (cell.param, c.memVol); break;
        case 0xE:
            switch (cell.param >> 4)
            {
                case 0x1: memLo (cell.param, c.memFineUp); break;
                case 0x2: memLo (cell.param, c.memFineDown); break;
                case 0xA: memLo (cell.param, c.memFineVolUp); break;
                case 0xB: memLo (cell.param, c.memFineVolDown); break;
                default: break;
            }
            break;
        case fxXmH: mem (cell.param, c.memGlobal); break;
        case fxXmP: mem (cell.param, c.memPan); break;
        case fxXmR: mem (cell.param, c.memRetrig); break;
        case fxXmT: mem (cell.param, c.memTremor); break;
        case fxXmX:
            if ((cell.param >> 4) == 1) memLo (cell.param, c.memXUp);
            else if ((cell.param >> 4) == 2) memLo (cell.param, c.memXDown);
            break;
        default: break;
    }
}

void ModEngine::processRow()
{
    if (song == nullptr || order >= (int) song->orders.size()) return;
    const int pi = song->orders[(size_t) order];
    if (pi < 0 || pi >= (int) song->patterns.size()) return;
    const auto& pat = song->patterns[(size_t) pi];
    if (row >= pat.rows) row = 0;

    const size_t vi = (size_t) order * 256 + (size_t) juce::jmin (row, 255);
    if (vi < visited.size())
    {
        bool inLoop = false;
        for (int i = 0; i < song->channels; ++i) inLoop |= ch[(size_t) i].loopCount > 0;
        if (visited[vi] && ! inLoop && ! loopPattern) { ++songLoops; std::fill (visited.begin(), visited.end(), (juce::uint8) 0); }
        visited[vi] = 1;
    }

    const bool s3m = song->s3mEffects();
    const bool xm = song->format == Format::XM || song->format == Format::MDL || song->format == Format::FAR || song->format == Format::F669;
    const bool itMode = song->format == Format::IT;
    const bool useInstruments = ! song->instruments.empty();
    const int u = unit();
    const int numCh = juce::jmin (song->channels, pat.channels);
    for (int i = 0; i < numCh; ++i)
    {
        auto& c = ch[(size_t) i];
        Cell cell = s3m ? convertS3m (pat.at (row, i), c) : pat.at (row, i);
        if (song->format == Format::FC)   // notes start the instrument macros; the generic part only sees speed / break
        {
            if (cell.vcmd == vcFcIndex && cell.instr > 0) { fcStart (c, cell.instr - 1, cell.vol); c.fc.porta = 0; }
            if (cell.fx == fxFcPorta) c.fc.porta = (juce::int8) cell.param;
            cell.note = 0; cell.instr = 0; cell.vcmd = vcNone; cell.vol = kVolNone;
            if (cell.fx == fxFcPorta) { cell.fx = 0; cell.param = 0; }
        }
        if (xm || s3m) applyMemory (cell, c);
        c.cell = cell;
        const int fx = cell.fx, p = cell.param, hi = p >> 4, lo = p & 15;
        const bool hasNote = cell.note > 0 && cell.note <= 120;
        const bool porta = fx == 0x3 || fx == 0x5 || cell.vcmd == vcPorta;
        const bool delay = fx == 0xE && hi == 0xD && lo > 0;

        if (cell.instr > 0 && ! (delay && useInstruments))
        {
            if (! useInstruments)
            {
                if (cell.instr <= song->numSamples())
                {
                    c.sample = cell.instr - 1;
                    const auto& s = song->samples[(size_t) c.sample];
                    c.volume = s.volume;
                    c.finetune = s.finetune;
                    if (s.pan >= 0) c.pan = (float) s.pan / 127.5f - 1.0f;
                }
            }
            else if (cell.instr <= (int) song->instruments.size())
            {
                c.instr = cell.instr;
                const int sn = song->sampleFor (c.instr, hasNote ? cell.note : c.lastNote);
                int ref = c.sample;
                if (sn >= 0 && hasNote && ! porta) ref = sn;
                const auto& insDef = song->instruments[(size_t) c.instr - 1];
                if (insDef.panDefault >= 0) c.pan = (float) insDef.panDefault / 127.5f - 1.0f;
                if (ref >= 0 && ref < song->numSamples())
                {
                    const auto& s = song->samples[(size_t) ref];
                    c.volume = s.volume;
                    if (s.pan >= 0) c.pan = (float) s.pan / 127.5f - 1.0f;
                }
                c.keyOn = true;
                c.fading = false;
                c.envVolTick = c.envPanTick = 0;
                c.fadeVol = 65536;
            }
        }
        if (hasNote)
        {
            if (porta)
            {
                int smp = c.sample;
                if (useInstruments) { const int sn = song->sampleFor (c.instr, cell.note); if (sn >= 0 && ! c.active) smp = sn; }
                c.portaTarget = notePeriod (song->noteFor (c.instr, cell.note), smp, c.finetune);
                if (! c.active && smp >= 0) { c.sample = smp; trigger (c, cell.note); }   // nothing playing: start it
            }
            else if (delay) { c.delayedNote = cell.note; c.delayedInstr = cell.instr; }
            else
            {
                if (fx == 0xE && hi == 0x5) c.finetune = lo >= 8 ? lo - 16 : lo;
                trigger (c, cell.note);
                if (fx == 0x9)
                {
                    if (song->format == Format::MDL) c.offsetHigh = cell.vcmd == vcOffsetHigh ? cell.vol : 0;
                    if (p) c.offsetMem = p;
                    c.pos = c.offsetMem * 256.0 + c.offsetHigh * 65536.0;
                    if (c.sample >= 0)
                    {
                        const auto& s = song->samples[(size_t) c.sample];
                        if (c.pos >= (double) s.pcm.size()) { if (s.hasLoop()) c.pos = s.loopStart; else c.active = false; }
                    }
                }
            }
        }
        else if (cell.note == kNoteCut) { c.active = false; c.cutFlag = true; }
        else if (cell.note == kNoteOff)
        {
            if (xm || (itMode && useInstruments)) keyOff (c);
            else { c.active = false; c.cutFlag = true; }
        }
        else if (cell.note == kNoteFade)
        {
            if (itMode && useInstruments) c.fading = true;
            else { c.active = false; c.cutFlag = true; }
        }

        // volume column (tick 0 part)
        if (! delay)
        {
            switch (cell.vcmd)
            {
                case vcNone: if (cell.vol <= 64) c.volume = cell.vol; break;
                case vcFineDown: c.volume = juce::jmax (0, c.volume - cell.vol); break;
                case vcFineUp: c.volume = juce::jmin (64, c.volume + cell.vol); break;
                case vcVibSpeed: if (cell.vol) c.vibSpeed = cell.vol; break;
                case vcVibDepth: if (cell.vol) c.vibDepth = cell.vol; break;
                case vcPan: c.pan = (float) (cell.vol * 17) / 127.5f - 1.0f; break;
                case vcPorta:
                {
                    static const int itPorta[10] = { 0, 1, 4, 8, 16, 32, 64, 96, 128, 255 };
                    if (cell.vol) c.portaSpeed = itMode ? itPorta[juce::jmin (9, (int) cell.vol)] : cell.vol * 16;
                    break;
                }
                case vcPanFine: c.pan = juce::jlimit (-1.0f, 1.0f, (float) cell.vol / 32.0f - 1.0f); break;
                default: break;
            }
        }

        c.outPeriod = c.period;
        switch (fx)
        {
            case 0x3: if (p) c.portaSpeed = p; break;
            case 0x4: if (hi) c.vibSpeed = hi; if (lo) c.vibDepth = lo; break;
            case fxFineVib: if (hi) c.vibSpeed = hi; if (lo) c.vibDepth = lo; break;
            case 0x7: if (hi) c.tremSpeed = hi; if (lo) c.tremDepth = lo; break;
            case 0x8: c.pan = (float) p / 127.5f - 1.0f; break;
            case 0x9: if (p) c.offsetMem = p; break;
            case 0xB: if (! loopPattern) { nextOrder = p; jumpFlag = true; if (! breakFlag) nextRow = 0; } break;
            case 0xC: c.volume = juce::jmin (64, p); break;
            case 0xD:
                if (! loopPattern)
                {
                    breakFlag = true;
                    nextRow = hi * 10 + lo;
                    if (song->format == Format::MOD) nextRow = juce::jmin (63, nextRow);
                    if (! jumpFlag) nextOrder = order + 1;
                }
                break;
            case 0xE:
                switch (hi)
                {
                    case 0x1: c.period = clampPeriod (c.period - lo * u); break;
                    case 0x2: c.period = clampPeriod (c.period + lo * u); break;
                    case 0x3: c.glissando = lo != 0; break;
                    case 0x4: c.vibWave = lo; break;
                    case 0x6:
                        if (lo == 0) c.loopRow = row;
                        else if (c.loopCount == 0) { c.loopCount = lo; nextRow = c.loopRow; nextOrder = order; breakFlag = true; jumpFlag = false; }
                        else if (--c.loopCount > 0) { nextRow = c.loopRow; nextOrder = order; breakFlag = true; jumpFlag = false; }
                        break;
                    case 0x7: c.tremWave = lo; break;
                    case 0x8: c.pan = (float) lo / 7.5f - 1.0f; break;
                    case 0xA: c.volume = juce::jmin (64, c.volume + lo); break;
                    case 0xB: c.volume = juce::jmax (0, c.volume - lo); break;
                    case 0xC: if (lo == 0) { c.volume = 0; c.cutFlag = true; } break;
                    case 0xE: if (patternDelayCount == 0) patternDelay = lo; break;
                    default: break;
                }
                break;
            case 0xF:
                if (p == 0) { /* F00: PT/FT2 stop – ignored */ }
                else if (p < 32) speed = p;
                else tempo = p;
                break;
            case fxSpeed: if (p) speed = p; break;
            case fxTempo: if (p >= 32) tempo = p; break;
            case fxMedTempo: if (p > 0) tempo = song->medTempo (p); break;
            case fxXmG: globalVol = juce::jmin (64, p); break;
            case fxXmK: if (p == 0) keyOff (c); break;
            case fxXmL: c.envVolTick = c.envPanTick = p; break;
            case fxXmX:
                if (hi == 1) c.period = clampPeriod (c.period - lo);
                else if (hi == 2) c.period = clampPeriod (c.period + lo);
                break;
            case fxChanVol: c.chanVol = juce::jmin (64, p); break;
            case fxChanVolSlide:
                if (lo == 0xF && hi) c.chanVol = juce::jmin (64, c.chanVol + hi);
                else if (hi == 0xF && lo) c.chanVol = juce::jmax (0, c.chanVol - lo);
                break;
            case fxItPanSlide:
                if (lo == 0xF && hi) c.pan = juce::jmax (-1.0f, c.pan - hi * 4 / 127.5f);
                else if (hi == 0xF && lo) c.pan = juce::jmin (1.0f, c.pan + lo * 4 / 127.5f);
                break;
            case fxOffsetHigh: c.offsetHigh = lo; break;
            case fxPastNote: pastNotes (i, lo); break;
            case fxOktUpOnce: c.period = clampPeriod (periodShift (c.period, p)); break;
            case fxOktDownOnce: c.period = clampPeriod (periodShift (c.period, -p)); break;
            default: break;
        }
        c.outPeriod = c.period;
        c.outVolume = c.volume;
        if (song->format == Format::FC) fcTick (c);
    }
}

// ============================================================================================ Future Composer macros
void ModEngine::fcSetSample (Channel& c, int s, bool restart)
{
    if (s < 0 || s >= song->numSamples()) return;
    const auto& smp = song->samples[(size_t) s];
    c.sample = s;
    if (restart || c.pos >= (double) smp.pcm.size()) c.pos = smp.hasLoop() && ! restart ? (double) smp.loopStart : 0.0;
    c.active = ! smp.pcm.empty();
}

void ModEngine::fcStart (Channel& c, int instrument, int index)
{
    const auto& fd = song->fc;
    auto& f = c.fc;
    const int ins = instrument >= 0 && instrument <= fd.numInstruments ? instrument : fd.numInstruments;
    f = {};
    f.on = true;
    f.block = ins * 64;
    f.index = index & 0x7F;
    auto vb = [&fd] (int o) { return o >= 0 && o < (int) fd.vol.size() ? (int) fd.vol[(size_t) o] : 0; };
    f.volSpeed = vb (f.block) > 0 ? vb (f.block) : 255;
    f.volCount = 1;
    f.volPos = f.block + 5;
    f.freqPos = vb (f.block + 1) * 64;
    f.vibSpeed = vb (f.block + 2);
    f.vibDepth = vb (f.block + 3);
    f.vibDelay = vb (f.block + 4);
    f.vibValue = f.vibDepth;
    if (f.freqPos + 1 < (int) fd.freq.size()) fcSetSample (c, fd.freq[(size_t) f.freqPos + 1], true);   // usually "E2 xx"
    c.instr = ins + 1;
    c.triggered = true;
    c.trigNote = Song::fcNoteForIndex (f.index);
    c.trigInstr = c.sample + 1;
    c.lastNote = c.trigNote;
    c.keyOn = true;
}

void ModEngine::fcTick (Channel& c)
{
    auto& f = c.fc;
    if (! f.on) return;
    const auto& fd = song->fc;
    auto fb = [&fd] (int o) { return o >= 0 && o < (int) fd.freq.size() ? (int) fd.freq[(size_t) o] : 0xE1; };
    auto vb = [&fd] (int o) { return o >= 0 && o < (int) fd.vol.size() ? (int) fd.vol[(size_t) o] : 0xE1; };
    auto restartVolume = [&f] { f.volPos = f.block + 5; f.volStop = false; f.volWait = 0; f.volCount = 1; };

    // frequency sequence: one pitch step per tick, commands in between
    if (! f.freqStop)
    {
        if (f.freqWait > 0) --f.freqWait;
        else
            for (int guard = 0; guard < 64; ++guard)
            {
                const int b = fb (f.freqPos);
                const int seqBase = (f.freqPos / 64) * 64;
                if (b == 0xE0)
                {
                    f.freqPos = seqBase + (fb (f.freqPos + 1) & 0x3F);
                    const int t = fb (f.freqPos);
                    if (t == 0xE0 || t == 0xE1) { f.pitch = (juce::int8) t; ++f.freqPos; break; }   // never a command right after a jump
                    continue;
                }
                if (b == 0xE1) { f.freqStop = true; break; }
                if (b == 0xE2) { fcSetSample (c, fb (f.freqPos + 1), true); restartVolume(); f.pitch = (juce::int8) fb (f.freqPos + 2); f.freqPos += 3; break; }
                if (b == 0xE4) { fcSetSample (c, fb (f.freqPos + 1), false); f.pitch = (juce::int8) fb (f.freqPos + 2); f.freqPos += 3; break; }
                if (b == 0xE3) { f.vibSpeed = fb (f.freqPos + 1); f.vibDepth = fb (f.freqPos + 2); f.pitch = (juce::int8) fb (f.freqPos + 3); f.freqPos += 4; break; }
                if (b == 0xE7) { f.freqPos = fb (f.freqPos + 1) * 64; continue; }
                if (b == 0xE8) { f.freqWait = juce::jmax (0, fb (f.freqPos + 1) - 1); f.freqPos += 2; break; }
                if (b == 0xE9 && fd.fc14)
                {
                    fcSetSample (c, 90 + fb (f.freqPos + 1) * 10 + fb (f.freqPos + 2), true);
                    restartVolume();
                    f.pitch = (juce::int8) fb (f.freqPos + 3);
                    f.freqPos += 4;
                    break;
                }
                if (b == 0xEA && fd.fc14) { f.bendSpeed = (juce::int8) fb (f.freqPos + 1); f.bendRemain = fb (f.freqPos + 2); f.pitch = (juce::int8) fb (f.freqPos + 3); f.freqPos += 4; break; }
                f.pitch = (juce::int8) b;
                ++f.freqPos;
                break;
            }
    }

    // volume sequence: one step every `speed` ticks; a volume slide holds the sequence
    if (f.volBendRemain > 0)
    {
        f.volBendStep = ! f.volBendStep;
        if (f.volBendStep) { --f.volBendRemain; f.volume = juce::jlimit (0, 64, f.volume + f.volBendSpeed); }
    }
    else if (! f.volStop)
    {
        if (f.volWait > 0) --f.volWait;
        else if (--f.volCount <= 0)
        {
            f.volCount = f.volSpeed;
            for (int guard = 0; guard < 64; ++guard)
            {
                const int b = vb (f.volPos);
                if (b == 0xE0)
                {
                    int t = vb (f.volPos + 1) & 0x3F;
                    if (t < 5) t += 256;   // FC's offset underflow
                    f.volPos = f.block + t;
                    continue;
                }
                if (b == 0xE1) { f.volStop = true; break; }
                if (b == 0xE8) { f.volWait = juce::jmax (0, vb (f.volPos + 1) - 1); f.volCount = 1; f.volPos += 2; break; }
                if (b == 0xEA && fd.fc14) { f.volBendSpeed = (juce::int8) vb (f.volPos + 1); f.volBendRemain = vb (f.volPos + 2); f.volPos += 3; break; }
                f.volume = juce::jmin (64, b & 0x7F);
                ++f.volPos;
                break;
            }
        }
    }

    // pitch: note index + macro pitch (negative = fixed note), vibrato, pitch slide, portamento
    const int fcNote = (f.pitch >= 0 ? f.pitch + f.index : f.pitch) & 0x7F;
    int period = periodForNote (Song::fcNoteForIndex (fcNote), 0);
    if (f.vibDelay > 0) --f.vibDelay;
    else if (f.vibDepth > 0)
    {
        if (f.vibUp) { f.vibValue += f.vibSpeed; if (f.vibValue > f.vibDepth * 2) { f.vibValue = f.vibDepth * 2; f.vibUp = false; } }
        else { f.vibValue -= f.vibSpeed; if (f.vibValue < 0) { f.vibValue = 0; f.vibUp = true; } }
        int vib = f.vibValue - f.vibDepth;
        for (int n = fcNote * 2 + 160; n < 256; n += 24) vib *= 2;
        period += vib;
    }
    if (f.bendRemain > 0)
    {
        f.bendStep = ! f.bendStep;
        if (f.bendStep) { --f.bendRemain; f.periodAdd -= f.bendSpeed; }
    }
    if (f.porta != 0)
    {
        f.portaStep = ! f.portaStep;
        if (f.portaStep) f.periodAdd -= f.porta;
    }
    period = juce::jlimit (113, 6848, period + f.periodAdd);
    c.period = c.outPeriod = period;
    c.volume = c.outVolume = f.volume;
}

void ModEngine::processTick()
{
    if (song == nullptr) return;
    const int numCh = song->channels;
    const int u = unit();
    for (int i = 0; i < numCh; ++i)
    {
        auto& c = ch[(size_t) i];
        if (song->format == Format::FC) { fcTick (c); continue; }
        const auto& cell = c.cell;
        const int fx = cell.fx, p = cell.param, hi = p >> 4, lo = p & 15;
        c.outPeriod = c.period;
        c.outVolume = c.volume;
        auto volSlide = [&] { if (hi) c.volume = juce::jmin (64, c.volume + hi); else c.volume = juce::jmax (0, c.volume - lo); c.outVolume = c.volume; };
        auto tonePorta = [&] {
            if (c.portaTarget == 0 || c.portaSpeed == 0) return;
            const int sp = c.portaSpeed * u;
            if (c.period < c.portaTarget) c.period = juce::jmin (c.portaTarget, c.period + sp);
            else if (c.period > c.portaTarget) c.period = juce::jmax (c.portaTarget, c.period - sp);
            c.outPeriod = c.period;
            if (c.glissando && song->periodMode == 0) c.outPeriod = periodForNote (noteForPeriod (c.period), c.finetune);
        };
        auto vibrato = [&] (int div) {
            c.outPeriod = c.period + (waveValue (c.vibWave, c.vibPos) * c.vibDepth * u) / div;
            c.vibPos = (c.vibPos + c.vibSpeed) & 63;
        };
        auto arp = [&] (double semis) { c.outPeriod = clampPeriod (periodShift (c.period, semis)); };

        // volume column (ticks > 0)
        switch (cell.vcmd)
        {
            case vcSlideDown: c.volume = juce::jmax (0, c.volume - cell.vol); c.outVolume = c.volume; break;
            case vcSlideUp: c.volume = juce::jmin (64, c.volume + cell.vol); c.outVolume = c.volume; break;
            case vcVibDepth: vibrato (128); break;
            case vcPanLeft: c.pan = juce::jmax (-1.0f, c.pan - cell.vol / 127.5f); break;
            case vcPanRight: c.pan = juce::jmin (1.0f, c.pan + cell.vol / 127.5f); break;
            case vcPorta: tonePorta(); break;
            case vcPitchDown: c.period = clampPeriod (c.period + cell.vol * 4 * u); c.outPeriod = c.period; break;
            case vcPitchUp: c.period = clampPeriod (c.period - cell.vol * 4 * u); c.outPeriod = c.period; break;
            default: break;
        }

        switch (fx)
        {
            case 0x0:
                if (p != 0)
                {
                    const int semis = (tick % 3) == 1 ? hi : (tick % 3) == 2 ? lo : 0;
                    if (song->periodMode == 0) c.outPeriod = juce::roundToInt (c.period * std::pow (2.0, -semis / 12.0));
                    else arp (semis);
                }
                break;
            case 0x1: c.period = clampPeriod (c.period - p * u); c.outPeriod = c.period; break;
            case 0x2: c.period = clampPeriod (c.period + p * u); c.outPeriod = c.period; break;
            case 0x3: tonePorta(); break;
            case 0x4: vibrato (128); break;
            case fxFineVib: vibrato (512); break;
            case 0x5: tonePorta(); volSlide(); break;
            case 0x6: vibrato (128); volSlide(); break;
            case 0x7:
                c.outVolume = juce::jlimit (0, 64, c.volume + (waveValue (c.tremWave, c.tremPos) * c.tremDepth) / 64);
                c.tremPos = (c.tremPos + c.tremSpeed) & 63;
                break;
            case 0xA: volSlide(); break;
            case 0xE:
                if (hi == 0x9 && lo > 0 && tick % lo == 0 && cell.note > 0 && cell.note <= 120) trigger (c, cell.note);
                else if (hi == 0xC && tick == lo) { c.volume = 0; c.outVolume = 0; c.cutFlag = true; }
                else if (hi == 0xD && tick == lo && c.delayedNote > 0)
                {
                    if (c.delayedInstr > 0)
                    {
                        if (song->instruments.empty())
                        {
                            if (c.delayedInstr <= song->numSamples())
                            {
                                c.sample = c.delayedInstr - 1;
                                c.volume = c.outVolume = song->samples[(size_t) c.sample].volume;
                                c.finetune = song->samples[(size_t) c.sample].finetune;
                            }
                        }
                        else if (c.delayedInstr <= (int) song->instruments.size())
                        {
                            c.instr = c.delayedInstr;
                            const int sn = song->sampleFor (c.instr, c.delayedNote);
                            if (sn >= 0) c.volume = c.outVolume = song->samples[(size_t) sn].volume;
                        }
                    }
                    trigger (c, c.delayedNote);
                    if (cell.vcmd == vcNone && cell.vol <= 64) c.volume = c.outVolume = cell.vol;
                    c.delayedNote = 0;
                }
                break;
            case fxXmH:
                if (hi) globalVol = juce::jmin (64, globalVol + hi); else globalVol = juce::jmax (0, globalVol - lo);
                break;
            case fxXmK: if (tick == p) keyOff (c); break;
            case fxXmP:
            {
                const float d = (hi ? (float) hi : -(float) lo) / 127.5f;
                c.pan = juce::jlimit (-1.0f, 1.0f, c.pan + d);
                break;
            }
            case fxXmR:
                if (lo > 0 && ++c.retrigCount >= lo)
                {
                    c.retrigCount = 0;
                    c.pos = 0.0;
                    c.active = c.sample >= 0 && ! song->samples[(size_t) c.sample].pcm.empty();
                    switch (hi)
                    {
                        case 1: case 2: case 3: case 4: case 5: c.volume -= 1 << (hi - 1); break;
                        case 6: c.volume = c.volume * 2 / 3; break;
                        case 7: c.volume /= 2; break;
                        case 9: case 10: case 11: case 12: case 13: c.volume += 1 << (hi - 9); break;
                        case 14: c.volume = c.volume * 3 / 2; break;
                        case 15: c.volume *= 2; break;
                        default: break;
                    }
                    c.volume = juce::jlimit (0, 64, c.volume);
                    c.outVolume = c.volume;
                }
                break;
            case fxXmT:
            {
                const int on = hi + 1, off = lo + 1;
                if (c.tremorCount % (on + off) >= on) c.outVolume = 0;
                ++c.tremorCount;
                break;
            }
            case fxChanVolSlide:
                if (lo == 0 && hi) c.chanVol = juce::jmin (64, c.chanVol + hi);
                else if (hi == 0 && lo) c.chanVol = juce::jmax (0, c.chanVol - lo);
                break;
            case fxItPanSlide:
                if (hi == 0 && lo) c.pan = juce::jmin (1.0f, c.pan + lo * 4 / 127.5f);
                else if (lo == 0 && hi) c.pan = juce::jmax (-1.0f, c.pan - hi * 4 / 127.5f);
                break;
            case fxTempoSlide:
                if (hi == 0) tempo = juce::jmax (32, tempo - lo);
                else tempo = juce::jmin (255, tempo + lo);
                break;
            case fxOktSlideUp: c.period = clampPeriod (periodShift (c.period, p)); c.outPeriod = c.period; break;
            case fxOktSlideDown: c.period = clampPeriod (periodShift (c.period, -p)); c.outPeriod = c.period; break;
            case fxOktArp1: { const int k = tick % 3; arp (k == 0 ? -hi : k == 1 ? 0 : lo); break; }
            case fxOktArp2: { const int k = tick % 4; arp (k == 1 ? lo : k == 3 ? -hi : 0); break; }
            case fxOktArp3: { const int k = tick % 3; arp (k == 2 ? 0 : lo * (k + 1)); break; }
            default: break;
        }
    }
}

namespace
{
float envValue (const Envelope& e, int t)
{
    const auto& pts = e.points;
    if (pts.empty()) return 1.0f;
    if (t <= pts.front().first) return (float) pts.front().second;
    for (size_t k = 1; k < pts.size(); ++k)
        if (t < pts[k].first)
        {
            const auto& a = pts[k - 1];
            const auto& b = pts[k];
            const float f = (float) (t - a.first) / (float) juce::jmax (1, b.first - a.first);
            return (float) a.second + ((float) b.second - (float) a.second) * f;
        }
    return (float) pts.back().second;
}

void envStep (const Envelope& e, int& t, bool keyOn)
{
    const auto& pts = e.points;
    if (pts.empty()) return;
    if (keyOn && e.sustain)
    {
        const int a = pts[(size_t) e.sus].first, b = pts[(size_t) e.susEnd].first;
        if (b <= a && t == a) return;               // sustain point
        if (b > a && t >= b) { t = a; return; }     // IT sustain loop
    }
    ++t;
    if (e.loop && t >= pts[(size_t) e.loopEnd].first) t = pts[(size_t) e.loopStart].first;
    if (t > pts.back().first) t = pts.back().first;
}
} // namespace

// envelopes, fadeout, auto vibrato, global volume -> mixVol/mixPan
void ModEngine::postTick()
{
    if (song == nullptr) return;
    for (int i = 0; i < song->channels && i < kMaxChannels; ++i) mixChannel (ch[(size_t) i]);
    for (auto& b : bg)
        if (b.active)
        {
            b.outPeriod = b.period;   // background voices keep their pitch; envelopes are added each tick
            mixChannel (b);
        }
}

void ModEngine::mixChannel (Channel& c)
{
    const bool useInstruments = ! song->instruments.empty();
    const bool it = song->format == Format::IT;
    {
        float vol = (float) c.outVolume * (float) c.chanVol / 64.0f;
        float pan = c.pan;
        if (c.sample >= 0 && c.sample < song->numSamples()) vol *= song->samples[(size_t) c.sample].globalGain;
        if (useInstruments && c.instr >= 1 && c.instr <= (int) song->instruments.size())
        {
            const auto& ins = song->instruments[(size_t) c.instr - 1];
            vol *= ins.globalGain;
            if (ins.vol.on)
            {
                vol *= envValue (ins.vol, c.envVolTick) / 64.0f;
                envStep (ins.vol, c.envVolTick, c.keyOn);
                // IT: envelope end (no loop) starts the fadeout, a final 0 ends the note
                const bool held = ins.vol.loop || (c.keyOn && ins.vol.sustain);
                if (it && ! held && c.envVolTick >= ins.vol.points.back().first)
                {
                    c.fading = true;
                    if (ins.vol.points.back().second == 0) c.active = false;
                }
            }
            if ((! c.keyOn && ins.vol.on) || c.fading)
            {
                c.fadeVol = juce::jmax (0, c.fadeVol - ins.fadeout * 2);
                if (c.fadeVol == 0) c.active = false;
            }
            vol *= (float) c.fadeVol / 65536.0f;
            if (ins.pan.on)
            {
                const float e = (envValue (ins.pan, c.envPanTick) - 32.0f) / 32.0f;
                pan = pan + e * (1.0f - std::abs (pan));
                envStep (ins.pan, c.envPanTick, c.keyOn);
            }
            if (ins.pitch.on && c.outPeriod > 0)
            {
                c.outPeriod = clampPeriod (periodShift (c.outPeriod, (envValue (ins.pitch, c.envPitchTick) - 32.0f) / 2.0));
                envStep (ins.pitch, c.envPitchTick, c.keyOn);
            }
            if (ins.vibDepth > 0 && ins.vibRate > 0 && c.active)
            {
                int amp = ins.vibDepth * 256;
                if (ins.vibSweep > 0 && c.avSweep < ins.vibSweep) amp = amp * c.avSweep / ins.vibSweep;
                ++c.avSweep;
                const int pos = (c.avPos >> 2) & 63;
                int w;
                switch (ins.vibType & 3)
                {
                    case 1: w = pos < 32 ? 255 : -255; break;
                    case 2: w = 255 - pos * 8; break;
                    case 3: w = pos * 8 - 255; break;
                    default: w = waveValue (0, pos); break;
                }
                c.outPeriod += (w * amp) / (256 * 256);
                c.avPos = (c.avPos + ins.vibRate) & 255;
            }
        }
        c.mixVol = vol * (float) globalVol / 64.0f;
        c.mixPan = juce::jlimit (-1.0f, 1.0f, pan);
    }
}

void ModEngine::advanceRow()
{
    if (song == nullptr) return;
    if (patternDelay > 0 && patternDelayCount < patternDelay) { ++patternDelayCount; return; }   // repeat row (without notes)
    patternDelay = patternDelayCount = 0;

    const int numOrders = (int) song->orders.size();
    if (breakFlag || jumpFlag)
    {
        const int o = nextOrder < 0 ? order + 1 : nextOrder;
        if (! loopPattern)
        {
            if (o <= order && ! (breakFlag && o == order)) ++songLoops;
            order = o;
        }
        row = nextRow < 0 ? 0 : nextRow;
    }
    else
    {
        ++row;
        const int pi = song->orders[(size_t) juce::jlimit (0, numOrders - 1, order)];
        const int rows = (pi >= 0 && pi < (int) song->patterns.size()) ? song->patterns[(size_t) pi].rows : 64;
        if (row >= rows)
        {
            row = 0;
            if (! loopPattern) ++order;
        }
    }
    breakFlag = jumpFlag = false;
    nextOrder = nextRow = -1;
    if (order >= numOrders || order < 0)
    {
        order = juce::jlimit (0, numOrders - 1, song->restartPos);
        ++songLoops;
    }
    // skip invalid pattern entries
    for (int guard = 0; guard < numOrders; ++guard)
    {
        const int pi = song->orders[(size_t) order];
        if (pi >= 0 && pi < (int) song->patterns.size()) break;
        order = (order + 1) % numOrders;
    }
}

void ModEngine::analysisTick()
{
    if (stopped || song == nullptr) return;
    // the current tick's state is already set up; advance sample positions by one tick, then step
    const double secs = tickSeconds();
    for (int i = 0; i < song->channels; ++i)
    {
        auto& c = ch[(size_t) i];
        if (! c.active || c.sample < 0 || c.outPeriod <= 0) continue;
        if (! advancePos (song->samples[(size_t) c.sample], c.pos, periodRate (c.outPeriod) * secs)) c.active = false;
    }
    for (auto& b : bg)
        if (b.active && b.sample >= 0 && b.outPeriod > 0 && ! advancePos (song->samples[(size_t) b.sample], b.pos, periodRate (b.outPeriod) * secs))
            b.active = false;
    if (++tick >= speed)
    {
        tick = 0;
        advanceRow();
        if (patternDelayCount == 0) processRow();
        else for (int i = 0; i < song->channels; ++i) ch[(size_t) i].cell = Cell();
    }
    else processTick();
    postTick();
}

void ModEngine::render (float* L, float* R, int n, juce::uint32 muteMask, float gain)
{
    if (stopped || song == nullptr) return;
    const int numCh = juce::jmin (song->channels, kMaxChannels);
    const float chGain = gain * 0.34f * std::sqrt (4.0f / (float) juce::jmax (4, numCh));
    const float sep = (float) stereoSeparation / 100.0f;
    int done = 0;
    while (done < n)
    {
        if (tickRemain <= 0.0)
        {
            tickRemain += sampleRate * tickSeconds();
        }
        const int todo = juce::jmin (n - done, juce::jmax (1, (int) std::ceil (tickRemain)));
        for (int i = 0; i < numCh; ++i)
        {
            auto& c = ch[(size_t) i];
            if (! c.active || c.sample < 0 || c.outPeriod <= 0) { c.level *= 0.9f; continue; }
            const auto& s = song->samples[(size_t) c.sample];
            const double inc = periodRate (c.outPeriod) / sampleRate;
            const bool muted = (muteMask >> i) & 1u;
            const float v = chGain * c.mixVol / 64.0f;
            const float e = juce::jlimit (-1.0f, 1.0f, c.mixPan * sep);
            const float gl = v * juce::jmin (1.0f, 1.0f - e), gr = v * juce::jmin (1.0f, 1.0f + e);
            float peak = 0.0f;
            for (int k = 0; k < todo; ++k)
            {
                const float x = samplePoint (s, c.pos, interpolate);
                if (! muted) { L[done + k] += x * gl; R[done + k] += x * gr; }
                peak = juce::jmax (peak, std::abs (x) * c.mixVol / 64.0f);
                if (! advancePos (s, c.pos, inc)) { c.active = false; break; }
            }
            c.level = juce::jmax (peak, c.level * 0.85f);
        }
        for (auto& b : bg)   // IT background voices (muted with their channel)
        {
            if (! b.active || b.sample < 0 || b.sample >= song->numSamples() || b.outPeriod <= 0) continue;
            const auto& s = song->samples[(size_t) b.sample];
            const double inc = periodRate (b.outPeriod) / sampleRate;
            const bool muted = b.owner >= 0 && ((muteMask >> b.owner) & 1u);
            const float v = chGain * b.mixVol / 64.0f;
            const float e = juce::jlimit (-1.0f, 1.0f, b.mixPan * sep);
            const float gl = v * juce::jmin (1.0f, 1.0f - e), gr = v * juce::jmin (1.0f, 1.0f + e);
            for (int k = 0; k < todo; ++k)
            {
                if (! muted) { const float x = samplePoint (s, b.pos, interpolate); L[done + k] += x * gl; R[done + k] += x * gr; }
                if (! advancePos (s, b.pos, inc)) { b.active = false; break; }
            }
        }
        done += todo;
        tickRemain -= todo;
        if (tickRemain <= 0.0)
        {
            if (++tick >= speed)
            {
                tick = 0;
                advanceRow();
                if (patternDelayCount == 0) processRow();
                else for (int i = 0; i < song->channels; ++i) ch[(size_t) i].cell = Cell();
            }
            else processTick();
            postTick();
        }
    }
}

// ============================================================================================ YmEngine
void YmEngine::setData (const YmData* d)
{
    ym = d;
    reset (0);
}

void YmEngine::prepare (double sr)
{
    sampleRate = sr;
    // YM2149 volume: 32 logarithmic steps of ~1.5 dB
    vol32[0] = 0.0f;
    for (int i = 1; i < 32; ++i) vol32[i] = (float) std::pow (10.0, -(31 - i) * 1.5 / 20.0);
    // envelope tables: 32 steps first segment, then 64 repeating
    auto seg = [] (int type, int k) { return type == 0 ? 31 - k : type == 1 ? k : type == 2 ? 0 : 31; };   // down, up, low, high
    for (int shape = 0; shape < 16; ++shape)
    {
        int a, b, c;
        if (shape < 4 || shape == 9) { a = 0; b = 2; c = 2; }
        else if (shape < 8 || shape == 15) { a = 1; b = 2; c = 2; }
        else if (shape == 8) { a = 0; b = 0; c = 0; }
        else if (shape == 10) { a = 0; b = 1; c = 0; }
        else if (shape == 11) { a = 0; b = 3; c = 3; }
        else if (shape == 12) { a = 1; b = 1; c = 1; }
        else if (shape == 13) { a = 1; b = 3; c = 3; }
        else { a = 1; b = 0; c = 1; }   // 14
        for (int k = 0; k < 32; ++k)
        {
            envTable[shape][k] = (juce::uint8) seg (a, k);
            envTable[shape][32 + k] = (juce::uint8) seg (b, k);
            envTable[shape][64 + k] = (juce::uint8) seg (c, k);
        }
    }
    if (ym) stepsPerSample = (ym->clock / 8.0) / sampleRate;
}

void YmEngine::reset (int f)
{
    std::fill (std::begin (reg), std::end (reg), (juce::uint8) 0);
    reg[7] = 0xff;
    for (int i = 0; i < 3; ++i) { toneCnt[i] = 0; toneOut[i] = 0; drum[i] = Drum(); level[i] = 0; }
    noiseCnt = noisePre = 0; lfsr = 1; noiseOut = 0;
    envCnt = envPos = envShape = 0;
    slot[0] = slot[1] = Fx();
    frameRemain = 0.0;
    songLoops = 0;
    dcL = dcR = 0.0;
    frame = ym ? juce::jlimit (0, juce::jmax (0, ym->frames - 1), f) : 0;
    if (ym) stepsPerSample = (ym->clock / 8.0) / sampleRate;
}

void YmEngine::startEffects (int f)
{
    static const int prediv[8] = { 0, 4, 10, 16, 50, 64, 100, 200 };
    const auto& r = ym->regs[(size_t) f];
    const bool v6 = ym->version.startsWith ("YM6");
    const bool v5 = ym->version.startsWith ("YM5");
    if (! v5 && ! v6) { slot[0].type = slot[1].type = -1; return; }
    for (int s = 0; s < 2; ++s)
    {
        const int code = s == 0 ? (r[1] >> 4) & 3 : (r[3] >> 4) & 3;
        const int pd = s == 0 ? (r[6] >> 5) & 7 : (r[8] >> 5) & 7;
        const int cnt = s == 0 ? r[14] : r[15];
        int type = v6 ? (s == 0 ? (r[1] >> 6) & 3 : (r[3] >> 6) & 3) : (s == 0 ? 0 : 1);
        auto& fx = slot[s];
        if (code == 0 || pd == 0 || cnt == 0)
        {
            if (fx.type != 1) fx.type = -1;
            if (fx.type == 1) fx.type = -1;
            continue;
        }
        const double freq = 2457600.0 / (prediv[pd] * (double) cnt);
        const int voice = code - 1;
        if (type == 1)
        {
            const int nd = reg[8 + voice] & 31;
            if (nd < (int) ym->drums.size() && ! ym->drums[(size_t) nd].empty())
            {
                auto& d = drum[voice];
                if (! d.on || d.index != nd || fx.type != 1 || fx.voice != voice) { d.pos = 0.0; }
                d.on = true; d.index = nd; d.step = freq / sampleRate;
            }
        }
        if (fx.type != type || fx.voice != voice) fx.phase = 0.0;
        fx.type = type; fx.voice = voice; fx.freq = freq;
    }
}

void YmEngine::writeFrame (int f)
{
    const auto& r = ym->regs[(size_t) f];
    for (int i = 0; i < 13; ++i) reg[i] = r[(size_t) i];
    // YM5/6: effect bits live in the high bits of some registers; mask them for the PSG
    if (ym->version[2] >= '5')
    {
        reg[1] &= 0x0f; reg[3] &= 0x0f; reg[5] &= 0x0f; reg[6] &= 0x1f;
        reg[8] &= 0x1f; reg[9] &= 0x1f; reg[10] &= 0x1f;
    }
    if (r[13] != 0xff)
    {
        reg[13] = r[13] & 15;
        envShape = reg[13];
        envPos = 0;
        envCnt = 0;
    }
    startEffects (f);
}

void YmEngine::render (float* L, float* R, int n, juce::uint32 muteMask, float gain)
{
    if (ym == nullptr || ym->frames <= 0) return;
    const double samplesPerFrame = sampleRate / ym->rate;
    const float sep = (float) stereoSeparation / 100.0f;
    const float panA = -0.8f * sep, panC = 0.8f * sep;
    float peak[3] = { 0, 0, 0 };
    for (int k = 0; k < n; ++k)
    {
        if (frameRemain <= 0.0)
        {
            writeFrame (frame);
            if (++frame >= ym->frames) { frame = ym->loopFrame; ++songLoops; }
            frameRemain += samplesPerFrame;
        }
        frameRemain -= 1.0;

        // run the PSG at clock/8 and average the steps of this output sample
        stepAcc += stepsPerSample;
        const int steps = (int) stepAcc;
        stepAcc -= steps;
        const int tp[3] = { reg[0] | ((reg[1] & 15) << 8), reg[2] | ((reg[3] & 15) << 8), reg[4] | ((reg[5] & 15) << 8) };
        const int np = juce::jmax (1, reg[6] & 31);
        const int ep = juce::jmax (1, reg[11] | (reg[12] << 8));
        float acc[3] = { 0, 0, 0 };
        for (int s = 0; s < steps; ++s)
        {
            for (int c = 0; c < 3; ++c)
                if (++toneCnt[c] >= juce::jmax (1, tp[c])) { toneCnt[c] = 0; toneOut[c] ^= 1; }
            if (++noiseCnt >= np * 2)
            {
                noiseCnt = 0;
                const juce::uint32 bit = (lfsr ^ (lfsr >> 3)) & 1u;
                lfsr = (lfsr >> 1) | (bit << 16);
                noiseOut = (int) (lfsr & 1u);
            }
            if (++envCnt >= ep)
            {
                envCnt = 0;
                if (++envPos >= 96) envPos = 32;
            }
            const int envLevel = envTable[envShape][envPos];
            for (int c = 0; c < 3; ++c)
            {
                const bool toneOff = (reg[7] >> c) & 1, noiseOff = (reg[7] >> (c + 3)) & 1;
                const bool on = (toneOut[c] || toneOff || tp[c] < 2) && (noiseOut || noiseOff);
                int v = reg[8 + c] & 31;
                const int lvl = (v & 16) ? envLevel : (v & 15) * 2 + 1;
                float out = on ? vol32[lvl] : 0.0f;
                if ((v & 15) == 0 && ! (v & 16)) out = 0.0f;
                acc[c] += out;
            }
        }
        const float inv = steps > 0 ? 1.0f / (float) steps : 0.0f;
        float out[3];
        for (int c = 0; c < 3; ++c) out[c] = acc[c] * inv;

        // special effects
        for (auto& fx : slot)
        {
            if (fx.type < 0) continue;
            fx.phase += fx.freq / sampleRate;
            const bool high = std::fmod (fx.phase, 1.0) < 0.5;
            if (fx.type == 0 || fx.type == 2)   // SID voice: square amplitude modulation
            {
                if (! high) out[fx.voice] = 0.0f;
            }
            else if (fx.type == 3)               // sync buzzer: retrigger envelope
            {
                if (fx.phase >= 1.0) { fx.phase -= 1.0; envPos = 0; envCnt = 0; }
            }
            if (fx.phase >= 1.0) fx.phase -= std::floor (fx.phase);
        }
        for (int c = 0; c < 3; ++c)
        {
            auto& d = drum[c];
            if (! d.on) continue;
            const auto& data = ym->drums[(size_t) d.index];
            const int i = (int) d.pos;
            if (i >= (int) data.size()) { d.on = false; continue; }
            out[c] = vol32[juce::jlimit (0, 31, (int) data[(size_t) i] >> 3)];
            d.pos += d.step;
        }

        for (int c = 0; c < 3; ++c)
        {
            peak[c] = juce::jmax (peak[c], out[c]);
            if ((muteMask >> c) & 1u) out[c] = 0.0f;
        }
        const float g = gain * 0.55f;
        float l = 0.0f, rr = 0.0f;
        const float pans[3] = { panA, 0.0f, panC };
        for (int c = 0; c < 3; ++c)
        {
            l += out[c] * (1.0f - pans[c]) * 0.5f;
            rr += out[c] * (1.0f + pans[c]) * 0.5f;
        }
        // DC blocker
        dcL += (l - dcL) * 0.0015;
        dcR += (rr - dcR) * 0.0015;
        L[k] += (float) (l - dcL) * g * 1.2f;
        R[k] += (float) (rr - dcR) * g * 1.2f;
    }
    for (int c = 0; c < 3; ++c) level[c] = juce::jmax (peak[c], level[c] * 0.85f);
}

// ============================================================================================ Sampler
void Sampler::ensureInit()
{
    if (init) return;
    init = true;
    for (int i = 0; i < 16; ++i) { cc7[i] = 1.0f; cc11[i] = 1.0f; bend[i] = 0.0f; bendRange[i] = 12; rpnMsb[i] = rpnLsb[i] = 127; offsetLo[i] = offsetHi[i] = 0; pan[i] = 0.0f; }
}

void Sampler::allOff()
{
    for (auto& v : voices) v.on = false;
}

void Sampler::noteOn (int c, int note, int vel, int sample)
{
    const bool chip = song != nullptr && song->format == Format::YM;   // YM songs: square/noise voice like the YM2149
    if (song == nullptr || (! chip && (sample < 0 || sample >= song->numSamples() || song->samples[(size_t) sample].pcm.empty()))) return;
    // monophonic per channel+note: retrigger same note
    Voice* free = nullptr;
    for (auto& v : voices)
        if (v.on && v.ch == c && v.note == note) { v.release = true; }
    for (auto& v : voices)
        if (! v.on) { free = &v; break; }
    if (free == nullptr)
    {
        free = &voices[0];
        for (auto& v : voices) if (v.env < free->env) free = &v;
    }
    free->on = true; free->release = false; free->ch = c; free->note = note; free->sample = sample;
    free->pos = 0.0; free->env = 1.0f;
    if (! chip && c < 16)
    {
        const auto& smp = song->samples[(size_t) sample];
        const double off = 256.0 * (offsetLo[c] | (offsetHi[c] << 7));
        if (off >= (double) smp.pcm.size()) { if (smp.hasLoop()) free->pos = smp.loopStart; else { free->on = false; return; } }
        else free->pos = off;
    }
    free->gain = (float) vel / 127.0f * (chip ? 0.5f : (float) song->samples[(size_t) sample].volume / 64.0f);
    free->sample = chip ? -1 : sample;
}

void Sampler::noteOff (int c, int note)
{
    for (auto& v : voices)
        if (v.on && v.ch == c && v.note == note) v.release = true;
}

void Sampler::handle (const juce::MidiMessage& m)
{
    ensureInit();
    const int c = juce::jlimit (0, 15, m.getChannel() - 1);
    if (m.isNoteOn())
    {
        int smp = program[c];
        if (song != nullptr && ! song->instruments.empty()) smp = song->sampleFor (program[c] + 1, juce::jlimit (1, 120, m.getNoteNumber() + 1));
        noteOn (c, m.getNoteNumber(), m.getVelocity(), smp);
    }
    else if (m.isNoteOff()) noteOff (c, m.getNoteNumber());
    else if (m.isProgramChange()) program[c] = m.getProgramChangeNumber();
    else if (m.isPitchWheel()) bend[c] = (float) (m.getPitchWheelValue() - 8192) / 8192.0f;
    else if (m.isAllNotesOff() || m.isAllSoundOff()) { for (auto& v : voices) if (v.ch == c) v.release = true; }
    else if (m.isController())
    {
        const int cc = m.getControllerNumber(), val = m.getControllerValue();
        if (cc == 7) cc7[c] = (float) val / 127.0f;
        else if (cc == 11) cc11[c] = (float) val / 127.0f;
        else if (cc == 10) pan[c] = juce::jlimit (-1.0f, 1.0f, (float) (val - 64) / 63.0f);
        else if (cc == 110) offsetLo[c] = val;
        else if (cc == 111) offsetHi[c] = val;
        else if (cc == 101) rpnMsb[c] = val;
        else if (cc == 100) rpnLsb[c] = val;
        else if (cc == 6 && rpnMsb[c] == 0 && rpnLsb[c] == 0) bendRange[c] = juce::jlimit (1, 48, val);
        else if (cc == 121) { cc7[c] = cc11[c] = 1.0f; bend[c] = 0.0f; offsetLo[c] = offsetHi[c] = 0; pan[c] = 0.0f; }
    }
}

void Sampler::preview (int instr, int note, bool on)
{
    ensureInit();
    if (on)
    {
        for (auto& v : voices) if (v.ch == 15 + 16) v.release = true;
        noteOn (31, note, 110, instr - 1);   // instr = entry of the sample list
    }
    else for (auto& v : voices) if (v.ch == 31) v.release = true;
}

void Sampler::render (float* L, float* R, int n, float gain)
{
    ensureInit();
    if (song == nullptr) return;
    const float relStep = 1.0f / (0.015f * (float) sampleRate);
    for (auto& v : voices)
    {
        if (! v.on) continue;
        if (v.sample < 0)
        {
            // chip voice (YM songs): square wave, MIDI channel 10 = noise
            const int c = v.ch & 15;
            const float gl = juce::jmin (1.0f, 1.0f - pan[c]), gr = juce::jmin (1.0f, 1.0f + pan[c]);
            const bool noise = c == 9;
            const double freq = 440.0 * std::pow (2.0, ((v.note - 69) + bend[c] * bendRange[c]) / 12.0);
            const double inc = (noise ? 8000.0 + v.note * 120.0 : freq) / sampleRate;
            const float g = gain * 0.3f * v.gain * cc7[c] * cc11[c];
            for (int k = 0; k < n; ++k)
            {
                if (v.release) { v.env -= relStep; if (v.env <= 0.0f) { v.on = false; break; } }
                v.pos += inc;
                float x;
                if (noise)
                {
                    if (v.pos >= 1.0) { v.pos -= 1.0; noiseState = noiseState * 1664525u + 1013904223u; }
                    x = (noiseState >> 31) ? 1.0f : -1.0f;
                }
                else
                {
                    v.pos -= std::floor (v.pos);
                    x = v.pos < 0.5 ? 1.0f : -1.0f;
                }
                x *= g * v.env;
                L[k] += x * gl; R[k] += x * gr;
            }
            continue;
        }
        if (v.sample >= song->numSamples()) { v.on = false; continue; }
        const auto& s = song->samples[(size_t) v.sample];
        const int c = v.ch & 15;
        const bool prev = v.ch >= 16;
        const float gl = prev ? 1.0f : juce::jmin (1.0f, 1.0f - pan[c]), gr = prev ? 1.0f : juce::jmin (1.0f, 1.0f + pan[c]);
        const double semis = (v.note - 60) + (prev ? 0.0 : bend[c] * bendRange[c]);
        const double inc = s.c5Rate() * std::pow (2.0, semis / 12.0) / sampleRate;
        const float g = gain * 0.5f * v.gain * (prev ? 1.0f : cc7[c] * cc11[c]);
        for (int k = 0; k < n; ++k)
        {
            if (v.release) { v.env -= relStep; if (v.env <= 0.0f) { v.on = false; break; } }
            const float x = samplePoint (s, v.pos, true) * g * v.env;
            L[k] += x * gl; R[k] += x * gr;
            if (! advancePos (s, v.pos, inc)) { v.on = false; break; }
        }
    }
}
} // namespace sw
