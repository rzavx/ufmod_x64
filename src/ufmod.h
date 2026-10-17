// ufmod.h
// Drop-in replacement header for the uFMOD API, backed by the native
// C++/x64 XM engine (xm_loader / xm_mixer / xm_player + ufmod_api.cpp).
//
// The function names, parameters, flags and semantics follow the original
// ufmod.inc, so existing call sites keep compiling. Differences:
//   * Output goes through XAudio2, not WINMM. The value returned by
//     uFMOD_PlaySong is a non-NULL token on success, NOT a usable HWAVEOUT
//     (it points to a NULL HWAVEOUT, so stray waveOut* calls fail harmlessly).
//   * XM_FILE takes an ANSI (CP_ACP) file name; use uFMOD_PlaySongW for a
//     wide-character file name / resource name.
//   * uFMOD_GetTitle returns an ANSI string.
//   * The volume curve is our own dB table (25 = 0 dB, -2.5 dB per step).
//   * uFMOD_Pause/Resume take effect immediately instead of asynchronously.
//
// Add xm_format.h, xm_loader.*, xm_mixer.*, xm_player.* and ufmod_api.cpp to
// your project (or build them as ufmod.lib and define UFMOD_AUTOLINK).
#pragma once

#ifdef _WIN32
#define UFMOD_CALL __stdcall
#else
#define UFMOD_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---- Flags for fdwSong ----
#define XM_RESOURCE   0   // lpXM = resource name (RT_RCDATA); param = HMODULE (0 = this exe)
#define XM_MEMORY     1   // lpXM = pointer to XM image; param = size in bytes (cast to void*)
#define XM_FILE       2   // lpXM = file name; param ignored
#define XM_NOLOOP     8   // play once instead of looping
#define XM_SUSPENDED  16  // load paused; call uFMOD_Resume() to start

// ---- Volume range ----
#define uFMOD_MIN_VOL      0
#define uFMOD_MAX_VOL      25
#define uFMOD_DEFAULT_VOL  25

// Stops any current song, loads the new one and plays it (unless
// XM_SUSPENDED). lpXM == 0 just stops playback. Returns non-NULL on success,
// NULL on failure (invalid/unreadable XM, no audio device, ...).
// Once it returns it is safe to free a memory image passed with XM_MEMORY.
void* UFMOD_CALL uFMOD_PlaySong(void* lpXM, void* param, int fdwSong);
void* UFMOD_CALL uFMOD_PlaySongW(void* lpXM, void* param, int fdwSong); // wide names

// Jumps to the given zero-based position in the pattern order table.
// Out-of-range values jump to position 0. Already sounding notes keep
// playing until they fade out.
void UFMOD_CALL uFMOD_Jump2Pattern(unsigned int pat);

void UFMOD_CALL uFMOD_Pause(void);
void UFMOD_CALL uFMOD_Resume(void);

// RMS volume of what is currently audible: low word = R, high word = L,
// 0..0x7FFF each. Returns 0 while paused or when nothing plays.
unsigned int UFMOD_CALL uFMOD_GetStats(void);

// low word = row, high word = order (position in the pattern order table).
unsigned int UFMOD_CALL uFMOD_GetRowOrder(void);

// Milliseconds of audio played since the song started (frozen while paused).
unsigned int UFMOD_CALL uFMOD_GetTime(void);

// Current song title (ANSI). Empty string if none / no title. The pointer
// stays valid until the next uFMOD_PlaySong call.
unsigned char* UFMOD_CALL uFMOD_GetTitle(void);

// Global volume, uFMOD_MIN_VOL..uFMOD_MAX_VOL (higher values clamp to max).
// Preserved across uFMOD_PlaySong calls; may be set before playing.
void UFMOD_CALL uFMOD_SetVolume(unsigned int vol);

// Benchmark: CPU cycles used by the mixer thread to produce ~21 ms of audio.
// Only updated if the library is compiled with UFMOD_BENCHMARK defined.
extern unsigned long uFMOD_tsc;

#ifdef __cplusplus
}
#endif

#if defined(_WIN32) && defined(UFMOD_AUTOLINK)
#pragma comment(lib, "ufmod.lib")
#endif
