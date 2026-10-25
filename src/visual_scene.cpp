#include "visual_scene.h"
#include "parser.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

namespace ttp_osu::visual {
namespace {
constexpr double Pi = 3.14159265358979323846;
constexpr double FrameMs = 1000.0 / 60;
// Original MainMenu.Prepare/intro callback 0x06001B70/0x06001BA3.
// The final 21 eligible 60 Hz ticks are elapsed-time based here, so a user's
// selected rendering FPS does not prolong the entrance or block full-screen exit.
constexpr double WelcomeRevealMs = 2438;
constexpr double WelcomeLayerSwitchMs = WelcomeRevealMs + 20 * FrameMs;
constexpr double WelcomeCompleteMs = WelcomeRevealMs + 21 * FrameMs;
constexpr double DepartureMs = 100 / .6 * FrameMs;
// osu! 0x060011B1 positions use H/480, but 0x060011B7 and
// Sprite 0x06003F15 scale standard-resolution artwork by H/768.
constexpr float TextureToLogical = 480.0f / 768.0f;
// Both built-ins use the original 540px menu-osu layout, as requested.
// Do not infer a different scale from each PNG's transparent canvas padding.
constexpr float LogoDiameter = 540.0f * TextureToLogical;
constexpr uint32_t AllFlags = TTP_OSU_SPECTRUM | TTP_OSU_STARS | TTP_OSU_FLASH |
    TTP_OSU_PARALLAX | TTP_OSU_LOGO;
float Unit(float value) { return std::clamp(value, 0.0f, 1.0f); }
float Ease(float value) { return value * (2.0f - value); }
uint32_t Color(COLORREF value, float alpha = 1) {
    return (static_cast<uint32_t>(Unit(alpha) * 255 + .5f) << 24) |
        (GetRValue(value) << 16) | (GetGValue(value) << 8) | GetBValue(value);
}
std::string_view Trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \r\n\t");
    if (first == value.npos) return {};
    return value.substr(first, value.find_last_not_of(" \r\n\t") - first + 1);
}
template<class T> bool Number(std::string_view value, T& result) {
    value = Trim(value);
    if (!value.empty() && value.front() == '+') value.remove_prefix(1);
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}
bool Utf8(std::span<const uint8_t> bytes) {
    for (size_t i = 0; i < bytes.size();) {
        uint32_t c = bytes[i++];
        if (!c) return false;
        if (c < 128) continue;
        unsigned extra{};
        uint32_t minimum{};
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; minimum = 0x80; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; minimum = 0x800; c &= 0xf; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; minimum = 0x10000; c &= 7; }
        else return false;
        if (i + extra > bytes.size()) return false;
        while (extra--) {
            const uint8_t next = bytes[i++];
            if ((next & 0xc0) != 0x80) return false;
            c = (c << 6) | (next & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}
}

std::vector<TimingPoint> ParseTiming(std::span<const uint8_t> bytes) {
    if (bytes.empty()) return {};
    if (bytes.size() > 16U * 1024 * 1024) throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
    if (!Utf8(bytes)) throw Failure{HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION)};
    if (bytes.size() >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf)
        bytes = bytes.subspan(3);
    struct Raw { double offset{}, length{}; uint32_t meter{4}, effects{}; bool red{true}; };
    std::vector<Raw> raw;
    std::string_view rest(reinterpret_cast<const char*>(bytes.data()), bytes.size()), section;
    bool header{};
    while (!rest.empty()) {
        const auto end = rest.find_first_of("\r\n");
        auto line = Trim(rest.substr(0, end));
        rest = end == rest.npos ? std::string_view{} : rest.substr(end + 1);
        if (line.empty() || line.starts_with("//")) continue;
        if (!header) {
            constexpr std::string_view prefix = "osu file format v";
            unsigned version{};
            if (!line.starts_with(prefix) || !Number(line.substr(prefix.size()), version) || !version)
                throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
            header = true;
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        if (section != "TimingPoints") continue;
        std::array<std::string_view, 8> fields{};
        size_t count{};
        while (!line.empty() && count < fields.size()) {
            const auto comma = line.find(',');
            fields[count++] = Trim(line.substr(0, comma));
            line = comma == line.npos ? std::string_view{} : line.substr(comma + 1);
        }
        Raw point;
        unsigned inherited = 1;
        if (count < 2 || !Number(fields[0], point.offset) || !Number(fields[1], point.length) ||
            !std::isfinite(point.offset) || !std::isfinite(point.length) ||
            std::abs(point.offset) > 1e12 || point.length == 0 || std::abs(point.length) > 1e9)
            throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
        if (count >= 3 && (!Number(fields[2], point.meter) || !point.meter || point.meter > 64))
            throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
        if (count >= 7 && (!Number(fields[6], inherited) || inherited > 1))
            throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
        if (count >= 8 && !Number(fields[7], point.effects))
            throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
        point.red = inherited != 0;
        if (point.red && (point.length < 1 || point.length > 60000))
            throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
        if (raw.size() >= 65536) throw Failure{HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)};
        raw.push_back(point);
    }
    if (!header) throw Failure{HRESULT_FROM_WIN32(ERROR_INVALID_DATA)};
    std::stable_sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) { return a.offset < b.offset; });
    std::vector<TimingPoint> result;
    result.reserve(raw.size());
    double length = 500, offset = 0;
    uint32_t meter = 4;
    // Before the first red point, osu uses that point's BPM extended backwards.
    for (const auto& item : raw) if (item.red) {
        length = item.length; offset = item.offset; meter = item.meter; break;
    }
    for (const auto& item : raw) {
        if (item.red) { length = item.length; offset = item.offset; meter = item.meter; }
        result.push_back({item.offset, length, offset, meter, (item.effects & 1) != 0});
    }
    return result;
}

BeatState EvaluateTiming(std::span<const TimingPoint> timing, double position, double bpm) {
    BeatState state;
    double offset{};
    if (!timing.empty()) {
        const auto point = std::upper_bound(timing.begin(), timing.end(), position,
            [](double value, const TimingPoint& item) { return value < item.offset; });
        const auto& current = point == timing.begin() ? timing.front() : *(point - 1);
        state.length = current.beat_length;
        state.meter = current.meter;
        state.marker = current.offset;
        state.kiai = point != timing.begin() && current.kiai;
        offset = current.beat_offset;
    } else {
        state.length = 60000.0 / std::clamp(bpm, 20.0, 400.0);
    }
    const double beats = (position - offset) / state.length;
    state.index = static_cast<int64_t>(std::floor(beats));
    state.phase = beats - std::floor(beats);
    return state;
}

Scene::Scene() {
    options_ = {sizeof(options_), AllFlags, 0x5454504fu, RGB(224, 224, 255), RGB(28, 20, 36), 120};
    random_ = options_.seed;
    particles_.reserve(600);
    waves_.reserve(8);
}
double Scene::TransitionAge() const noexcept {
    return transition_now_ >= transition_start_ ?
        static_cast<double>(transition_now_ - transition_start_) : 0;
}
void Scene::Transition(uint32_t phase, uint64_t now) {
    if (phase > 2) throw Failure{E_INVALIDARG};
    transition_now_ = std::max(transition_now_, now);
    // Repeating an active phase is idempotent. Once welcome has completed,
    // an explicit new entrance on the same instance starts it again.
    if (phase && phase == transition_phase_ &&
        (phase != 1 || TransitionAge() < WelcomeCompleteMs)) return;
    // A departure can interrupt welcome. Freeze its current mask/ring layout
    // and fade the remaining welcome text over the original 120ms; otherwise
    // both text and the spectrum above the black mask would disappear at once.
    exit_from_welcome_ = phase == 2 && transition_phase_ == 1 && TransitionAge() < WelcomeCompleteMs;
    exit_welcome_age_ = exit_from_welcome_ ? TransitionAge() : 0;
    transition_phase_ = phase;
    transition_start_ = transition_now_ = now;
}
TtpOsuVisualTransitionState Scene::TransitionState(uint64_t now) noexcept {
    transition_now_ = std::max(transition_now_, now);
    const double duration = transition_phase_ == 1 ? WelcomeCompleteMs :
        transition_phase_ == 2 ? DepartureMs : 0;
    const bool complete = !transition_phase_ || TransitionAge() >= duration;
    const uint32_t phase = transition_phase_ == 1 && complete ? 0 : transition_phase_;
    return {sizeof(TtpOsuVisualTransitionState), phase, complete ? 1u : 0u,
        static_cast<uint32_t>(std::ceil(duration))};
}
void Scene::SetLogo(uint32_t kind, uint32_t width, uint32_t height) {
    if (kind > 2 || (kind == 2 && (!width || !height))) throw Failure{E_INVALIDARG};
    logo_width_ = logo_height_ = LogoDiameter;
    if (kind == 2) {
        const float divisor = static_cast<float>(std::max(width, height));
        logo_width_ *= width / divisor;
        logo_height_ *= height / divisor;
    }
}
void Scene::Configure(const TtpOsuVisualOptions& options) {
    if (!std::isfinite(options.fallback_bpm) || options.fallback_bpm < 20 || options.fallback_bpm > 400 ||
        (options.flags & ~AllFlags)) throw Failure{E_INVALIDARG};
    if (options.seed != options_.seed) random_ = options.seed ? options.seed : 0x5454504f;
    options_ = options;
    if (!(options.flags & TTP_OSU_STARS)) particles_.clear();
    if (!(options.flags & TTP_OSU_FLASH)) left_flash_ = right_flash_ = {};
}
void Scene::SetTiming(std::vector<TimingPoint> points) {
    timing_ = std::move(points);
    beat_valid_ = false;
    particles_.clear();
    waves_.clear();
    left_flash_ = right_flash_ = {};
    burst_valid_ = false;
}
void Scene::SetImage(const TtpOsuVisualImage* value) {
    Image next;
    next.revision = image_.revision + 1;
    if (value) {
        if (!value->bgra || !value->width || !value->height || value->width > 8192 || value->height > 8192 ||
            value->stride < value->width * 4 ||
            static_cast<uint64_t>(value->width) * value->height * 4 > 128ULL * 1024 * 1024 ||
            static_cast<uint64_t>(value->stride) * value->height > UINT32_MAX)
            throw Failure{E_INVALIDARG};
        next.width = value->width;
        next.height = value->height;
        next.pixels.resize(static_cast<size_t>(value->width) * value->height * 4);
        for (uint32_t y = 0; y < value->height; ++y)
            memcpy(next.pixels.data() + static_cast<size_t>(y) * value->width * 4,
                value->bgra + static_cast<size_t>(y) * value->stride, static_cast<size_t>(value->width) * 4);
    }
    image_ = std::move(next);
}
void Scene::ResetTemporal() {
    fft_.fill(0); bars_.fill(0);
    particles_.clear(); waves_.clear();
    left_flash_ = right_flash_ = {};
    beat_valid_ = analyzed_ = burst_valid_ = false;
    ring_remainder_ = envelope_remainder_ = 0;
    left_peak_ = right_peak_ = amplitude_ = amplitude_smooth_ = pulse_ = 0;
    shift_ = 0;
}
void Scene::Analyze(const TtpOsuVisualFrame& frame) {
    std::array<float, 2048> real{}, imaginary{};
    const size_t count = std::min<size_t>(frame.count, real.size());
    left_peak_ = right_peak_ = 0;
    // BASS_ChannelGetLevel's peak covers approximately 20 ms, separately per channel.
    // A small ASIO output buffer may use preceding, actually played PCM to fill
    // the FFT window. The cursor offset distinguishes that history from future
    // samples. Prefer 20ms forward; when insufficient, take the newest available
    // 20ms (including real history) rather than treating old window head as now.
    const size_t peak_count = std::min<size_t>(frame.count, frame.sample_rate / 50);
    const size_t cursor = frame.pcm_cursor_offset;
    const size_t peak_start = frame.count - cursor >= peak_count ? cursor : frame.count - peak_count;
    const size_t peak_end = peak_start + peak_count;
    for (size_t i = peak_start; i < peak_end; ++i) {
        const float left = frame.left[i] / 32768.0f;
        const float right = frame.right ? frame.right[i] / 32768.0f : left;
        left_peak_ = std::max(left_peak_, std::abs(left));
        right_peak_ = std::max(right_peak_, std::abs(right));
    }
    for (size_t i = 0; i < count; ++i) {
        const float l = frame.left[i] / 32768.0f;
        const float r = frame.right ? frame.right[i] / 32768.0f : l;
        const double window = .5 - .5 * std::cos(2 * Pi * static_cast<double>(i) / 2047.0);
        real[i] = static_cast<float>((l + r) * .5 * window);
    }
    for (size_t i = 1, j = 0; i < real.size(); ++i) {
        size_t bit = real.size() >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(real[i], real[j]);
    }
    for (size_t length = 2; length <= real.size(); length <<= 1) {
        const float wr = static_cast<float>(std::cos(-2 * Pi / static_cast<double>(length)));
        const float wi = static_cast<float>(std::sin(-2 * Pi / static_cast<double>(length)));
        for (size_t base = 0; base < real.size(); base += length) {
            float ur = 1, ui = 0;
            for (size_t j = 0; j < length / 2; ++j) {
                const size_t a = base + j, b = a + length / 2;
                const float vr = real[b] * ur - imaginary[b] * ui;
                const float vi = real[b] * ui + imaginary[b] * ur;
                real[b] = real[a] - vr; imaginary[b] = imaginary[a] - vi;
                real[a] += vr; imaginary[a] += vi;
                const float previous = ur;
                ur = ur * wr - ui * wi; ui = previous * wi + ui * wr;
            }
        }
    }
    // Normalize a bin-centred full-scale sine to 1 before the menu's gain.
    // Original welcome pre-stage uses 2, then restores 1.6 at t=2438ms.
    analyzed_intro_ = transition_phase_ == 1 && TransitionAge() < WelcomeRevealMs;
    const float fft_gain = analyzed_intro_ ? 2.0f : 1.6f;
    // The DC component is single-sided, unlike the paired positive frequency bins.
    for (size_t i = 0; i < fft_.size(); ++i) {
        const float normalization = (i ? 2.0f : 1.0f) / 1023.5f;
        fft_[i] = std::hypot(real[i], imaginary[i]) * normalization * fft_gain;
    }
    amplitude_ = (left_peak_ + right_peak_) * .5f * Unit(frame.volume);
    ++fft_count_;
    analyzed_ = true;
    analyzed_cursor_offset_ = frame.pcm_cursor_offset;
    revision_ = frame.revision;
}
void Scene::RingStep(double milliseconds, bool playing) {
    const float decay = static_cast<float>(std::pow(.949999988079071, milliseconds / FrameMs));
    if (!playing) for (auto& value : fft_) value = value < .01f ? 0 : value * decay;
    for (size_t i = 0; i < bars_.size(); ++i) {
        auto& bar = bars_[1023 - i];
        bar = std::max(bar, fft_[(i + shift_) % 1024] * 3) * decay;
        if (bar < .01f) bar = 0;
    }
}
uint32_t Scene::Random() noexcept {
    random_ ^= random_ << 13; random_ ^= random_ >> 17; random_ ^= random_ << 5;
    return random_;
}
float Scene::RandomRange(float minimum, float maximum) noexcept {
    return minimum + (maximum - minimum) * static_cast<float>((Random() >> 8) / 16777216.0);
}
void Scene::Burst() {
    if (!(options_.flags & TTP_OSU_STARS)) return;
    // Bounded even under adversarial charts with a new Kiai transition every 501ms.
    if (particles_.size() + 200 > 600) particles_.erase(particles_.begin(), particles_.begin() +
        static_cast<ptrdiff_t>(particles_.size() + 200 - 600));
    // MainMenu.Update (0x06001B8E) chooses one fan direction per burst.
    const int sign = static_cast<int>(Random() % 3) - 1;
    for (unsigned i = 0; i < 100; ++i) {
        const float spread = sign * (static_cast<float>(i) - 50) / 50 * 450;
        const float lifetime = static_cast<float>(300 + Random() % 1000);
        const float scale = RandomRange(1, 2.8f), start_rotation = RandomRange(-2, 2), end_rotation = RandomRange(-2, 2);
        for (unsigned side = 0; side < 2; ++side) {
            const float jitter = static_cast<float>(static_cast<int>(Random() % 100) - 50);
            const float vy = static_cast<float>(static_cast<int>(Random() % 100) - 800);
            particles_.push_back({now_ + i * 10, lifetime, (side ? -spread : spread) + jitter,
                vy, scale, start_rotation, end_rotation, side != 0});
        }
    }
    last_burst_ = now_; burst_valid_ = true; ++bursts_;
}
float Scene::FlashAlpha(const Flash& value) const {
    if (value.target == 0 && value.initial == 0) return 0;
    const double age = static_cast<double>(now_ - value.start);
    if (age < 64) return value.initial + (value.target - value.initial) * static_cast<float>(age / 64);
    const float t = Unit(static_cast<float>((age - 64) / std::max(1.0, value.duration - 64)));
    return value.target * (1 - t * t);
}
void Scene::Update(const TtpOsuVisualFrame& frame) {
    if (frame.playback > 4 || !std::isfinite(frame.position_ms) || std::abs(frame.position_ms) > 1e12 ||
        !std::isfinite(frame.volume) || !std::isfinite(frame.pointer_x) || !std::isfinite(frame.pointer_y) ||
        frame.count > 65536 || frame.pcm_cursor_offset > frame.count ||
        (frame.count && (!frame.left || frame.sample_rate < 1000 || frame.sample_rate > 768000)))
        throw Failure{E_INVALIDARG};
    const bool changed = initialized_ && frame.timeline != timeline_;
    const bool stopped = initialized_ && playback_ != frame.playback && frame.playback != 2 && frame.playback != 3;
    const uint64_t elapsed = !initialized_ || frame.now_ms < last_now_ ? 0 : frame.now_ms - last_now_;
    const bool stalled = initialized_ && (frame.now_ms < last_now_ || elapsed > 1000);
    if (changed || stopped || stalled) ResetTemporal();
    now_ = frame.now_ms;
    transition_now_ = std::max(transition_now_, frame.now_ms);
    const double dt = changed || stopped || stalled ? 0.0 : static_cast<double>(elapsed);
    timeline_ = frame.timeline;
    last_now_ = frame.now_ms;
    initialized_ = true;
    playback_ = frame.playback;
    pointer_x_ = Unit(frame.pointer_x); pointer_y_ = Unit(frame.pointer_y);
    pointer_inside_ = frame.pointer_inside != 0;
    const bool playing = frame.playback == 2;
    const bool intro_fft = transition_phase_ == 1 && TransitionAge() < WelcomeRevealMs;
    if (playing && frame.count && (!analyzed_ || analyzed_intro_ != intro_fft || frame.revision != revision_ ||
        frame.pcm_cursor_offset != analyzed_cursor_offset_)) Analyze(frame);
    if (playing && frame.count) amplitude_ = (left_peak_ + right_peak_) * .5f * Unit(frame.volume);
    if (!playing || !frame.count) {
        const float decay = static_cast<float>(std::pow(.9, dt / FrameMs));
        left_peak_ *= decay; right_peak_ *= decay; amplitude_ *= decay;
    }
    const double position = frame.playback == 0 || frame.playback == 4 ? static_cast<double>(now_ % 1000) : frame.position_ms;
    BeatState next = EvaluateTiming(frame.playback == 0 || frame.playback == 4 ? std::span<const TimingPoint>{} : timing_,
        position, frame.playback == 0 || frame.playback == 4 ? 60 : options_.fallback_bpm);
    const bool new_beat = beat_valid_ && (next.index != beat_.index || next.length != beat_.length);
    if (playing && beat_valid_ && !beat_.kiai && next.kiai &&
        frame.position_ms >= next.marker && frame.position_ms - next.marker < 500 &&
        (!burst_valid_ || now_ - last_burst_ > 500)) Burst();
    if (playing && new_beat) {
        strength_latched_ = strength_;
        if (waves_.size() == 8) waves_.erase(waves_.begin());
        const float departure_scale = transition_phase_ == 2 ?
            -.003f * static_cast<float>(std::min(TransitionAge(), DepartureMs) / FrameMs) : 0;
        waves_.push_back({now_, 1.05f + hover_ + departure_scale - .05f * Ease(pulse_), .1f * strength_});
        if ((options_.flags & TTP_OSU_FLASH) && (next.kiai || (next.index > 0 && next.index % next.meter == 0))) {
            const auto target = [](float peak) { return std::min(1.0f, std::max(0.0f, (2 * peak * 32768 - 20000) / 45536) / 2); };
            if (!next.kiai || next.index % 2 == 0)
                left_flash_ = {now_, FlashAlpha(left_flash_), target(left_peak_), std::max(300.0, next.length)};
            if (!next.kiai || next.index % 2 != 0)
                right_flash_ = {now_, FlashAlpha(right_flash_), target(right_peak_), std::max(300.0, next.length)};
        }
    }
    beat_ = next; beat_valid_ = true;
    // Fixed 60Hz envelope steps preserve response when rendering FPS changes.
    envelope_remainder_ += dt;
    while (envelope_remainder_ >= FrameMs) {
        envelope_remainder_ -= FrameMs;
        amplitude_smooth_ = .9f * amplitude_smooth_ + .1f * amplitude_;
        const float target = Unit(.5f - .5f * static_cast<float>(beat_.phase) + amplitude_ - amplitude_smooth_);
        pulse_ = .5f * pulse_ + .5f * target;
        const float peak = (left_peak_ + right_peak_) * 32768;
        const float strength = beat_.kiai ? 1 : .6f + .4f * Unit((peak - 30000) / 35536);
        strength_ = .8f * strength_ + .2f * strength;
        const float parallax = (options_.flags & TTP_OSU_PARALLAX) ? 1.0f / 60 : 0;
        // CircularHitTest 0x06000747 uses the rendered centre and the animated
        // scale, with a separate 300px logical interaction diameter.
        const float dx = (pointer_x_ - .5f) * logical_width_ * (1 + parallax);
        const float pointer_y = 240 + (pointer_y_ - .5f) * surface_logical_height_;
        const float dy = (pointer_y - 240) * (1 + parallax);
        const float radius = 150 * (1.05f + hover_ - .05f * Ease(pulse_));
        const bool over = pointer_inside_ && pointer_y >= 0 && pointer_y <= 480 &&
            dx * dx + dy * dy < radius * radius;
        // MainMenu's departure branch replaces hover accumulation completely.
        if (transition_phase_ != 2)
            hover_ = std::clamp(hover_ + (over ? .012f : -.012f), 0.0f, .1f);
    }
    double remaining = dt;
    while (remaining > 0) {
        const double slice = std::min(remaining, 10.0 - ring_remainder_);
        RingStep(slice, playing && frame.count);
        remaining -= slice;
        ring_remainder_ += slice;
        if (ring_remainder_ >= 10.0 - 1e-9) {
            ring_remainder_ = 0;
            shift_ = (shift_ + 50) % 1024;
        }
    }
    std::erase_if(particles_, [&](const Particle& particle) {
        return now_ >= particle.start && now_ - particle.start >= particle.lifetime;
    });
    std::erase_if(waves_, [&](const Wave& wave) { return now_ - wave.start >= 1000; });
}

RenderFrame Scene::BuildFrame(uint32_t width, uint32_t height) const {
    if (!width || !height || width > 16384 || height > 16384) throw Failure{E_INVALIDARG};
    RenderFrame result{width, height, options_.background, {}};
    result.quads.reserve(1024 + particles_.size() + waves_.size() + 8);
    // Original Screen/viewport caps H at floor(W*.8) (0x060011B9,
    // 0x06001D99). Centre that common canvas inside TTPlayer's full monitor,
    // equivalent to osu!'s centred letterbox path; never shrink only the logo.
    const uint32_t canvas_height = std::min(height, std::max(1u, width * 4 / 5));
    const float scale = static_cast<float>(canvas_height) / 480;
    const float top = static_cast<float>(height - canvas_height) * .5f;
    const float logical_width = static_cast<float>(width) / scale;
    logical_width_ = logical_width;
    surface_logical_height_ = height / scale;
    const float pointer_y = 240 + (pointer_y_ - .5f) * surface_logical_height_;
    const bool pointer_inside = pointer_inside_ && pointer_y >= 0 && pointer_y <= 480;
    const double transition_age = TransitionAge();
    const bool departing = transition_phase_ == 2;
    const bool interrupted_welcome = departing && exit_from_welcome_;
    const bool welcome = interrupted_welcome || (transition_phase_ == 1 && transition_age < WelcomeCompleteMs);
    const double welcome_age = interrupted_welcome ? exit_welcome_age_ : transition_age;
    const float departure_ticks = departing ? static_cast<float>(std::min(transition_age, DepartureMs) / FrameMs) : 0;
    const float departure_scale = -.003f * departure_ticks;
    const float departure_rotation = .001f * departure_ticks;
    const float foreground_x = !welcome && (options_.flags & TTP_OSU_PARALLAX) && pointer_inside ?
        (pointer_x_ - .5f) * logical_width / 60 : 0;
    const float foreground_y = !welcome && (options_.flags & TTP_OSU_PARALLAX) && pointer_inside ?
        (pointer_y - 240) / 60 : 0;
    const float cx = logical_width * .5f - foreground_x, cy = 240 - foreground_y;
    const float logo_scale = 1.05f + hover_ + departure_scale - .05f * Ease(pulse_);
    const float halo_scale = 1.05f + hover_ + departure_scale + .03f * Ease(pulse_);
    const auto add = [&](Texture texture, float x, float y, float w, float h, float rotation,
        uint32_t color, bool additive = false, bool flip = false) {
        result.quads.push_back({x * scale, top + y * scale, w * scale, h * scale, rotation, color, texture, additive, flip});
    };
    const auto add_logo = [&] {
        if (!(options_.flags & TTP_OSU_LOGO)) return;
        add(Texture::Logo, cx, cy, logo_width_*logo_scale, logo_height_*logo_scale, departure_rotation,
            Color(RGB(255,255,255), 1 - .002f * departure_ticks));
        // The original writes departure alpha to both sprites, then overwrites
        // the halo with the normal pulse alpha later in MainMenu.Update.
        const float halo_alpha = (beat_.kiai ? .1f : .4f) * (1-Ease(pulse_)) * strength_latched_;
        add(Texture::Logo, cx, cy, logo_width_*halo_scale, logo_height_*halo_scale, departure_rotation,
            Color(RGB(255,255,255), halo_alpha), beat_.kiai && playback_ == 2);
    };
    if (!image_.pixels.empty()) {
        const float cover = std::max(logical_width / image_.width, 480.0f / image_.height) *
            ((options_.flags & TTP_OSU_PARALLAX) ? 1.01f : 1.0f);
        add(Texture::Background, logical_width * .5f - foreground_x / 3, 240 - foreground_y / 3,
            image_.width * cover, image_.height * cover, 0, 0xffffffff);
    }
    if (options_.flags & TTP_OSU_FLASH) {
        // The original flashes explicitly bypass the manager offset. Their
        // edges stay anchored to the viewport while stars/logo use parallax.
        const float w = 310 * TextureToLogical, h = 192 * 4 * TextureToLogical;
        add(Texture::Flash, w * .5f, 240, w, h, 0, Color(RGB(255,255,255), FlashAlpha(left_flash_)), true, true);
        add(Texture::Flash, logical_width - w * .5f, 240, w, h, 0,
            Color(RGB(255,255,255), FlashAlpha(right_flash_)), true);
    }
    if (options_.flags & TTP_OSU_LOGO) for (const auto& wave : waves_) {
        const float age = Unit(static_cast<float>(now_ - wave.start) / 1000);
        const float size = 520 * TextureToLogical * wave.scale * (1 + .4f * Ease(age));
        add(Texture::Shockwave, cx, cy, size, size, 0, Color(RGB(255,255,255), wave.strength * (1-age)), true);
    }
    if (options_.flags & TTP_OSU_STARS) for (const auto& particle : particles_) {
        if (now_ < particle.start) continue;
        const float age = static_cast<float>(now_ - particle.start);
        const float seconds = age / 1000, t = Unit(age / particle.lifetime), ease = Ease(t);
        const float x = (particle.right ? logical_width - 100 : 100) + particle.vx * seconds - foreground_x;
        const float y = 490 + particle.vy * seconds + .5f * 864 * seconds * seconds - foreground_y;
        const float size = TextureToLogical * (1 + (particle.final_scale - 1) * ease);
        add(Texture::Star, x, y, 33 * size, 34 * size, particle.rotation_start +
            (particle.rotation_end - particle.rotation_start) * ease, Color(RGB(255,255,255), 1-ease), true);
    }
    const bool logo_before_ring = welcome && welcome_age < WelcomeLayerSwitchMs;
    if (logo_before_ring) add_logo();
    if (welcome && welcome_age < WelcomeRevealMs + 240) {
        const float mask_alpha = 1 - Unit(static_cast<float>((welcome_age - WelcomeRevealMs) / 240));
        result.quads.push_back({width * .5f, height * .5f, static_cast<float>(width), static_cast<float>(height),
            0, Color(RGB(0,0,0), mask_alpha), Texture::White});
        const float welcome_alpha = welcome_age < WelcomeRevealMs ?
            Unit(static_cast<float>(welcome_age / 1800)) :
            1 - Unit(static_cast<float>((welcome_age - WelcomeRevealMs) / 120));
        const float text_alpha = welcome_alpha * (interrupted_welcome ?
            1 - Unit(static_cast<float>(transition_age / 120)) : 1);
        const float text_scale = .9f + .0008f * static_cast<float>(std::min(welcome_age, WelcomeRevealMs) / FrameMs);
        const float text_scale_y = Ease(Unit(static_cast<float>((welcome_age - 200) / 400)));
        if (text_alpha > 0 && text_scale_y > 0)
            add(Texture::Welcome, logical_width * .5f, 240, 375 * TextureToLogical * text_scale,
                78 * TextureToLogical * text_scale * text_scale_y, 0, Color(RGB(255,255,255), text_alpha));
    }
    if (options_.flags & TTP_OSU_SPECTRUM) {
        const float radius = 150 * (1.05f + hover_ + departure_scale + .03f * (1-pulse_) * (1-pulse_)) +
            (welcome && welcome_age < WelcomeRevealMs ? 8.0f : 0.0f);
        // The original uses process-global startup time here. A DLL can enter
        // repeatedly, so use its local welcome clock with the same slope.
        const float ring_alpha = logo_before_ring ? .2f * static_cast<float>(welcome_age / 2558) :
            (beat_.kiai ? 1.0f : .7f);
        for (size_t i = 0; i < bars_.size(); ++i) {
            const float alpha = std::max(0.0f, .4f * ring_alpha * std::min(1.0f, (bars_[i]-.04f)/.08f));
            if (alpha <= 0 || bars_[i] < .01f) continue;
            const float theta = static_cast<float>(2 * Pi * (.4 + static_cast<double>(i) / 1024 * 4));
            const float length = 115 * TextureToLogical * bars_[i];
            add(Texture::Spectrum, cx + (radius+length*.5f)*std::cos(theta), cy+(radius+length*.5f)*std::sin(theta),
                length, 38 * .5f * TextureToLogical, theta, Color(options_.accent, alpha), true);
        }
    }
    if (!logo_before_ring) add_logo();
    if (departing) {
        const float progress = Unit(static_cast<float>(transition_age / DepartureMs));
        result.quads.push_back({width * .5f, height * .5f, static_cast<float>(width), static_cast<float>(height),
            0, Color(RGB(0,0,0), progress), Texture::White});
    }
    if (top > 0) {
        // Identical CPU/GPU clipping of the effective viewport, including
        // moving stars and long bars. These masks are outside the scene layers.
        result.quads.push_back({width * .5f, top * .5f, static_cast<float>(width), top,
            0, 0xff000000, Texture::White});
        result.quads.push_back({width * .5f, height - top * .5f, static_cast<float>(width), top,
            0, 0xff000000, Texture::White});
    }
    return result;
}
TtpOsuVisualStats Scene::Stats() const noexcept {
    const float departure_scale = transition_phase_ == 2 ?
        -.003f * static_cast<float>(std::min(TransitionAge(), DepartureMs) / FrameMs) : 0;
    return {sizeof(TtpOsuVisualStats), static_cast<uint32_t>(particles_.size()), bursts_, fft_count_,
        static_cast<uint32_t>(timing_.size()), beat_.kiai ? 1u : 0u, 0,
        1.05f + hover_ + departure_scale - .05f * Ease(pulse_), FlashAlpha(left_flash_), FlashAlpha(right_flash_), revision_};
}
}
