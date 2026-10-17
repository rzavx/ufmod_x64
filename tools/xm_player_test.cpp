// xm_player_test.cpp
// Stage 4 test: plays an XM file through XAudio2.
//
// Project files: xm_format.h, xm_loader.h/.cpp, xm_mixer.h/.cpp,
//                xm_player.h/.cpp, xm_player_test.cpp   (VC++ 2022, x64)
//
//   xm_player_test.exe  song.xm
//
// Keys:  SPACE pause/resume   + / -  volume   N next order   Q quit

#include "xm_loader.h"
#include "xm_player.h"
#include <conio.h>
#include <windows.h>
#include <cstdio>
#include <memory>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: xm_player_test <song.xm>\n");
        return 1;
    }

    auto song = std::make_shared<xm::Song>();
    xm::LoadResult lr = xm::XmLoader::LoadFromFile(argv[1], *song);
    if (lr != xm::LoadResult::Ok) {
        std::fprintf(stderr, "Load failed: %s\n", xm::ToString(lr));
        return 1;
    }

    xm::PlayerOptions opt;
    opt.noLoop = false;

    xm::XmPlayer player;
    xm::PlayerError pe = player.Open(song, opt);
    if (pe != xm::PlayerError::Ok) {
        std::fprintf(stderr, "Open failed: %s\n", xm::ToString(pe));
        return 1;
    }

    std::printf("Playing \"%s\"  (SPACE pause, +/- volume, N next order, Q quit)\n", song->name.c_str());

    int volume = opt.volume;
    uint16_t jumpTarget = 0;

    for (;;) {
        if (_kbhit()) {
            int c = _getch();
            if (c == 'q' || c == 'Q') break;
            if (c == ' ') { if (player.IsPaused()) player.Resume(); else player.Pause(); }
            if (c == '+' || c == '=') { if (volume < 25) player.SetVolume(static_cast<uint8_t>(++volume)); }
            if (c == '-' || c == '_') { if (volume > 0)  player.SetVolume(static_cast<uint8_t>(--volume)); }
            if (c == 'n' || c == 'N') {
                jumpTarget = static_cast<uint16_t>((player.GetRowOrder() >> 16) + 1);
                if (jumpTarget >= song->songLength) jumpTarget = 0;
                player.JumpToOrder(jumpTarget);
            }
        }

        uint32_t ro = player.GetRowOrder();
        uint32_t ms = player.GetTimeMs();
        std::printf("\rorder %3u/%-3u row %3u  time %02u:%02u.%01u  vol %2d  %s   ",
                    ro >> 16, song->songLength, ro & 0xFFFF,
                    ms / 60000, (ms / 1000) % 60, (ms / 100) % 10,
                    volume, player.IsPaused() ? "[paused]" : "        ");
        std::fflush(stdout);

        if (player.IsFinished()) break;
        Sleep(50);
    }

    std::printf("\n");
    player.Close();
    return 0;
}
