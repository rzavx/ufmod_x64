// xm_mixer.cpp
// Stage 3: tick-accurate sequencer + effect engine. See xm_mixer.h for
// the list of implemented effects and known simplifications.
//
// Bug fix carried over from Stage 2: the very first row of the song was
// never triggered (the sequencer only triggered a row after a full tick
// cycle had already elapsed in silence). Stage 3's Init() now triggers
// row 0 immediately, before any audio is rendered.

#include "xm_mixer.h"
#include <cmath>
#include <algorithm>
#include <cstdlib>

namespace xm {

namespace {
    constexpr int64_t kFixedOne = int64_t(1) << 32;

    int64_t DoubleToFixed(double v) {
        if (!(v > 0.0)) v = 0.0;          // also catches NaN
        if (v > 1024.0) v = 1024.0;       // sanity cap: avoids 32.32 overflow
        return static_cast<int64_t>(v * static_cast<double>(kFixedOne));
    }

    double PeriodFromNote(int noteIndex, int8_t finetune) {
        return 7680.0 - noteIndex * 64.0 - finetune / 2.0;
    }

    double FrequencyFromPeriod(double period) {
        return 8363.0 * std::pow(2.0, (4608.0 - period) / 768.0);
    }

    // waveform: 0=sine, 1=ramp down, 2=square, 3=random. phase advances by
    // "speed" units per tick; one full cycle = 64 phase units.
    double WaveformValue(uint8_t waveform, double phase) {
        double p = std::fmod(phase, 64.0);
        if (p < 0) p += 64.0;
        switch (waveform & 0x3) {
            case 0: return std::sin(p * (2.0 * 3.14159265358979323846 / 64.0));
            case 1: return 1.0 - (p / 32.0);
            case 2: return (p < 32.0) ? 1.0 : -1.0;
            default: return (std::rand() / static_cast<double>(RAND_MAX)) * 2.0 - 1.0;
        }
    }

    // Value (0..64) of an envelope at tick position 'pos' (linear interpolation).
    int EvalEnvelope(const Envelope& e, uint16_t pos) {
        const auto& p = e.points;
        if (p.empty()) return 64;
        if (pos >= p.back().x) return p.back().y;
        for (size_t i = 0; i + 1 < p.size(); ++i) {
            if (pos >= p[i].x && pos < p[i + 1].x) {
                int dx = p[i + 1].x - p[i].x;
                if (dx <= 0) return p[i].y;
                return p[i].y + (static_cast<int>(p[i + 1].y) - p[i].y) * (pos - p[i].x) / dx;
            }
        }
        return p.front().y;
    }

    // Advances an envelope by one tick, honouring sustain (while the key is
    // held) and loop points.
    void StepEnvelope(const Envelope& e, uint16_t& pos, bool keyOn) {
        const auto& p = e.points;
        if (!e.enabled || p.empty()) return;
        auto xOf = [&](uint8_t idx) { return p[std::min<size_t>(idx, p.size() - 1)].x; };
        if (e.sustain && keyOn && pos == xOf(e.sustainPoint)) return;   // hold
        if (pos < p.back().x) ++pos;
        if (e.loop && pos == xOf(e.loopEndPoint)) pos = xOf(e.loopStartPoint);
    }

    void ApplyTonePorta(MixerChannel& ch) {
        double speed = ch.tonePortaMem * 4.0;
        if (ch.period < ch.targetPeriod) ch.period = std::min(ch.period + speed, ch.targetPeriod);
        else if (ch.period > ch.targetPeriod) ch.period = std::max(ch.period - speed, ch.targetPeriod);
    }

    void ApplyVolSlide(MixerChannel& ch) {
        uint8_t x = ch.volSlideMem >> 4, y = ch.volSlideMem & 0xF;
        int v = ch.volume;
        if (x > 0) v += x; else if (y > 0) v -= y;
        ch.volume = static_cast<uint8_t>(std::clamp(v, 0, 64));
    }
}

void XmMixer::Init(const Song& song, uint32_t sampleRate) {
    song_ = &song;
    sampleRate_ = sampleRate;
    channels_.assign(song.numChannels, MixerChannel{});

    orderIndex_ = 0; row_ = 0; tick_ = 0;
    tempo_ = song.defaultTempo ? song.defaultTempo : 6;
    bpm_   = song.defaultBPM   ? song.defaultBPM   : 125;
    samplesPerTick_ = (sampleRate_ * 2.5) / bpm_;
    tickFrameAccum_ = 0.0;
    framesRendered_ = 0;
    finished_ = false;

    // Headroom: several loud channels summed together would clip, so the
    // per-channel gain shrinks as the channel count grows (>4 channels).
    mixGain_ = 0.35 * std::min(1.0, std::sqrt(4.0 / std::max<double>(1.0, song.numChannels)));

    globalVolume_ = 64;
    globalVolSlideMem_ = 0;
    jumpPending_ = breakPending_ = patternLoopPending_ = false;
    patternLoopRow_ = 0; patternLoopCounter_ = 0;
    patternDelayCounter_ = 0;

    SetPosition(0);
    if (!finished_) {
        TriggerRow(row_);
        ApplyContinuousTickEffects(0);
    }
}

void XmMixer::SetPosition(uint16_t orderIndex) {
    if (!song_ || song_->songLength == 0) { finished_ = true; return; }

    uint16_t idx = orderIndex;
    if (idx >= song_->songLength) {
        if (noLoop_) { finished_ = true; return; }
        idx = (song_->restartPosition < song_->songLength) ? song_->restartPosition : 0;
    }

    for (uint16_t tries = 0; tries < song_->songLength; ++tries) {
        if (idx >= song_->songLength) {
            if (noLoop_) { finished_ = true; return; }
            idx = (song_->restartPosition < song_->songLength) ? song_->restartPosition : 0;
        }
        uint8_t patIdx = song_->patternOrder[idx];
        if (patIdx < song_->patterns.size()) {
            orderIndex_ = idx; row_ = 0; tick_ = 0; finished_ = false;
            return;
        }
        ++idx;
    }
    finished_ = true;
}

void XmMixer::JumpToOrder(uint16_t orderIndex) {
    jumpPending_ = breakPending_ = patternLoopPending_ = false;
    patternDelayCounter_ = 0;
    SetPosition(orderIndex);
    if (!finished_) {
        tickFrameAccum_ = 0.0;
        TriggerRow(row_);
        ApplyContinuousTickEffects(0);
    }
}

const Pattern* XmMixer::CurrentPattern() const {
    if (!song_ || orderIndex_ >= song_->songLength) return nullptr;
    uint8_t patIdx = song_->patternOrder[orderIndex_];
    if (patIdx >= song_->patterns.size()) return nullptr;
    return &song_->patterns[patIdx];
}

void XmMixer::KeyOff(MixerChannel& ch) {
    ch.keyOn = false;
    // FT2: with no volume envelope, key-off cuts the note right away.
    if (ch.instrument && !ch.instrument->volumeEnvelope.enabled) ch.volume = 0;
}

void XmMixer::RecomputeChannelPitch(MixerChannel& ch) {
    if (!ch.sample) return;
    // Keep the stored period inside a playable range so runaway portamento
    // can't push the frequency to absurd (or negative) values.
    ch.period = std::clamp(ch.period, 0.0, 9216.0);
    double effectivePeriod = std::clamp(ch.period + ch.vibratoOffset - ch.arpeggioSemis * 64.0, 0.0, 9216.0);
    double freq = FrequencyFromPeriod(effectivePeriod);
    double framesPerOutputFrame = freq / static_cast<double>(sampleRate_);
    ch.step = DoubleToFixed(framesPerOutputFrame);
}

void XmMixer::TriggerNote(MixerChannel& ch, const Note& note) {
    if (note.instrument != 0 && note.instrument <= song_->instruments.size()) {
        ch.instrument = &song_->instruments[note.instrument - 1];
    }

    if (note.note == kKeyOffNote) {
        KeyOff(ch);
    } else if (note.note >= 1 && note.note <= 96) {
        if (ch.instrument) {
            uint8_t sampIdx = ch.instrument->sampleForNote[note.note - 1];
            if (sampIdx < ch.instrument->samples.size()) {
                const Sample& s = ch.instrument->samples[sampIdx];
                ch.sample      = &s;
                ch.position    = 0;
                ch.active      = !s.data.empty();
                ch.keyOn       = true;
                ch.envelopeVolume = 1.0;
                ch.fadeoutVol  = 65536;
                ch.volEnvPos = ch.panEnvPos = 0;
                ch.reverse     = false;
                ch.volume      = std::min<uint8_t>(s.volume, 64);
                ch.panning     = s.panning;

                int noteIndex = (note.note - 1) + s.relativeNote;
                ch.period = PeriodFromNote(noteIndex, s.finetune);
                ch.targetPeriod = ch.period;
                ch.vibratoOffset = 0;
                ch.vibPhase = 0;
                ch.tremPhase = 0;
            }
        }
    }

    if (note.volume >= 0x10 && note.volume <= 0x50) {
        ch.volume = static_cast<uint8_t>(note.volume - 0x10);
    }

    RecomputeChannelPitch(ch);
}

void XmMixer::InitRowEffect(MixerChannel& ch, const Note& note) {
    ch.activeEffectType  = note.effectType;
    ch.activeEffectParam = note.effectParam;

    switch (note.effectType) {
        case 1: if (note.effectParam) ch.portaUpMem = note.effectParam; break;
        case 2: if (note.effectParam) ch.portaDownMem = note.effectParam; break;
        case 3: if (note.effectParam) ch.tonePortaMem = note.effectParam; break;
        case 5: if (note.effectParam) ch.volSlideMem = note.effectParam; break;
        case 4: {
            uint8_t sp = note.effectParam >> 4, dp = note.effectParam & 0xF;
            if (sp) ch.vibSpeedMem = sp;
            if (dp) ch.vibDepthMem = dp;
            break;
        }
        case 6: if (note.effectParam) ch.volSlideMem = note.effectParam; break;
        case 7: {
            uint8_t sp = note.effectParam >> 4, dp = note.effectParam & 0xF;
            if (sp) ch.tremSpeedMem = sp;
            if (dp) ch.tremDepthMem = dp;
            break;
        }
        case 8: ch.panning = note.effectParam; break;
        case 9:
            if (ch.sample) {
                int64_t off = static_cast<int64_t>(note.effectParam) * 256;
                if (off < ch.sample->length) ch.position = off << 32;
                else ch.active = false;
            }
            break;
        case 10: if (note.effectParam) ch.volSlideMem = note.effectParam; break;
        case 11: jumpPending_ = true; jumpTargetOrder_ = note.effectParam; break;
        case 12: ch.volume = std::min<uint8_t>(note.effectParam, 64); break;
        case 13: breakPending_ = true;
                 breakTargetRow_ = (note.effectParam >> 4) * 10 + (note.effectParam & 0xF);
                 break;
        case 14: { // extended effects
            uint8_t sub = note.effectParam >> 4, y = note.effectParam & 0xF;
            switch (sub) {
                case 0x1: if (y) ch.finePortaUpMem = y; ch.period -= ch.finePortaUpMem * 4.0; break;
                case 0x2: if (y) ch.finePortaDownMem = y; ch.period += ch.finePortaDownMem * 4.0; break;
                case 0x3: break; // glissando control - not implemented
                case 0x4: ch.vibWaveform = y; break;
                case 0x5: break; // set finetune - not implemented
                case 0x6:
                    if (y == 0) {
                        patternLoopRow_ = row_;
                    } else {
                        if (patternLoopCounter_ == 0) patternLoopCounter_ = y;
                        else --patternLoopCounter_;
                        if (patternLoopCounter_ > 0) patternLoopPending_ = true;
                    }
                    break;
                case 0x7: ch.tremWaveform = y; break;
                case 0x9: break; // retrigger timing handled per-tick in ProcessContinuousEffect
                case 0xA: if (y) ch.fineVolUpMem = y;
                          ch.volume = std::min<uint8_t>(ch.volume + ch.fineVolUpMem, 64); break;
                case 0xB: if (y) ch.fineVolDownMem = y;
                          ch.volume = (ch.volume > ch.fineVolDownMem) ? ch.volume - ch.fineVolDownMem : 0; break;
                case 0xC: if (y == 0) ch.volume = 0;
                          else { ch.noteCutPending = true; ch.noteCutTick = y; }
                          break;
                case 0xD: break; // note delay already special-cased before InitRowEffect runs
                case 0xE: patternDelayCounter_ = y; break;
                default: break;
            }
            break;
        }
        case 15:
            if (note.effectParam > 0 && note.effectParam < 0x20) tempo_ = note.effectParam;
            else if (note.effectParam >= 0x20) { bpm_ = note.effectParam; samplesPerTick_ = (sampleRate_ * 2.5) / bpm_; }
            break;
        case 16: globalVolume_ = std::min<uint8_t>(note.effectParam, 64); break;
        case 17: if (note.effectParam) globalVolSlideMem_ = note.effectParam; break;
        case 20: KeyOff(ch); break;
        case 21: break; // set envelope position - needs the (not yet built) envelope engine
        case 25: if (note.effectParam) ch.panSlideMem = note.effectParam; break;
        case 27: {
            uint8_t x = note.effectParam >> 4, y = note.effectParam & 0xF;
            if (y) ch.retrigInterval = y;
            ch.retrigVolCode = x;
            break;
        }
        case 29:
            if (note.effectParam) {
                ch.tremorOnTicks  = (note.effectParam >> 4) + 1;
                ch.tremorOffTicks = (note.effectParam & 0xF) + 1;
            }
            break;
        case 33: { // X1/X2 extra-fine porta
            uint8_t sub = note.effectParam >> 4, y = note.effectParam & 0xF;
            if (sub == 1) { if (y) ch.xFinePortaUpMem = y; ch.period -= ch.xFinePortaUpMem; }
            else if (sub == 2) { if (y) ch.xFinePortaDownMem = y; ch.period += ch.xFinePortaDownMem; }
            break;
        }
        default: break;
    }

    RecomputeChannelPitch(ch);
}

void XmMixer::ProcessContinuousEffect(MixerChannel& ch, uint16_t tick) {
    switch (ch.activeEffectType) {
        case 0: { // arpeggio
            uint8_t x = ch.activeEffectParam >> 4, y = ch.activeEffectParam & 0xF;
            // FastTracker II counts its row timer DOWN, so its arpeggio walks
            // root -> y -> x (not ProTracker's root -> x -> y) and its phase
            // depends on the row speed.
            // (tick 0 of a row always plays the plain note in FT2.)
            int idx = (tick == 0) ? 0 : ((static_cast<int>(tempo_) - static_cast<int>(tick)) % 3 + 3) % 3;
            ch.arpeggioSemis = (idx == 1) ? x : (idx == 2 ? y : 0);
            break;
        }
        case 1: if (tick > 0) ch.period -= ch.portaUpMem * 4.0; break;
        case 2: if (tick > 0) ch.period += ch.portaDownMem * 4.0; break;
        case 3: if (tick > 0) ApplyTonePorta(ch); break;
        case 5: if (tick > 0) { ApplyTonePorta(ch); ApplyVolSlide(ch); } break;
        case 4:
            if (tick > 0) {
                ch.vibratoOffset = WaveformValue(ch.vibWaveform, ch.vibPhase) * ch.vibDepthMem * (255.0 / 32.0);
                ch.vibPhase += ch.vibSpeedMem;
            }
            break;
        case 6:
            if (tick > 0) {
                ch.vibratoOffset = WaveformValue(ch.vibWaveform, ch.vibPhase) * ch.vibDepthMem * (255.0 / 32.0);
                ch.vibPhase += ch.vibSpeedMem;
                ApplyVolSlide(ch);
            }
            break;
        case 7:
            if (tick > 0) {
                ch.tickVolumeOffset = static_cast<int>(WaveformValue(ch.tremWaveform, ch.tremPhase) * ch.tremDepthMem * (255.0 / 64.0));
                ch.tremPhase += ch.tremSpeedMem;
            }
            break;
        case 10: if (tick > 0) ApplyVolSlide(ch); break;
        case 14: { // E9y retrigger (part of extended effects)
            uint8_t sub = ch.activeEffectParam >> 4, y = ch.activeEffectParam & 0xF;
            if (sub == 9 && y > 0 && tick > 0 && (tick % y) == 0) ch.position = 0;
            break;
        }
        case 17:
            if (tick > 0) {
                uint8_t x = globalVolSlideMem_ >> 4, y = globalVolSlideMem_ & 0xF;
                int gv = globalVolume_;
                if (x > 0) gv += x; else if (y > 0) gv -= y;
                globalVolume_ = static_cast<uint8_t>(std::clamp(gv, 0, 64));
            }
            break;
        case 25:
            if (tick > 0) {
                uint8_t x = ch.panSlideMem >> 4, y = ch.panSlideMem & 0xF;
                int p = ch.panning;
                if (x > 0) p += x; else if (y > 0) p -= y;
                ch.panning = static_cast<uint8_t>(std::clamp(p, 0, 255));
            }
            break;
        case 27:
            if (ch.retrigInterval > 0 && tick > 0 && (tick % ch.retrigInterval) == 0) {
                ch.position = 0;
                int v = ch.volume;
                switch (ch.retrigVolCode) {
                    case 0x0: break;
                    case 0x1: v -= 1; break;  case 0x2: v -= 2; break;
                    case 0x3: v -= 4; break;  case 0x4: v -= 8; break;
                    case 0x5: v -= 16; break;
                    case 0x6: v = v * 2 / 3; break; case 0x7: v = v / 2; break;
                    case 0x8: break;
                    case 0x9: v += 1; break;  case 0xA: v += 2; break;
                    case 0xB: v += 4; break;  case 0xC: v += 8; break;
                    case 0xD: v += 16; break;
                    case 0xE: v = v * 3 / 2; break; case 0xF: v = v * 2; break;
                }
                ch.volume = static_cast<uint8_t>(std::clamp(v, 0, 64));
            }
            break;
        case 29:
            if (ch.tremorOnTicks + ch.tremorOffTicks > 0) {
                uint16_t cycle = tick % static_cast<uint16_t>(ch.tremorOnTicks + ch.tremorOffTicks);
                ch.tremorMuted = cycle >= ch.tremorOnTicks;
            }
            break;
        default: break;
    }
}

void XmMixer::TriggerRow(uint16_t row) {
    const Pattern* pat = CurrentPattern();
    uint16_t patRows = pat ? pat->numRows : 1;
    if (row >= patRows) row = 0;
    row_ = row;
    tick_ = 0;

    if (!pat) return;

    for (uint16_t ch = 0; ch < song_->numChannels; ++ch) {
        size_t idx = static_cast<size_t>(row_) * song_->numChannels + ch;
        Note note = (idx < pat->rows.size()) ? pat->rows[idx] : Note{};
        MixerChannel& channel = channels_[ch];

        bool isNoteDelay = (note.effectType == 14) && ((note.effectParam >> 4) == 0xD) && ((note.effectParam & 0xF) > 0);
        if (isNoteDelay) {
            channel.pendingNote      = note;
            channel.noteDelayPending = true;
            channel.noteDelayTick    = note.effectParam & 0xF;
            channel.activeEffectType = 0; // no continuous effect for a delayed-note cell
            channel.activeEffectParam = 0;
            continue;
        }

        bool isTonePorta = (note.effectType == 3 || note.effectType == 5);
        if (isTonePorta && note.note >= 1 && note.note <= 96) {
            const Instrument* inst = (note.instrument != 0 && note.instrument <= song_->instruments.size())
                                          ? &song_->instruments[note.instrument - 1]
                                          : channel.instrument;
            if (inst) {
                uint8_t sampIdx = inst->sampleForNote[note.note - 1];
                if (sampIdx < inst->samples.size()) {
                    const Sample& s = inst->samples[sampIdx];
                    int noteIndex = (note.note - 1) + s.relativeNote;
                    channel.targetPeriod = PeriodFromNote(noteIndex, s.finetune);
                }
            }
            if (note.volume >= 0x10 && note.volume <= 0x50) {
                channel.volume = static_cast<uint8_t>(note.volume - 0x10);
            }
        } else {
            TriggerNote(channel, note);
        }

        InitRowEffect(channel, note);
    }
}

void XmMixer::MoveToNextRow() {
    if (patternDelayCounter_ > 0) { --patternDelayCounter_; return; }

    if (patternLoopPending_) {
        patternLoopPending_ = false;
        TriggerRow(patternLoopRow_);
        return;
    }

    if (jumpPending_ || breakPending_) {
        uint16_t targetOrder = jumpPending_ ? jumpTargetOrder_ : static_cast<uint16_t>(orderIndex_ + 1);
        uint16_t targetRow   = breakPending_ ? breakTargetRow_ : 0;
        jumpPending_ = false; breakPending_ = false;
        SetPosition(targetOrder);
        if (finished_) return;
        TriggerRow(targetRow);
        return;
    }

    const Pattern* pat = CurrentPattern();
    uint16_t patRows = pat ? pat->numRows : 1;
    if (static_cast<uint16_t>(row_ + 1) >= patRows) {
        SetPosition(static_cast<uint16_t>(orderIndex_ + 1));
        if (finished_) return;
        TriggerRow(0);
    } else {
        TriggerRow(static_cast<uint16_t>(row_ + 1));
    }
}

void XmMixer::ProcessEnvelopes(MixerChannel& ch) {
    int envVol = 64;
    ch.envPan = 32;
    if (ch.instrument) {
        const Instrument& inst = *ch.instrument;
        if (inst.volumeEnvelope.enabled) {
            envVol = EvalEnvelope(inst.volumeEnvelope, ch.volEnvPos);
            StepEnvelope(inst.volumeEnvelope, ch.volEnvPos, ch.keyOn);
        }
        if (inst.panningEnvelope.enabled) {
            ch.envPan = EvalEnvelope(inst.panningEnvelope, ch.panEnvPos);
            StepEnvelope(inst.panningEnvelope, ch.panEnvPos, ch.keyOn);
        }
        if (!ch.keyOn) {
            ch.fadeoutVol -= inst.volumeFadeout;
            if (ch.fadeoutVol <= 0) { ch.fadeoutVol = 0; ch.active = false; }
        }
    }
    ch.envelopeVolume = (envVol / 64.0) * (ch.fadeoutVol / 65536.0);

    // FastTracker II panning-envelope law: the envelope pushes the pan
    // towards the nearer edge less the closer the pan already is to it.
    int pan = ch.panning + ((ch.envPan - 32) * (128 - std::abs(ch.panning - 128))) / 32;
    ch.finalPanning = std::clamp(pan, 0, 255);
}

void XmMixer::ApplyContinuousTickEffects(uint16_t tickInRow) {
    for (auto& ch : channels_) {
        ch.tickVolumeOffset = 0;
        ch.tremorMuted = false;
        ch.arpeggioSemis = 0;

        ProcessContinuousEffect(ch, tickInRow);

        ProcessEnvelopes(ch);

        if (ch.noteCutPending && tickInRow == ch.noteCutTick) {
            ch.volume = 0;
            ch.noteCutPending = false;
        }

        if (ch.noteDelayPending && tickInRow == ch.noteDelayTick) {
            TriggerNote(ch, ch.pendingNote);
            InitRowEffect(ch, ch.pendingNote); // in case the delayed cell also set panning/volume/etc.
            ch.noteDelayPending = false;
        }

        RecomputeChannelPitch(ch);
    }
}

uint32_t XmMixer::RenderInt16Stereo(int16_t* out, uint32_t frameCount) {
    if (!song_) return 0;

    uint32_t written = 0;
    while (written < frameCount) {
        if (finished_) break;

        double framesLeftInTick = samplesPerTick_ - tickFrameAccum_;
        uint32_t chunk = static_cast<uint32_t>(std::ceil(framesLeftInTick));
        chunk = std::max<uint32_t>(chunk, 1);
        chunk = std::min<uint32_t>(chunk, frameCount - written);

        MixBlock(out + static_cast<size_t>(written) * 2, chunk);
        written += chunk;
        framesRendered_ += chunk;
        tickFrameAccum_ += chunk;

        if (tickFrameAccum_ >= samplesPerTick_ - 0.0001) {
            tickFrameAccum_ -= samplesPerTick_;
            ++tick_;
            if (tick_ >= tempo_) {
                tick_ = 0;
                MoveToNextRow();
                if (!finished_) ApplyContinuousTickEffects(0);
            } else {
                ApplyContinuousTickEffects(tick_);
            }
        }
    }

    for (uint32_t i = written; i < frameCount; ++i) {
        out[i * 2 + 0] = 0;
        out[i * 2 + 1] = 0;
    }
    return written;
}

void XmMixer::MixBlock(int16_t* out, uint32_t frameCount) {
    for (uint32_t i = 0; i < frameCount; ++i) {
        int32_t accL = 0, accR = 0;

        for (auto& ch : channels_) {
            if (!ch.active || !ch.sample || ch.sample->data.empty()) continue;
            const Sample& s = *ch.sample;

            int64_t framePos = ch.position >> 32;
            if (framePos < 0 || framePos >= s.length) { ch.active = false; continue; }

            uint32_t frac = static_cast<uint32_t>(ch.position & 0xFFFFFFFFu);
            int16_t s0 = s.data[static_cast<size_t>(framePos)];
            // Neighbour for interpolation: wraps to the loop start at the loop end.
            const bool loopOn = s.loopType == LoopType::Forward && s.loopLength > 0;
            const int64_t endFrames = loopOn ? std::min<int64_t>(s.loopStart + s.loopLength, s.length) : s.length;
            int16_t s1 = (framePos + 1 < endFrames)
                             ? s.data[static_cast<size_t>(framePos + 1)]
                             : (loopOn ? s.data[static_cast<size_t>(std::min<int64_t>(s.loopStart, s.length - 1))] : s0);

            double t = frac / static_cast<double>(kFixedOne);
            double sample = s0 + (s1 - s0) * t;

            int effVolume = std::clamp(static_cast<int>(ch.volume) + ch.tickVolumeOffset, 0, 64);
            if (ch.tremorMuted) effVolume = 0;

            double vol = (effVolume / 64.0) * ch.envelopeVolume * (globalVolume_ / 64.0) * masterGain_ * mixGain_;
            // Equal-power pan law (like FastTracker II): constant loudness
            // across the stereo field, centre = 0.707 per side.
            double panPos = ch.finalPanning / 255.0;
            double panR = std::sqrt(panPos);
            double panL = std::sqrt(1.0 - panPos);

            accL += static_cast<int32_t>(sample * vol * panL);
            accR += static_cast<int32_t>(sample * vol * panR);

            // Advance, then handle loop / end of sample.
            const bool looping = s.loopType != LoopType::None && s.loopLength > 0;
            const int64_t loopStartF = static_cast<int64_t>(std::min(s.loopStart, s.length)) << 32;
            const int64_t loopEndF   = static_cast<int64_t>(std::min(s.loopStart + s.loopLength, s.length)) << 32;
            const int64_t lenF       = static_cast<int64_t>(s.length) << 32;

            if (!ch.reverse) ch.position += ch.step; else ch.position -= ch.step;

            if (looping && loopEndF > loopStartF) {
                if (s.loopType == LoopType::PingPong) {
                    for (int guard = 0; guard < 8; ++guard) {   // bounce (handles very high pitches)
                        if (!ch.reverse && ch.position >= loopEndF) {
                            ch.position = 2 * loopEndF - ch.position - 1;
                            ch.reverse = true;
                        } else if (ch.reverse && ch.position < loopStartF) {
                            ch.position = 2 * loopStartF - ch.position;
                            ch.reverse = false;
                        } else break;
                    }
                    ch.position = std::clamp<int64_t>(ch.position, loopStartF, loopEndF - 1);
                } else if (ch.position >= loopEndF) {
                    ch.position = loopStartF + ((ch.position - loopEndF) % (loopEndF - loopStartF));
                }
            } else if (ch.position >= lenF) {
                ch.active = false;
            }
        }

        out[i * 2 + 0] = static_cast<int16_t>(std::clamp(accL, -32768, 32767));
        out[i * 2 + 1] = static_cast<int16_t>(std::clamp(accR, -32768, 32767));
    }
}

void XmMixer::SetMasterVolume(uint8_t vol) {
    if (vol > 25) vol = 25;
    // Logarithmic (dB) scale like uFMOD's: 25 = 0 dB, each step below is
    // -2.5 dB (so 1 = -60 dB), 0 = silence. The exact table of the original
    // library isn't documented, so this curve is our own choice.
    masterGain_ = (vol == 0) ? 0.0 : std::pow(10.0, -(25 - vol) * 2.5 / 20.0);
}

uint32_t XmMixer::GetRowOrder() const {
    return (static_cast<uint32_t>(orderIndex_) << 16) | row_;
}

uint32_t XmMixer::GetTimeMs() const {
    if (sampleRate_ == 0) return 0;
    return static_cast<uint32_t>((framesRendered_ * 1000ull) / sampleRate_);
}

} // namespace xm
