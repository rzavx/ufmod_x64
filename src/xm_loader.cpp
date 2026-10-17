// xm_loader.cpp
// Implementation of the XM file loader. See xm_loader.h / xm_format.h.
//
// Parsing strategy: every variable-size section (instrument header,
// sample header count, pattern header) carries its own "size" field in
// the file. We always read known fields, then seek to
// (sectionStart + declaredSize) before moving on. This is the standard,
// robust way to parse XM files and tolerates files written by trackers
// that pad or extend these headers slightly differently.

#include "xm_loader.h"
#include <fstream>
#include <vector>
#include <cstring>

namespace xm {

// ---------------------------------------------------------------------
// ByteReader: bounds-checked little-endian cursor over an in-memory buffer
// ---------------------------------------------------------------------
namespace {

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    size_t Pos() const { return pos_; }
    size_t Size() const { return size_; }
    bool   Eof()  const { return pos_ >= size_; }

    bool CanRead(size_t n) const { return pos_ + n <= size_; }

    bool Seek(size_t absolutePos) {
        if (absolutePos > size_) return false;
        pos_ = absolutePos;
        return true;
    }

    bool Skip(size_t n) {
        if (!CanRead(n)) return false;
        pos_ += n;
        return true;
    }

    bool ReadU8(uint8_t& out) {
        if (!CanRead(1)) return false;
        out = data_[pos_++];
        return true;
    }

    bool ReadI8(int8_t& out) {
        uint8_t u;
        if (!ReadU8(u)) return false;
        out = static_cast<int8_t>(u);
        return true;
    }

    bool ReadU16(uint16_t& out) {
        if (!CanRead(2)) return false;
        out = static_cast<uint16_t>(data_[pos_]) |
              (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
        pos_ += 2;
        return true;
    }

    bool ReadU32(uint32_t& out) {
        if (!CanRead(4)) return false;
        out = static_cast<uint32_t>(data_[pos_]) |
              (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
              (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
              (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
        pos_ += 4;
        return true;
    }

    // Reads n raw bytes into out (resized to n).
    bool ReadBytes(size_t n, std::vector<uint8_t>& out) {
        if (!CanRead(n)) return false;
        out.assign(data_ + pos_, data_ + pos_ + n);
        pos_ += n;
        return true;
    }

    // Reads n bytes as a trimmed string (trailing 0x00 / 0x20 removed).
    bool ReadFixedString(size_t n, std::string& out) {
        if (!CanRead(n)) return false;
        size_t end = n;
        while (end > 0 && (data_[pos_ + end - 1] == 0x00 || data_[pos_ + end - 1] == 0x20))
            --end;
        out.assign(reinterpret_cast<const char*>(data_ + pos_), end);
        pos_ += n;
        return true;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

// Decodes delta-compressed sample data (XM samples store differences,
// not absolute values) and promotes 8-bit samples to 16-bit by <<8.
bool DecodeSampleData(ByteReader& r, int32_t numSampleFrames, bool is16Bit,
                       std::vector<int16_t>& outPcm) {
    outPcm.resize(numSampleFrames);
    if (is16Bit) {
        int16_t acc = 0;
        for (int32_t i = 0; i < numSampleFrames; ++i) {
            uint16_t raw;
            if (!r.ReadU16(raw)) return false;
            acc = static_cast<int16_t>(acc + static_cast<int16_t>(raw));
            outPcm[i] = acc;
        }
    } else {
        int8_t acc = 0;
        for (int32_t i = 0; i < numSampleFrames; ++i) {
            uint8_t raw;
            if (!r.ReadU8(raw)) return false;
            acc = static_cast<int8_t>(acc + static_cast<int8_t>(raw));
            // multiply instead of "<< 8": left-shifting a negative value is
            // undefined behavior before C++20.
            outPcm[i] = static_cast<int16_t>(static_cast<int>(acc) * 256);
        }
    }
    return true;
}

bool ReadEnvelope(const uint16_t rawPoints[24], uint8_t numPoints,
                   uint8_t sustainPoint, uint8_t loopStart, uint8_t loopEnd,
                   uint8_t typeFlags, Envelope& outEnv) {
    outEnv.points.clear();
    if (numPoints > 12) numPoints = 12;
    for (uint8_t i = 0; i < numPoints; ++i) {
        EnvelopePoint p;
        p.x = rawPoints[i * 2 + 0];
        p.y = rawPoints[i * 2 + 1];
        outEnv.points.push_back(p);
    }
    outEnv.sustainPoint   = sustainPoint;
    outEnv.loopStartPoint = loopStart;
    outEnv.loopEndPoint   = loopEnd;
    outEnv.enabled = (typeFlags & 0x01) != 0;
    outEnv.sustain = (typeFlags & 0x02) != 0;
    outEnv.loop    = (typeFlags & 0x04) != 0;
    return true;
}

LoadResult ParseInstrument(ByteReader& r, Instrument& inst) {
    size_t instStart = r.Pos();

    uint32_t instSize = 0;
    if (!r.ReadU32(instSize)) return LoadResult::Truncated;
    if (!r.ReadFixedString(22, inst.name)) return LoadResult::Truncated;

    uint8_t instType = 0;
    if (!r.ReadU8(instType)) return LoadResult::Truncated; // usually 0, unused
    (void)instType;

    uint16_t numSamples = 0;
    if (!r.ReadU16(numSamples)) return LoadResult::Truncated;

    if (numSamples > 0) {
        uint32_t sampleHeaderSize = 0;
        if (!r.ReadU32(sampleHeaderSize)) return LoadResult::Truncated;

        std::vector<uint8_t> keymap;
        if (!r.ReadBytes(96, keymap)) return LoadResult::Truncated;
        std::memcpy(inst.sampleForNote.data(), keymap.data(), 96);

        uint16_t volEnvRaw[24], panEnvRaw[24];
        for (int i = 0; i < 24; ++i) if (!r.ReadU16(volEnvRaw[i])) return LoadResult::Truncated;
        for (int i = 0; i < 24; ++i) if (!r.ReadU16(panEnvRaw[i])) return LoadResult::Truncated;

        uint8_t numVolPoints, numPanPoints;
        uint8_t volSustain, volLoopStart, volLoopEnd;
        uint8_t panSustain, panLoopStart, panLoopEnd;
        uint8_t volType, panType;
        if (!r.ReadU8(numVolPoints))  return LoadResult::Truncated;
        if (!r.ReadU8(numPanPoints))  return LoadResult::Truncated;
        if (!r.ReadU8(volSustain))    return LoadResult::Truncated;
        if (!r.ReadU8(volLoopStart))  return LoadResult::Truncated;
        if (!r.ReadU8(volLoopEnd))    return LoadResult::Truncated;
        if (!r.ReadU8(panSustain))    return LoadResult::Truncated;
        if (!r.ReadU8(panLoopStart))  return LoadResult::Truncated;
        if (!r.ReadU8(panLoopEnd))    return LoadResult::Truncated;
        if (!r.ReadU8(volType))       return LoadResult::Truncated;
        if (!r.ReadU8(panType))       return LoadResult::Truncated;

        if (!r.ReadU8(inst.vibratoType))  return LoadResult::Truncated;
        if (!r.ReadU8(inst.vibratoSweep)) return LoadResult::Truncated;
        if (!r.ReadU8(inst.vibratoDepth)) return LoadResult::Truncated;
        if (!r.ReadU8(inst.vibratoRate))  return LoadResult::Truncated;
        if (!r.ReadU16(inst.volumeFadeout)) return LoadResult::Truncated;

        // 2 reserved bytes
        if (!r.Skip(2)) return LoadResult::Truncated;

        ReadEnvelope(volEnvRaw, numVolPoints, volSustain, volLoopStart, volLoopEnd, volType, inst.volumeEnvelope);
        ReadEnvelope(panEnvRaw, numPanPoints, panSustain, panLoopStart, panLoopEnd, panType, inst.panningEnvelope);

        // Jump to the declared end of the instrument header (robust against
        // any extra/padding bytes we didn't explicitly account for) before
        // reading the per-sample headers.
        if (!r.Seek(instStart + instSize)) return LoadResult::Truncated;

        // --- Sample headers ---
        struct RawSampleHeader {
            int32_t length = 0, loopStart = 0, loopLength = 0;
            uint8_t volume = 64;
            int8_t  finetune = 0;
            uint8_t type = 0;
            uint8_t panning = 128;
            int8_t  relativeNote = 0;
            std::string name;
        };
        std::vector<RawSampleHeader> headers(numSamples);

        for (uint16_t i = 0; i < numSamples; ++i) {
            RawSampleHeader& h = headers[i];
            uint32_t len = 0, loopStart = 0, loopLen = 0;
            if (!r.ReadU32(len))       return LoadResult::Truncated;
            if (!r.ReadU32(loopStart)) return LoadResult::Truncated;
            if (!r.ReadU32(loopLen))   return LoadResult::Truncated;
            h.length = static_cast<int32_t>(len);
            h.loopStart = static_cast<int32_t>(loopStart);
            h.loopLength = static_cast<int32_t>(loopLen);

            if (!r.ReadU8(h.volume))   return LoadResult::Truncated;
            if (!r.ReadI8(h.finetune)) return LoadResult::Truncated;
            if (!r.ReadU8(h.type))     return LoadResult::Truncated;
            if (!r.ReadU8(h.panning))  return LoadResult::Truncated;
            if (!r.ReadI8(h.relativeNote)) return LoadResult::Truncated;

            uint8_t reserved;
            if (!r.ReadU8(reserved)) return LoadResult::Truncated; // reserved / name-length in some tools
            if (!r.ReadFixedString(22, h.name)) return LoadResult::Truncated;
        }

        // --- Sample data (immediately follows all sample headers, in order) ---
        inst.samples.resize(numSamples);
        for (uint16_t i = 0; i < numSamples; ++i) {
            const RawSampleHeader& h = headers[i];
            Sample& s = inst.samples[i];
            s.name = h.name;
            s.volume = h.volume;
            s.finetune = h.finetune;
            s.panning = h.panning;
            s.relativeNote = h.relativeNote;
            s.was16Bit = (h.type & 0x10) != 0;

            uint8_t loopBits = h.type & 0x03;
            s.loopType = loopBits == 1 ? LoopType::Forward
                       : loopBits == 2 ? LoopType::PingPong
                                       : LoopType::None;

            // h.length/loopStart/loopLength are in BYTES for 16-bit samples
            // in the file; convert to sample frames.
            int32_t frameDivisor = s.was16Bit ? 2 : 1;
            s.length     = h.length     / frameDivisor;
            s.loopStart  = h.loopStart  / frameDivisor;
            s.loopLength = h.loopLength / frameDivisor;

            if (s.length > 0) {
                if (!DecodeSampleData(r, s.length, s.was16Bit, s.data))
                    return LoadResult::Truncated;
            }
        }
    } else {
        // No samples: still respect the declared instrument header size.
        if (!r.Seek(instStart + instSize)) return LoadResult::Truncated;
    }

    return LoadResult::Ok;
}

LoadResult ParsePattern(ByteReader& r, uint16_t numChannels, Pattern& pat) {
    size_t patStart = r.Pos();

    uint32_t patHeaderLen = 0;
    if (!r.ReadU32(patHeaderLen)) return LoadResult::Truncated;

    uint8_t packingType = 0;
    if (!r.ReadU8(packingType)) return LoadResult::Truncated; // always 0

    uint16_t numRows = 0;
    if (!r.ReadU16(numRows)) return LoadResult::Truncated;
    if (numRows == 0) numRows = 1; // some tools write 0 meaning "1 row"

    uint16_t packedSize = 0;
    if (!r.ReadU16(packedSize)) return LoadResult::Truncated;

    // Jump to the declared end of the pattern header before reading
    // packed note data (robust against header-size variations).
    if (!r.Seek(patStart + patHeaderLen)) return LoadResult::Truncated;

    pat.numRows = numRows;
    pat.rows.assign(static_cast<size_t>(numRows) * numChannels, Note{});

    size_t dataEnd = r.Pos() + packedSize;
    if (dataEnd > r.Size()) return LoadResult::Truncated;

    for (uint16_t row = 0; row < numRows; ++row) {
        for (uint16_t ch = 0; ch < numChannels; ++ch) {
            if (r.Pos() >= dataEnd) {
                // No (more) packed data: packedSize == 0 means an empty
                // pattern; otherwise the remaining cells are simply empty.
                continue;
            }
            Note note;
            uint8_t first = 0;
            if (!r.ReadU8(first)) return LoadResult::Truncated;

            if (first & 0x80) {
                // Packed note: bits of 'first' say which fields are present.
                if (first & 0x01) { if (!r.ReadU8(note.note))        return LoadResult::Truncated; }
                if (first & 0x02) { if (!r.ReadU8(note.instrument))  return LoadResult::Truncated; }
                if (first & 0x04) { if (!r.ReadU8(note.volume))      return LoadResult::Truncated; }
                if (first & 0x08) { if (!r.ReadU8(note.effectType))  return LoadResult::Truncated; }
                if (first & 0x10) { if (!r.ReadU8(note.effectParam)) return LoadResult::Truncated; }
            } else {
                // Unpacked note: 'first' IS the note value, 4 more bytes follow.
                note.note = first;
                if (!r.ReadU8(note.instrument))  return LoadResult::Truncated;
                if (!r.ReadU8(note.volume))      return LoadResult::Truncated;
                if (!r.ReadU8(note.effectType))  return LoadResult::Truncated;
                if (!r.ReadU8(note.effectParam)) return LoadResult::Truncated;
            }

            pat.rows[static_cast<size_t>(row) * numChannels + ch] = note;
        }
    }

    // Snap to the declared end regardless of how much we actually consumed,
    // so the caller's cursor lines up with the next pattern.
    r.Seek(dataEnd);
    return LoadResult::Ok;
}

LoadResult ParseAll(ByteReader& r, Song& song) {
    // --- Fixed-layout header ---
    std::string idText;
    if (!r.ReadFixedString(17, idText)) return LoadResult::Truncated;
    if (idText != "Extended Module:") return LoadResult::InvalidSignature;

    if (!r.ReadFixedString(20, song.name)) return LoadResult::Truncated;

    uint8_t marker = 0;
    if (!r.ReadU8(marker)) return LoadResult::Truncated; // should be 0x1A
    if (marker != 0x1A) return LoadResult::InvalidSignature;

    if (!r.ReadFixedString(20, song.trackerName)) return LoadResult::Truncated;
    if (!r.ReadU16(song.versionNumber)) return LoadResult::Truncated;

    // "Header size" counts from the start of its own field (so it includes
    // those 4 bytes): the standard value 276 = 4 + 16 + 256.
    size_t headerStart = r.Pos();
    uint32_t headerSize = 0;
    if (!r.ReadU32(headerSize)) return LoadResult::Truncated;

    if (!r.ReadU16(song.songLength))      return LoadResult::Truncated;
    if (!r.ReadU16(song.restartPosition)) return LoadResult::Truncated;
    if (!r.ReadU16(song.numChannels))     return LoadResult::Truncated;

    uint16_t numPatterns = 0, numInstruments = 0, flags = 0;
    if (!r.ReadU16(numPatterns))    return LoadResult::Truncated;
    if (!r.ReadU16(numInstruments)) return LoadResult::Truncated;
    if (!r.ReadU16(flags))          return LoadResult::Truncated;
    song.linearFrequencyTable = (flags & 0x01) != 0;

    if (!r.ReadU16(song.defaultTempo)) return LoadResult::Truncated;
    if (!r.ReadU16(song.defaultBPM))   return LoadResult::Truncated;

    std::vector<uint8_t> orderTable;
    if (!r.ReadBytes(256, orderTable)) return LoadResult::Truncated;
    std::memcpy(song.patternOrder.data(), orderTable.data(), 256);

    if (song.numChannels == 0 || song.numChannels > 128) return LoadResult::Corrupt;
    if (song.songLength > 256) return LoadResult::Corrupt;

    // Jump to the declared end of the main header before reading patterns -
    // robust against header_size values that differ from what we parsed.
    if (headerSize < 20 || !r.Seek(headerStart + headerSize)) return LoadResult::Truncated;

    // --- Patterns ---
    song.patterns.resize(numPatterns);
    for (uint16_t i = 0; i < numPatterns; ++i) {
        LoadResult pr = ParsePattern(r, song.numChannels, song.patterns[i]);
        if (pr != LoadResult::Ok) return pr;
    }

    // --- Instruments ---
    song.instruments.resize(numInstruments);
    for (uint16_t i = 0; i < numInstruments; ++i) {
        LoadResult ir = ParseInstrument(r, song.instruments[i]);
        if (ir != LoadResult::Ok) return ir;
    }

    return LoadResult::Ok;
}

} // anonymous namespace

const char* ToString(LoadResult r) {
    switch (r) {
        case LoadResult::Ok:                return "Ok";
        case LoadResult::FileNotFound:      return "FileNotFound";
        case LoadResult::IoError:           return "IoError";
        case LoadResult::InvalidSignature:  return "InvalidSignature";
        case LoadResult::Truncated:         return "Truncated";
        case LoadResult::Corrupt:           return "Corrupt";
        default:                            return "Unknown";
    }
}

LoadResult XmLoader::LoadFromMemory(const uint8_t* data, size_t size, Song& outSong) {
    if (!data || size == 0) return LoadResult::Truncated;
    ByteReader r(data, size);
    return ParseAll(r, outSong);
}

LoadResult XmLoader::LoadFromFile(const std::wstring& path, Song& outSong) {
#ifdef _MSC_VER
    // std::ifstream(const std::wstring&) is a Microsoft STL extension.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
#else
    // Non-MSVC fallback (used only for portable test builds): ASCII-only.
    std::string narrow;
    for (wchar_t wc : path) narrow.push_back(wc < 0x80 ? static_cast<char>(wc) : '?');
    std::ifstream file(narrow, std::ios::binary | std::ios::ate);
#endif
    if (!file.is_open()) return LoadResult::FileNotFound;

    std::streamsize fileSize = file.tellg();
    if (fileSize <= 0) return LoadResult::IoError;
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), fileSize))
        return LoadResult::IoError;

    return LoadFromMemory(buffer.data(), buffer.size(), outSong);
}

} // namespace xm
