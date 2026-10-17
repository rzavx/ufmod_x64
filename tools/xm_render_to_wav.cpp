// xm_render_to_wav.cpp
// Loads an XM file, mixes N seconds of audio, writes a standard .wav file.
// This lets you *listen* to Stage 2's output before Stage 4 (XAudio2) exists.
//
// Build as a VC++ console app (x64), add xm_format.h, xm_loader.h/.cpp,
// xm_mixer.h/.cpp and this file to the project. Run:
//   xm_render_to_wav.exe  song.xm  out.wav  [seconds]

#include "xm_loader.h"
#include "xm_mixer.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstdint>
#include <cstdlib>

namespace {

#pragma pack(push, 1)
struct WavHeader {
    char     riff[4]       = {'R','I','F','F'};
    uint32_t chunkSize      = 0;
    char     wave[4]       = {'W','A','V','E'};
    char     fmt[4]        = {'f','m','t',' '};
    uint32_t fmtSize        = 16;
    uint16_t audioFormat    = 1; // PCM
    uint16_t numChannels    = 2;
    uint32_t sampleRate     = 48000;
    uint32_t byteRate       = 0;
    uint16_t blockAlign     = 0;
    uint16_t bitsPerSample  = 16;
    char     data[4]       = {'d','a','t','a'};
    uint32_t dataSize       = 0;
};
#pragma pack(pop)

bool WriteWav(const std::wstring& path, const std::vector<int16_t>& interleavedStereo, uint32_t sampleRate) {
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;

    WavHeader hdr;
    hdr.sampleRate = sampleRate;
    hdr.blockAlign = hdr.numChannels * (hdr.bitsPerSample / 8);
    hdr.byteRate = hdr.sampleRate * hdr.blockAlign;
    hdr.dataSize = static_cast<uint32_t>(interleavedStereo.size() * sizeof(int16_t));
    hdr.chunkSize = 36 + hdr.dataSize;

    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    f.write(reinterpret_cast<const char*>(interleavedStereo.data()), hdr.dataSize);
    return f.good();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::wcerr << L"usage: xm_render_to_wav <song.xm> <out.wav> [seconds=20]\n";
        return 1;
    }

    double seconds = (argc >= 4) ? _wtof(argv[3]) : 20.0;
    const uint32_t sampleRate = 48000;

    xm::Song song;
    xm::LoadResult lr = xm::XmLoader::LoadFromFile(argv[1], song);
    if (lr != xm::LoadResult::Ok) {
        std::wcerr << L"Failed to load XM: " << xm::ToString(lr) << L"\n";
        return 1;
    }

    std::wcout << L"Loaded \"" << argv[1] << L"\" - "
               << song.numChannels << L" channels, "
               << song.patterns.size() << L" patterns, "
               << song.instruments.size() << L" instruments\n";

    xm::XmMixer mixer;
    mixer.Init(song, sampleRate);
    mixer.SetMasterVolume(25); // max

    uint32_t totalFrames = static_cast<uint32_t>(seconds * sampleRate);
    std::vector<int16_t> buffer(static_cast<size_t>(totalFrames) * 2);

    uint32_t written = mixer.RenderInt16Stereo(buffer.data(), totalFrames);
    if (written < totalFrames) {
        std::wcout << L"(song ended early at " << written << L" of " << totalFrames << L" frames)\n";
        buffer.resize(static_cast<size_t>(written) * 2);
    }

    if (!WriteWav(argv[2], buffer, sampleRate)) {
        std::wcerr << L"Failed to write " << argv[2] << L"\n";
        return 1;
    }

    std::wcout << L"Wrote " << argv[2] << L" (" << (written / double(sampleRate)) << L" seconds)\n";
    return 0;
}
