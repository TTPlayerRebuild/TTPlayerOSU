#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace ttp_osu::installation {
std::wstring ExpandPath(std::wstring_view);
std::wstring NormalizePath(std::wstring_view);
std::filesystem::path DefaultExecutable(unsigned source);
std::filesystem::path DefaultLazerRoamingRoot();
std::filesystem::path LazerDataRoot(const std::filesystem::path& roaming,
                                  const std::filesystem::path& executable);
}
