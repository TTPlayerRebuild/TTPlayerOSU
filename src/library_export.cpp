#include "export_internal.h"
#include "realm_library.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <map>
#include <set>

namespace ttp_osu::library {
namespace {
namespace fs=std::filesystem;
[[noreturn]] void Fail(DWORD error) {throw Failure{HRESULT_FROM_WIN32(error?error:ERROR_WRITE_FAULT)};}
std::wstring Native(const fs::path& input) {
    auto path=fs::absolute(input).lexically_normal().native();
    if(path.starts_with(L"\\\\?\\"))return path;
    return path.starts_with(L"\\\\")?L"\\\\?\\UNC\\"+path.substr(2):L"\\\\?\\"+path;
}
bool Inside(const fs::path& base,const fs::path& child) {
    auto a=base.begin(),b=child.begin();
    for(;a!=base.end();++a,++b)if(b==child.end()||_wcsicmp(a->c_str(),b->c_str()))return false;
    return true;
}
std::wstring Fold(std::wstring_view value) {
    const int n=LCMapStringW(LOCALE_INVARIANT,LCMAP_UPPERCASE,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(n<=0)Fail(ERROR_INVALID_NAME);
    std::wstring result(n,L'\0');
    if(LCMapStringW(LOCALE_INVARIANT,LCMAP_UPPERCASE,value.data(),static_cast<int>(value.size()),result.data(),n)!=n)
        Fail(ERROR_INVALID_NAME);
    return result;
}
bool Reserved(std::wstring_view value) {
    auto name=Fold(value.substr(0,value.find(L'.')));
    return name==L"CON"||name==L"PRN"||name==L"AUX"||name==L"NUL"||
        (name.size()==4&&(name.starts_with(L"COM")||name.starts_with(L"LPT"))&&name[3]>=L'1'&&name[3]<=L'9');
}
std::string Utf8(std::wstring_view text) {
    // WC_ERR_INVALID_CHARS is not supported by XP. Validate UTF-16 ourselves.
    for(size_t i=0;i<text.size();++i) {
        const auto ch=text[i];
        if(ch>=0xd800&&ch<=0xdbff) {
            if(++i==text.size()||text[i]<0xdc00||text[i]>0xdfff)Fail(ERROR_INVALID_NAME);
        }else if(ch>=0xdc00&&ch<=0xdfff)Fail(ERROR_INVALID_NAME);
    }
    const int n=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if(n<=0||n>65535)Fail(ERROR_FILENAME_EXCED_RANGE);
    std::string result(n,'\0');
    if(WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),n,nullptr,nullptr)!=n)
        Fail(ERROR_INVALID_NAME);
    return result;
}
std::wstring ArchiveName(std::wstring name) {
    for(auto& ch:name)if(ch<32||wcschr(L"<>:\"/\\|?*",ch))ch=L'_';
    while(!name.empty()&&(name.back()==L'.'||name.back()==L' '))name.pop_back();
    if(name.empty())name=L"beatmap";
    if(Reserved(name))name=L"_"+name;
    if(name.size()>160) {name.resize(160);if(name.back()>=0xd800&&name.back()<=0xdbff)name.pop_back();}
    return name;
}
struct Handle {
    HANDLE h{INVALID_HANDLE_VALUE};
    ~Handle(){Close();}
    void Close(){if(h!=INVALID_HANDLE_VALUE){CloseHandle(h);h=INVALID_HANDLE_VALUE;}}
};
uint32_t Crc(uint32_t crc,const unsigned char* bytes,size_t size) {
    static const auto table=[] {
        std::array<uint32_t,256> values{};
        for(uint32_t i=0;i<256;++i) {uint32_t n=i;for(int b=0;b<8;++b)n=(n>>1)^((n&1)?0xedb88320u:0);values[i]=n;}
        return values;
    }();
    for(size_t i=0;i<size;++i)crc=(crc>>8)^table[(crc^bytes[i])&255];
    return crc;
}
struct Member {
    ExportFile file;std::string name;uint64_t size{},offset{};uint32_t crc{};WORD date{},time{};FILETIME modified{};
};
struct Output {
    Handle handle;fs::path temporary;uint64_t offset{};
    explicit Output(const fs::path& directory) {
        for(unsigned i=0;i<10000;++i) {
            temporary=directory/(L".ttplayer-osz-"+std::to_wstring(GetCurrentProcessId())+L"-"+
                std::to_wstring(GetCurrentThreadId())+L"-"+std::to_wstring(i)+L".tmp");
            handle.h=CreateFileW(Native(temporary).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(handle.h!=INVALID_HANDLE_VALUE)return;
            if(GetLastError()!=ERROR_FILE_EXISTS)Fail(GetLastError());
        }
        temporary.clear();Fail(ERROR_FILE_EXISTS);
    }
    ~Output(){handle.Close();if(!temporary.empty())DeleteFileW(Native(temporary).c_str());}
    void Bytes(const void* data,DWORD length) {
        DWORD written{};if(!WriteFile(handle.h,data,length,&written,nullptr))Fail(GetLastError());
        if(written!=length)Fail(ERROR_WRITE_FAULT);offset+=length;
    }
    void U16(uint16_t n){const unsigned char b[]{static_cast<unsigned char>(n),static_cast<unsigned char>(n>>8)};Bytes(b,2);}
    void U32(uint32_t n){U16(static_cast<uint16_t>(n));U16(static_cast<uint16_t>(n>>16));}
    void U64(uint64_t n){U32(static_cast<uint32_t>(n));U32(static_cast<uint32_t>(n>>32));}
    void Name(const std::string& n){Bytes(n.data(),static_cast<DWORD>(n.size()));}
    void Finish(){if(!FlushFileBuffers(handle.h))Fail(GetLastError());handle.Close();}
};
std::vector<Member> Prepare(ExportSet& set,const Context& context) {
    std::vector<Member> result;std::set<std::wstring> names;
    std::sort(set.files.begin(),set.files.end(),[](const auto& a,const auto& b){return a.name<b.name;});
    for(auto& file:set.files) {
        context.Check();file.name=ExportMemberName(std::move(file.name));
        if(!names.insert(Fold(file.name)).second)Fail(ERROR_DUP_NAME);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if(!GetFileAttributesExW(Native(file.path).c_str(),GetFileExInfoStandard,&data))Fail(GetLastError());
        if(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))Fail(ERROR_NOT_SUPPORTED);
        Member m;m.name=Utf8(file.name);m.file=std::move(file);
        m.size=(uint64_t(data.nFileSizeHigh)<<32)|data.nFileSizeLow;m.modified=data.ftLastWriteTime;
        FILETIME local{};
        if(!FileTimeToLocalFileTime(&m.modified,&local)||!FileTimeToDosDateTime(&local,&m.date,&m.time)) {m.date=33;m.time=0;}
        result.push_back(std::move(m));
    }
    // A member may not also be a directory prefix of another member.
    for(const auto& name:names)for(size_t p=name.find(L'/');p!=name.npos;p=name.find(L'/',p+1))
        if(names.contains(name.substr(0,p)))Fail(ERROR_DUP_NAME);
    return result;
}
void WriteZip(Output& out,std::vector<Member>& members,const Context& context) {
    // Stored ZIP members preserve all audio/map bytes without another codec or
    // a full-file buffer. ZIP64 is emitted only where standard ZIP overflows.
    std::array<unsigned char,256*1024> buffer{};uint64_t completed{},total{};
    for(const auto& m:members) {if(m.size>UINT64_MAX-total)Fail(ERROR_FILE_TOO_LARGE);total+=m.size;}
    for(auto& m:members) {
        context.Progress(TTP_OSU_EXPORT_WRITE,completed,total,m.file.name.c_str());
        Handle input;input.h=CreateFileW(Native(m.file.path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(input.h==INVALID_HANDLE_VALUE)Fail(GetLastError());
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(input.h,&info))Fail(GetLastError());
        if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
            ((uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow)!=m.size||
            CompareFileTime(&info.ftLastWriteTime,&m.modified)!=0)Fail(ERROR_FILE_INVALID);
        const bool large=m.size>=UINT32_MAX;m.offset=out.offset;
        out.U32(0x04034b50);out.U16(large?45:20);out.U16(0x0808);out.U16(0);out.U16(m.time);out.U16(m.date);
        out.U32(0);out.U32(large?UINT32_MAX:0);out.U32(large?UINT32_MAX:0);
        out.U16(static_cast<uint16_t>(m.name.size()));out.U16(large?20:0);out.Name(m.name);
        if(large){out.U16(1);out.U16(16);out.U64(m.size);out.U64(m.size);}
        uint64_t read{};uint32_t crc=UINT32_MAX;
        while(read<m.size) {
            context.Check();DWORD count{};const DWORD wanted=static_cast<DWORD>(std::min<uint64_t>(buffer.size(),m.size-read));
            if(!ReadFile(input.h,buffer.data(),wanted,&count,nullptr))Fail(GetLastError());
            if(!count)Fail(ERROR_HANDLE_EOF);
            crc=Crc(crc,buffer.data(),count);out.Bytes(buffer.data(),count);read+=count;completed+=count;
            context.Progress(TTP_OSU_EXPORT_WRITE,completed,total,m.file.name.c_str());
        }
        m.crc=crc^UINT32_MAX;out.U32(0x08074b50);out.U32(m.crc);
        if(large){out.U64(m.size);out.U64(m.size);}else{out.U32(static_cast<uint32_t>(m.size));out.U32(static_cast<uint32_t>(m.size));}
    }
    const uint64_t central=out.offset;
    for(const auto& m:members) {
        context.Check();const bool large=m.size>=UINT32_MAX,large_offset=m.offset>=UINT32_MAX;
        const uint16_t extra=(large?16:0)+(large_offset?8:0);
        out.U32(0x02014b50);out.U16(45);out.U16((large||large_offset)?45:20);out.U16(0x0808);out.U16(0);
        out.U16(m.time);out.U16(m.date);out.U32(m.crc);
        out.U32(large?UINT32_MAX:static_cast<uint32_t>(m.size));out.U32(large?UINT32_MAX:static_cast<uint32_t>(m.size));
        out.U16(static_cast<uint16_t>(m.name.size()));out.U16(extra?extra+4:0);out.U16(0);out.U16(0);out.U16(0);
        out.U32(FILE_ATTRIBUTE_ARCHIVE);out.U32(large_offset?UINT32_MAX:static_cast<uint32_t>(m.offset));out.Name(m.name);
        if(extra){out.U16(1);out.U16(extra);if(large){out.U64(m.size);out.U64(m.size);}if(large_offset)out.U64(m.offset);}
    }
    const uint64_t length=out.offset-central;
    if(members.size()>=UINT16_MAX||central>=UINT32_MAX||length>=UINT32_MAX) {
        const uint64_t end=out.offset;
        out.U32(0x06064b50);out.U64(44);out.U16(45);out.U16(45);out.U32(0);out.U32(0);
        out.U64(members.size());out.U64(members.size());out.U64(length);out.U64(central);
        out.U32(0x07064b50);out.U32(0);out.U64(end);out.U32(1);
    }
    out.U32(0x06054b50);out.U16(0);out.U16(0);
    const auto count=static_cast<uint16_t>(std::min<size_t>(members.size(),UINT16_MAX));out.U16(count);out.U16(count);
    out.U32(static_cast<uint32_t>(std::min<uint64_t>(length,UINT32_MAX)));
    out.U32(static_cast<uint32_t>(std::min<uint64_t>(central,UINT32_MAX)));out.U16(0);
    context.Check();out.Finish();
}
HRESULT WINAPI Run(const TtpOsuLibraryHost* host,const TtpOsuExportRequest* request,
    TtpOsuExported visitor,void* opaque) noexcept {
    try {
        if(!host||host->size<sizeof(*host)||!request||request->size<sizeof(*request)||
            (request->source!=1&&request->source!=2)||!request->root||!*request->root||
            !request->destination||!*request->destination||!request->identities||
            !request->count||request->count>MaximumTracks||request->flags>3||request->flags==2)return E_INVALIDARG;
        if(wcsnlen(request->root,32768)==32768||wcsnlen(request->destination,32768)==32768)return E_INVALIDARG;
        const Context context{*host};context.Check();
        auto root=fs::absolute(request->root).lexically_normal();
        const auto destination=fs::absolute(request->destination).lexically_normal();
        auto protected_root=root;
        if(!_wcsicmp(root.filename().c_str(),L"files")||!_wcsicmp(root.filename().c_str(),L"osu!.exe"))protected_root=root.parent_path();
        // Never put output or temporary archives into the game's storage tree.
        if(Inside(protected_root,destination))return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        std::vector<std::wstring> identities;uint64_t chars{};
        for(uint32_t i=0;i<request->count;++i) {
            const auto* id=request->identities[i];if(!id||!*id||wcsnlen(id,32768)==32768)return E_INVALIDARG;
            chars+=wcslen(id);if(chars>64ull*1024*1024)return E_OUTOFMEMORY;identities.emplace_back(id);
        }
        auto sets=request->source==1?PlanStableExport(root,identities,context):PlanLazerExport(root,identities,context);
        const bool single=(request->flags&TTP_OSU_EXPORT_SINGLE_FILE)!=0;
        if(sets.empty()||(single&&sets.size()!=1))return E_INVALIDARG;
        const auto directory=single?destination.parent_path():destination;
        if(!fs::is_directory(directory))return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        const auto actual_directory=fs::canonical(directory);
        for(const auto& set:sets)
            if(Inside(fs::canonical(set.source_root),actual_directory))return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        for(auto& set:sets) {
            auto members=Prepare(set,context);
            Output output(directory);WriteZip(output,members,context);
            fs::path final;
            const auto name=ArchiveName(set.name);
            for(uint64_t suffix=1;;++suffix) {
                context.Check();
                final=single?destination:directory/(name+(suffix==1?L"":L" ("+std::to_wstring(suffix)+L")")+L".osz");
                const DWORD flags=MOVEFILE_WRITE_THROUGH|((single&&(request->flags&TTP_OSU_EXPORT_REPLACE))?MOVEFILE_REPLACE_EXISTING:0);
                if(MoveFileExW(Native(output.temporary).c_str(),Native(final).c_str(),flags))break;
                const auto error=GetLastError();
                if(single||(error!=ERROR_ALREADY_EXISTS&&error!=ERROR_FILE_EXISTS))Fail(error);
            }
            output.temporary.clear();
            if(visitor) {const auto hr=visitor(opaque,final.c_str());if(FAILED(hr))return hr;}
        }
        return S_OK;
    }catch(const Failure& error){return error.result;}
     catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
     catch(const fs::filesystem_error& error){return HRESULT_FROM_WIN32(error.code().value()?error.code().value():ERROR_WRITE_FAULT);}
     catch(...){return E_FAIL;}
}
}
std::wstring ExportMemberName(std::wstring name) {
    if(name.empty()||name.size()>32767)Fail(ERROR_INVALID_NAME);
    std::replace(name.begin(),name.end(),L'\\',L'/');
    for(wchar_t ch:name)if(ch<32||wcschr(L"<>:\"|?*",ch))Fail(ERROR_INVALID_NAME);
    size_t start{};
    while(start<name.size()) {
        const auto end=name.find(L'/',start);
        const auto part=std::wstring_view(name).substr(start,end==name.npos?name.size()-start:end-start);
        if(part.empty()||part==L"."||part==L".."||part.back()==L' '||part.back()==L'.'||Reserved(part))Fail(ERROR_INVALID_NAME);
        if(end==name.npos)break;start=end+1;if(start==name.size())Fail(ERROR_INVALID_NAME);
    }
    Utf8(name);return name;
}
}
extern "C" HRESULT WINAPI ttp_osu_export_query(uint32_t version,TtpOsuExportApi* out) noexcept {
    if(!out)return E_POINTER;
    if(version!=TTP_OSU_EXPORT_ABI)return E_NOINTERFACE;
    if(out->size<sizeof(*out))return E_INVALIDARG;
    const TtpOsuExportApi api{sizeof(api),TTP_OSU_EXPORT_ABI,ttp_osu::library::Run};
    std::memcpy(out,&api,sizeof(api));return S_OK;
}
