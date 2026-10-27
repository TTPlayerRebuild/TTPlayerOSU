#include "library_internal.h"
#include "export_internal.h"
#include "installation_internal.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <set>
#include <string_view>

namespace ttp_osu::library {
namespace {
constexpr size_t MaximumTextBytes=16U*1024*1024;
constexpr uint64_t MaximumRetainedText=512ULL*1024*1024;
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit Handle(HANDLE v):value(v){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
struct FindHandle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~FindHandle(){if(value!=INVALID_HANDLE_VALUE)FindClose(value);}
};
struct Insensitive {
    bool operator()(const std::wstring& a,const std::wstring& b) const {return _wcsicmp(a.c_str(),b.c_str())<0;}
};
std::wstring_view Trim(std::wstring_view text) {
    const size_t first=text.find_first_not_of(L" \r\n\t");
    return first==text.npos?std::wstring_view{}:text.substr(first,text.find_last_not_of(L" \r\n\t")-first+1);
}
std::wstring_view Unquote(std::wstring_view text) {
    text=Trim(text);
    return text.size()>=2&&text.front()==L'"'&&text.back()==L'"'?text.substr(1,text.size()-2):text;
}
std::filesystem::path Absolute(const std::filesystem::path& path) {
    if(path.empty())throw Failure{HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)};
    const DWORD count=GetFullPathNameW(path.c_str(),0,nullptr,nullptr);
    if(!count || count>=32768)throw Failure{HRESULT_FROM_WIN32(count?ERROR_FILENAME_EXCED_RANGE:GetLastError())};
    std::wstring full(count,L'\0');
    const DWORD written=GetFullPathNameW(path.c_str(),count,full.data(),nullptr);
    if(!written || written>=count)throw Failure{HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME)};
    full.resize(written);return std::filesystem::path(full).lexically_normal();
}
// Extended paths keep Unicode names and paths longer than MAX_PATH usable on
// XP as well. Public records expose normal absolute paths to the player.
std::wstring Native(const std::filesystem::path& path) {
    const auto& value=path.native();
    if(value.starts_with(L"\\\\?\\"))return value;
    if(value.starts_with(L"\\\\"))return L"\\\\?\\UNC\\"+value.substr(2);
    return L"\\\\?\\"+value;
}
DWORD Attributes(const std::filesystem::path& path) {return GetFileAttributesW(Native(path).c_str());}
bool Directory(const std::filesystem::path& path) {
    const DWORD attr=Attributes(path);return attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_DIRECTORY)!=0;
}
std::optional<std::wstring> Environment(const wchar_t* key) {
    const DWORD count=GetEnvironmentVariableW(key,nullptr,0);
    if(!count || count>=32768)return {};
    std::wstring value(count,L'\0');const DWORD copied=GetEnvironmentVariableW(key,value.data(),count);
    if(!copied || copied>=count)return {};value.resize(copied);return value;
}
std::optional<std::wstring> Decode(std::span<const uint8_t> data) {
    if(data.size()>=3&&data[0]==0xef&&data[1]==0xbb&&data[2]==0xbf)data=data.subspan(3);
    if(data.empty()||data.size()>MaximumTextBytes)return {};
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,
        reinterpret_cast<const char*>(data.data()),static_cast<int>(data.size()),nullptr,0);
    if(count<=0)return {};
    std::wstring text(static_cast<size_t>(count),L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(data.data()),
        static_cast<int>(data.size()),text.data(),count)!=count || text.find(L'\0')!=text.npos)return {};
    return text;
}
// Maps can contain megabytes of hit objects; audio/metadata/events precede that
// section in the standard format. Stop there without reading audio or artwork.
std::optional<std::vector<uint8_t>> ReadText(const std::filesystem::path& path,const Context& context,bool header_only) {
    context.Check();
    Handle file(CreateFileW(Native(path).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)return {};
    BY_HANDLE_FILE_INFORMATION information{};
    if(!GetFileInformationByHandle(file.value,&information) ||
        (information.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return {};
    std::vector<uint8_t> bytes;bytes.reserve(64U*1024);
    std::array<uint8_t,64U*1024> buffer{};
    size_t scan{};
    for(;;) {
        context.Check();DWORD count{};
        if(!ReadFile(file.value,buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr))return {};
        if(!count)return bytes;
        if(bytes.size()+count>MaximumTextBytes)return {};
        bytes.insert(bytes.end(),buffer.begin(),buffer.begin()+count);
        if(header_only) {
            while(scan<bytes.size()) {
                const size_t begin=scan;
                auto end=std::find(bytes.begin()+static_cast<ptrdiff_t>(scan),bytes.end(),uint8_t('\n'));
                if(end==bytes.end())break;
                scan=static_cast<size_t>(end-bytes.begin())+1;
                std::string_view line(reinterpret_cast<const char*>(bytes.data()+begin),scan-begin);
                const auto first=line.find_first_not_of(" \t\r\n");
                if(first==line.npos)continue;
                line=line.substr(first,line.find_last_not_of(" \t\r\n")-first+1);
                if(line=="[HitObjects]") {bytes.resize(begin);return bytes;}
            }
        }
    }
}
std::optional<std::filesystem::path> ConfiguredSongs(const std::filesystem::path& install,const Context& context,bool allow_offline=false) {
    std::vector<std::filesystem::path> configs;
    if(const auto user=Environment(L"USERNAME")) {
        // USERNAME cannot introduce an alternate path through this filename.
        if(user->find_first_of(L"\\/:\"<>|?*")==user->npos)
            configs.push_back(install/(L"osu!."+*user+L".cfg"));
    }
    configs.push_back(install/L"osu!.cfg");
    for(const auto& path:configs) {
        const auto bytes=ReadText(path,context,false);if(!bytes)continue;
        const auto decoded=Decode(*bytes);if(!decoded)continue;
        std::wstring_view remaining(*decoded);
        while(!remaining.empty()) {
            const auto end=remaining.find_first_of(L"\r\n");
            const auto line=Trim(remaining.substr(0,end));
            remaining=end==remaining.npos?std::wstring_view{}:remaining.substr(end+1);
            const auto split=line.find(L'=');
            if(split==line.npos||Trim(line.substr(0,split))!=L"BeatmapDirectory")continue;
            const auto value=Unquote(line.substr(split+1));if(value.empty())break;
            const auto requested=std::filesystem::path(value);
            const auto selected=Absolute(requested.is_absolute()?requested:install/requested);
            if(allow_offline || Directory(selected))return selected;
            // A user-specified storage path must not silently read a stale
            // default Songs directory when that storage is disconnected.
            throw Failure{HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)};
        }
    }
    return {};
}
std::filesystem::path ResolveSongs(const std::filesystem::path& supplied,const Context& context) {
    auto root=Absolute(supplied);
    const DWORD attr=Attributes(root);
    if(attr==INVALID_FILE_ATTRIBUTES)throw Failure{HRESULT_FROM_WIN32(GetLastError())};
    if(!(attr&FILE_ATTRIBUTE_DIRECTORY)) {
        if(_wcsicmp(root.filename().c_str(),L"osu!.exe"))throw Failure{HRESULT_FROM_WIN32(ERROR_DIRECTORY)};
        root=root.parent_path();
    }
    if(!_wcsicmp(root.filename().c_str(),L"Songs"))return root;
    // Only treat a folder as an installation when its executable/config is
    // present; an explicitly supplied custom song directory is itself a root.
    if(Attributes(root/L"osu!.exe")!=INVALID_FILE_ATTRIBUTES ||
        Attributes(root/L"osu!.cfg")!=INVALID_FILE_ATTRIBUTES) {
        if(auto configured=ConfiguredSongs(root,context))return *configured;
        const auto songs=root/L"Songs";if(Directory(songs))return songs;
        throw Failure{HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)};
    }
    if(Directory(root/L"Songs"))return root/L"Songs";
    return root;
}
bool Reserved(std::wstring_view component) {
    component=component.substr(0,component.find(L'.'));
    std::wstring name(component);for(auto& c:name)c=static_cast<wchar_t>(towupper(c));
    if(name==L"CON"||name==L"PRN"||name==L"AUX"||name==L"NUL")return true;
    return name.size()==4&&(name.starts_with(L"COM")||name.starts_with(L"LPT"))&&name[3]>=L'1'&&name[3]<=L'9';
}
bool Within(const std::filesystem::path& base,const std::filesystem::path& candidate) {
    auto a=base.begin(),b=candidate.begin();
    for(;a!=base.end();++a,++b)if(b==candidate.end()||_wcsicmp(a->c_str(),b->c_str()))return false;
    return b!=candidate.end();
}
std::optional<std::filesystem::path> Reference(const std::filesystem::path& map,
    const std::filesystem::path& set,const std::wstring& value) {
    if(value.empty()||value.size()>=32768||value.find_first_of(L":|\"<>?*")!=value.npos||
        std::any_of(value.begin(),value.end(),[](wchar_t c){return c<32;}))return {};
    std::wstring normalized(value);std::replace(normalized.begin(),normalized.end(),L'/',L'\\');
    const std::filesystem::path relative(normalized);
    if(relative.is_absolute()||relative.has_root_name()||relative.has_root_directory())return {};
    for(const auto& item:relative) {
        const auto& part=item.native();
        if(part!=L"."&&part!=L".."&&(part.empty()||part.back()==L' '||part.back()==L'.'||Reserved(part)))return {};
    }
    const auto path=(map.parent_path()/relative).lexically_normal();
    if(path.native().size()>=32760||!Within(set,path))return {};
    // Do not follow junctions/symlinks from a map into another data directory.
    auto scan=set;
    auto remaining=path.begin();for(auto i=set.begin();i!=set.end();++i)++remaining;
    for(;remaining!=path.end();++remaining) {
        scan/=*remaining;const DWORD attr=Attributes(scan);
        if(attr==INVALID_FILE_ATTRIBUTES||(attr&FILE_ATTRIBUTE_REPARSE_POINT))return {};
        const bool last=std::next(remaining)==path.end();
        if(last?(attr&FILE_ATTRIBUTE_DIRECTORY)!=0:(attr&FILE_ATTRIBUTE_DIRECTORY)==0)return {};
    }
    return path;
}
uint64_t FileSize(const std::filesystem::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if(!GetFileAttributesExW(Native(path).c_str(),GetFileExInfoStandard,&data))return 0;
    return (uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;
}
void Metadata(std::span<const uint8_t> bytes,Track& track) {
    const auto decoded=Decode(bytes);if(!decoded)return;
    std::wstring_view remaining(*decoded),section;
    std::wstring title_unicode,artist_unicode;
    while(!remaining.empty()) {
        const auto end=remaining.find_first_of(L"\r\n");const auto line=Trim(remaining.substr(0,end));
        remaining=end==remaining.npos?std::wstring_view{}:remaining.substr(end+1);
        if(line.size()>=2&&line.front()==L'['&&line.back()==L']'){section=Trim(line.substr(1,line.size()-2));continue;}
        if(section!=L"Metadata")continue;
        const auto colon=line.find(L':');if(colon==line.npos)continue;
        const auto key=Trim(line.substr(0,colon)),value=Trim(line.substr(colon+1));
        if(value.size()>8192)continue;
        const auto set=[&](std::wstring& field){if(field.empty())field=value;};
        if(key==L"Title")set(track.title);else if(key==L"TitleUnicode")set(title_unicode);
        else if(key==L"Artist")set(track.artist);else if(key==L"ArtistUnicode")set(artist_unicode);
        else if(key==L"Source")set(track.album);else if(key==L"Creator")set(track.creator);
        else if(key==L"BeatmapSetID"&&value.size()<12) {
            const std::wstring number(value);wchar_t* tail{};errno=0;const long id=wcstol(number.c_str(),&tail,10);
            if(!errno&&tail==number.c_str()+number.size()&&id>0&&id<=INT32_MAX)track.beatmap_set_id=static_cast<int32_t>(id);
        }
    }
    if(!title_unicode.empty())track.title=std::move(title_unicode);
    if(!artist_unicode.empty())track.artist=std::move(artist_unicode);
}
struct Entry {std::wstring name;DWORD attributes{};};
std::vector<Entry> Entries(const std::filesystem::path& directory,const Context& context,bool root) {
    WIN32_FIND_DATAW data{};FindHandle search;
    search.value=FindFirstFileW(Native(directory/L"*").c_str(),&data);
    if(search.value==INVALID_HANDLE_VALUE) {
        const DWORD error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND)return {};
        if(root)throw Failure{HRESULT_FROM_WIN32(error)};
        return {};
    }
    std::vector<Entry> entries;
    uint64_t retained_names{};
    do {
        context.Check();
        if(!wcscmp(data.cFileName,L".")||!wcscmp(data.cFileName,L"..")||
            (data.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))continue;
        if(entries.size()>=MaximumMaps)throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
        retained_names+=sizeof(Entry)+uint64_t(wcslen(data.cFileName))*sizeof(wchar_t);
        if(retained_names>128ULL*1024*1024)throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
        entries.push_back({data.cFileName,data.dwFileAttributes});
    } while(FindNextFileW(search.value,&data));
    const DWORD error=GetLastError();if(error!=ERROR_NO_MORE_FILES)throw Failure{HRESULT_FROM_WIN32(error)};
    std::sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b){
        const int result=_wcsicmp(a.name.c_str(),b.name.c_str());return result?result<0:a.name<b.name;});
    return entries;
}
}

std::filesystem::path DiscoverStableRoot(const Context& context) {
    context.Check();
    const auto root=installation::DefaultExecutable(1).parent_path();
    if(Directory(root))return root;
    throw Failure{HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)};
}

std::filesystem::path StableSongsDirectory(const std::filesystem::path& installation) {
    const TtpOsuLibraryHost host{sizeof(host)};
    if(const auto configured=ConfiguredSongs(installation,Context{host},true))return *configured;
    return installation/L"Songs";
}

std::vector<ExportSet> PlanStableExport(const std::filesystem::path& supplied,
    const std::vector<std::wstring>& identities,const Context& context) {
    const auto root=ResolveSongs(supplied,context);
    std::set<std::wstring,Insensitive> selected;
    for(const auto& identity:identities) {
        context.Check();
        if(!identity.starts_with(L"stable:"))throw Failure{E_INVALIDARG};
        const auto audio=Absolute(identity.substr(7));
        if(!Within(root,audio))throw Failure{E_INVALIDARG};
        auto relative=audio.lexically_relative(root);
        // The library treats each immediate Songs subdirectory as one set.
        const auto set=relative.has_parent_path()?root/(*relative.begin()):root;
        if(!Reference(set/L"export.osu",set,audio.lexically_relative(set).wstring()))
            throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)};
        selected.insert(set.wstring());
    }
    std::vector<ExportSet> result;
    for(const auto& value:selected) {
        const std::filesystem::path directory(value);
        ExportSet set;set.name=directory.filename().wstring();set.source_root=root;
        std::vector<std::filesystem::path> pending{directory};
        size_t directories{};bool has_map{};
        while(!pending.empty()) {
            context.Check();auto current=std::move(pending.back());pending.pop_back();
            if(++directories>MaximumMaps)throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
            // Unlike discovery, export must report an unreadable resource,
            // rather than silently deliver an incomplete beatmap package.
            for(const auto& entry:std::filesystem::directory_iterator(current)) {
                context.Check();const auto path=entry.path();const DWORD attributes=Attributes(path);
                if(attributes==INVALID_FILE_ATTRIBUTES)throw Failure{HRESULT_FROM_WIN32(GetLastError())};
                if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)throw Failure{HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)};
                if(attributes&FILE_ATTRIBUTE_DIRECTORY) {
                    if(std::distance(path.begin(),path.end())-std::distance(directory.begin(),directory.end())>64)
                        throw Failure{HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE)};
                    pending.push_back(path);continue;
                }
                if(set.files.size()>=MaximumMaps)throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
                auto name=ExportMemberName(path.lexically_relative(directory).wstring());
                has_map|=_wcsicmp(path.extension().c_str(),L".osu")==0;
                set.files.push_back({path,std::move(name)});
                context.Progress(TTP_OSU_EXPORT_PLAN,set.files.size(),0,path.c_str());
            }
        }
        if(!has_map)throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
        result.push_back(std::move(set));
    }
    return result;
}

std::vector<Track> EnumerateStable(const std::filesystem::path& supplied,const Context& context) {
    const auto root=ResolveSongs(supplied,context);
    struct Pending {std::filesystem::path directory,set;unsigned depth{};};
    std::vector<Pending> pending{{root,root,0}};
    std::vector<Track> records;
    std::map<std::wstring,size_t,Insensitive> known;
    uint64_t maps{},directories{},retained{},pending_chars=root.native().size()*2;
    while(!pending.empty()) {
        context.Check();auto current=std::move(pending.back());pending.pop_back();
        pending_chars-=current.directory.native().size()+current.set.native().size();
        if(++directories>MaximumMaps)throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
        context.Progress(TTP_OSU_LIBRARY_SCAN,maps,0,current.directory.c_str());
        const auto entries=Entries(current.directory,context,current.depth==0);
        // Reverse directory push retains deterministic ascending DFS order.
        for(auto i=entries.rbegin();i!=entries.rend();++i)if(i->attributes&FILE_ATTRIBUTE_DIRECTORY) {
            if(current.depth>=64)continue;
            const auto child=current.directory/i->name;
            if(child.native().size()>=32760)continue;
            pending_chars+=child.native().size()+(current.depth?current.set:child).native().size();
            if(pending_chars>64ULL*1024*1024)throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
            pending.push_back({child,current.depth?current.set:child,current.depth+1});
        }
        for(const auto& entry:entries) {
            if(entry.attributes&FILE_ATTRIBUTE_DIRECTORY)continue;
            const auto map=current.directory/entry.name;
            if(_wcsicmp(map.extension().c_str(),L".osu"))continue;
            if(++maps>MaximumMaps)throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
            context.Progress(TTP_OSU_LIBRARY_READ,maps,0,map.c_str());
            const auto bytes=ReadText(map,context,true);if(!bytes)continue;
            const auto refs=Parse(*bytes);if(!refs)continue;
            const auto audio=Reference(map,current.set,refs->audio);if(!audio)continue;
            Track track;track.audio=audio->native();track.beatmap=map.native();
            track.file_size=FileSize(*audio);if(!track.file_size)continue;
            if(const auto background=Reference(map,current.set,refs->background))track.background=background->native();
            Metadata(*bytes,track);
            if(track.title.empty())track.title=audio->stem().native();
            // Each set keeps its own identity even if a future storage provider
            // exposes shared content hashes. Stable absolute audio paths already
            // distinguish separate set directories, including unsubmitted maps.
            track.identity=L"stable:"+track.audio;
            const auto [found,inserted]=known.try_emplace(track.identity,records.size());
            if(inserted) {
                if(records.size()>=MaximumTracks)throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
                const size_t chars=track.identity.size()+track.audio.size()+track.beatmap.size()+track.background.size()+
                    track.title.size()+track.artist.size()+track.album.size()+track.creator.size();
                retained+=uint64_t(chars)*sizeof(wchar_t)+uint64_t(track.identity.size())*sizeof(wchar_t);
                if(retained>MaximumRetainedText)throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
                records.push_back(std::move(track));
            } else {
                auto& previous=records[found->second];
                const size_t old_size=previous.background.size()+previous.artist.size()+previous.album.size()+previous.creator.size();
                if(previous.background.empty())previous.background=std::move(track.background);
                if(previous.artist.empty())previous.artist=std::move(track.artist);
                if(previous.album.empty())previous.album=std::move(track.album);
                if(previous.creator.empty())previous.creator=std::move(track.creator);
                if(previous.beatmap_set_id<0)previous.beatmap_set_id=track.beatmap_set_id;
                retained+=uint64_t(previous.background.size()+previous.artist.size()+previous.album.size()+previous.creator.size()-old_size)*sizeof(wchar_t);
                if(retained>MaximumRetainedText)throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
            }
        }
    }
    context.Progress(TTP_OSU_LIBRARY_READ,maps,maps,root.c_str());
    return records;
}
}
