#include "parser.h"
#include <cstring>
#include <cwchar>
#include <new>

namespace ttp_osu {
void CheckCanceled(const TtpOsuHost& host) {
    if (host.canceled && host.canceled(host.context))
        throw Failure{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
}
std::span<const unsigned char> ReadMember(const TtpOsuHost& host,const wchar_t* archive,const std::wstring& name) {
    CheckCanceled(host);
    const unsigned char* bytes{};uint32_t size{};
    const auto hr=host.read_member(host.context,archive,name.c_str(),&bytes,&size);
    if(FAILED(hr))throw Failure{hr};
    CheckCanceled(host);
    if(size>16U*1024*1024)throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
    if(size && !bytes)throw Failure{E_POINTER};
    return {bytes,size};
}
namespace {
template<class F> HRESULT Boundary(F&& body) noexcept {
    try {return body();}
    catch(const Failure& e){return e.result;}
    catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
    catch(...){return E_FAIL;}
}
std::vector<std::wstring> Names(const TtpOsuHost* host,const wchar_t* archive) {
    if(!host || host->size<sizeof(TtpOsuHost) || !host->read_member || !host->list_members ||
        !archive || !*archive || wcsnlen(archive,32768)==32768 ||
        _wcsicmp(std::filesystem::path(archive).extension().c_str(),L".osz"))
        throw Failure{E_INVALIDARG};
    CheckCanceled(*host);
    struct Listing {const TtpOsuHost* host;std::vector<std::wstring> names;size_t total{};HRESULT error{S_OK};} listing{host,{}};
    const auto hr=host->list_members(host->context,archive,
        [](void* opaque,const wchar_t* name)->HRESULT {
            auto& out=*static_cast<Listing*>(opaque);
            if(FAILED(out.error))return out.error;
            out.error=Boundary([&]()->HRESULT {
                CheckCanceled(*out.host);
                if(!name)return E_POINTER;
                const auto length=wcsnlen(name,32768);
                if(length==32768 || out.names.size()>=1024U*1024 || (out.total+=length)>32U*1024*1024)
                    return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
                out.names.emplace_back(name,length);return S_OK;
            });return out.error;
        },&listing);
    if(FAILED(listing.error))throw Failure{listing.error};
    if(FAILED(hr))throw Failure{hr};
    CheckCanceled(*host);return std::move(listing.names);
}
HRESULT WINAPI ResolveApi(const TtpOsuHost* host,const wchar_t* archive,
    const wchar_t* required,TtpOsuVisitMedia visitor,void* context) noexcept {
    return Boundary([&]() -> HRESULT {
        if(!visitor)return E_POINTER;
        const auto entries=Names(host,archive);
        const auto selected=Resolve(*host,archive,entries,required?required:L"");
        for(const auto& media:selected) {
            CheckCanceled(*host);
            const TtpOsuMedia item{media.beatmap.c_str(),media.audio.c_str(),media.background.c_str()};
            const auto hr=visitor(context,&item);if(FAILED(hr))return hr;
        }
        return S_OK;
    });
}
bool Image(std::span<const unsigned char> bytes) {
    return (bytes.size()>=24 && !memcmp(bytes.data(),"\x89PNG\r\n\x1a\n",8)) ||
        (bytes.size()>=4 && bytes[0]==0xff && bytes[1]==0xd8 && bytes[2]==0xff) ||
        (bytes.size()>=26 && bytes[0]=='B' && bytes[1]=='M') ||
        (bytes.size()>=13 && (!memcmp(bytes.data(),"GIF87a",6)||!memcmp(bytes.data(),"GIF89a",6)));
}
HRESULT WINAPI CoverApi(const TtpOsuHost* host,const wchar_t* archive,
    const wchar_t* required,TtpOsuVisitBytes visitor,void* context) noexcept {
    return Boundary([&]() -> HRESULT {
        if(!visitor || !required || !*required)return E_INVALIDARG;
        const auto entries=Names(host,archive);
        const auto selected=Resolve(*host,archive,entries,required);
        if(selected.empty() || selected.front().background.empty())return S_FALSE;
        const auto bytes=ReadMember(*host,archive,selected.front().background);
        if(!Image(bytes))return S_FALSE;
        return visitor(context,bytes.data(),static_cast<uint32_t>(bytes.size()));
    });
}
}
}
extern "C" HRESULT WINAPI ttp_osu_query(uint32_t version,TtpOsuApi* out) noexcept {
    if(!out)return E_POINTER;
    if(out->size<sizeof(*out))return E_INVALIDARG;
    *out={sizeof(*out),0,nullptr,nullptr};
    if(version!=TTP_OSU_ABI_VERSION)return E_NOINTERFACE;
    *out={sizeof(*out),TTP_OSU_ABI_VERSION,ttp_osu::ResolveApi,ttp_osu::CoverApi};
    return S_OK;
}
