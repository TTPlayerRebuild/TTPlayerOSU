#pragma once
#include "library_internal.h"
#include <ttp_osu/export.h>

namespace ttp_osu::library {
struct ExportFile { std::filesystem::path path; std::wstring name; };
struct ExportSet { std::wstring name; std::vector<ExportFile> files; std::filesystem::path source_root; };
std::vector<ExportSet> PlanStableExport(const std::filesystem::path&,
    const std::vector<std::wstring>&,const Context&);
std::vector<ExportSet> PlanLazerExport(const std::filesystem::path&,
    const std::vector<std::wstring>&,const Context&);
// Also checks names which Windows would alias (device names, ADS, trailing
// spaces/dots, case-insensitive collisions), before creating any output.
std::wstring ExportMemberName(std::wstring name);
}
