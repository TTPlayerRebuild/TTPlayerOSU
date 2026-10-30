// SDK declarations for handle-based rename; the existing XP thunk object
// provides SetFileInformationByHandle on XP. Final imports are audited.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#include "import_internal.h"
#include "export_internal.h"
#include "installation_internal.h"
#include "executable_identity.h"
#include <ttp_osu/import.h>
#include <sha256.hpp>
#include <shlobj.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <set>

namespace ttp_osu::importer {
namespace fs=std::filesystem;
namespace {
[[noreturn]] void Fail(DWORD error){throw Failure{HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE)};}
void Check(HRESULT hr){if(FAILED(hr))throw Failure{hr};}
bool Text(const wchar_t* s){return s&&*s&&wcsnlen(s,32768)<32768;}
struct Handle {
    HANDLE h{INVALID_HANDLE_VALUE};
    ~Handle(){Close();}
    void Close(){if(h!=INVALID_HANDLE_VALUE&&h){CloseHandle(h);h=INVALID_HANDLE_VALUE;}}
};
struct Hash {
    sha256_state value{};
    Hash(){sha_init(value);}
    void Add(const void* bytes,uint32_t size){sha_process(value,bytes,size);}
    Digest Finish(){Digest result{};sha_done(value,result.data());return result;}
};
struct Md5 {
    HCRYPTPROV provider{};HCRYPTHASH hash{};
    Md5(){if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_FULL,CRYPT_VERIFYCONTEXT))Fail(GetLastError());
        if(!CryptCreateHash(provider,CALG_MD5,0,0,&hash)){const auto error=GetLastError();CryptReleaseContext(provider,0);provider=0;Fail(error);}}
    ~Md5(){if(hash)CryptDestroyHash(hash);if(provider)CryptReleaseContext(provider,0);}
    void Add(const unsigned char* bytes,DWORD size){if(!CryptHashData(hash,bytes,size,0))Fail(GetLastError());}
    std::wstring Finish(){unsigned char bytes[16]{};DWORD size=16;if(!CryptGetHashParam(hash,HP_HASHVAL,bytes,&size,0)||size!=16)Fail(GetLastError());
        std::wstring result;for(const auto b:bytes){result+=L"0123456789abcdef"[b>>4];result+=L"0123456789abcdef"[b&15];}return result;}
};
std::wstring Native(const fs::path& p){auto s=fs::absolute(p).lexically_normal().native();
    if(s.starts_with(L"\\\\?\\"))return s;return s.starts_with(L"\\\\")?L"\\\\?\\UNC\\"+s.substr(2):L"\\\\?\\"+s;}
bool Within(const fs::path& base,const fs::path& child){auto a=base.begin(),b=child.begin();
    for(;a!=base.end();++a,++b)if(b==child.end()||_wcsicmp(a->c_str(),b->c_str()))return false;return true;}
BY_HANDLE_FILE_INFORMATION Information(HANDLE h){BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(h,&info))Fail(GetLastError());
    if(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))Fail(ERROR_NOT_SUPPORTED);return info;}
uint64_t Size(const BY_HANDLE_FILE_INFORMATION& info){return (uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;}
bool Same(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){return
    a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow&&
    Size(a)==Size(b)&&CompareFileTime(&a.ftLastWriteTime,&b.ftLastWriteTime)==0;}
Digest ReadHash(HANDLE h,const library::Context& context,uint32_t phase,const wchar_t* name,HANDLE output=INVALID_HANDLE_VALUE){
    const auto info=Information(h);LARGE_INTEGER zero{};
    if(!SetFilePointerEx(h,zero,nullptr,FILE_BEGIN))Fail(GetLastError());
    Hash hash;std::array<unsigned char,256*1024> buffer{};uint64_t done{};
    for(;;){context.Progress(phase,done,Size(info),name);DWORD count{};
        if(!ReadFile(h,buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr))Fail(GetLastError());
        if(!count)break;hash.Add(buffer.data(),count);done+=count;
        if(output!=INVALID_HANDLE_VALUE){DWORD written{};if(!WriteFile(output,buffer.data(),count,&written,nullptr)||written!=count)Fail(GetLastError());}
    }
    if(done!=Size(info)||!Same(info,Information(h)))Fail(ERROR_FILE_INVALID);
    return hash.Finish();
}
Digest FileHash(const fs::path& path,const library::Context& context,uint32_t phase){
    Handle file;file.h=CreateFileW(Native(path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file.h==INVALID_HANDLE_VALUE)Fail(GetLastError());return ReadHash(file.h,context,phase,path.c_str());
}
void Copy(HANDLE input,const fs::path& path,const Digest& expected,const library::Context& context){
    Handle out;out.h=CreateFileW(Native(path).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(out.h==INVALID_HANDLE_VALUE)Fail(GetLastError());
    try {const auto hash=ReadHash(input,context,TTP_OSU_IMPORT_COPY,path.c_str(),out.h);
        if(hash!=expected)Fail(ERROR_FILE_INVALID);if(!FlushFileBuffers(out.h))Fail(GetLastError());out.Close();
        if(FileHash(path,context,TTP_OSU_IMPORT_CHECK_COPY)!=expected)Fail(ERROR_CRC);
    }catch(...){out.Close();DeleteFileW(Native(path).c_str());throw;}
}
std::vector<Member> Manifest(const TtpOsuImportHost& host,const fs::path& archive,const library::Context& context){
    struct Names {std::vector<std::wstring> values;uint64_t chars{};} names;
    Check(host.list_members(host.context,archive.c_str(),[](void* opaque,const wchar_t* name)->HRESULT {
        try {auto& n=*static_cast<Names*>(opaque);if(!Text(name))return E_INVALIDARG;
            n.chars+=wcslen(name);if(n.values.size()>=250000||n.chars>32ull*1024*1024)return E_OUTOFMEMORY;
            n.values.emplace_back(name);return S_OK;
        }catch(...){return E_OUTOFMEMORY;}
    },&names));
    std::vector<Member> result;std::set<std::wstring> unique;bool has_map{};
    for(const auto& raw:names.values){
        Member member;member.name=library::ExportMemberName(raw);
        if(!unique.insert(Fold(member.name)).second)Fail(ERROR_DUP_NAME);
        member.beatmap=_wcsicmp(fs::path(member.name).extension().c_str(),L".osu")==0;
        IStream* pointer{};const auto opened=host.open_member(host.context,archive.c_str(),raw.c_str(),&pointer);
        struct Release {void operator()(IStream* p)const{if(p)p->Release();}};
        std::unique_ptr<IStream,Release> stream(pointer);Check(opened);if(!stream)Fail(ERROR_INVALID_HANDLE);
        Hash hash;std::unique_ptr<Md5> md5;if(member.beatmap)md5=std::make_unique<Md5>();
        std::vector<unsigned char> map;std::array<unsigned char,256*1024> buffer{};
        for(;;){context.Progress(TTP_OSU_IMPORT_MANIFEST,result.size(),names.values.size(),raw.c_str());
            ULONG count{};const auto hr=stream->Read(buffer.data(),static_cast<ULONG>(buffer.size()),&count);Check(hr);
            if(count>buffer.size())Fail(ERROR_INVALID_DATA);
            if(!count)break;hash.Add(buffer.data(),count);member.size+=count;
            if(md5){md5->Add(buffer.data(),count);if(map.size()+count>16u*1024*1024)Fail(ERROR_FILE_TOO_LARGE);
                map.insert(map.end(),buffer.begin(),buffer.begin()+count);}
            // Even a short successful read must be followed through EOF/CRC.
        }
        member.hash=hash.Finish();
        if(md5){member.md5=md5->Finish();if(!Parse(map))Fail(ERROR_INVALID_DATA);has_map=true;}
        result.push_back(std::move(member));
    }
    if(!has_map)Fail(ERROR_INVALID_DATA);
    for(const auto& name:unique)for(size_t at=name.find(L'/');at!=name.npos;at=name.find(L'/',at+1))
        if(unique.contains(name.substr(0,at)))Fail(ERROR_DUP_NAME);
    return result;
}
std::wstring Quote(std::wstring_view s){std::wstring out=L"\"";size_t slashes{};
    for(wchar_t c:s){if(c==L'\\'){++slashes;continue;}out.append(slashes*(c==L'"'?2:1),L'\\');slashes=0;
        if(c==L'"')out+=L'\\';out+=c;}out.append(slashes*2,L'\\');out+=L'"';return out;}
std::wstring JournalValue(const fs::path& path,const wchar_t* key){wchar_t text[32768]{};
    const DWORD n=GetPrivateProfileStringW(L"Import",key,L"",text,32768,path.c_str());
    if(n==32767)Fail(ERROR_INVALID_DATA);return std::wstring(text,n);}
void Journal(const fs::path& directory,const fs::path& source,const fs::path& exe,const fs::path& work,
    const Digest& hash,const wchar_t* state,const std::wstring& cleanup_recovery={}){
    std::wstring text=L"\ufeff[Import]\r\nSource="+source.native()+L"\r\nExecutable="+exe.native()+L"\r\nWork="+
        work.filename().native()+L"\r\nSHA256="+Hex(hash)+L"\r\nState="+state+L"\r\n";
    const auto recovery=cleanup_recovery.empty()?JournalValue(directory/L"task.ini",L"CleanupRecovery"):cleanup_recovery;
    if(!recovery.empty())text+=L"CleanupRecovery="+recovery+L"\r\n";
    const auto temporary=directory/L"task.ini.tmp";Handle file;
    file.h=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file.h==INVALID_HANDLE_VALUE)Fail(GetLastError());DWORD count{};const auto bytes=static_cast<DWORD>(text.size()*sizeof(wchar_t));
    if(!WriteFile(file.h,text.data(),bytes,&count,nullptr)||count!=bytes||!FlushFileBuffers(file.h))Fail(GetLastError());file.Close();
    if(!MoveFileExW(temporary.c_str(),(directory/L"task.ini").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))Fail(GetLastError());
}
HRESULT RecycleSource(const fs::path& source,const BY_HANDLE_FILE_INFORMATION& identity,
    const Digest& expected,const library::Context& context,const fs::path& directory,
    const fs::path& executable,const fs::path& work) {
    try {
        wchar_t volume[MAX_PATH]{};
        if(!GetVolumePathNameW(source.c_str(),volume,MAX_PATH)||GetDriveTypeW(volume)!=DRIVE_FIXED)
            return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        Handle file;file.h=CreateFileW(Native(source).c_str(),GENERIC_READ|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file.h==INVALID_HANDLE_VALUE)Fail(GetLastError());
        if(!Same(identity,Information(file.h))||ReadHash(file.h,context,TTP_OSU_IMPORT_CHECK_COPY,source.c_str())!=expected)
            Fail(ERROR_FILE_INVALID);
        GUID guid{};Check(CoCreateGuid(&guid));wchar_t text[40]{};StringFromGUID2(guid,text,40);
        const auto temporary=source.parent_path()/(std::wstring(L".ttplayer-osz-")+text+L".osz");
        // Keep the recovery name even if Shell fails and another process has
        // occupied the original name before rollback. Never lose that location.
        Journal(directory,source,executable,work,expected,L"verified",temporary.native());
        const auto path=temporary.native();const auto bytes=static_cast<DWORD>(offsetof(FILE_RENAME_INFO,FileName)+(path.size()+1)*sizeof(wchar_t));
        std::vector<unsigned char> storage(bytes);auto* rename=reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        rename->ReplaceIfExists=FALSE;rename->RootDirectory=nullptr;rename->FileNameLength=static_cast<DWORD>(path.size()*sizeof(wchar_t));
        std::memcpy(rename->FileName,path.data(),rename->FileNameLength);
        // Rename the verified open file, not a possibly replaced original path.
        if(!SetFileInformationByHandle(file.h,FileRenameInfo,rename,bytes))Fail(GetLastError());
        file.Close();std::wstring from=temporary.native();from.push_back(L'\0');from.push_back(L'\0');
        SHFILEOPSTRUCTW operation{};operation.wFunc=FO_DELETE;operation.pFrom=from.c_str();
        operation.fFlags=FOF_ALLOWUNDO|FOF_NOCONFIRMATION|FOF_NOERRORUI|FOF_SILENT;
        const int error=SHFileOperationW(&operation);
        if(error||operation.fAnyOperationsAborted||fs::exists(temporary)) {
            // Restore only if the old name is still free, never overwrite it.
            if(fs::exists(temporary))MoveFileW(temporary.c_str(),source.c_str());
            return HRESULT_FROM_WIN32(error?error:ERROR_CANCELLED);
        }
        return S_OK;
    }catch(const Failure& e){return e.result;}
}
fs::path TaskRoot(){wchar_t base[MAX_PATH]{};Check(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,base));
    const auto root=fs::path(base)/L"TTPlayer"/L"OsuImports";fs::create_directories(root);
    // Redirected Windows profiles are valid; use their resolved physical root.
    return fs::canonical(root);}
HRESULT WINAPI Run(const TtpOsuImportHost* host,const TtpOsuImportRequest* request,TtpOsuImported visit,void* opaque) noexcept {
    try {
        if(!host||host->size<sizeof(*host)||!host->list_members||!host->open_member||!request||request->size<sizeof(*request)||
            !Text(request->archive)||!Text(request->executable)||request->source<1||request->source>2||request->flags>3||request->wait_ms>120000||!visit)return E_INVALIDARG;
        const TtpOsuLibraryHost callbacks{sizeof(callbacks),host->context,host->canceled,host->progress};
        const library::Context context{callbacks};context.Check();
        const auto exe=fs::canonical(installation::NormalizePath(request->executable));
        if(static_cast<uint32_t>(installation::IdentifyOsuExecutable(exe.native()))!=request->source)return HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT);
        const auto source=fs::canonical(request->archive);
        if(_wcsicmp(source.extension().c_str(),L".osz"))return E_INVALIDARG;
        const auto root=request->source==1?library::StableSongsDirectory(exe.parent_path()):
            installation::LazerDataRoot(installation::DefaultLazerRoamingRoot(),exe);
        // A package already inside the game's watched/storage tree cannot be
        // promised retention at that path. Never submit that original.
        if(Within(fs::weakly_canonical(root),source))return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
        Handle original;original.h=CreateFileW(Native(source).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(original.h==INVALID_HANDLE_VALUE)Fail(GetLastError());
        const auto identity=Information(original.h);
        const auto source_hash=ReadHash(original.h,context,TTP_OSU_IMPORT_CHECK_COPY,source.c_str());
        Hash key;const auto identity_text=source.native()+L"\n"+exe.native()+L"\n"+Hex(source_hash);
        key.Add(identity_text.data(),static_cast<uint32_t>(identity_text.size()*sizeof(wchar_t)));
        const auto directory=TaskRoot()/(std::to_wstring(request->source)+L"-"+Hex(key.Finish()).substr(0,32));
        fs::create_directory(directory);
        if(fs::canonical(directory)!=directory)Fail(ERROR_ACCESS_DENIED);
        // Serialize duplicate submissions across player instances.
        Handle lease;lease.h=CreateFileW((directory/L"task.lock").c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(lease.h==INVALID_HANDLE_VALUE)Fail(GetLastError());
        const auto journal=directory/L"task.ini";const auto previous=JournalValue(journal,L"State");
        auto name=JournalValue(journal,L"Work");
        if(name.empty())name=source.filename().native();
        if(name!=fs::path(name).filename().native()||_wcsicmp(fs::path(name).extension().c_str(),L".osz"))Fail(ERROR_INVALID_NAME);
        auto work=directory/name;
        bool submitted=previous==L"submitted"||previous==L"pending";
        // Resume retains the same task. A consumed copy is not needed to verify
        // an already imported package; the source remains locked and readable.
        if(!submitted){
            if(fs::exists(work)){if(FileHash(work,context,TTP_OSU_IMPORT_CHECK_COPY)!=source_hash)Fail(ERROR_CRC);}
            else Copy(original.h,work,source_hash,context);
        }
        const auto members=Manifest(*host,source,context);
        HRESULT detail=S_FALSE;
        const auto verify=[&]()->bool {
            try {context.Progress(TTP_OSU_IMPORT_VERIFY,0,0,root.c_str());
                const auto files=request->source==1?MatchStable(exe,root,members,context):MatchLazer(root,members,context);
                detail=files.size()==members.size()?S_OK:S_FALSE;return detail==S_OK;
            }catch(const Failure& e){if(e.result==HRESULT_FROM_WIN32(ERROR_CANCELLED))throw;detail=e.result;return false;}
        };
        bool confirmed=verify();const bool already=confirmed;
        if(!confirmed&&!submitted){
            if(request->source==1&&fs::exists(root/work.filename())){
                auto stem=source.stem().native();if(stem.size()>80)stem.resize(80);
                const auto alternative=directory/(stem+L" [TTPlayer "+directory.filename().native()+L"].osz");
                if(fs::exists(root/alternative.filename())||fs::exists(alternative))Fail(ERROR_FILE_EXISTS);
                if(!MoveFileW(work.c_str(),alternative.c_str()))Fail(GetLastError());work=alternative;
            }
            if(work.native().size()>=248)Fail(ERROR_FILENAME_EXCED_RANGE);
            Journal(directory,source,exe,work,source_hash,L"prepared");context.Check();
            auto command=Quote(exe.native())+L" "+Quote(work.native());STARTUPINFOW start{sizeof(start)};PROCESS_INFORMATION process{};
            // No file association, shell, injected code or database mutation.
            if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,exe.parent_path().c_str(),&start,&process))Fail(GetLastError());
            CloseHandle(process.hThread);CloseHandle(process.hProcess);submitted=true;
            Journal(directory,source,exe,work,source_hash,L"submitted");
        }
        if(!confirmed){
            const DWORD begin=GetTickCount();
            try {while(static_cast<DWORD>(GetTickCount()-begin)<request->wait_ms){
                context.Progress(TTP_OSU_IMPORT_WAIT,GetTickCount()-begin,request->wait_ms,root.c_str());
                for(unsigned i=0;i<10;++i){context.Check();Sleep(100);}
                if(verify()){confirmed=true;break;}
            }}catch(const Failure& e){if(e.result!=HRESULT_FROM_WIN32(ERROR_CANCELLED))throw;detail=e.result;}
        }
        uint32_t retention=TTP_OSU_IMPORT_RETAINED;
        if(confirmed&&!(request->flags&TTP_OSU_IMPORT_KEEP_SOURCE)) {
            if(request->flags&TTP_OSU_IMPORT_SOURCE_REFERENCED)retention=TTP_OSU_IMPORT_REFERENCED;
            else {
                // Revalidate immediately before releasing the lock for Shell.
                // Only local fixed volumes provide the requested recycle path.
                if(!Same(identity,Information(original.h)))Fail(ERROR_FILE_INVALID);
                original.Close();detail=RecycleSource(source,identity,source_hash,context,directory,exe,work);
                retention=SUCCEEDED(detail)?TTP_OSU_IMPORT_RECYCLED:TTP_OSU_IMPORT_CLEANUP_FAILED;
            }
        }
        Journal(directory,source,exe,work,source_hash,confirmed?L"confirmed":L"pending");
        if(confirmed&&fs::exists(work)){
            // Work is ours, not the original. Never recursively delete a task.
            const TtpOsuLibraryHost quiet{sizeof(quiet)};const library::Context cleanup{quiet};
            if(FileHash(work,cleanup,TTP_OSU_IMPORT_CHECK_COPY)==source_hash)DeleteFileW(Native(work).c_str());
        }
        const TtpOsuImportResult result{sizeof(result),static_cast<uint32_t>(confirmed?TTP_OSU_IMPORT_CONFIRMED:TTP_OSU_IMPORT_PENDING),
            retention,already,detail,directory.c_str()};return visit(opaque,&result);
    }catch(const Failure& e){return e.result;}
     catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
     catch(const fs::filesystem_error& e){return HRESULT_FROM_WIN32(e.code().value()?e.code().value():ERROR_GEN_FAILURE);}
     catch(...){return E_FAIL;}
}
}
std::wstring Hex(const Digest& value){std::wstring text;text.reserve(64);
    for(const auto b:value){text+=L"0123456789abcdef"[b>>4];text+=L"0123456789abcdef"[b&15];}return text;}
std::wstring Fold(std::wstring_view value){if(value.empty())return {};const int count=LCMapStringW(LOCALE_INVARIANT,LCMAP_UPPERCASE,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(count<=0)Fail(ERROR_INVALID_NAME);std::wstring result(count,L'\0');
    if(LCMapStringW(LOCALE_INVARIANT,LCMAP_UPPERCASE,value.data(),static_cast<int>(value.size()),result.data(),count)!=count)Fail(ERROR_INVALID_NAME);return result;}
bool VerifyFiles(const std::vector<InstalledMember>& files,const std::vector<Member>& members,const library::Context& context){
    if(files.size()!=members.size())return false;
    try {for(const auto& file:files){context.Check();if(file.member>=members.size())return false;
        WIN32_FILE_ATTRIBUTE_DATA info{};if(!GetFileAttributesExW(Native(file.path).c_str(),GetFileExInfoStandard,&info)||
            (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
            ((uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow)!=members[file.member].size)return false;
        if(FileHash(file.path,context,TTP_OSU_IMPORT_VERIFY)!=members[file.member].hash)return false;
    }return true;}catch(const Failure& e){if(e.result==HRESULT_FROM_WIN32(ERROR_CANCELLED))throw;return false;}
}
}
extern "C" HRESULT WINAPI ttp_osu_import_query(uint32_t version,TtpOsuImportApi* out) noexcept {
    if(!out)return E_POINTER;if(version!=TTP_OSU_IMPORT_ABI)return E_NOINTERFACE;if(out->size<sizeof(*out))return E_INVALIDARG;
    const TtpOsuImportApi api{sizeof(api),TTP_OSU_IMPORT_ABI,ttp_osu::importer::Run};std::memcpy(out,&api,sizeof(api));return S_OK;
}
