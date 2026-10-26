#pragma once
#include "library_internal.h"

namespace ttp_osu::library {
std::filesystem::path DiscoverLazerRoot(const Context&);
// Reads an immutable in-memory snapshot through official Realm Core APIs.
// Never opens the game database for writing or creates its lock/sidecar files.
std::vector<Track> EnumerateLazer(const std::filesystem::path&,const Context&);
}
