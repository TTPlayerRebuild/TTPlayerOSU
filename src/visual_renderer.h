#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
struct TtpOsuVisualImage;

namespace ttp_osu::visual {
struct RenderFrame;
struct Image;
struct IntroFont;

// All device operations, including destruction, belong to the target window's
// message thread. Paint is independent of the device and may run on the host's
// serialized HDC rendering thread. No host callback is made by this class.
class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    HRESULT Paint(HDC, const RECT&, const RenderFrame&, const Image&);
    HRESULT Present(HWND, const RenderFrame&, const Image&);
    void Detach();
    uint32_t Backend() const noexcept;
    bool CanDetach() const noexcept;
    void SetLogo(uint32_t kind, const TtpOsuVisualImage* image);
    void SetIntroFont(IntroFont& font);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
