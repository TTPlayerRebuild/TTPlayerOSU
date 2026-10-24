#pragma once
#include <ttp_osu/abi.h>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace ttp_osu {
struct Failure { HRESULT result; };
struct References { std::wstring audio, background; };
struct Media { std::wstring beatmap, audio, background; };
void CheckCanceled(const TtpOsuHost&);
std::span<const unsigned char> ReadMember(const TtpOsuHost&, const wchar_t*, const std::wstring&);
std::optional<References> Parse(std::span<const unsigned char>);
std::vector<Media> Resolve(const TtpOsuHost&, const wchar_t*, std::span<const std::wstring>, std::wstring_view);
}
