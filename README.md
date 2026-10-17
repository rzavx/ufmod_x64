# ufmod_x64

A native **C++17 / x64** player for FastTracker II **`.XM`** modules, with a **drop-in uFMOD-style C API** and **XAudio2** output.

The original uFMOD library ships as 32-bit x86 assembly, so it cannot be linked into x64 programs. `ufmod_x64` is a clean rewrite in plain C++: the XM loader, software mixer and effect engine were written from the public XM file-format description, not derived from uFMOD's source. Only the *API* is kept compatible, so existing call sites keep working.

> **Status:** builds with Visual Studio 2022 (x64). The engine has been checked against libopenmpt on OpenMPT's XM edge-case test files and under ASan/UBSan/TSan, but it is **not a bit-exact FastTracker II clone** — see [Known limitations](#known-limitations).

## Features

- Plays XM files from **disk, memory or a Win32 resource** (`XM_FILE`, `XM_MEMORY`, `XM_RESOURCE`)
- Classic uFMOD API: `uFMOD_PlaySong`, `Jump2Pattern`, `Pause`/`Resume`, `GetStats` (VU levels), `GetRowOrder`, `GetTime`, `GetTitle`, `SetVolume`
- XAudio2 output on a worker thread (4 queued buffers, ~43 ms each); position and VU stats follow what is *audible*, not what is pre-rendered
- Software mixer with linear interpolation, forward and ping-pong loops, equal-power panning and channel-count headroom (no clipping on dense songs)
- Volume and panning **envelopes** (sustain + loop), fadeout, key-off
- Effect engine: arpeggio, portamento (up/down/tone/fine/extra-fine), vibrato, tremolo, volume/panning/global-volume slides, sample offset, position jump, pattern break, pattern loop/delay, note cut/delay, retrigger, multi-retrigger, tremor, set speed/BPM, global volume, with parameter memory
- Bounds-checked loader: truncated or corrupt files fail cleanly instead of crashing
- No third-party dependencies; only the Windows SDK

## Building

Requirements: Visual Studio 2022 with the **Desktop development with C++** workload and a Windows 10/11 SDK.

1. Open `ufmod_x64.sln`.
2. Select **x64** (Debug or Release) and build the solution.
3. Set `ufmod_test` as the startup project, pass the path of an `.xm` file as the command-line argument, and run.

Build outputs go to `bin\x64\<Configuration>\`, including **`ufmod.lib`**.

| Project | Type | Description |
|---|---|---|
| `ufmod` | Static library | The whole engine (`src\*.cpp`) → `ufmod.lib` |
| `ufmod_test` | Console | Player using only the classic uFMOD API, with VU meters |
| `xm_player_test` | Console | Player using the `xm::XmPlayer` class directly |
| `xm_loader_test` | Console | Prints module, instrument and sample info |
| `xm_render_to_wav` | Console | Renders an XM to a WAV file (`xm_render_to_wav song.xm out.wav 30`) |

Controls in `ufmod_test`: `Space` pause/resume, `+`/`-` volume, `N` next order, `R` rewind, `Q` quit.

### Using it in your own project

Either add the `ufmod` project / `ufmod.lib` and put `src\` on your include path, or add the `.cpp` files from `src\` directly. `xaudio2.lib` is linked automatically through a `#pragma comment`. You need **C++17**, **x64**, and **Precompiled Headers turned off** for these files.

```cpp
#include "ufmod.h"

// Play from a file (ANSI path); use uFMOD_PlaySongW for wide paths / resource names
if (!uFMOD_PlaySong((void*)"song.xm", nullptr, XM_FILE)) {
    // invalid XM or no audio device
}

uFMOD_SetVolume(20);                    // 0..25
unsigned ro = uFMOD_GetRowOrder();      // low word = row, high word = order
unsigned vu = uFMOD_GetStats();         // low word = right, high word = left RMS (0..0x7FFF)

uFMOD_PlaySong(nullptr, nullptr, 0);    // stop (call before exiting)
```

Other sources:

```cpp
uFMOD_PlaySong(xmData, (void*)xmSize, XM_MEMORY);                  // from memory
uFMOD_PlaySong((void*)MAKEINTRESOURCEA(IDR_MUSIC), hModule, XM_RESOURCE);  // RT_RCDATA resource
uFMOD_PlaySong((void*)"song.xm", nullptr, XM_FILE | XM_NOLOOP);    // play once
uFMOD_PlaySong((void*)"song.xm", nullptr, XM_FILE | XM_SUSPENDED); // load paused, then uFMOD_Resume()
```

### Using the C++ classes directly

```cpp
#include "xm_loader.h"
#include "xm_player.h"

auto song = std::make_shared<xm::Song>();
if (xm::XmLoader::LoadFromFile(L"song.xm", *song) == xm::LoadResult::Ok) {
    xm::XmPlayer player;
    player.Open(song);                  // starts playing
    // player.Pause(); player.Resume(); player.JumpToOrder(3); ...
}
```

## Differences from the original uFMOD

- `uFMOD_PlaySong` returns a non-NULL **token**, not a usable `HWAVEOUT` (it points to a NULL handle, so stray `waveOut*` calls fail harmlessly).
- `XM_FILE` takes an **ANSI** path; `uFMOD_PlaySongW` takes wide strings. `uFMOD_GetTitle` returns ANSI.
- Volume uses a dB curve of our own: 25 = 0 dB, −2.5 dB per step, 0 = silence.
- `uFMOD_Pause`/`Resume` take effect immediately instead of asynchronously.
- `uFMOD_tsc` (mixer benchmark) is only updated if you build with `UFMOD_BENCHMARK` defined.
- Output is XAudio2, not WINMM; Windows 10+ is required.

## Architecture

```
src/
  xm_format.h      Song / Pattern / Instrument / Sample data structures
  xm_loader.*      XM parser (header, packed patterns, instruments, delta-coded samples)
  xm_mixer.*       Tick-accurate sequencer, effect engine, envelopes, mixing
  xm_player.*      XAudio2 engine + source voice + buffer-refill worker thread
  ufmod_api.cpp    The classic C API on top of XmLoader + XmPlayer (one global mutex)
  ufmod.h          Public header
```

`XmMixer` is independent of Windows, so it can be reused for offline rendering (see `xm_render_to_wav`).

## Testing

- Loaded and rendered **84 XM files from OpenMPT's test suite** with no crashes or sanitizer reports (ASan/UBSan), and compared the output against **libopenmpt**.
- The player and API layers were exercised against a fake XAudio2 under ASan and TSan.
- All sources and tools were cross-compiled against the real Windows/XAudio2 headers with mingw-w64.
- Many simple effect tests match libopenmpt closely (several channels correlate at ~1.000), but OpenMPT's suite is deliberately full of FastTracker II quirks, so overall agreement is partial. The engine has **not** been listened to against a large library of real songs.

## Known limitations

- Instrument **auto-vibrato** and the **`L`** effect (set envelope position) are not implemented.
- The **Amiga frequency table** is not implemented; the linear formula is used for both frequency modes.
- Some FastTracker II quirks differ from the reference: arpeggio at speeds ≥ 16, pattern loop is tracked per mixer instead of per channel, and some note-delay edge cases.
- Old XM versions (below `0x0104`) have not been tested.
- XM only: no MOD, S3M, IT or other tracker formats.

Bug reports are welcome — please include the module (or a link), and the time in the song where it sounds wrong.

## License

This project is licensed under the [MIT License](LICENSE).
