// xm_loader.h
// Loads a FastTracker II (.XM) module from disk or memory into the
// xm::Song structures declared in xm_format.h.

#pragma once

#include "xm_format.h"
#include <string>
#include <cstdint>
#include <cstddef>

namespace xm {

enum class LoadResult {
    Ok = 0,
    FileNotFound,
    IoError,
    InvalidSignature,   // missing "Extended Module: " id text
    Truncated,          // file ends before all declared data was read
    Corrupt             // internal size/count fields are nonsensical
};

const char* ToString(LoadResult r);

class XmLoader {
public:
    // Loads from a file on disk. Path is wide for Unicode filename support.
    static LoadResult LoadFromFile(const std::wstring& path, Song& outSong);

    // Loads from an in-memory image (e.g. embedded resource or network buffer).
    static LoadResult LoadFromMemory(const uint8_t* data, size_t size, Song& outSong);
};

} // namespace xm
