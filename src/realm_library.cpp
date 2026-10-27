#include "realm_library.h"
#include "export_internal.h"
#include "installation_internal.h"
#include <shlobj.h>
#include <realm/group.hpp>
#include <realm/list.hpp>
#include <realm/obj.hpp>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

namespace ttp_osu::library {
namespace {
namespace fs=std::filesystem;
constexpr uint64_t MaximumDatabaseBytes=256ull*1024*1024;
constexpr uint64_t MaximumTextBytes=128ull*1024*1024;
constexpr size_t MaximumFieldChars=32768;

[[noreturn]] void Fail(DWORD error) { throw Failure{HRESULT_FROM_WIN32(error)}; }
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE h):value(h) {}
    ~Handle() { if(value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};

std::vector<char> Snapshot(const fs::path& path,const Context& context,uint64_t maximum,
                           DWORD sharing=FILE_SHARE_READ) {
    context.Check();
    Handle file(CreateFileW(path.c_str(),GENERIC_READ,sharing,nullptr,OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(file.value==INVALID_HANDLE_VALUE) Fail(GetLastError());
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file.value,&size)) Fail(GetLastError());
    if(size.QuadPart<0 || uint64_t(size.QuadPart)>maximum) Fail(ERROR_FILE_TOO_LARGE);
    std::vector<char> bytes;
    try { bytes.resize(static_cast<size_t>(size.QuadPart)); }
    catch(const std::bad_alloc&) { Fail(ERROR_NOT_ENOUGH_MEMORY); }
    size_t position=0;
    while(position<bytes.size()) {
        context.Check();
        DWORD count=0, wanted=static_cast<DWORD>(std::min<size_t>(1024*1024,bytes.size()-position));
        if(!ReadFile(file.value,bytes.data()+position,wanted,&count,nullptr)) Fail(GetLastError());
        if(count==0) Fail(ERROR_HANDLE_EOF);
        position+=count;
        context.Progress(TTP_OSU_LIBRARY_READ,position,bytes.size(),path.c_str());
    }
    // The handle has denied write/delete sharing for the entire snapshot.
    // Group is attached only to the resulting private immutable memory below.
    return bytes;
}

std::wstring FromUtf8(std::string_view text) {
    if(text.empty()) return {};
    if(text.size()>MaximumFieldChars*4) Fail(ERROR_INVALID_DATA);
    int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(size<=0 || size>static_cast<int>(MaximumFieldChars)) Fail(ERROR_INVALID_DATA);
    std::wstring result(static_cast<size_t>(size),L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),size)!=size)
        Fail(ERROR_INVALID_DATA);
    if(result.find(L'\0')!=std::wstring::npos) Fail(ERROR_INVALID_DATA);
    return result;
}
std::wstring Text(realm::StringData value) {
    return value.is_null()?std::wstring{}:FromUtf8(std::string_view(value.data(),value.size()));
}
realm::ColKey Column(const realm::Obj& object,const char* name,realm::ColumnType type,bool required=false) {
    const auto key=object.get_table()->get_column_key(name);
    if(!key) { if(required) Fail(ERROR_NOT_SUPPORTED); return {}; }
    if(key.get_type()!=type || key.is_collection()) Fail(ERROR_NOT_SUPPORTED);
    return key;
}
std::wstring String(const realm::Obj& object,const char* name,bool required=false) {
    const auto key=Column(object,name,realm::col_type_String,required);
    return key?Text(object.get<realm::StringData>(key)):std::wstring{};
}
realm::Obj Link(const realm::Obj& object,const char* name,bool required=false) {
    const auto key=Column(object,name,realm::col_type_Link,required);
    if(!key || object.is_null(key)) return {};
    return object.get_linked_object(key);
}
realm::LnkLst Links(const realm::Obj& object,const char* name) {
    const auto key=object.get_table()->get_column_key(name);
    if(!key || key.get_type()!=realm::col_type_Link || !key.is_list()) Fail(ERROR_NOT_SUPPORTED);
    return object.get_linklist(key);
}
int32_t OnlineId(const realm::Obj& object) {
    const auto key=Column(object,"OnlineID",realm::col_type_Int);
    if(!key || object.is_null(key)) return -1;
    const auto value=object.get<int64_t>(key);
    return value>0 && value<=INT32_MAX?static_cast<int32_t>(value):-1;
}
bool Deleted(const realm::Obj& object) {
    const auto key=Column(object,"DeletePending",realm::col_type_Bool);
    return key && !object.is_null(key) && object.get<bool>(key);
}
std::wstring SetIdentity(const realm::Obj& object) {
    const auto key=object.get_table()->get_column_key("ID");
    if(!key || object.is_null(key)) Fail(ERROR_INVALID_DATA);
    const auto value=object.get_any(key);
    if(value.get_type()!=realm::type_UUID) Fail(ERROR_NOT_SUPPORTED);
    return FromUtf8(value.get_uuid().to_string());
}

std::wstring FoldCase(std::wstring_view value) {
    if(value.empty()) return {};
    // XP's CompareStringOrdinal thunk does not consistently honour ignoreCase.
    // Map with the invariant locale once, then compare UTF-16 code units.
    // Do not use locale-sensitive linguistic string comparison for filenames.
    const int size=LCMapStringW(LOCALE_INVARIANT,LCMAP_UPPERCASE,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(size<=0 || size>static_cast<int>(MaximumFieldChars)) Fail(ERROR_INVALID_DATA);
    std::wstring result(static_cast<size_t>(size),L'\0');
    if(LCMapStringW(LOCALE_INVARIANT,LCMAP_UPPERCASE,value.data(),static_cast<int>(value.size()),result.data(),size)!=size)
        Fail(ERROR_INVALID_DATA);
    return result;
}
std::wstring Filename(std::wstring value) {
    std::replace(value.begin(),value.end(),L'/',L'\\');
    return FoldCase(value);
}
bool RegularFile(const fs::path& path,uint64_t* size=nullptr) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&data) ||
       (data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    if(size) *size=(uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;
    return true;
}
fs::path HashPath(const fs::path& files,std::wstring hash) {
    if(hash.size()!=64) Fail(ERROR_INVALID_DATA);
    for(auto& ch:hash) {
        if(ch>=L'A'&&ch<=L'F') ch+=L'a'-L'A';
        if(!((ch>=L'0'&&ch<=L'9')||(ch>=L'a'&&ch<=L'f'))) Fail(ERROR_INVALID_DATA);
    }
    return files/hash.substr(0,1)/hash.substr(0,2)/hash;
}

}

fs::path DiscoverLazerRoot(const Context& context) {
    context.Check();
    return installation::LazerDataRoot(installation::DefaultLazerRoamingRoot(),installation::DefaultExecutable(2));
}

std::vector<ExportSet> PlanLazerExport(const fs::path& input,
    const std::vector<std::wstring>& identities,const Context& context) {
    auto root=input.lexically_normal();
    if(root.filename().empty())root=root.parent_path();
    if(FoldCase(root.filename().wstring())==L"FILES")root=root.parent_path();
    root=fs::absolute(root).lexically_normal();
    const auto files=root/L"files";
    std::map<std::wstring,std::set<std::wstring>> requested;
    for(const auto& value:identities) {
        if(!value.starts_with(L"lazer:"))Fail(ERROR_INVALID_PARAMETER);
        const auto split=value.find(L':',6);
        if(split==value.npos || split==6)Fail(ERROR_INVALID_PARAMETER);
        const auto hash=value.substr(split+1);HashPath(files,hash);
        requested[value.substr(6,split-6)].insert(hash);
    }
    auto snapshot=Snapshot(root/L"client.realm",context,MaximumDatabaseBytes);
    if(snapshot.empty())Fail(ERROR_INVALID_DATA);
    try {
        realm::Group group(realm::BinaryData(snapshot.data(),snapshot.size()),false);
        const auto sets=group.get_table("class_BeatmapSet");
        if(!sets)Fail(ERROR_NOT_SUPPORTED);
        if(sets->size()>MaximumMaps)Fail(ERROR_FILE_TOO_LARGE);
        std::vector<ExportSet> result;uint64_t done{},retained{};
        for(const auto& set:*sets) {
            context.Progress(TTP_OSU_EXPORT_PLAN,done++,sets->size(),L"client.realm");
            if(Deleted(set))continue;
            const auto identity=SetIdentity(set);auto wanted=requested.find(identity);
            if(wanted==requested.end())continue;
            ExportSet output;output.source_root=root;
            const auto maps=Links(set,"Beatmaps");
            for(size_t i=0;i<maps.size();++i) {
                context.Check();const auto map=maps.get_object(i);if(Deleted(map))continue;
                const auto metadata=Link(map,"Metadata",true);if(!metadata)continue;
                auto title=String(metadata,"Title"),artist=String(metadata,"Artist");
                if(title.empty())title=String(metadata,"TitleUnicode");
                if(artist.empty())artist=String(metadata,"ArtistUnicode");
                output.name=artist.empty()?title:artist+L" - "+title;break;
            }
            if(output.name.empty())output.name=identity;
            if(const auto id=OnlineId(set);id>0)output.name=std::to_wstring(id)+L" "+output.name;
            const auto usages=Links(set,"Files");
            if(usages.size()>MaximumMaps)Fail(ERROR_FILE_TOO_LARGE);
            bool has_map{};
            for(size_t i=0;i<usages.size();++i) {
                context.Check();const auto usage=usages.get_object(i);const auto file=Link(usage,"File",true);
                if(!file)Fail(ERROR_INVALID_DATA);
                auto name=ExportMemberName(String(usage,"Filename",true));
                const auto hash=String(file,"Hash",true);const auto path=HashPath(files,hash);
                if(!RegularFile(path))Fail(ERROR_FILE_NOT_FOUND);
                retained+=2ull*(name.size()+path.native().size());
                if(retained>MaximumTextBytes)Fail(ERROR_FILE_TOO_LARGE);
                has_map|=_wcsicmp(fs::path(name).extension().c_str(),L".osu")==0;
                output.files.push_back({path,std::move(name)});
                wanted->second.erase(hash);
            }
            if(!has_map || !wanted->second.empty())Fail(ERROR_FILE_NOT_FOUND);
            result.push_back(std::move(output));requested.erase(wanted);
        }
        if(!requested.empty())Fail(ERROR_NOT_FOUND);
        return result;
    }catch(const realm::Exception&){Fail(ERROR_INVALID_DATA);}
}

std::vector<Track> EnumerateLazer(const fs::path& input,const Context& context) {
    context.Check();
    auto root=input.lexically_normal();
    if(root.filename().empty()) root=root.parent_path();
    if(FoldCase(root.filename().wstring())==L"FILES") root=root.parent_path();
    root=fs::absolute(root).lexically_normal();
    const auto database=root/L"client.realm", files=root/L"files";
    context.Progress(TTP_OSU_LIBRARY_DISCOVER,0,0,database.c_str());
    auto snapshot=Snapshot(database,context,MaximumDatabaseBytes);
    if(snapshot.empty()) Fail(ERROR_INVALID_DATA);
    try {
        // No DB/Realm coordinator is used: both create lock and sidecar files.
        realm::Group group(realm::BinaryData(snapshot.data(),snapshot.size()),false);
        const auto sets=group.get_table("class_BeatmapSet");
        if(!sets) Fail(ERROR_NOT_SUPPORTED);
        if(sets->size()>MaximumMaps) Fail(ERROR_FILE_TOO_LARGE);
        std::vector<Track> tracks;
        uint64_t done=0, maps=0, text_bytes=0;
        for(const auto& set:*sets) {
            context.Check();
            context.Progress(TTP_OSU_LIBRARY_SCAN,done++,sets->size(),database.c_str());
            if(Deleted(set)) continue;
            const auto identity=SetIdentity(set);
            const auto file_list=Links(set,"Files"), beatmaps=Links(set,"Beatmaps");
            if(file_list.size()>MaximumMaps || beatmaps.size()>MaximumMaps-maps) Fail(ERROR_FILE_TOO_LARGE);
            maps+=beatmaps.size();
            std::map<std::wstring,std::wstring> hashes;
            uint64_t map_text_bytes=0;
            for(size_t i=0;i<file_list.size();++i) {
                if((i&255)==0) context.Check();
                const auto usage=file_list.get_object(i);
                const auto file=Link(usage,"File",true);
                if(!file) continue;
                auto filename=Filename(String(usage,"Filename",true));
                auto hash=String(file,"Hash",true);
                map_text_bytes+=2ull*(filename.size()+hash.size());
                if(map_text_bytes>MaximumTextBytes) Fail(ERROR_FILE_TOO_LARGE);
                HashPath(files,hash); // validate before using the hash as a path
                if(!hashes.emplace(std::move(filename),std::move(hash)).second) Fail(ERROR_INVALID_DATA);
            }
            std::map<std::wstring,size_t> emitted;
            for(size_t i=0;i<beatmaps.size();++i) {
                context.Check();
                const auto beatmap=beatmaps.get_object(i);
                if(Deleted(beatmap)) continue;
                const auto metadata=Link(beatmap,"Metadata",true);
                if(!metadata) continue;
                const auto audio_original=String(metadata,"AudioFile",true);
                const auto audio_name=Filename(audio_original);
                const auto audio=hashes.find(audio_name);
                if(audio==hashes.end() || audio_name.empty()) continue;
                Track track;
                track.audio=HashPath(files,audio->second).wstring();
                if(!RegularFile(track.audio,&track.file_size)) continue;
                const auto background=hashes.find(Filename(String(metadata,"BackgroundFile")));
                if(background!=hashes.end()) {
                    auto path=HashPath(files,background->second);
                    if(RegularFile(path)) track.background=path.wstring();
                }
                if(auto existing=emitted.find(audio->second);existing!=emitted.end()) {
                    if(tracks[existing->second].background.empty()) {
                        text_bytes+=2ull*track.background.size();
                        if(text_bytes>MaximumTextBytes) Fail(ERROR_FILE_TOO_LARGE);
                        tracks[existing->second].background=std::move(track.background);
                    }
                    continue;
                }
                const auto map=hashes.find(Filename(String(beatmap,"Path")));
                if(map!=hashes.end()) {
                    auto path=HashPath(files,map->second);
                    if(RegularFile(path)) track.beatmap=path.wstring();
                }
                track.identity=L"lazer:"+identity+L":"+audio->second;
                track.title=String(metadata,"TitleUnicode");
                if(track.title.empty()) track.title=String(metadata,"Title");
                if(track.title.empty()) track.title=fs::path(audio_original).stem().wstring();
                track.artist=String(metadata,"ArtistUnicode");
                if(track.artist.empty()) track.artist=String(metadata,"Artist");
                track.album=String(metadata,"Source");
                if(const auto author=Link(metadata,"Author")) track.creator=String(author,"Username");
                track.beatmap_set_id=OnlineId(set);
                // BeatmapInfo.Length is gameplay length, not full audio duration.
                track.duration_ms=0;
                text_bytes+=2ull*(track.identity.size()+track.audio.size()+track.beatmap.size()+track.background.size()+
                    track.title.size()+track.artist.size()+track.album.size()+track.creator.size());
                if(text_bytes>MaximumTextBytes || tracks.size()>=MaximumTracks) Fail(ERROR_FILE_TOO_LARGE);
                emitted.emplace(audio->second,tracks.size());
                tracks.push_back(std::move(track));
            }
        }
        context.Progress(TTP_OSU_LIBRARY_SCAN,sets->size(),sets->size(),database.c_str());
        return tracks;
    } catch(const Failure&) { throw; }
      catch(const std::bad_alloc&) { Fail(ERROR_NOT_ENOUGH_MEMORY); }
      catch(const realm::Exception& error) {
        // Unsupported/encrypted/corrupt databases must not trigger upgrades.
        (void)error;
        Fail(ERROR_INVALID_DATA);
      }
}
}
