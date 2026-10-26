#pragma once
#include <ttp_osu/visual.h>
#include <array>
#include <cstdint>
#include <span>
#include <vector>
#include "visual_text.h"

namespace ttp_osu::visual {
enum class Texture { White, Logo, Spectrum, Star, Flash, Shockwave, Background, Welcome,
    LazerLogo, LazerFill, LazerStar, LazerFlash, LazerIntro, LazerFont, LazerCustomFont,
    RulesetOsu, RulesetTaiko, RulesetCatch, RulesetMania, Triangle, OutlineTriangle, LazerCircle,
    LazerTTPlayerIntro, SpectrumHigh, Count };
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
    float u0{}, v0{}, u1{1}, v1{1}; // optional atlas subrectangle
};
struct LazerTriangle { float x{}, y{}, speed{}; };
struct RenderFrame {
    uint32_t width{}, height{};
    COLORREF background{};
    std::vector<Quad> quads;
    std::vector<LazerTriangle> lazer_triangles;
    float lazer_intro_progress{-1}; // original R threshold / G alpha shader
    uint64_t lazer_fill_revision{};
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
    void SetStyle(uint32_t style);
    void SetIntroText(bool custom, std::array<std::wstring,3> fields, IntroFont font);
    void Transition(uint32_t phase, uint64_t now_ms);
    TtpOsuVisualTransitionState TransitionState(uint64_t now_ms) noexcept;
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
    void ResetLazer();
    void UpdateLazer(double elapsed, const TtpOsuVisualFrame&);
    RenderFrame BuildLazerFrame(uint32_t width, uint32_t height) const;
    float LazerLogoScale() const noexcept;
    void LazerBurst();
    double TransitionAge() const noexcept;
    void Analyze(const TtpOsuVisualFrame&);
    void RingStep(double milliseconds);
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
    uint64_t transition_start_{}, transition_now_{};
    uint32_t transition_phase_{};
    double exit_welcome_age_{};
    bool exit_from_welcome_{};
    uint32_t random_{}, shift_{}, playback_{}, bursts_{}, fft_count_{};
    uint32_t analyzed_cursor_offset_{};
    bool initialized_{}, analyzed_{}, analyzed_intro_{}, beat_valid_{}, burst_valid_{};
    double ring_remainder_{}, envelope_remainder_{};
    float left_peak_{}, right_peak_{}, amplitude_{}, amplitude_smooth_{}, pulse_{}, hover_{};
    float strength_{0.6f}, strength_latched_{0.6f}, pointer_x_{0.5f}, pointer_y_{0.5f};
    bool pointer_inside_{};
    float logo_width_{337.5f}, logo_height_{337.5f};
    mutable float logical_width_{640}; // last surface, for circular pointer hit testing
    mutable float surface_logical_height_{480}; // includes centred narrow-screen letterboxing
    uint32_t style_{}, logo_kind_{};
    bool custom_intro_text_{};
    std::array<std::wstring,3> intro_text_;
    std::vector<IntroGlyph> intro_glyphs_;
    uint32_t intro_font_width_{}, intro_font_height_{};
    std::array<float, 200> lazer_bars_{};
    std::vector<LazerTriangle> lazer_triangles_;
    double lazer_sample_time_{}, lazer_triangle_time_{}, lazer_beat_age_{1e9};
    float lazer_amplitude_scale_{1}, lazer_hover_scale_{1}, lazer_hover_from_{1}, lazer_hover_target_{1};
    float lazer_hover_age_{500}, lazer_beat_strength_{}, lazer_velocity_{.5f};
    float lazer_parallax_x_{}, lazer_parallax_y_{}, lazer_background_x_{}, lazer_background_y_{};
    float lazer_flash_{}, lazer_visualizer_alpha_{.5f};
    float lazer_pending_velocity_{};
    float lazer_bounce_scale_{1}, lazer_bounce_from_{1}, lazer_bounce_age_{1000};
    double lazer_click_age_{1e9};
    bool lazer_button_down_{}, lazer_press_armed_{}, lazer_bounce_releasing_{};
    uint32_t lazer_offset_{};
    uint64_t lazer_fill_revision_{1};
    bool lazer_over_{};
};
}
