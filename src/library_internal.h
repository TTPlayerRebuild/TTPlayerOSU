#pragma once
#include <ttp_osu/library.h>
#include "parser.h"
#include <filesystem>
#include <string>
#include <vector>

namespace ttp_osu::library {
struct Track {
    std::wstring identity,audio,beatmap,background,title,artist,album,creator;
    uint64_t duration_ms{},file_size{};
    int32_t beatmap_set_id{-1};
};
struct Context {
    const TtpOsuLibraryHost& host;
    void Check() const;
    void Progress(uint32_t phase,uint64_t done,uint64_t total,const wchar_t* current) const;
};
std::filesystem::path DiscoverStableRoot(const Context&);
std::filesystem::path StableSongsDirectory(const std::filesystem::path& installation);
std::vector<Track> EnumerateStable(const std::filesystem::path&,const Context&);
// Shared bounds: at most 1M maps / 250K distinct songs and 512MiB text retained.
inline constexpr size_t MaximumMaps=1000000, MaximumTracks=250000;
}
