#pragma once

#include "installation_internal.h"
#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <vector>

namespace ttp_osu::installation {

enum class OsuExecutableKind { none, stable, lazer };

namespace osu_identity {

// Inspect the selected image as data, including 64-bit lazer from the 32-bit
// player. Never load or start the executable. This is feature detection, not
// publisher authentication: version resources are not a digital signature.
class File final {
public:
    explicit File(const wchar_t* path) noexcept
        : handle_(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {
        LARGE_INTEGER size{};
        if (handle_ != INVALID_HANDLE_VALUE && GetFileType(handle_) == FILE_TYPE_DISK &&
            GetFileSizeEx(handle_, &size) && size.QuadPart > 0)
            size_ = static_cast<std::uint64_t>(size.QuadPart);
    }
    ~File() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    std::uint64_t size() const noexcept { return size_; }
    bool contains(std::uint64_t offset, std::uint64_t count) const noexcept {
        return offset <= size_ && count <= size_ - offset;
    }
    bool read(std::uint64_t offset, void* output, DWORD count) const noexcept {
        if (!contains(offset, count)) return false;
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        DWORD copied = 0;
        return SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) &&
               ReadFile(handle_, output, count, &copied, nullptr) && copied == count;
    }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::uint64_t size_ = 0;
};

inline bool IsExecutableImage(const File& file) noexcept {
    IMAGE_DOS_HEADER dos{};
    if (!file.read(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)) || dos.e_lfanew > 16 * 1024 * 1024)
        return false;
    const auto pe_offset = static_cast<std::uint64_t>(dos.e_lfanew);
    DWORD signature = 0;
    IMAGE_FILE_HEADER coff{};
    if (!file.read(pe_offset, &signature, sizeof(signature)) || signature != IMAGE_NT_SIGNATURE ||
        !file.read(pe_offset + sizeof(signature), &coff, sizeof(coff)) ||
        !(coff.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
        (coff.Characteristics & IMAGE_FILE_DLL) || !coff.NumberOfSections ||
        coff.NumberOfSections > 96 || coff.SizeOfOptionalHeader > 4096)
        return false;

    const auto optional_offset = pe_offset + sizeof(signature) + sizeof(coff);
    WORD magic = 0;
    DWORD headers_size = 0, image_size = 0, entry_point = 0;
    IMAGE_DATA_DIRECTORY resources{};
    if (!file.read(optional_offset, &magic, sizeof(magic))) return false;
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && coff.Machine == IMAGE_FILE_MACHINE_I386) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        if (coff.SizeOfOptionalHeader < sizeof(optional) ||
            !file.read(optional_offset, &optional, sizeof(optional)) ||
            optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE)
            return false;
        headers_size = optional.SizeOfHeaders;
        image_size = optional.SizeOfImage;
        entry_point = optional.AddressOfEntryPoint;
        resources = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
               (coff.Machine == IMAGE_FILE_MACHINE_AMD64 || coff.Machine == 0xaa64 /* ARM64 */)) {
        IMAGE_OPTIONAL_HEADER64 optional{};
        if (coff.SizeOfOptionalHeader < sizeof(optional) ||
            !file.read(optional_offset, &optional, sizeof(optional)) ||
            optional.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_RESOURCE)
            return false;
        headers_size = optional.SizeOfHeaders;
        image_size = optional.SizeOfImage;
        entry_point = optional.AddressOfEntryPoint;
        resources = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_RESOURCE];
    } else return false;

    const auto section_offset = optional_offset + coff.SizeOfOptionalHeader;
    const auto section_bytes = coff.NumberOfSections * sizeof(IMAGE_SECTION_HEADER);
    if (!headers_size || headers_size > image_size || !file.contains(0, headers_size) ||
        section_offset + section_bytes > headers_size || !entry_point || entry_point >= image_size ||
        !resources.VirtualAddress || !resources.Size || resources.VirtualAddress >= image_size ||
        resources.Size > image_size - resources.VirtualAddress)
        return false;

    bool resources_backed = false, executable_entry = false;
    for (unsigned i = 0; i < coff.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (!file.read(section_offset + i * sizeof(section), &section, sizeof(section))) return false;
        if (section.SizeOfRawData &&
            (section.PointerToRawData < headers_size ||
             !file.contains(section.PointerToRawData, section.SizeOfRawData))) return false;
        if (section.VirtualAddress >= image_size ||
            section.Misc.VirtualSize > image_size - section.VirtualAddress) return false;
        if (entry_point >= section.VirtualAddress &&
            entry_point - section.VirtualAddress < section.SizeOfRawData &&
            (section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) executable_entry = true;
        if (resources.VirtualAddress >= section.VirtualAddress) {
            const auto delta = resources.VirtualAddress - section.VirtualAddress;
            if (delta < section.SizeOfRawData && resources.Size <= section.SizeOfRawData - delta)
                resources_backed = true;
        }
    }
    return resources_backed && executable_entry;
}

inline std::wstring_view VersionString(const std::vector<BYTE>& data, WORD language,
                                       WORD codepage, const wchar_t* field) noexcept {
    wchar_t query[128]{};
    if (swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", language, codepage, field) < 0)
        return {};
    void* pointer = nullptr;
    UINT characters = 0;
    if (!VerQueryValueW(data.data(), query, &pointer, &characters) || !pointer || !characters)
        return {};
    const auto offset = reinterpret_cast<std::uintptr_t>(pointer) -
                        reinterpret_cast<std::uintptr_t>(data.data());
    if (offset >= data.size() || characters > (data.size() - offset) / sizeof(wchar_t)) return {};
    const auto text = static_cast<const wchar_t*>(pointer);
    size_t length = 0;
    while (length < characters && text[length]) ++length;
    if (length == characters) return {};
    auto result = std::wstring_view(text, length);
    while (!result.empty() && iswspace(result.front())) result.remove_prefix(1);
    while (!result.empty() && iswspace(result.back())) result.remove_suffix(1);
    return result;
}

inline bool Equal(std::wstring_view actual, std::wstring_view expected) noexcept {
    return actual.size() == expected.size() &&
           _wcsnicmp(actual.data(), expected.data(), actual.size()) == 0;
}

inline bool VersionRange(const std::vector<BYTE>& data, const void* pointer, size_t bytes) noexcept {
    if (!pointer) return false;
    const auto offset = reinterpret_cast<std::uintptr_t>(pointer) -
                        reinterpret_cast<std::uintptr_t>(data.data());
    return offset < data.size() && bytes <= data.size() - offset;
}

inline OsuExecutableKind VersionIdentity(const wchar_t* path) {
    DWORD unused = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path, &unused);
    if (!bytes || bytes > 1024 * 1024) return OsuExecutableKind::none;
    std::vector<BYTE> data(bytes);
    if (!GetFileVersionInfoW(path, 0, bytes, data.data())) return OsuExecutableKind::none;
    void* fixed_pointer = nullptr;
    UINT fixed_size = 0;
    if (!VerQueryValueW(data.data(), L"\\", &fixed_pointer, &fixed_size) ||
        fixed_size < sizeof(VS_FIXEDFILEINFO) ||
        !VersionRange(data, fixed_pointer, sizeof(VS_FIXEDFILEINFO))) return OsuExecutableKind::none;
    const auto fixed = static_cast<const VS_FIXEDFILEINFO*>(fixed_pointer);
    if (fixed->dwSignature != VS_FFI_SIGNATURE || fixed->dwFileType != VFT_APP)
        return OsuExecutableKind::none;
    struct Translation { WORD language, codepage; };
    void* translation_pointer = nullptr;
    UINT translation_size = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", &translation_pointer,
                        &translation_size) || !translation_pointer ||
        !translation_size || translation_size % sizeof(Translation) ||
        translation_size > 64 * sizeof(Translation) ||
        !VersionRange(data, translation_pointer, translation_size)) return OsuExecutableKind::none;
    const auto translations = static_cast<const Translation*>(translation_pointer);
    for (size_t i = 0; i < translation_size / sizeof(Translation); ++i) {
        const auto language = translations[i].language, codepage = translations[i].codepage;
        const auto field = [&](const wchar_t* name) { return VersionString(data, language, codepage, name); };
        const auto company = field(L"CompanyName");
        if (!Equal(company, L"ppy") && !Equal(company, L"ppy Pty Ltd")) continue;
        const auto product = field(L"ProductName"), description = field(L"FileDescription");
        const auto original = field(L"OriginalFilename"), internal = field(L"InternalName");
        if (Equal(product, L"osu!") && Equal(description, L"osu!") &&
            Equal(original, L"osu!.exe") && Equal(internal, L"osu!.exe"))
            return OsuExecutableKind::stable;
        if (Equal(product, L"osu!(lazer)") && Equal(description, L"osu!(lazer)") &&
            Equal(original, L"osu!.dll") && Equal(internal, L"osu!.dll"))
            return OsuExecutableKind::lazer;
    }
    return OsuExecutableKind::none;
}

} // namespace osu_identity

inline OsuExecutableKind IdentifyOsuExecutable(std::wstring_view raw_path) noexcept {
    try {
        while (!raw_path.empty() && iswspace(raw_path.front())) raw_path.remove_prefix(1);
        while (!raw_path.empty() && iswspace(raw_path.back())) raw_path.remove_suffix(1);
        if (raw_path.size() >= 2 && raw_path.front() == L'"' && raw_path.back() == L'"')
            raw_path = raw_path.substr(1, raw_path.size() - 2);
        if (raw_path.empty() || raw_path.size() > 32767 || raw_path.find(L'\0') != raw_path.npos)
            return OsuExecutableKind::none;
        const auto path = ExpandPath(raw_path);
        if (path.empty() || path.size() > 32767) return OsuExecutableKind::none;
        const osu_identity::File file(path.c_str());
        if (!osu_identity::IsExecutableImage(file)) return OsuExecutableKind::none;
        // Keep the read-only handle open while querying resources so a renamed
        // or replaced path cannot change the image between the two checks.
        return osu_identity::VersionIdentity(path.c_str());
    } catch (...) {
        return OsuExecutableKind::none;
    }
}

} // namespace ttp_osu::installation
