#include "SampleShare.h"
#include "Export.h"

namespace sw
{
namespace
{
juce::CriticalSection& writeLock()
{
    static juce::CriticalSection cs;
    return cs;
}

juce::String moduleKey (const Song& song)
{
    const auto id = song.sourcePath.isNotEmpty() ? song.sourcePath : song.title;
    return safeFileName (song.title.isNotEmpty() ? song.title : juce::String ("Spurwerk")) + "_"
         + juce::String::toHexString ((juce::int64) id.toLowerCase().hashCode64()).substring (0, 10);
}

void pruneOld()
{
    const auto limit = juce::Time::getCurrentTime() - juce::RelativeTime::days (14);
    for (const auto& d : shareRoot().findChildFiles (juce::File::findDirectories, false))
        if (d.getChildFile ("index.xml").getLastModificationTime() < limit) d.deleteRecursively();
}
} // namespace

juce::File shareRoot()
{
    auto d = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("Spurwerk").getChildFile ("share");
    d.createDirectory();
    return d;
}

void publishSongSamples (const Song& song)
{
    if (! song.hasSamples()) return;
    int used = 0;
    for (const auto& s : song.samples) if (! s.pcm.empty()) ++used;
    if (used == 0) return;

    const juce::ScopedLock sl (writeLock());
    const auto dir = shareRoot().getChildFile (moduleKey (song));
    dir.deleteRecursively();
    if (! dir.createDirectory()) return;

    juce::XmlElement index ("SpurwerkShare");
    index.setAttribute ("version", 1);
    index.setAttribute ("title", song.title);
    index.setAttribute ("format", song.formatName);
    index.setAttribute ("source", song.sourcePath);
    for (int i = 0; i < song.numSamples(); ++i)
    {
        const auto& smp = song.samples[(size_t) i];
        if (smp.pcm.empty()) continue;
        const auto title = sampleTitle (song, i);
        const auto base = safeFileName (title);
        const auto wav = dir.getChildFile (base + ".wav");
        if (! writeSampleWav (smp, wav)) continue;
        auto* e = index.createNewChildElement ("Sample");
        e->setAttribute ("index", i);
        e->setAttribute ("name", title);
        e->setAttribute ("wav", wav.getFileName());
        e->setAttribute ("c5", smp.c5Rate());
        e->setAttribute ("loopStart", smp.hasLoop() ? smp.loopStart : 0);
        e->setAttribute ("loopLength", smp.hasLoop() ? smp.loopLength : 0);
        e->setAttribute ("volume", smp.volume);
        e->setAttribute ("frames", (int) smp.pcm.size());
        MidiExportOptions o;
        o.onlySample = i;
        juce::String sum;
        const auto mf = exportMidi (song, o, &sum);
        if (! sum.startsWith ("0 "))
        {
            const auto mid = dir.getChildFile (base + ".mid");
            if (writeMidiFile (mf, mid)) e->setAttribute ("midi", mid.getFileName());
        }
    }
    index.writeTo (dir.getChildFile ("index.xml"));   // written last: a folder without index is ignored
    pruneOld();
}

void publishSongSamplesAsync (const Song& song)
{
    if (! song.hasSamples()) return;
    auto copy = std::make_shared<Song> (song);
    juce::Thread::launch ([copy] { publishSongSamples (*copy); });
}

juce::MemoryBlock makePlayerState (const Sample& s, const juce::String& name, const juce::String& source,
                                   const juce::String& shareFolder, int shareIndex, float gain, const juce::MemoryBlock& midi)
{
    juce::XmlElement x ("SpurwerkPlayer");
    x.setAttribute ("version", 1);
    x.setAttribute ("gain", (double) gain);
    x.setAttribute ("name", name);
    x.setAttribute ("source", source);
    x.setAttribute ("shareFolder", shareFolder);
    x.setAttribute ("shareIndex", shareIndex);
    if (! s.pcm.empty())
    {
        x.setAttribute ("c5", s.c5Rate());
        x.setAttribute ("loopStart", s.loopStart);
        x.setAttribute ("loopLength", s.loopLength);
        x.setAttribute ("volume", s.volume);
        juce::MemoryOutputStream pcm;
        for (float v : s.pcm) pcm.writeShort ((short) juce::jlimit (-32768, 32767, juce::roundToInt (v * 32767.0f)));
        x.createNewChildElement ("pcm")->addTextElement (pcm.getMemoryBlock().toBase64Encoding());
    }
    if (midi.getSize() > 0) x.createNewChildElement ("midi")->addTextElement (midi.toBase64Encoding());
    // same layout as juce::AudioProcessor::copyXmlToBinary ("VC2!", length, XML, 0) without needing juce_audio_processors
    juce::MemoryBlock out;
    {
        juce::MemoryOutputStream os (out, false);
        os.writeInt (0x21324356);
        os.writeInt (0);
        x.writeTo (os, juce::XmlElement::TextFormat().singleLine());
        os.writeByte (0);
    }
    const auto len = (juce::uint32) out.getSize() - 9;
    out.copyFrom (&len, 4, 4);
    return out;
}

std::vector<SharedModule> listSharedModules()
{
    std::vector<SharedModule> list;
    for (const auto& d : shareRoot().findChildFiles (juce::File::findDirectories, false))
    {
        const auto idx = d.getChildFile ("index.xml");
        auto xml = juce::XmlDocument::parse (idx);
        if (xml == nullptr || ! xml->hasTagName ("SpurwerkShare")) continue;
        SharedModule m;
        m.folder = d;
        m.title = xml->getStringAttribute ("title");
        m.formatName = xml->getStringAttribute ("format");
        m.sourcePath = xml->getStringAttribute ("source");
        m.time = idx.getLastModificationTime();
        for (auto* e : xml->getChildWithTagNameIterator ("Sample"))
        {
            SharedSample s;
            s.index = e->getIntAttribute ("index");
            s.name = e->getStringAttribute ("name");
            s.wav = d.getChildFile (e->getStringAttribute ("wav"));
            if (e->hasAttribute ("midi")) s.midi = d.getChildFile (e->getStringAttribute ("midi"));
            s.c5Rate = e->getDoubleAttribute ("c5", 8363.0);
            s.loopStart = e->getIntAttribute ("loopStart");
            s.loopLength = e->getIntAttribute ("loopLength");
            s.volume = e->getIntAttribute ("volume", 64);
            s.frames = e->getIntAttribute ("frames");
            if (s.wav.existsAsFile()) m.samples.push_back (s);
        }
        if (! m.samples.empty()) list.push_back (std::move (m));
    }
    std::sort (list.begin(), list.end(), [] (const SharedModule& a, const SharedModule& b) { return a.time > b.time; });
    return list;
}
} // namespace sw
