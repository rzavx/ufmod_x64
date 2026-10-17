// ufmod_test.cpp
// Stage 5 test: uses ONLY the classic uFMOD API from ufmod.h.
//
// Project files: ufmod.h, ufmod_api.cpp, xm_format.h, xm_loader.h/.cpp,
//                xm_mixer.h/.cpp, xm_player.h/.cpp, ufmod_test.cpp
//
//   ufmod_test.exe  song.xm
// Keys: SPACE pause/resume  +/- volume  N next order  R rewind  Q quit

#include "ufmod.h"
#include <conio.h>
#include <windows.h>
#include <cstdio>
#include <cmath>

static void PrintBar(unsigned level) {         // 0..0x7FFF -> 0..20 chars, dB scaled
    double db = level ? 20.0 * std::log10(level / 32767.0) : -60.0;   // -60..0 dB
    int n = static_cast<int>((db + 60.0) / 60.0 * 20.0 + 0.5);
    if (n < 0) n = 0;
    if (n > 20) n = 20;
    for (int i = 0; i < 20; ++i) std::putchar(i < n ? '#' : '.');
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: ufmod_test <song.xm>\n"); return 1; }

    if (!uFMOD_PlaySong(argv[1], nullptr, XM_FILE)) {
        std::printf("uFMOD_PlaySong failed (invalid XM or no audio device)\n");
        return 1;
    }

    std::printf("Playing \"%s\"   SPACE pause, +/- volume, N next order, R rewind, Q quit\n",
                reinterpret_cast<const char*>(uFMOD_GetTitle()));

    int vol = uFMOD_DEFAULT_VOL;
    bool paused = false;

    for (;;) {
        if (_kbhit()) {
            int c = _getch();
            if (c == 'q' || c == 'Q') break;
            if (c == ' ') { paused = !paused; if (paused) uFMOD_Pause(); else uFMOD_Resume(); }
            if ((c == '+' || c == '=') && vol < uFMOD_MAX_VOL) uFMOD_SetVolume(++vol);
            if ((c == '-' || c == '_') && vol > uFMOD_MIN_VOL) uFMOD_SetVolume(--vol);
            if (c == 'n' || c == 'N') uFMOD_Jump2Pattern((uFMOD_GetRowOrder() >> 16) + 1);
            if (c == 'r' || c == 'R') uFMOD_Jump2Pattern(0);
        }

        unsigned ro = uFMOD_GetRowOrder(), ms = uFMOD_GetTime(), st = uFMOD_GetStats();
        std::printf("\rord %3u row %3u  %02u:%02u  vol %2d  L[", ro >> 16, ro & 0xFFFF,
                    ms / 60000, (ms / 1000) % 60, vol);
        PrintBar(st >> 16);
        std::printf("] R[");
        PrintBar(st & 0xFFFF);
        std::printf("] %s ", paused ? "paused" : "      ");
        std::fflush(stdout);
        Sleep(60);
    }

    std::printf("\n");
    uFMOD_PlaySong(nullptr, nullptr, 0);   // stop and release the audio device
    return 0;
}
