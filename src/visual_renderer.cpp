#include "visual_renderer.h"
#include "visual_scene.h"
#include "parser.h"
#include <d3d9.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <stdexcept>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace ttp_osu::visual {
namespace {
constexpr float pi = 3.14159265358979323846f;
constexpr size_t texture_count = static_cast<size_t>(Texture::Welcome) + 1;
constexpr uint64_t max_cpu_pixels = 16ull * 1024 * 1024;
constexpr size_t maximum_quads = 8192;

uint8_t Byte(float value) {
    return static_cast<uint8_t>(std::clamp(value, 0.f, 255.f) + 0.5f);
}
uint32_t Premultiplied(float r, float g, float b, float alpha) {
    const float a = std::clamp(alpha, 0.f, 1.f);
    return (uint32_t(Byte(a * 255)) << 24) | (uint32_t(Byte(r * a)) << 16) |
        (uint32_t(Byte(g * a)) << 8) | Byte(b * a);
}
Image MakeImage(uint32_t width, uint32_t height) {
    Image image;
    image.width = width;
    image.height = height;
    image.revision = 1;
    image.pixels.resize(static_cast<size_t>(width) * height * 4);
    return image;
}
void Pixel(Image& image, uint32_t x, uint32_t y, uint32_t color) {
    std::memcpy(image.pixels.data() + (static_cast<size_t>(y) * image.width + x) * 4,
        &color, sizeof(color));
}
bool ValidImage(const Image& image) {
    return image.width && image.height && image.width <= 16384 && image.height <= 16384 &&
        uint64_t(image.width) * image.height * 4 == image.pixels.size();
}
float RoundedBox(float x, float y, float half_width, float half_height, float radius) {
    const float dx = std::abs(x) - half_width + radius;
    const float dy = std::abs(y) - half_height + radius;
    return std::hypot(std::max(dx, 0.f), std::max(dy, 0.f)) +
        std::min(std::max(dx, dy), 0.f) - radius;
}

// The user-supplied PNG is embedded byte-for-byte. Decode its native dimensions
// and alpha without drawing text, recoloring or substituting extracted osu! art.
Image MakeLogo(uint32_t id = 101) {
    const HMODULE module = reinterpret_cast<HMODULE>(&__ImageBase);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    const DWORD size = resource ? SizeofResource(module, resource) : 0;
    HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes || !size) throw std::runtime_error("Missing embedded TTPlayer logo");
    struct Startup {
        ULONG_PTR token{};
        Startup() {
            Gdiplus::GdiplusStartupInput input;
            if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok)
                throw std::runtime_error("GDI+ startup failed");
        }
        ~Startup() { Gdiplus::GdiplusShutdown(token); }
    } startup;
    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!storage) throw std::bad_alloc();
    void* writable = GlobalLock(storage);
    if (!writable) { GlobalFree(storage); throw std::bad_alloc(); }
    std::memcpy(writable, bytes, size);
    GlobalUnlock(storage);
    struct Stream {
        IStream* value{};
        ~Stream() { if (value) value->Release(); }
    } stream;
    if (FAILED(CreateStreamOnHGlobal(storage, TRUE, &stream.value))) {
        GlobalFree(storage); throw std::runtime_error("Logo stream failed");
    }
    Gdiplus::Bitmap bitmap(stream.value, FALSE);
    if (bitmap.GetLastStatus() != Gdiplus::Ok || !bitmap.GetWidth() || !bitmap.GetHeight() ||
        bitmap.GetWidth() > 4096 || bitmap.GetHeight() > 4096)
        throw std::runtime_error("Invalid embedded TTPlayer logo");
    Image image = MakeImage(bitmap.GetWidth(), bitmap.GetHeight());
    Gdiplus::Rect bounds(0, 0, static_cast<INT>(image.width), static_cast<INT>(image.height));
    Gdiplus::BitmapData data{};
    if (bitmap.LockBits(&bounds, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) != Gdiplus::Ok)
        throw std::runtime_error("Logo pixels unavailable");
    for (uint32_t y = 0; y < image.height; ++y) {
        const auto* source = static_cast<const uint8_t*>(data.Scan0) + ptrdiff_t(y) * data.Stride;
        std::memcpy(image.pixels.data() + size_t(y) * image.width * 4, source, size_t(image.width) * 4);
    }
    bitmap.UnlockBits(&data);
    return image;
}
Image MakeSpectrum() {
    Image image = MakeImage(128, 32);
    for (uint32_t y = 0; y < 32; ++y) for (uint32_t x = 0; x < 128; ++x) {
        const float outer = RoundedBox(float(x) - 63.5f, float(y) - 8.f, 62.f, 7.f, 5.f);
        const float outline = std::clamp(1.f - std::abs(outer), 0.f, 1.f);
        const float solid = 0.7f * std::clamp(0.5f - RoundedBox(float(x) - 63.5f,
            float(y) - 25.f, 61.5f, 2.f, 2.f), 0.f, 1.f);
        Pixel(image, x, y, Premultiplied(255, 255, 255, std::max(outline, solid)));
    }
    return image;
}
Image MakeStar() {
    Image image = MakeImage(64, 64);
    std::array<float, 10> px{}, py{};
    for (size_t i = 0; i < px.size(); ++i) {
        const float angle = -pi / 2 + float(i) * pi / 5;
        const float radius = (i & 1) ? 11.8f : 27.f;
        px[i] = std::cos(angle) * radius + 31.5f;
        py[i] = std::sin(angle) * radius + 31.5f;
    }
    for (uint32_t y = 0; y < 64; ++y) for (uint32_t x = 0; x < 64; ++x) {
        bool inside = false;
        float distance = 100.f;
        for (size_t i = 0, j = 9; i < 10; j = i++) {
            const float dx = px[j] - px[i], dy = py[j] - py[i];
            const float t = std::clamp(((float(x) - px[i]) * dx + (float(y) - py[i]) * dy) /
                (dx * dx + dy * dy), 0.f, 1.f);
            distance = std::min(distance, std::hypot(float(x) - px[i] - t * dx,
                float(y) - py[i] - t * dy));
            if ((py[i] > float(y)) != (py[j] > float(y)) &&
                float(x) < (px[j] - px[i]) * (float(y) - py[i]) / (py[j] - py[i]) + px[i])
                inside = !inside;
        }
        const float alpha = inside ? std::min(1.f, 0.5f + distance) :
            std::max(std::max(0.f, 0.5f - distance), 0.2f * std::exp(-distance * distance / 12.f));
        Pixel(image, x, y, Premultiplied(235, 246, 255, alpha));
    }
    return image;
}
Image MakeFlash() {
    Image image = MakeImage(256, 192);
    for (uint32_t y = 0; y < image.height; ++y) for (uint32_t x = 0; x < image.width; ++x) {
        // The default orientation is the right edge; the scene mirrors the
        // left-hand quad. This keeps the strongest flash at the screen edge.
        const float horizontal = float(x) / 255.f;
        const float vertical = std::sin(pi * (float(y) + 0.5f) / 192.f);
        Pixel(image, x, y, Premultiplied(255, 255, 255,
            horizontal * horizontal * std::pow(vertical, 1.3f)));
    }
    return image;
}
Image MakeShockwave() {
    Image image = MakeImage(256, 256);
    for (uint32_t y = 0; y < 256; ++y) for (uint32_t x = 0; x < 256; ++x) {
        const float d = (std::hypot(float(x) - 127.5f, float(y) - 127.5f) - 125.f) / 2.f;
        Pixel(image, x, y, Premultiplied(255, 255, 255, std::exp(-d * d)));
    }
    return image;
}
uint32_t TintPremultiplied(uint32_t argb) {
    const uint32_t a = argb >> 24;
    return (a << 24) | ((((argb >> 16) & 255) * a / 255) << 16) |
        ((((argb >> 8) & 255) * a / 255) << 8) | ((argb & 255) * a / 255);
}
bool ValidQuad(const Quad& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.width) &&
        std::isfinite(q.height) && std::isfinite(q.rotation) && q.width > 0 && q.height > 0 &&
        std::abs(q.x) <= 1000000 && std::abs(q.y) <= 1000000 && q.width <= 1000000 &&
        q.height <= 1000000 && static_cast<size_t>(q.texture) < texture_count && (q.color >> 24);
}
std::array<float, 4> Sample(const Image& image, float u, float v) {
    const float fx = std::clamp(u * float(image.width) - 0.5f, 0.f, float(image.width - 1));
    const float fy = std::clamp(v * float(image.height) - 0.5f, 0.f, float(image.height - 1));
    const uint32_t x0 = static_cast<uint32_t>(fx), y0 = static_cast<uint32_t>(fy);
    const uint32_t x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
    const float tx = fx - float(x0), ty = fy - float(y0);
    const uint8_t* a = image.pixels.data() + (size_t(y0) * image.width + x0) * 4;
    const uint8_t* b = image.pixels.data() + (size_t(y0) * image.width + x1) * 4;
    const uint8_t* c = image.pixels.data() + (size_t(y1) * image.width + x0) * 4;
    const uint8_t* d = image.pixels.data() + (size_t(y1) * image.width + x1) * 4;
    std::array<float, 4> result{};
    for (size_t n = 0; n < 4; ++n)
        result[n] = (float(a[n]) + (float(b[n]) - a[n]) * tx) * (1 - ty) +
            (float(c[n]) + (float(d[n]) - c[n]) * tx) * ty;
    return result;
}
}

struct Renderer::Impl {
    std::array<Image, texture_count> images;
    bool assets_ready{};
    uint32_t backend{};
    HDC memory_dc{};
    HBITMAP dib{};
    HGDIOBJ previous_bitmap{};
    uint8_t* dib_pixels{};
    uint32_t dib_width{}, dib_height{};
    DWORD dib_thread{};
    HMODULE d3d_module{};
    IDirect3D9* d3d{};
    IDirect3DDevice9* device{};
    HWND device_window{};
    DWORD device_thread{};
    D3DPRESENT_PARAMETERS presentation{};
    D3DCAPS9 caps{};
    struct GpuTexture { IDirect3DTexture9* value{}; uint64_t revision{}; uint32_t width{}, height{}; };
    std::array<GpuTexture, texture_count> textures;
    struct Vertex { float x, y, z, rhw; DWORD color; float u, v; };
    std::vector<Vertex> vertices;
    ~Impl() {
        Detach();
        if (memory_dc && previous_bitmap) SelectObject(memory_dc, previous_bitmap);
        if (dib) DeleteObject(dib);
        if (memory_dc) DeleteDC(memory_dc);
    }
    bool CanDetach() const noexcept { return !device_thread || device_thread == GetCurrentThreadId(); }
    void Detach() {
        // The public ABI refuses destruction from another device thread. This
        // defensive guard also prevents a wrong-thread D3D release if misused.
        if (!CanDetach()) return;
        for (auto& t : textures) { if (t.value) t.value->Release(); t = {}; }
        if (device) device->Release();
        if (d3d) d3d->Release();
        if (d3d_module) FreeLibrary(d3d_module);
        device = nullptr; d3d = nullptr; d3d_module = nullptr;
        device_window = nullptr; device_thread = 0; presentation = {}; caps = {};
        if (backend == 2) backend = 0;
    }
    void Assets() {
        if (assets_ready) return;
        images[static_cast<size_t>(Texture::White)] = MakeImage(2, 2);
        std::fill(images[static_cast<size_t>(Texture::White)].pixels.begin(),
            images[static_cast<size_t>(Texture::White)].pixels.end(), uint8_t(255));
        if (images[static_cast<size_t>(Texture::Logo)].pixels.empty())
            images[static_cast<size_t>(Texture::Logo)] = MakeLogo();
        images[static_cast<size_t>(Texture::Spectrum)] = MakeSpectrum();
        images[static_cast<size_t>(Texture::Star)] = MakeStar();
        images[static_cast<size_t>(Texture::Flash)] = MakeFlash();
        images[static_cast<size_t>(Texture::Shockwave)] = MakeShockwave();
        vertices.reserve(6 * 2048);
        assets_ready = true;
    }
    void TransitionAssets(const RenderFrame& frame) {
        auto& welcome = images[static_cast<size_t>(Texture::Welcome)];
        if (welcome.pixels.empty() && std::any_of(frame.quads.begin(), frame.quads.end(),
            [](const Quad& quad) { return quad.texture == Texture::Welcome && ValidQuad(quad); }))
            welcome = MakeLogo(103);
    }
    const Image& GetImage(Texture texture, const Image& background) const {
        return texture == Texture::Background ? background : images[static_cast<size_t>(texture)];
    }
    HRESULT Dib(uint32_t width, uint32_t height) {
        // A compatible DC created with a null source belongs to its creating
        // thread. A host may replace its visual worker between fullscreen and
        // main-window rendering; do not reuse that worker's DC on another one.
        if (memory_dc && dib_thread != GetCurrentThreadId()) {
            if (previous_bitmap) SelectObject(memory_dc, previous_bitmap);
            if (dib) DeleteObject(dib);
            DeleteDC(memory_dc);
            memory_dc = nullptr; dib = nullptr; previous_bitmap = nullptr;
            dib_pixels = nullptr; dib_width = dib_height = 0;
        }
        if (dib && dib_width == width && dib_height == height) return S_OK;
        if (!memory_dc) {
            memory_dc = CreateCompatibleDC(nullptr);
            if (!memory_dc) return E_OUTOFMEMORY;
            dib_thread = GetCurrentThreadId();
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = static_cast<LONG>(width);
        info.bmiHeader.biHeight = -static_cast<LONG>(height);
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* pixels{};
        HBITMAP replacement = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!replacement || !pixels) { if (replacement) DeleteObject(replacement); return E_OUTOFMEMORY; }
        HGDIOBJ old = SelectObject(memory_dc, replacement);
        if (!old || old == HGDI_ERROR) { DeleteObject(replacement); return E_FAIL; }
        if (!previous_bitmap) previous_bitmap = old;
        if (dib) DeleteObject(dib);
        dib = replacement; dib_pixels = static_cast<uint8_t*>(pixels);
        dib_width = width; dib_height = height;
        return S_OK;
    }
    void DrawCpu(const Quad& q, const Image& image) {
        const float cosine = std::cos(q.rotation), sine = std::sin(q.rotation);
        const float rx = (std::abs(cosine) * q.width + std::abs(sine) * q.height) * 0.5f;
        const float ry = (std::abs(sine) * q.width + std::abs(cosine) * q.height) * 0.5f;
        const int left = std::max(0, int(std::floor(q.x - rx)));
        const int top = std::max(0, int(std::floor(q.y - ry)));
        const int right = std::min(int(dib_width), int(std::ceil(q.x + rx)));
        const int bottom = std::min(int(dib_height), int(std::ceil(q.y + ry)));
        if (left >= right || top >= bottom) return;
        const float alpha = float(q.color >> 24) / 255.f;
        const std::array<float, 3> tint{float(q.color & 255) / 255.f * alpha,
            float((q.color >> 8) & 255) / 255.f * alpha,
            float((q.color >> 16) & 255) / 255.f * alpha};
        const float inverse_width = 1.f / q.width, inverse_height = 1.f / q.height;
        for (int y = top; y < bottom; ++y) {
            const float dy = float(y) + 0.5f - q.y;
            // Long, narrow spectrum sprites can have a huge rotated bounding
            // box. Intersect each scanline with the two local axes first, so
            // CPU work follows the actual bar area rather than length squared.
            float minimum_x = float(left) + 0.5f - q.x;
            float maximum_x = float(right) - 0.5f - q.x;
            const auto intersect = [&](float coefficient, float bias, float extent) {
                if (std::abs(coefficient) < 0.000001f) return std::abs(bias) <= extent + 0.0001f;
                float a = (-extent - bias) / coefficient, b = (extent - bias) / coefficient;
                if (a > b) std::swap(a, b);
                minimum_x = std::max(minimum_x, a - 0.0001f);
                maximum_x = std::min(maximum_x, b + 0.0001f);
                return minimum_x <= maximum_x;
            };
            if (!intersect(cosine, dy * sine, q.width * 0.5f) ||
                !intersect(-sine, dy * cosine, q.height * 0.5f)) continue;
            const int row_left = std::max(left, int(std::ceil(minimum_x + q.x - 0.5f)));
            const int row_right = std::min(right, int(std::floor(maximum_x + q.x - 0.5f)) + 1);
            if (row_left >= row_right) continue;
            auto* dst = dib_pixels + (size_t(y) * dib_width + row_left) * 4;
            for (int x = row_left; x < row_right; ++x, dst += 4) {
                const float dx = float(x) + 0.5f - q.x;
                float u = (dx * cosine + dy * sine) * inverse_width + 0.5f;
                const float v = (-dx * sine + dy * cosine) * inverse_height + 0.5f;
                if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
                if (q.flip_x) u = 1 - u;
                const auto src = Sample(image, u, v);
                const float remaining = q.additive ? 1.f : 1.f - src[3] * alpha / 255.f;
                for (size_t c = 0; c < 3; ++c) dst[c] = Byte(src[c] * tint[c] + dst[c] * remaining);
                dst[3] = 255;
            }
        }
    }
    HRESULT Device(HWND window, uint32_t width, uint32_t height) {
        if (GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
        if (!CanDetach()) return RPC_E_WRONG_THREAD;
        if (device && device_window != window) Detach();
        if (!device) {
            // Use an absolute system path. XP lacks LOAD_LIBRARY_SEARCH_SYSTEM32
            // and a relative LoadLibrary would permit a local d3d9.dll preload.
            wchar_t directory[MAX_PATH + 1]{};
            const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
            if (!length || length >= MAX_PATH - 10) return E_FAIL;
            const std::wstring path = std::wstring(directory, length) + L"\\d3d9.dll";
            d3d_module = LoadLibraryW(path.c_str());
            if (!d3d_module) return HRESULT_FROM_WIN32(GetLastError());
            using CreateD3D = IDirect3D9* (WINAPI*)(UINT);
            const auto create = reinterpret_cast<CreateD3D>(GetProcAddress(d3d_module, "Direct3DCreate9"));
            if (!create || !(d3d = create(D3D_SDK_VERSION))) { Detach(); return E_NOINTERFACE; }
            presentation = {};
            presentation.Windowed = TRUE; presentation.SwapEffect = D3DSWAPEFFECT_DISCARD;
            presentation.hDeviceWindow = window;
            presentation.BackBufferWidth = width; presentation.BackBufferHeight = height;
            presentation.BackBufferFormat = D3DFMT_UNKNOWN;
            presentation.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            HRESULT result = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                &presentation, &device);
            if (FAILED(result)) { Detach(); return result; }
            device_window = window; device_thread = GetCurrentThreadId();
            result = device->GetDeviceCaps(&caps);
            if (FAILED(result)) { Detach(); return result; }
        }
        const HRESULT cooperative = device->TestCooperativeLevel();
        if (cooperative == D3DERR_DEVICELOST) return cooperative;
        if (FAILED(cooperative) && cooperative != D3DERR_DEVICENOTRESET) return cooperative;
        if (cooperative == D3DERR_DEVICENOTRESET || presentation.BackBufferWidth != width ||
            presentation.BackBufferHeight != height) {
            presentation.BackBufferWidth = width; presentation.BackBufferHeight = height;
            const HRESULT reset = device->Reset(&presentation);
            if (FAILED(reset)) return reset;
        }
        return S_OK;
    }
    HRESULT Upload(Texture slot, const Image& image) {
        auto& texture = textures[static_cast<size_t>(slot)];
        if (texture.value && texture.revision == image.revision && texture.width == image.width &&
            texture.height == image.height) return S_OK;
        uint32_t width = std::min(image.width, std::min(uint32_t(caps.MaxTextureWidth), uint32_t(4096)));
        uint32_t height = std::min(image.height, std::min(uint32_t(caps.MaxTextureHeight), uint32_t(4096)));
        if (!width || !height) return D3DERR_NOTAVAILABLE;
        if (caps.TextureCaps & D3DPTEXTURECAPS_POW2) {
            // Resize, rather than pad, so fixed UVs remain valid on early GPUs.
            uint32_t power = 1; while (power <= width / 2) power *= 2; width = power;
            power = 1; while (power <= height / 2) power *= 2; height = power;
        }
        if (caps.TextureCaps & D3DPTEXTURECAPS_SQUAREONLY) width = height = std::min(width, height);
        IDirect3DTexture9* replacement{};
        HRESULT result = device->CreateTexture(width, height, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED, &replacement, nullptr);
        if (FAILED(result)) return result;
        D3DLOCKED_RECT locked{};
        result = replacement->LockRect(0, &locked, nullptr, 0);
        if (FAILED(result)) { replacement->Release(); return result; }
        for (uint32_t y = 0; y < height; ++y) {
            auto* dst = static_cast<uint8_t*>(locked.pBits) + size_t(y) * size_t(locked.Pitch);
            if (width == image.width && height == image.height)
                std::memcpy(dst, image.pixels.data() + size_t(y) * image.width * 4, size_t(width) * 4);
            else for (uint32_t x = 0; x < width; ++x) {
                const auto pixel = Sample(image, (float(x) + 0.5f) / float(width),
                    (float(y) + 0.5f) / float(height));
                for (size_t c = 0; c < 4; ++c) dst[size_t(x) * 4 + c] = Byte(pixel[c]);
            }
        }
        result = replacement->UnlockRect(0);
        if (FAILED(result)) { replacement->Release(); return result; }
        if (texture.value) texture.value->Release();
        texture = {replacement, image.revision, image.width, image.height};
        return S_OK;
    }
    void AddVertices(const Quad& q) {
        const float cosine = std::cos(q.rotation), sine = std::sin(q.rotation);
        const DWORD tint = TintPremultiplied(q.color);
        std::array<Vertex, 4> corners{};
        constexpr float xs[]{-0.5f, 0.5f, 0.5f, -0.5f};
        constexpr float ys[]{-0.5f, -0.5f, 0.5f, 0.5f};
        for (size_t i = 0; i < corners.size(); ++i) {
            const float x = xs[i] * q.width, y = ys[i] * q.height;
            const float u = xs[i] + 0.5f;
            corners[i] = {q.x + x * cosine - y * sine - 0.5f,
                q.y + x * sine + y * cosine - 0.5f, 0, 1, tint,
                q.flip_x ? 1.f - u : u, ys[i] + 0.5f};
        }
        for (const size_t index : {0u, 1u, 2u, 0u, 2u, 3u}) vertices.push_back(corners[index]);
    }
    HRESULT Flush(Texture texture, bool additive) {
        if (vertices.empty()) return S_OK;
        HRESULT result = device->SetTexture(0, textures[static_cast<size_t>(texture)].value);
        if (SUCCEEDED(result)) result = device->SetRenderState(D3DRS_DESTBLEND,
            additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
        if (SUCCEEDED(result)) result = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
            static_cast<UINT>(vertices.size() / 3), vertices.data(), sizeof(Vertex));
        vertices.clear();
        return result;
    }
    HRESULT RenderGpu(const RenderFrame& frame, const Image& background) {
        std::array<bool, texture_count> used{};
        for (const auto& q : frame.quads) {
            if (!ValidQuad(q)) continue;
            const auto index = static_cast<size_t>(q.texture);
            const auto& image = GetImage(q.texture, background);
            if (used[index] || !ValidImage(image)) continue;
            const HRESULT upload = Upload(q.texture, image);
            if (FAILED(upload)) return upload;
            used[index] = true;
        }
        HRESULT result = device->Clear(0, nullptr, D3DCLEAR_TARGET,
            D3DCOLOR_XRGB(GetRValue(frame.background), GetGValue(frame.background), GetBValue(frame.background)), 1, 0);
        if (FAILED(result)) return result;
        result = device->BeginScene();
        if (FAILED(result)) return result;
        device->SetVertexShader(nullptr); device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        device->SetRenderState(D3DRS_ZENABLE, FALSE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_COLORWRITEENABLE, 15);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        Texture current = Texture::White;
        bool additive = false;
        vertices.clear();
        const size_t maximum_vertices = std::min<size_t>(caps.MaxPrimitiveCount / 2, 2048) * 6;
        if (!maximum_vertices) result = D3DERR_NOTAVAILABLE;
        for (const auto& q : frame.quads) {
            if (FAILED(result)) break;
            if (!ValidQuad(q) || !used[static_cast<size_t>(q.texture)]) continue;
            if (current != q.texture || additive != q.additive || vertices.size() >= maximum_vertices) {
                result = Flush(current, additive);
                if (FAILED(result)) break;
            }
            current = q.texture; additive = q.additive;
            AddVertices(q);
        }
        if (SUCCEEDED(result)) result = Flush(current, additive);
        const HRESULT ended = device->EndScene();
        if (FAILED(result)) return result;
        if (FAILED(ended)) return ended;
        return device->Present(nullptr, nullptr, nullptr, nullptr);
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
void Renderer::SetLogo(uint32_t kind, const TtpOsuVisualImage* source) {
    if (kind > 2 || (kind != 2 && source)) throw Failure{E_INVALIDARG};
    Image next;
    if (kind == 2) {
        if (!source || source->size < sizeof(*source) || !source->bgra ||
            !source->width || !source->height || source->width > 8192 || source->height > 8192 ||
            source->stride < source->width * 4 ||
            uint64_t(source->width) * source->height * 4 > 128ull * 1024 * 1024 ||
            uint64_t(source->stride) * source->height > UINT32_MAX) throw Failure{E_INVALIDARG};
        next = MakeImage(source->width, source->height);
        for (uint32_t y = 0; y < source->height; ++y)
            std::memcpy(next.pixels.data() + size_t(y) * source->width * 4,
                source->bgra + size_t(y) * source->stride, size_t(source->width) * 4);
    } else next = MakeLogo(kind == 1 ? 102 : 101);
    auto& current = impl_->images[static_cast<size_t>(Texture::Logo)];
    next.revision = current.revision + 1;
    current = std::move(next); // CPU and GPU consume the same committed revision.
}
uint32_t Renderer::Backend() const noexcept { return impl_->backend; }
bool Renderer::CanDetach() const noexcept { return impl_->CanDetach(); }
void Renderer::Detach() { impl_->Detach(); }
HRESULT Renderer::Paint(HDC dc, const RECT& rect, const RenderFrame& frame, const Image& background) {
    const int64_t target_width = int64_t(rect.right) - rect.left;
    const int64_t target_height = int64_t(rect.bottom) - rect.top;
    if (!dc || !frame.width || !frame.height || frame.width > 16384 || frame.height > 16384 ||
        uint64_t(frame.width) * frame.height > max_cpu_pixels || frame.quads.size() > maximum_quads ||
        target_width <= 0 || target_height <= 0 || target_width > INT_MAX || target_height > INT_MAX)
        return E_INVALIDARG;
    impl_->Assets();
    impl_->TransitionAssets(frame);
    const HRESULT result = impl_->Dib(frame.width, frame.height);
    if (FAILED(result)) return result;
    // GDI batches references to source DIBs. Finish prior copies before writing
    // the same allocation again, otherwise CPU/GDI can race on a subsequent frame.
    GdiFlush();
    const uint32_t fill = 0xff000000u | (uint32_t(GetRValue(frame.background)) << 16) |
        (uint32_t(GetGValue(frame.background)) << 8) | GetBValue(frame.background);
    auto* pixels = reinterpret_cast<uint32_t*>(impl_->dib_pixels);
    std::fill_n(pixels, size_t(frame.width) * frame.height, fill);
    for (const auto& q : frame.quads) {
        if (!ValidQuad(q)) continue;
        const auto& image = impl_->GetImage(q.texture, background);
        if (ValidImage(image)) impl_->DrawCpu(q, image);
    }
    const int width = static_cast<int>(target_width), height = static_cast<int>(target_height);
    BOOL copied;
    if (uint32_t(width) == frame.width && uint32_t(height) == frame.height)
        copied = BitBlt(dc, rect.left, rect.top, width, height, impl_->memory_dc, 0, 0, SRCCOPY);
    else {
        const int old_mode = SetStretchBltMode(dc, COLORONCOLOR);
        copied = StretchBlt(dc, rect.left, rect.top, width, height, impl_->memory_dc, 0, 0,
            int(frame.width), int(frame.height), SRCCOPY);
        if (old_mode) SetStretchBltMode(dc, old_mode);
    }
    GdiFlush();
    if (!copied) return E_FAIL;
    impl_->backend = 1;
    return S_OK;
}
HRESULT Renderer::Present(HWND window, const RenderFrame& frame, const Image& background) {
    if (!window || !IsWindow(window) || !frame.width || !frame.height || frame.width > 16384 ||
        frame.height > 16384 || frame.quads.size() > maximum_quads) return E_INVALIDARG;
    if (IsIconic(window)) return S_FALSE;
    impl_->Assets();
    impl_->TransitionAssets(frame);
    const HRESULT ready = impl_->Device(window, frame.width, frame.height);
    if (FAILED(ready)) return ready;
    const HRESULT result = impl_->RenderGpu(frame, background);
    if (SUCCEEDED(result)) impl_->backend = 2;
    return result;
}
}
