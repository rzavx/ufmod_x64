// xm_format.h
// In-memory representation of a parsed FastTracker II (.XM) module.
// Based on the public, documented XM file format (header, pattern order
// table, packed pattern data, instrument/sample headers, delta-coded
// sample data). This is a clean-room implementation against the file
// format spec, independent of any existing XM player's source code.
//
// Stage 1 of the uFMOD -> native C++/x64 rewrite: file loading only.
// Mixing/playback (Stage 2+) will consume these structures.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace xm {

// ---------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------
// note value: 0 = empty, 1..96 = C-0 .. B-7, 97 = Key Off
constexpr uint8_t kKeyOffNote = 97;

struct Note {
    uint8_t note = 0;
    uint8_t instrument = 0;   // 0 = none, else 1-based index into Song::instruments
    uint8_t volume = 0;       // 0 = no volume-column command, else $10-$50 = set volume etc.
    uint8_t effectType = 0;
    uint8_t effectParam = 0;
};

struct Pattern {
    uint16_t numRows = 0;
    // Row-major: rows[r * numChannels + c]
    std::vector<Note> rows;
};

// ---------------------------------------------------------------------
// Instruments / samples
// ---------------------------------------------------------------------
enum class LoopType : uint8_t {
    None     = 0,
    Forward  = 1,
    PingPong = 2
};

struct EnvelopePoint {
    uint16_t x = 0; // tick position
    uint16_t y = 0; // value, 0..64
};

struct Envelope {
    std::vector<EnvelopePoint> points;
    uint8_t sustainPoint = 0;
    uint8_t loopStartPoint = 0;
    uint8_t loopEndPoint = 0;
    bool enabled  = false;
    bool sustain  = false;
    bool loop     = false;
};

struct Sample {
    std::string name;
    int32_t     length     = 0;  // in samples (frames), not bytes
    int32_t     loopStart  = 0;  // in samples
    int32_t     loopLength = 0;  // in samples
    uint8_t     volume     = 64; // 0..64
    int8_t      finetune   = 0;  // -128..127
    LoopType    loopType   = LoopType::None;
    bool        was16Bit   = false; // true if source data was 16-bit
    uint8_t     panning    = 128;   // 0..255
    int8_t      relativeNote = 0;

    // Decoded PCM, always stored as signed 16-bit mono, regardless of the
    // source bit depth (8-bit samples are promoted by shifting left 8).
    std::vector<int16_t> data;
};

struct Instrument {
    std::string name;
    std::array<uint8_t, 96> sampleForNote{}; // keymap: note-1 -> sample index (0-based)

    Envelope volumeEnvelope;
    Envelope panningEnvelope;

    uint8_t  vibratoType  = 0;
    uint8_t  vibratoSweep = 0;
    uint8_t  vibratoDepth = 0;
    uint8_t  vibratoRate  = 0;
    uint16_t volumeFadeout = 0;

    std::vector<Sample> samples;
};

// ---------------------------------------------------------------------
// Song
// ---------------------------------------------------------------------
struct Song {
    std::string name;
    std::string trackerName;
    uint16_t versionNumber   = 0;    // e.g. 0x0104
    uint16_t songLength      = 0;    // number of entries used in patternOrder
    uint16_t restartPosition = 0;
    uint16_t numChannels     = 0;
    bool     linearFrequencyTable = true; // false = Amiga (log) frequency table
    uint16_t defaultTempo = 6;   // ticks per row
    uint16_t defaultBPM   = 125;

    std::array<uint8_t, 256> patternOrder{};

    std::vector<Pattern>    patterns;
    std::vector<Instrument> instruments;

    // Convenience accessor: note at (order index, row, channel).
    // Returns nullptr if out of range or the referenced pattern is empty.
    const Note* GetNote(uint16_t orderIndex, uint16_t row, uint16_t channel) const {
        if (orderIndex >= songLength) return nullptr;
        uint8_t patIdx = patternOrder[orderIndex];
        if (patIdx >= patterns.size()) return nullptr;
        const Pattern& pat = patterns[patIdx];
        if (row >= pat.numRows || channel >= numChannels) return nullptr;
        size_t idx = static_cast<size_t>(row) * numChannels + channel;
        if (idx >= pat.rows.size()) return nullptr;
        return &pat.rows[idx];
    }
};

} // namespace xm
