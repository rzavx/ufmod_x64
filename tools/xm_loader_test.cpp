// xm_loader_test.cpp
// Minimal console test for Stage 1. Build as a VC++ console app (x64),
// add xm_format.h, xm_loader.h, xm_loader.cpp to the project, then run:
//   xm_loader_test.exe  path\to\song.xm

#include "xm_loader.h"
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcerr << L"usage: xm_loader_test <file.xm>\n";
        return 1;
    }

    xm::Song song;
    xm::LoadResult result = xm::XmLoader::LoadFromFile(argv[1], song);

    std::cout << "Load result: " << xm::ToString(result) << "\n";
    if (result != xm::LoadResult::Ok) {
        return 1;
    }

    std::cout << "Song name     : " << song.name << "\n";
    std::cout << "Tracker       : " << song.trackerName << "\n";
    std::cout << "Channels      : " << song.numChannels << "\n";
    std::cout << "Song length   : " << song.songLength << " orders\n";
    std::cout << "Patterns      : " << song.patterns.size() << "\n";
    std::cout << "Instruments   : " << song.instruments.size() << "\n";
    std::cout << "Default tempo : " << song.defaultTempo << "\n";
    std::cout << "Default BPM   : " << song.defaultBPM << "\n";
    std::cout << "Freq table    : " << (song.linearFrequencyTable ? "Linear" : "Amiga") << "\n\n";

    for (size_t i = 0; i < song.instruments.size(); ++i) {
        const xm::Instrument& inst = song.instruments[i];
        if (inst.name.empty() && inst.samples.empty()) continue;
        std::cout << "  [" << i + 1 << "] " << inst.name
                   << "  (" << inst.samples.size() << " sample(s))\n";
        for (size_t s = 0; s < inst.samples.size(); ++s) {
            const xm::Sample& smp = inst.samples[s];
            std::cout << "        sample " << s << ": \"" << smp.name << "\""
                       << ", " << smp.length << " frames"
                       << ", " << (smp.was16Bit ? "16-bit" : "8-bit")
                       << ", loop=" << (int)smp.loopType << "\n";
        }
    }

    return 0;
}
