#pragma once
#include <ttp_osu/visual.h>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ttp_osu::visual {
enum class Texture { White, Logo, Spectrum, Star, Flash, Shockwave, Background };
struct Image {
    uint32_t width{}, height{};
    std::vector<uint8_t> pixels; // tightly packed premultiplied BGRA
    uint64_t revision{};
};
struct Quad {
    float x{}, y{}, width{}, height{}, rotation{};
    uint32_t color{0xffffffff}; // straight ARGB tint; renderer premultiplies it
    Texture texture{Texture::White};
    bool additive{}, flip_x{};
};
struct RenderFrame {
    uint32_t width{}, height{};
    COLORREF background{};
    std::vector<Quad> quads;
};
struct TimingPoint {
    double offset{}, beat_length{500}, beat_offset{};
    uint32_t meter{4};
    bool kiai{};
};
struct BeatState {
    double length{500}, phase{}, marker{};
    int64_t index{};
    uint32_t meter{4};
    bool kiai{};
};
// Parse into a temporary vector before replacing a live scene. Inherited timing
// points carry effects, but never replace a red point's positive beat length.
std::vector<TimingPoint> ParseTiming(std::span<const uint8_t> bytes);
BeatState EvaluateTiming(std::span<const TimingPoint>, double position, double bpm);

class Scene {
public:
    Scene();
    void Configure(const TtpOsuVisualOptions&);
    void SetTiming(std::vector<TimingPoint>);
    void SetImage(const TtpOsuVisualImage*);
    void SetLogo(uint32_t kind, uint32_t width = 0, uint32_t height = 0);
    void Update(const TtpOsuVisualFrame&);
    RenderFrame BuildFrame(uint32_t width, uint32_t height) const;
    const Image& Background() const noexcept { return image_; }
    TtpOsuVisualStats Stats() const noexcept;
    // Internal numerical inspection used by private tests, never part of ABI.
    const std::array<float, 1024>& Spectrum() const noexcept { return fft_; }
    const std::array<float, 1024>& Bars() const noexcept { return bars_; }
    uint32_t SpectrumShift() const noexcept { return shift_; }
private:
    struct Particle {
        uint64_t start{};
        float lifetime{}, vx{}, vy{}, final_scale{}, rotation_start{}, rotation_end{};
        bool right{};
    };
    struct Wave { uint64_t start{}; float scale{}, strength{}; };
    struct Flash {
        uint64_t start{};
        float initial{}, target{};
        double duration{300};
    };
    void ResetTemporal();
    void Analyze(const TtpOsuVisualFrame&);
    void RingStep(double milliseconds, bool playing);
    void Burst();
    float FlashAlpha(const Flash&) const;
    uint32_t Random() noexcept;
    float RandomRange(float minimum, float maximum) noexcept;
    TtpOsuVisualOptions options_{};
    Image image_;
    std::vector<TimingPoint> timing_;
    std::array<float, 1024> fft_{}, bars_{};
    std::vector<Particle> particles_;
    std::vector<Wave> waves_;
    Flash left_flash_, right_flash_;
    BeatState beat_;
    uint64_t now_{}, last_now_{}, timeline_{}, revision_{}, last_burst_{};
    uint32_t random_{}, shift_{}, playback_{}, bursts_{}, fft_count_{};
    uint32_t analyzed_cursor_offset_{};
    bool initialized_{}, analyzed_{}, beat_valid_{}, burst_valid_{};
    double ring_remainder_{}, envelope_remainder_{};
    float left_peak_{}, right_peak_{}, amplitude_{}, amplitude_smooth_{}, pulse_{}, hover_{};
    float strength_{0.6f}, strength_latched_{0.6f}, pointer_x_{0.5f}, pointer_y_{0.5f};
    bool pointer_inside_{};
    float logo_width_{337.5f}, logo_height_{337.5f};
    mutable float logical_width_{640}; // last surface, for circular pointer hit testing
    mutable float surface_logical_height_{480}; // includes centred narrow-screen letterboxing
};
}
