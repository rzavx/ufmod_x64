// xm_player.h
// Stage 4: real-time audio output for the XM mixer using XAudio2.
//
// XmPlayer owns an XmMixer, an XAudio2 engine + source voice and a worker
// thread that keeps a small ring of PCM buffers filled. All Windows/XAudio2
// types are hidden behind a pimpl, so this header is plain C++.
//
// Threading: every public method may be called from any thread (typically
// your UI/main thread). Mixing runs on the worker thread.
//
// Position reporting (GetTimeMs / GetRowOrder) is derived from the voice's
// "samples played" counter, so it follows what you actually HEAR rather than
// what the mixer has already rendered ahead of time (accurate to one
// buffer, ~43 ms).
//
// Build requirements (VC++ 2022, x64): Windows 10 SDK, links xaudio2.lib
// (done by #pragma comment in xm_player.cpp), C++17.

#pragma once

#include "xm_format.h"
#include <cstdint>
#include <memory>

namespace xm {

enum class PlayerError {
    Ok = 0,
    AlreadyOpen,
    BadArgument,
    ComInitFailed,
    EngineCreateFailed,     // XAudio2Create failed (no audio service?)
    MasteringVoiceFailed,   // no usable output device
    SourceVoiceFailed,
    StartFailed,
    ThreadFailed
};

const char* ToString(PlayerError e);

struct PlayerOptions {
    uint32_t sampleRate     = 48000;
    bool     noLoop         = false; // stop at the end instead of restarting
    bool     startSuspended = false; // open paused; call Start() to begin
    uint8_t  volume         = 25;    // 0..25
    // Optional: if set, the worker thread stores here the CPU cycles it takes
    // to mix ~21 ms of audio (the uFMOD_tsc benchmark counter).
    volatile uint32_t* benchmarkOut = nullptr;
};

class XmPlayer {
public:
    XmPlayer();
    ~XmPlayer();
    XmPlayer(const XmPlayer&) = delete;
    XmPlayer& operator=(const XmPlayer&) = delete;

    // Opens the audio device and prepares playback of 'song'. Playback
    // begins immediately unless options.startSuspended is set. The song is
    // shared so it stays alive as long as the player needs it.
    PlayerError Open(std::shared_ptr<const Song> song,
                     const PlayerOptions& options = PlayerOptions());

    // Stops playback and releases the device. Safe to call repeatedly.
    void Close();

    bool IsOpen() const;

    bool Start();          // begin / resume output
    bool Resume() { return Start(); }
    void Pause();          // freeze output (queued audio is kept)
    bool IsPaused() const;

    // True once a non-looping song has fully played out.
    bool IsFinished() const;

    void SetVolume(uint8_t vol);            // 0..25
    void JumpToOrder(uint16_t orderIndex);  // takes effect within ~170 ms

    uint32_t GetTimeMs() const;     // time actually played, in ms
    uint32_t GetRowOrder() const;   // low word = row, high word = order
    uint32_t GetStats() const;      // RMS of what is audible: low word = R, high word = L, 0..0x7FFF
    const Song* GetSong() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace xm
