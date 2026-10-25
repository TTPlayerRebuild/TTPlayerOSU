#include "visual_scene.h"
#include "visual_renderer.h"
#include "parser.h"
#include <algorithm>
#include <cwchar>
#include <cstring>
#include <mutex>
#include <new>

namespace ttp_osu::visual {
namespace {
struct Instance {
    std::mutex mutex;
    Scene scene;
    Renderer renderer;
    uint64_t timing_request{};
};
template<class F> HRESULT Boundary(F&& operation) noexcept {
    try { return operation(); }
    catch (const Failure& error) { return error.result; }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
Instance& Get(void* opaque) {
    if (!opaque) throw Failure{E_POINTER};
    return *static_cast<Instance*>(opaque);
}
HRESULT WINAPI Create(void** out) noexcept {
    if (!out) return E_POINTER;
    *out = nullptr;
    return Boundary([&]() -> HRESULT { *out = new Instance; return S_OK; });
}
void WINAPI Destroy(void* opaque) noexcept {
    // Host owns lifetime and must quiesce callbacks before destroy. Graphics
    // destruction must run on the same UI thread used for present/detach.
    if (!opaque) return;
    auto* instance = static_cast<Instance*>(opaque);
    {
        std::lock_guard lock(instance->mutex);
        // Keep the instance valid for the owner thread to detach and retry.
        if (!instance->renderer.CanDetach()) return;
    }
    delete instance;
}
HRESULT WINAPI Configure(void* opaque, const TtpOsuVisualOptions* options) noexcept {
    return Boundary([&]() -> HRESULT {
        auto& instance = Get(opaque);
        if (!options || options->size < sizeof(*options)) return E_INVALIDARG;
        std::lock_guard lock(instance.mutex);
        instance.scene.Configure(*options);
        return S_OK;
    });
}
HRESULT WINAPI Timing(void* opaque, const uint8_t* bytes, uint32_t size) noexcept {
    return Boundary([&]() -> HRESULT {
        auto& instance = Get(opaque);
        if (size && !bytes) return E_POINTER;
        uint64_t request;
        {
            std::lock_guard lock(instance.mutex);
            request = ++instance.timing_request;
        }
        auto parsed = ParseTiming({bytes, size});
        std::lock_guard lock(instance.mutex);
        if (request != instance.timing_request) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        instance.scene.SetTiming(std::move(parsed));
        return S_OK;
    });
}
std::vector<std::wstring> ArchiveNames(const TtpOsuHost* host, const wchar_t* archive) {
    if (!host || host->size < sizeof(*host) || !host->read_member || !host->list_members ||
        !archive || !*archive || wcsnlen(archive, 32768) == 32768 ||
        _wcsicmp(std::filesystem::path(archive).extension().c_str(), L".osz"))
        throw Failure{E_INVALIDARG};
    CheckCanceled(*host);
    struct Listing {
        const TtpOsuHost* host;
        std::vector<std::wstring> names;
        size_t total{};
        HRESULT error{S_OK};
    } listing{host, {}};
    const HRESULT result = host->list_members(host->context, archive,
        [](void* context, const wchar_t* name) -> HRESULT {
            auto& out = *static_cast<Listing*>(context);
            if (FAILED(out.error)) return out.error;
            out.error = Boundary([&]() -> HRESULT {
                CheckCanceled(*out.host);
                if (!name) return E_POINTER;
                const size_t length = wcsnlen(name, 32768);
                if (length == 32768 || out.names.size() >= 1024U * 1024 ||
                    (out.total += length) > 32U * 1024 * 1024)
                    return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
                out.names.emplace_back(name, length);
                return S_OK;
            });
            return out.error;
        }, &listing);
    if (FAILED(listing.error)) throw Failure{listing.error};
    if (FAILED(result)) throw Failure{result};
    CheckCanceled(*host);
    // Preserve the OSZ ABI 1 Resolve ordering: timing, audio and cover must all
    // choose the same first matching beatmap in the archive's enumeration order.
    return std::move(listing.names);
}
HRESULT WINAPI ArchiveTiming(void* opaque, const TtpOsuHost* host, const wchar_t* archive,
    const wchar_t* audio) noexcept {
    return Boundary([&]() -> HRESULT {
        auto& instance = Get(opaque);
        if (audio && wcsnlen(audio, 32768) == 32768) return E_INVALIDARG;
        uint64_t request;
        {
            std::lock_guard lock(instance.mutex);
            request = ++instance.timing_request;
            // A new source must not show the previous song's Kiai while archive
            // reads or a password interaction are pending.
            instance.scene.SetTiming({});
        }
        // All host callbacks and archive I/O happen without the scene mutex.
        const auto names = ArchiveNames(host, archive);
        const auto selected = Resolve(*host, archive, names, audio ? audio : L"");
        std::vector<TimingPoint> points;
        if (!selected.empty()) points = ParseTiming(ReadMember(*host, archive, selected.front().beatmap));
        CheckCanceled(*host);
        std::lock_guard lock(instance.mutex);
        if (request != instance.timing_request) return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        instance.scene.SetTiming(std::move(points));
        return selected.empty() ? S_FALSE : S_OK;
    });
}
HRESULT WINAPI SetImage(void* opaque, const TtpOsuVisualImage* image) noexcept {
    return Boundary([&]() -> HRESULT {
        auto& instance = Get(opaque);
        if (image && image->size < sizeof(*image)) return E_INVALIDARG;
        std::lock_guard lock(instance.mutex);
        instance.scene.SetImage(image);
        return S_OK;
    });
}
HRESULT WINAPI Update(void* opaque, const TtpOsuVisualFrame* frame) noexcept {
    return Boundary([&]() -> HRESULT {
        auto& instance = Get(opaque);
        if (!frame || frame->size < sizeof(*frame)) return E_INVALIDARG;
        std::lock_guard lock(instance.mutex);
        instance.scene.Update(*frame);
        return S_OK;
    });
}
HRESULT WINAPI Paint(void* opaque, HDC dc, const RECT* area) noexcept {
    return Boundary([&]() -> HRESULT {
        auto& instance = Get(opaque);
        if (!dc || !area || area->right <= area->left || area->bottom <= area->top ||
            static_cast<int64_t>(area->right) - area->left > 16384 ||
            static_cast<int64_t>(area->bottom) - area->top > 16384) return E_INVALIDARG;
        std::lock_guard lock(instance.mutex);
        return instance.renderer.Paint(dc, *area,
            instance.scene.BuildFrame(static_cast<uint32_t>(area->right - area->left),
                static_cast<uint32_t>(area->bottom - area->top)), instance.scene.Background());
    });
}
HRESULT WINAPI Present(void* opaque, HWND window) noexcept {
    return Boundary([&]() -> HRESULT {
        auto& instance = Get(opaque);
        RECT area{};
        if (!window || !IsWindow(window) || !GetClientRect(window, &area)) return E_INVALIDARG;
        if (area.right <= 0 || area.bottom <= 0) return S_FALSE;
        std::lock_guard lock(instance.mutex);
        return instance.renderer.Present(window, instance.scene.BuildFrame(
            static_cast<uint32_t>(area.right), static_cast<uint32_t>(area.bottom)), instance.scene.Background());
    });
}
void WINAPI Detach(void* opaque) noexcept {
    if (!opaque) return;
    try {
        auto& instance = *static_cast<Instance*>(opaque);
        std::lock_guard lock(instance.mutex);
        instance.renderer.Detach();
    } catch (...) {}
}
HRESULT WINAPI Stats(void* opaque, TtpOsuVisualStats* out) noexcept {
    return Boundary([&]() -> HRESULT {
        auto& instance = Get(opaque);
        if (!out || out->size < sizeof(*out)) return E_INVALIDARG;
        std::lock_guard lock(instance.mutex);
        *out = instance.scene.Stats();
        out->backend = instance.renderer.Backend();
        return S_OK;
    });
}
HRESULT WINAPI SetLogo(void* opaque, uint32_t kind, const TtpOsuVisualImage* image) noexcept {
    return Boundary([&]() -> HRESULT {
        auto& instance = Get(opaque);
        std::lock_guard lock(instance.mutex);
        instance.renderer.SetLogo(kind, image);
        instance.scene.SetLogo(kind, image ? image->width : 0, image ? image->height : 0);
        return S_OK;
    });
}
HRESULT WINAPI Transition(void* opaque, uint32_t phase, uint64_t now) noexcept {
    return Boundary([&]() -> HRESULT {
        auto& instance = Get(opaque);
        std::lock_guard lock(instance.mutex);
        instance.scene.Transition(phase, now);
        return S_OK;
    });
}
HRESULT WINAPI TransitionState(void* opaque, uint64_t now, TtpOsuVisualTransitionState* out) noexcept {
    return Boundary([&]() -> HRESULT {
        auto& instance = Get(opaque);
        if (!out || out->size < sizeof(*out)) return E_INVALIDARG;
        std::lock_guard lock(instance.mutex);
        *out = instance.scene.TransitionState(now);
        return S_OK;
    });
}
}
}

extern "C" HRESULT WINAPI ttp_osu_query_visual(uint32_t version, TtpOsuVisualApi* out) noexcept {
    if (!out) return E_POINTER;
    if (version < 1 || version > TTP_OSU_VISUAL_ABI) return E_NOINTERFACE;
    const uint32_t bytes = version == 1 ? TTP_OSU_VISUAL_ABI1_SIZE :
        version == 2 ? TTP_OSU_VISUAL_ABI2_SIZE : sizeof(*out);
    if (out->size < bytes) return E_INVALIDARG;
    using namespace ttp_osu::visual;
    const TtpOsuVisualApi api{bytes, version, Create, Destroy, Configure, Timing, ArchiveTiming,
        SetImage, Update, Paint, Present, Detach, Stats, SetLogo, Transition, TransitionState};
    // Old callers own shorter tables: never touch an appended function slot.
    std::memcpy(out, &api, bytes);
    return S_OK;
}
