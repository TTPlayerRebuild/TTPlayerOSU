#include <ttp_osu/download_page.h>
#include <string_view>
#include <string>
#include <algorithm>
#include <cwchar>
#include <new>
#include <cstddef>

namespace {
struct Url { std::wstring_view host, path; };
bool Equal(std::wstring_view a, std::wstring_view b) {
    return a.size()==b.size() && !_wcsnicmp(a.data(),b.data(),a.size());
}
Url Parse(const wchar_t* input) {
    if(!input)return {};
    const size_t length=wcsnlen_s(input,8193);
    if(!length||length>8192)return {};
    std::wstring_view url(input,length);
    if(std::any_of(url.begin(),url.end(),[](wchar_t c){return c<=32||c==127||c==L'\\';}))return {};
    const auto scheme=url.find(L"://");
    if(scheme==url.npos||!Equal(url.substr(0,scheme),L"https"))return {};
    url.remove_prefix(scheme+3);
    const auto slash=url.find_first_of(L"/?#");
    auto host=url.substr(0,slash);
    if(host.ends_with(L":443"))host.remove_suffix(4);
    if(host.empty()||host.find_first_of(L"@:%")!=host.npos)return {};
    if(slash==url.npos)return {host,L"/"};
    auto path=url.substr(slash);path=path.substr(0,path.find_first_of(L"?#"));
    return {host,path.empty()?L"/":path};
}
HRESULT WINAPI Page(TtpOsuDownloadPage* page) {
    if(!page||page->size<sizeof(*page))return E_INVALIDARG;
    *page={sizeof(*page),L"Sayobot osu! 镜像站",L"https://osu.sayobot.cn/home"};
    return S_OK;
}
HRESULT WINAPI Recognize(const wchar_t* page,const wchar_t* target,wchar_t* filename,uint32_t capacity) {
    if(!filename||!capacity)return E_POINTER;
    filename[0]=0;
    try {
        if(!Equal(Parse(page).host,L"osu.sayobot.cn"))return S_FALSE;
        const auto url=Parse(target);
        if(!Equal(url.host,L"txy1.sayobot.cn")&&!Equal(url.host,L"dl.sayobot.cn"))return S_FALSE;
        constexpr std::wstring_view prefix=L"/beatmaps/download/";
        if(!url.path.starts_with(prefix))return S_FALSE;
        auto rest=url.path.substr(prefix.size());const auto slash=rest.find(L'/');
        if(slash==rest.npos)return S_FALSE;
        const auto type=rest.substr(0,slash),id=rest.substr(slash+1);
        if(type!=L"full"&&type!=L"novideo"&&type!=L"mini")return S_FALSE;
        if(id.empty()||id.size()>10||id.front()==L'0'||
            !std::all_of(id.begin(),id.end(),[](wchar_t c){return c>=L'0'&&c<=L'9';}))return S_FALSE;
        const auto name=L"Sayobot "+std::wstring(id)+(type==L"full"?L"":L" - "+std::wstring(type))+L".osz";
        if(name.size()+1>capacity)return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        std::copy(name.begin(),name.end(),filename);filename[name.size()]=0;return S_OK;
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}catch(...){return E_FAIL;}
}
const wchar_t* WINAPI Script() {
    static const wchar_t script[] =
#include "download_page_script.inc"
    ;return script;
}
HRESULT WINAPI RecognizeAudio(const wchar_t* page,const wchar_t* target) {
    if(!Equal(Parse(page).host,L"osu.sayobot.cn"))return S_FALSE;
    const auto url=Parse(target);
    constexpr std::wstring_view prefix=L"/beatmaps/files/";
    if(!Equal(url.host,L"dl.sayobot.cn")||!url.path.starts_with(prefix))return S_FALSE;
    auto path=url.path.substr(prefix.size());const auto slash=path.find(L'/');
    if(slash==path.npos||slash==0||slash>10||path.front()==L'0')return S_FALSE;
    for(const auto c:path.substr(0,slash))if(c<L'0'||c>L'9')return S_FALSE;
    path.remove_prefix(slash+1);
    if(path.empty()||path.front()==L'/'||path.find(L"../")!=path.npos||path.find(L"/..")!=path.npos)return S_FALSE;
    for(size_t i=0;i+2<path.size();++i)if(path[i]==L'%'&&
        (Equal(path.substr(i,3),L"%2e")||Equal(path.substr(i,3),L"%2f")||Equal(path.substr(i,3),L"%5c")))return S_FALSE;
    const auto dot=path.rfind(L'.');if(dot==path.npos)return S_FALSE;
    for(const auto ext:{L".mp3",L".ogg",L".wav",L".flac",L".m4a",L".aac",L".opus",L".wma"})
        if(Equal(path.substr(dot),ext))return S_OK;
    return S_FALSE;
}
}
extern "C" HRESULT WINAPI ttp_osu_download_page_query(uint32_t abi,TtpOsuDownloadPageApi* api) {
    if(abi!=1&&abi!=2)return E_NOINTERFACE;
    const auto size=abi==1?TTP_OSU_DOWNLOAD_PAGE_ABI1_SIZE:sizeof(*api);
    if(!api||api->size<size)return E_INVALIDARG;
    const TtpOsuDownloadPageApi value{static_cast<uint32_t>(size),abi,Page,Recognize,Script,RecognizeAudio};
    memcpy(api,&value,size);return S_OK;
}
