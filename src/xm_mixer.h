// xm_mixer.h
// Stage 3: adds the XM effect engine on top of the Stage 2 mixer core.
//
// Implemented effect-column commands:
//   0  Arpeggio                      8  Set panning           A  Volume slide
//   1  Porta up                      9  Sample offset         B  Position jump
//   2  Porta down                    C  Set volume            D  Pattern break
//   3  Tone portamento               F  Set speed/BPM         G  Set global volume
//   4  Vibrato                       H  Global volume slide   K  Key off
//   5  Tone porta + vol slide        P  Panning slide         R  Multi retrig note
//   6  Vibrato + vol slide           T  Tremor
//   7  Tremolo
//   E1/E2  Fine porta up/down        E9  Retrigger note       EC  Note cut
//   EA/EB  Fine volume slide up/down E6  Pattern loop         ED  Note delay
//   E4/E7  Vibrato/tremolo waveform  EE  Pattern delay         X1/X2 Extra-fine porta
//
// Effect memory (param 0 = "reuse last nonzero param") is implemented for
// the commands that commonly rely on it (porta, tone porta, vibrato,
// tremolo, volume/panning slides, fine slides).
//
// Also added: volume + panning envelopes (points, sustain, loop), instrument
// fadeout after Key Off, and true forward / ping-pong sample loops.
//
// Still NOT implemented (future stage):
//   - Instrument auto-vibrato (type/sweep/depth/rate).
//   - Set Envelope Position (L effect).
//   - Exact Amiga (non-linear) frequency/period table - linear formula
//     is still used for both frequency-table modes.
//   - Pattern loop (E6) is tracked per-mixer rather than per-channel
//     (FT2 tracks it per-channel); fine for the overwhelming majority of
//     real songs, which only use E6 on one channel at a time.
//   - Multi-retrig (R) volume-change codes are approximated for the
//     common cases (fixed add/subtract); the multiply/divide codes
//     (2/3x, 3/2x, etc.) are simplified to a fixed percentage.
//   - Tremolo/vibrato "random" waveform uses an ordinary PRNG, not the
//     exact table FT2 uses, so it won't be sample-accurate.

#pragma once

#include "xm_format.h"
#include <cstdint>
#include <cstddef>
#include <vector>

namespace xm {

struct MixerChannel {
    const Sample*     sample     = nullptr;
    const Instrument* instrument = nullptr;

    int64_t  position = 0;   // 32.32 fixed-point, in source sample frames
    int64_t  step      = 0;  // 32.32 fixed-point, source frames per output frame

    uint8_t  volume  = 64;   // 0..64, set by note/volume-column/effects
    uint8_t  panning = 128;  // 0..255

    bool     active = false; // still producing audio
    bool     keyOn  = true;  // false after Key Off; drives fadeout below

    // --- pitch state (all in "period" units, FT2-style) ---
    double period      = 0;  // current base period (portamento operates on this)
    double targetPeriod = 0; // tone-portamento destination
    double vibratoOffset = 0; // added on top of 'period' each tick
    int    arpeggioSemis = 0; // extra semitone offset this tick (arpeggio)

    // --- per-tick transient volume/mute (tremolo, tremor) ---
    int   tickVolumeOffset = 0; // added to 'volume' for mixing this tick only
    bool  tremorMuted = false;

    // --- envelopes + key-off fadeout ---
    double   envelopeVolume = 1.0;  // combined volume-envelope * fadeout factor, 0..1
    int32_t  fadeoutVol = 65536;    // 65536 = full; drops by Instrument::volumeFadeout per tick after key off
    uint16_t volEnvPos = 0, panEnvPos = 0; // envelope positions, in ticks
    int      envPan = 32;           // panning-envelope value 0..64 (32 = centre / no effect)
    int      finalPanning = 128;    // panning actually used for mixing, 0..255
    bool     reverse = false;       // ping-pong loop: currently playing backwards

    // --- effect memory / running state ---
    uint8_t portaUpMem = 0, portaDownMem = 0, tonePortaMem = 0;
    uint8_t volSlideMem = 0;
    uint8_t finePortaUpMem = 0, finePortaDownMem = 0;
    uint8_t xFinePortaUpMem = 0, xFinePortaDownMem = 0;
    uint8_t fineVolUpMem = 0, fineVolDownMem = 0;
    uint8_t panSlideMem = 0;
    uint8_t vibSpeedMem = 0, vibDepthMem = 0;
    double  vibPhase = 0;
    uint8_t vibWaveform = 0;
    uint8_t tremSpeedMem = 0, tremDepthMem = 0;
    double  tremPhase = 0;
    uint8_t tremWaveform = 0;
    uint8_t retrigInterval = 0, retrigVolCode = 0;
    uint8_t tremorOnTicks = 0, tremorOffTicks = 0;

    // active effect for this row, re-applied every tick until the next row
    uint8_t activeEffectType = 0, activeEffectParam = 0;

    // note-delay (EDy): the trigger itself is postponed to tick y
    bool  noteDelayPending = false;
    uint8_t noteDelayTick = 0;
    Note  pendingNote{};

    // note-cut (ECy): volume forced to 0 once tick y is reached
    bool  noteCutPending = false;
    uint8_t noteCutTick = 0;
};

class XmMixer {
public:
    void Init(const Song& song, uint32_t sampleRate);
    void SetPosition(uint16_t orderIndex);

    // Jumps to the start of the given order and immediately triggers that
    // row (clears any pending jump/break/loop/delay). Used by the player
    // for "jump to pattern" (Stage 5: uFMOD_Jump2Pattern).
    void JumpToOrder(uint16_t orderIndex);

    uint32_t RenderInt16Stereo(int16_t* out, uint32_t frameCount);

    void SetMasterVolume(uint8_t vol); // 0..25, dB scale (25 = 0 dB, -2.5 dB/step, 0 = mute)
    void SetNoLoop(bool noLoop) { noLoop_ = noLoop; }

    uint32_t GetRowOrder() const;
    uint32_t GetTimeMs() const;
    bool     IsFinished() const { return finished_; }

private:
    // --- sequencing ---
    void TriggerRow(uint16_t row);
    void MoveToNextRow();
    void ApplyContinuousTickEffects(uint16_t tickInRow);
    const Pattern* CurrentPattern() const;

    // --- note / effect processing ---
    void TriggerNote(MixerChannel& ch, const Note& note);
    void InitRowEffect(MixerChannel& ch, const Note& note);
    void ProcessContinuousEffect(MixerChannel& ch, uint16_t tickInRow);
    void RecomputeChannelPitch(MixerChannel& ch);
    void ProcessEnvelopes(MixerChannel& ch);
    void KeyOff(MixerChannel& ch);

    // --- mixing ---
    void MixBlock(int16_t* out, uint32_t frameCount);

    const Song* song_ = nullptr;
    uint32_t sampleRate_ = 48000;
    double   masterGain_ = 1.0;   // user volume (SetMasterVolume)
    double   mixGain_    = 0.35;  // fixed headroom, depends on channel count

    std::vector<MixerChannel> channels_;

    uint16_t orderIndex_ = 0;
    uint16_t row_        = 0;
    uint16_t tick_       = 0;
    uint16_t tempo_      = 6;
    uint16_t bpm_        = 125;

    double samplesPerTick_ = 0.0;
    double tickFrameAccum_ = 0.0;

    uint64_t framesRendered_ = 0;
    bool finished_ = false;
    bool noLoop_   = false;

    // --- song-level (global) effect state ---
    uint8_t  globalVolume_ = 64;        // 0..64, set by Gxx / Hxy
    uint8_t  globalVolSlideMem_ = 0;

    bool     jumpPending_  = false;
    uint16_t jumpTargetOrder_ = 0;
    bool     breakPending_ = false;
    uint16_t breakTargetRow_ = 0;

    bool     patternLoopPending_ = false;
    uint16_t patternLoopRow_     = 0;
    uint8_t  patternLoopCounter_ = 0;

    uint8_t  patternDelayCounter_ = 0; // extra tick-rows to repeat, EEy
};

} // namespace xm
