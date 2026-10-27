#include <ttp_osu/installation.h>
#include "installation_internal.h"
#include "executable_identity.h"
#include "storage_config.h"
#include "library_internal.h"
#include <shlobj.h>
#include <cstring>
#include <new>

namespace ttp_osu::installation {
namespace fs=std::filesystem;
std::wstring ExpandPath(std::wstring_view value) {
    const std::wstring source(value);
    const DWORD count=ExpandEnvironmentStringsW(source.c_str(),nullptr,0);
    if(!count || count>32768)return source;
    std::wstring expanded(count,L'\0');
    const DWORD copied=ExpandEnvironmentStringsW(source.c_str(),expanded.data(),count);
    if(!copied || copied>count)return source;
    expanded.resize(copied-1);return expanded;
}
std::wstring NormalizePath(std::wstring_view value) {
    while(!value.empty() && iswspace(value.front()))value.remove_prefix(1);
    while(!value.empty() && iswspace(value.back()))value.remove_suffix(1);
    if(value.size()>=2 && value.front()==L'"' && value.back()==L'"')
        value=value.substr(1,value.size()-2);
    return ExpandPath(value);
}
namespace {
fs::path SpecialFolder(int id) {
    wchar_t path[MAX_PATH]{};
    if(FAILED(SHGetFolderPathW(nullptr,id,nullptr,SHGFP_TYPE_CURRENT,path)))
        throw Failure{HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)};
    return path;
}
bool Regular(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(path,error);
}
bool ValidString(const wchar_t* text) noexcept {
    return !text || wcsnlen(text,32768)<32768;
}
template<class F> HRESULT Guard(F&& body) noexcept {
    try {return body();}
    catch(const Failure& error){return error.result;}
    catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
    catch(const fs::filesystem_error& error) {
        return HRESULT_FROM_WIN32(error.code().value()?error.code().value():ERROR_READ_FAULT);
    } catch(...){return E_FAIL;}
}
}
fs::path DefaultExecutable(unsigned source) {
    const auto local=SpecialFolder(CSIDL_LOCAL_APPDATA);
    if(source==TTP_OSU_EXECUTABLE_STABLE)return local/L"osu!"/L"osu!.exe";
    const auto root=local/L"osulazer";
    const auto current=root/L"current"/L"osu!.exe";
    if(Regular(current) || !Regular(root/L"osu!.exe"))return current;
    return root/L"osu!.exe";
}
fs::path DefaultLazerRoamingRoot() {return SpecialFolder(CSIDL_APPDATA)/L"osu";}
fs::path LazerDataRoot(const fs::path& roaming,const fs::path& executable) {
    const auto program=executable.parent_path();
    const auto base=!program.empty() && Regular(program/L"framework.ini")?program:roaming;
    const auto redirected=base.empty()?std::wstring{}:ReadLazerStorageRoot(base/L"storage.ini");
    return redirected.empty()?base:fs::path(redirected);
}
namespace {
HRESULT WINAPI Identify(const wchar_t* path,uint32_t* kind) noexcept {
    if(!kind)return E_POINTER;
    *kind=TTP_OSU_EXECUTABLE_NONE;
    if(!ValidString(path))return E_INVALIDARG;
    *kind=static_cast<uint32_t>(IdentifyOsuExecutable(path?path:L""));
    return *kind?S_OK:S_FALSE;
}
HRESULT WINAPI Discover(const TtpOsuInstallationRequest* request,
                       TtpOsuVisitInstallation visit,void* user) noexcept {
    return Guard([&]() -> HRESULT {
        if(!request || request->size<sizeof(*request) || request->flags || !visit ||
           (request->source!=1 && request->source!=2) ||
           !ValidString(request->executable) || !ValidString(request->roaming_root))return E_INVALIDARG;
        const auto executable=request->executable?fs::path(NormalizePath(request->executable)):
            DefaultExecutable(request->source);
        fs::path directory;
        if(request->source==TTP_OSU_EXECUTABLE_STABLE) {
            if(!executable.empty())directory=library::StableSongsDirectory(executable.parent_path());
        } else {
            const auto roaming=request->roaming_root?fs::path(NormalizePath(request->roaming_root)):
                DefaultLazerRoamingRoot();
            const auto root=LazerDataRoot(roaming,executable);
            if(!root.empty())directory=root/L"files";
        }
        const auto kind=static_cast<uint32_t>(IdentifyOsuExecutable(executable.native()));
        const TtpOsuInstallation result{sizeof(result),request->source,kind,
            executable.c_str(),directory.c_str()};
        return visit(user,&result);
    });
}
}
}
extern "C" HRESULT WINAPI ttp_osu_installation_query(uint32_t version,TtpOsuInstallationApi* out) noexcept {
    if(!out)return E_POINTER;
    if(version!=TTP_OSU_INSTALLATION_ABI)return E_NOINTERFACE;
    if(out->size<sizeof(*out))return E_INVALIDARG;
    const TtpOsuInstallationApi api{sizeof(api),TTP_OSU_INSTALLATION_ABI,
        ttp_osu::installation::Identify,ttp_osu::installation::Discover};
    std::memcpy(out,&api,sizeof(api));return S_OK;
}
