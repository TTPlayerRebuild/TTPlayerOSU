#include "library_internal.h"
#include "realm_library.h"
#include <cstring>
#include <cwchar>
#include <new>

namespace ttp_osu::library {
void Context::Check() const {
    if(host.canceled && host.canceled(host.context))
        throw Failure{HRESULT_FROM_WIN32(ERROR_CANCELLED)};
}
void Context::Progress(uint32_t phase,uint64_t done,uint64_t total,const wchar_t* current) const {
    Check();
    if(host.progress) {
        const HRESULT result=host.progress(host.context,phase,done,total,current?current:L"");
        if(FAILED(result)) throw Failure{result};
    }
    Check();
}
namespace {
HRESULT WINAPI Enumerate(const TtpOsuLibraryHost* host,const TtpOsuLibraryRequest* request,
    TtpOsuVisitLibraryTrack visit,void* user) noexcept {
    try {
        if(!host || host->size<sizeof(*host) || !request || request->size<sizeof(*request) ||
            !visit || request->flags || (request->source!=TTP_OSU_LIBRARY_STABLE &&
            request->source!=TTP_OSU_LIBRARY_LAZER)) return E_INVALIDARG;
        if(request->root && wcsnlen(request->root,32768)==32768) return E_INVALIDARG;
        const Context context{*host};context.Progress(TTP_OSU_LIBRARY_DISCOVER,0,0,request->root);
        const bool stable=request->source==TTP_OSU_LIBRARY_STABLE;
        const auto root=request->root&&*request->root ? std::filesystem::path(request->root) :
            stable?DiscoverStableRoot(context):DiscoverLazerRoot(context);
        auto records=stable?EnumerateStable(root,context):EnumerateLazer(root,context);
        context.Check();
        for(size_t index=0;index<records.size();++index) {
            const auto& record=records[index];
            context.Progress(TTP_OSU_LIBRARY_DELIVER,index,records.size(),record.audio.c_str());
            const TtpOsuLibraryTrack track{sizeof(TtpOsuLibraryTrack),request->source,
                record.identity.c_str(),record.audio.c_str(),record.beatmap.c_str(),record.background.c_str(),
                record.title.c_str(),record.artist.c_str(),record.album.c_str(),record.creator.c_str(),
                record.duration_ms,record.file_size,record.beatmap_set_id};
            const HRESULT result=visit(user,&track);if(FAILED(result))return result;
        }
        context.Progress(TTP_OSU_LIBRARY_DELIVER,records.size(),records.size(),root.c_str());
        return S_OK;
    } catch(const Failure& error) {return error.result;}
      catch(const std::bad_alloc&) {return E_OUTOFMEMORY;}
      catch(const std::filesystem::filesystem_error& error) {
        return HRESULT_FROM_WIN32(error.code().value()?static_cast<DWORD>(error.code().value()):ERROR_READ_FAULT);
      } catch(...) {return E_FAIL;}
}
}
}
extern "C" HRESULT WINAPI ttp_osu_library_query(uint32_t version,TtpOsuLibraryApi* out) noexcept {
    if(!out)return E_POINTER;
    if(version!=TTP_OSU_LIBRARY_ABI)return E_NOINTERFACE;
    if(out->size<sizeof(*out))return E_INVALIDARG;
    const TtpOsuLibraryApi api{sizeof(api),TTP_OSU_LIBRARY_ABI,ttp_osu::library::Enumerate};
    std::memcpy(out,&api,sizeof(api));return S_OK;
}
