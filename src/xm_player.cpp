// xm_player.cpp
// XAudio2 output for XmMixer. See xm_player.h.
//
// Design:
//  - kNumBuffers PCM buffers of kFramesPerBuffer stereo frames are kept
//    queued on one source voice.
//  - The voice callback only signals an event (XAudio2 requires callbacks
//    to be short); a worker thread does the actual mixing and resubmits
//    buffers as they complete.
//  - Each buffer remembers the mixer's row/order at its start so that
//    GetRowOrder() can report what is audible, not what is pre-rendered.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xaudio2.h>

#include "xm_player.h"
#include "xm_mixer.h"

#include <array>
#include <chrono>
#include <cmath>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "xaudio2.lib")

namespace xm {

namespace {
    constexpr uint32_t kNumBuffers      = 4;
    constexpr uint32_t kFramesPerBuffer = 2048; // ~43 ms at 48 kHz

    inline uint64_t ReadCycles() {
#ifdef _MSC_VER
        return __rdtsc();
#else
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    }
}

const char* ToString(PlayerError e) {
    switch (e) {
        case PlayerError::Ok:                   return "Ok";
        case PlayerError::AlreadyOpen:          return "AlreadyOpen";
        case PlayerError::BadArgument:          return "BadArgument";
        case PlayerError::ComInitFailed:        return "ComInitFailed";
        case PlayerError::EngineCreateFailed:   return "EngineCreateFailed";
        case PlayerError::MasteringVoiceFailed: return "MasteringVoiceFailed";
        case PlayerError::SourceVoiceFailed:    return "SourceVoiceFailed";
        case PlayerError::StartFailed:          return "StartFailed";
        case PlayerError::ThreadFailed:         return "ThreadFailed";
        default:                                return "Unknown";
    }
}

// ---------------------------------------------------------------------
// Voice callback: only wakes the worker thread.
// ---------------------------------------------------------------------
class BufferEndCallback : public IXAudio2VoiceCallback {
public:
    HANDLE event = nullptr;

    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override { if (event) SetEvent(event); }
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void*) override { if (event) SetEvent(event); }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
};

// ---------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------
struct XmPlayer::Impl {
    struct Slot {
        std::vector<int16_t> pcm;
        uint64_t startFrame = 0; // frames submitted before this buffer
        uint32_t rowOrder   = 0; // mixer position at the buffer's start
        uint32_t frames     = 0; // frames actually contained in this buffer
        uint16_t rmsL = 0, rmsR = 0; // RMS level of this buffer, 0..0x7FFF
        bool     valid      = false;
    };

    // --- song / mixer (mixer_ guarded by mixerMutex_) ---
    std::shared_ptr<const Song> song;
    XmMixer    mixer;
    std::mutex mixerMutex;
    uint32_t   sampleRate = 48000;
    volatile uint32_t* benchmarkOut = nullptr;

    // --- XAudio2 ---
    bool                     comInitialized = false;
    IXAudio2*                engine  = nullptr;
    IXAudio2MasteringVoice*  master  = nullptr;
    IXAudio2SourceVoice*     source  = nullptr;
    BufferEndCallback        callback;

    // --- buffer ring + worker ---
    std::array<Slot, kNumBuffers> slots;
    uint32_t           nextSlot = 0;
    uint64_t           framesSubmitted = 0;
    std::mutex         snapMutex;           // guards Slot::startFrame/rowOrder/valid
    std::thread        worker;
    std::atomic<bool>  quit{false};
    HANDLE             wakeEvent = nullptr;

    // --- state visible to other threads ---
    std::atomic<bool>     opened{false};
    std::atomic<bool>     paused{true};
    std::atomic<bool>     streamDone{false}; // mixer ended, last buffer submitted
    std::atomic<bool>     finished{false};   // ...and it has finished playing
    std::atomic<uint64_t> samplesPlayed{0};

    ~Impl() { Teardown(); }

    // Newest buffer that has started playing. Caller must hold snapMutex.
    const Slot* FindPlayingSlot(uint64_t played, bool fallbackToOldest) const {
        const Slot* best = nullptr;
        const Slot* oldest = nullptr;
        for (const auto& sl : slots) {
            if (!sl.valid) continue;
            if (!oldest || sl.startFrame < oldest->startFrame) oldest = &sl;
            if (sl.startFrame <= played && (!best || sl.startFrame > best->startFrame)) best = &sl;
        }
        return best ? best : (fallbackToOldest ? oldest : nullptr);
    }


    // Renders one buffer and submits it. Returns false when nothing was
    // submitted (song ended or submit failed).
    bool SubmitNext() {
        if (streamDone.load() || !source) return false;

        Slot& s = slots[nextSlot];
        if (s.pcm.size() != static_cast<size_t>(kFramesPerBuffer) * 2)
            s.pcm.assign(static_cast<size_t>(kFramesPerBuffer) * 2, 0);

        uint32_t rowOrder = 0, written = 0;
        bool ended = false;
        uint64_t c0 = 0, c1 = 0;
        {
            std::lock_guard<std::mutex> lock(mixerMutex);
            rowOrder = mixer.GetRowOrder();
            c0 = ReadCycles();
            written  = mixer.RenderInt16Stereo(s.pcm.data(), kFramesPerBuffer);
            c1 = ReadCycles();
            ended    = mixer.IsFinished() || written < kFramesPerBuffer;
        }

        if (benchmarkOut && written > 0) {
            uint64_t framesFor21ms = static_cast<uint64_t>(sampleRate) * 21 / 1000;
            *benchmarkOut = static_cast<uint32_t>((c1 - c0) * framesFor21ms / written);
        }

        // RMS per channel of this buffer (for GetStats / VU meters).
        uint16_t rmsL = 0, rmsR = 0;
        if (written > 0) {
            double sumL = 0.0, sumR = 0.0;
            for (uint32_t i = 0; i < written; ++i) {
                double l = s.pcm[static_cast<size_t>(i) * 2];
                double r = s.pcm[static_cast<size_t>(i) * 2 + 1];
                sumL += l * l;
                sumR += r * r;
            }
            rmsL = static_cast<uint16_t>(std::min(32767.0, std::sqrt(sumL / written)));
            rmsR = static_cast<uint16_t>(std::min(32767.0, std::sqrt(sumR / written)));
        }

        if (written == 0) {
            streamDone.store(true);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(snapMutex);
            s.startFrame = framesSubmitted;
            s.rowOrder   = rowOrder;
            s.frames     = written;
            s.rmsL       = rmsL;
            s.rmsR       = rmsR;
            s.valid      = true;
        }
        framesSubmitted += written;

        XAUDIO2_BUFFER buf = {};
        buf.AudioBytes = written * 2 * sizeof(int16_t);
        buf.pAudioData = reinterpret_cast<const BYTE*>(s.pcm.data());
        buf.Flags      = ended ? XAUDIO2_END_OF_STREAM : 0;

        if (FAILED(source->SubmitSourceBuffer(&buf))) {
            streamDone.store(true);
            return false;
        }

        if (ended) streamDone.store(true);
        nextSlot = (nextSlot + 1) % kNumBuffers;
        return true;
    }

    // Tops the voice's queue up to kNumBuffers.
    void FillQueue() {
        if (!source) return;
        XAUDIO2_VOICE_STATE st = {};
        source->GetState(&st);
        samplesPlayed.store(st.SamplesPlayed);

        uint32_t queued = st.BuffersQueued;
        while (!streamDone.load() && queued < kNumBuffers) {
            if (!SubmitNext()) break;
            ++queued;
        }
        if (streamDone.load() && queued == 0) finished.store(true);
    }

    void WorkerLoop() {
        while (!quit.load()) {
            FillQueue();
            WaitForSingleObject(wakeEvent, 50);
        }
    }

    void Teardown() {
        opened.store(false);
        quit.store(true);
        if (wakeEvent) SetEvent(wakeEvent);
        if (worker.joinable()) worker.join();

        if (source) {
            source->Stop(0);
            source->FlushSourceBuffers();
            source->DestroyVoice();   // blocks until callbacks have finished
            source = nullptr;
        }
        if (master) { master->DestroyVoice(); master = nullptr; }
        if (engine) { engine->Release(); engine = nullptr; }
        callback.event = nullptr;
        if (wakeEvent) { CloseHandle(wakeEvent); wakeEvent = nullptr; }
        if (comInitialized) { CoUninitialize(); comInitialized = false; }

        for (auto& s : slots) { s.valid = false; }
        nextSlot = 0;
        framesSubmitted = 0;
        samplesPlayed.store(0);
        streamDone.store(false);
        finished.store(false);
        paused.store(true);
        song.reset();
    }
};

// ---------------------------------------------------------------------
// XmPlayer
// ---------------------------------------------------------------------
XmPlayer::XmPlayer() : impl_(new Impl) {}
XmPlayer::~XmPlayer() { Close(); }

PlayerError XmPlayer::Open(std::shared_ptr<const Song> song, const PlayerOptions& options) {
    Impl& d = *impl_;
    if (d.opened.load()) return PlayerError::AlreadyOpen;
    if (!song || song->numChannels == 0 || options.sampleRate < 8000 || options.sampleRate > 192000)
        return PlayerError::BadArgument;

    d.quit.store(false);
    d.song = std::move(song);
    d.sampleRate = options.sampleRate;
    d.benchmarkOut = options.benchmarkOut;

    // COM: XAudio2 2.9 doesn't strictly need it, but older runtimes do.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    d.comInitialized = SUCCEEDED(hr);   // S_OK or S_FALSE both need CoUninitialize
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { d.Teardown(); return PlayerError::ComInitFailed; }

    if (FAILED(XAudio2Create(&d.engine, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        d.Teardown(); return PlayerError::EngineCreateFailed;
    }
    if (FAILED(d.engine->CreateMasteringVoice(&d.master))) {
        d.Teardown(); return PlayerError::MasteringVoiceFailed;
    }

    WAVEFORMATEX wfx = {};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = d.sampleRate;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize          = 0;

    d.wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d.wakeEvent) { d.Teardown(); return PlayerError::ThreadFailed; }
    d.callback.event = d.wakeEvent;

    if (FAILED(d.engine->CreateSourceVoice(&d.source, &wfx, 0,
                                           XAUDIO2_DEFAULT_FREQ_RATIO, &d.callback))) {
        d.Teardown(); return PlayerError::SourceVoiceFailed;
    }

    {
        std::lock_guard<std::mutex> lock(d.mixerMutex);
        d.mixer.SetNoLoop(options.noLoop);
        d.mixer.Init(*d.song, d.sampleRate);
        d.mixer.SetMasterVolume(options.volume);
    }

    // Prime the queue before the voice starts so playback begins without
    // an underrun, then hand over to the worker thread.
    d.FillQueue();

    try {
        d.worker = std::thread([&d] { d.WorkerLoop(); });
    } catch (...) {
        d.Teardown(); return PlayerError::ThreadFailed;
    }

    d.opened.store(true);

    if (!options.startSuspended && !Start()) {
        d.Teardown();
        return PlayerError::StartFailed;
    }
    return PlayerError::Ok;
}

void XmPlayer::Close() { impl_->Teardown(); }

bool XmPlayer::IsOpen() const { return impl_->opened.load(); }

bool XmPlayer::Start() {
    Impl& d = *impl_;
    if (!d.opened.load() || !d.source) return false;
    if (FAILED(d.source->Start(0))) return false;
    d.paused.store(false);
    return true;
}

void XmPlayer::Pause() {
    Impl& d = *impl_;
    if (!d.opened.load() || !d.source) return;
    d.source->Stop(0);
    d.paused.store(true);
}

bool XmPlayer::IsPaused() const   { return impl_->paused.load(); }
bool XmPlayer::IsFinished() const { return impl_->finished.load(); }

void XmPlayer::SetVolume(uint8_t vol) {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mixerMutex);
    d.mixer.SetMasterVolume(vol);
}

void XmPlayer::JumpToOrder(uint16_t orderIndex) {
    Impl& d = *impl_;
    if (!d.opened.load()) return;
    std::lock_guard<std::mutex> lock(d.mixerMutex);
    d.mixer.JumpToOrder(orderIndex);
}

uint32_t XmPlayer::GetTimeMs() const {
    const Impl& d = *impl_;
    if (d.sampleRate == 0) return 0;
    return static_cast<uint32_t>((d.samplesPlayed.load() * 1000ull) / d.sampleRate);
}

uint32_t XmPlayer::GetRowOrder() const {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.snapMutex);
    const Impl::Slot* s = d.FindPlayingSlot(d.samplesPlayed.load(), true);
    return s ? s->rowOrder : 0;
}

uint32_t XmPlayer::GetStats() const {
    Impl& d = *impl_;
    if (d.paused.load() || d.finished.load()) return 0;
    std::lock_guard<std::mutex> lock(d.snapMutex);
    uint64_t played = d.samplesPlayed.load();
    const Impl::Slot* s = d.FindPlayingSlot(played, false);
    if (!s || played >= s->startFrame + s->frames) return 0;   // buffer already played out
    return (static_cast<uint32_t>(s->rmsL) << 16) | s->rmsR;
}

const Song* XmPlayer::GetSong() const { return impl_->song.get(); }

} // namespace xm
