#pragma once
#include "library_internal.h"
#include <array>
#include <map>

namespace ttp_osu::importer {
using Digest=std::array<unsigned char,32>;
struct Member {
    std::wstring name,md5;
    Digest hash{};
    uint64_t size{};
    bool beatmap{};
};
struct InstalledMember { std::filesystem::path path; size_t member{}; };
std::wstring Hex(const Digest&);
std::wstring Fold(std::wstring_view);
bool VerifyFiles(const std::vector<InstalledMember>&,const std::vector<Member>&,
    const library::Context&);
// Only returns candidates backed by committed, non-deleted database records.
std::vector<InstalledMember> MatchStable(const std::filesystem::path& executable,
    const std::filesystem::path& songs,const std::vector<Member>&,const library::Context&);
std::vector<InstalledMember> MatchLazer(const std::filesystem::path& root,
    const std::vector<Member>&,const library::Context&);
}
