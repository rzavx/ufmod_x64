// ufmod_api.cpp
// Stage 5: the classic uFMOD C API (see ufmod.h) implemented on top of
// XmLoader + XmPlayer. One song plays at a time, exactly like the original.
//
// Thread safety: every entry point takes one global mutex, so the functions
// may be called from any thread.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ufmod.h"
#include "xm_loader.h"
#include "xm_player.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

extern "C" {
unsigned long uFMOD_tsc = 0;
}

namespace {

std::mutex                      g_mutex;
std::unique_ptr<xm::XmPlayer>   g_player;
unsigned int                    g_volume = uFMOD_DEFAULT_VOL;
void*                           g_waveOutToken = nullptr; // returned as the "HWAVEOUT*"

std::wstring AnsiToWide(const char* s) {
    if (!s) return std::wstring();
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &w[0], n);
    w.resize(static_cast<size_t>(n) - 1); // drop the terminator
    return w;
}

bool LoadSong(void* lpXM, void* param, int flags, bool wide, xm::Song& song) {
    xm::LoadResult r = xm::LoadResult::Corrupt;

    switch (flags & 3) {
        case XM_FILE: {
            if (!lpXM) return false;
            std::wstring path = wide ? std::wstring(static_cast<const wchar_t*>(lpXM))
                                     : AnsiToWide(static_cast<const char*>(lpXM));
            r = xm::XmLoader::LoadFromFile(path, song);
            break;
        }
        case XM_MEMORY: {
            size_t size = static_cast<size_t>(reinterpret_cast<uintptr_t>(param));
            if (!lpXM || size == 0) return false;
            r = xm::XmLoader::LoadFromMemory(static_cast<const uint8_t*>(lpXM), size, song);
            break;
        }
        case XM_RESOURCE: {
            HMODULE mod = static_cast<HMODULE>(param);   // NULL = the executable itself
            HRSRC res = wide ? FindResourceW(mod, static_cast<LPCWSTR>(lpXM), MAKEINTRESOURCEW(10))   // 10 = RT_RCDATA
                             : FindResourceA(mod, static_cast<LPCSTR>(lpXM),  MAKEINTRESOURCEA(10));
            if (!res) return false;
            HGLOBAL h = LoadResource(mod, res);
            if (!h) return false;
            const void* data = LockResource(h);
            DWORD size = SizeofResource(mod, res);
            if (!data || size == 0) return false;
            r = xm::XmLoader::LoadFromMemory(static_cast<const uint8_t*>(data), size, song);
            break;
        }
        default:
            return false;
    }
    return r == xm::LoadResult::Ok;
}

void* PlaySongImpl(void* lpXM, void* param, int flags, bool wide) {
    std::lock_guard<std::mutex> lock(g_mutex);

    // Always stop the current song first (even if the new one fails to load).
    if (g_player) { g_player->Close(); g_player.reset(); }
    if (!lpXM) return nullptr;

    auto song = std::make_shared<xm::Song>();
    if (!LoadSong(lpXM, param, flags, wide, *song)) return nullptr;

    xm::PlayerOptions opt;
    opt.noLoop         = (flags & XM_NOLOOP) != 0;
    opt.startSuspended = (flags & XM_SUSPENDED) != 0;
    opt.volume         = static_cast<uint8_t>(g_volume);
#ifdef UFMOD_BENCHMARK
    static_assert(sizeof(unsigned long) == 4, "uFMOD_tsc must be a 32-bit DWORD");
    opt.benchmarkOut = reinterpret_cast<volatile uint32_t*>(&uFMOD_tsc);
#endif

    auto player = std::make_unique<xm::XmPlayer>();
    if (player->Open(song, opt) != xm::PlayerError::Ok) return nullptr;

    g_player = std::move(player);
    return &g_waveOutToken;
}

} // namespace

extern "C" {

void* UFMOD_CALL uFMOD_PlaySong(void* lpXM, void* param, int fdwSong)  { return PlaySongImpl(lpXM, param, fdwSong, false); }
void* UFMOD_CALL uFMOD_PlaySongW(void* lpXM, void* param, int fdwSong) { return PlaySongImpl(lpXM, param, fdwSong, true); }

void UFMOD_CALL uFMOD_Jump2Pattern(unsigned int pat) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_player) return;
    const xm::Song* song = g_player->GetSong();
    if (!song) return;
    if (pat >= song->songLength) pat = 0;   // out of range -> rewind, as documented
    g_player->JumpToOrder(static_cast<uint16_t>(pat));
}

void UFMOD_CALL uFMOD_Pause(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_player) g_player->Pause();
}

void UFMOD_CALL uFMOD_Resume(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_player) g_player->Resume();
}

unsigned int UFMOD_CALL uFMOD_GetStats(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_player ? g_player->GetStats() : 0;
}

unsigned int UFMOD_CALL uFMOD_GetRowOrder(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_player ? g_player->GetRowOrder() : 0;
}

unsigned int UFMOD_CALL uFMOD_GetTime(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_player ? g_player->GetTimeMs() : 0;
}

unsigned char* UFMOD_CALL uFMOD_GetTitle(void) {
    static unsigned char empty[1] = { 0 };
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_player) return empty;
    const xm::Song* song = g_player->GetSong();
    return song ? reinterpret_cast<unsigned char*>(const_cast<char*>(song->name.c_str())) : empty;
}

void UFMOD_CALL uFMOD_SetVolume(unsigned int vol) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (vol > uFMOD_MAX_VOL) vol = uFMOD_MAX_VOL;
    g_volume = vol;                       // remembered for future songs
    if (g_player) g_player->SetVolume(static_cast<uint8_t>(vol));
}

} // extern "C"
