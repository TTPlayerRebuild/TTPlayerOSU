// osu!lazer visual algorithms adapted from ppy/osu (MIT).
// See licenses/osu-MIT.txt and assets/README.md for source/resource provenance.
#include "visual_scene.h"
#include "parser.h"
#include "visual_torus_metrics.h"
#include <algorithm>
#include <cmath>
#include <string_view>

namespace ttp_osu::visual {
namespace {
constexpr float Pi = 3.14159265358979323846f;
constexpr float LogoSize = 512, CircleSize = LogoSize * .94f;
float Clamp(float v) { return std::clamp(v, 0.f, 1.f); }
float Out(float t) { t = Clamp(t); return t * (2 - t); }
float Quint(float t) { return 1 - std::pow(1 - Clamp(t), 5.f); }
float Elastic(float t) {
    t = Clamp(t);
    if (t == 0 || t == 1) return t;
    return std::pow(2.f, -10 * t) * std::sin((10 * t - .75f) * (2 * Pi / 3)) + 1;
}
uint32_t Tint(COLORREF rgb, float alpha = 1) {
    return (static_cast<uint32_t>(Clamp(alpha) * 255 + .5f) << 24) |
        (GetRValue(rgb) << 16) | (GetGValue(rgb) << 8) | GetBValue(rgb);
}
float Damp(float from, float to, float retention, double elapsed) {
    return to + (from - to) * static_cast<float>(std::pow(retention, elapsed));
}

}

void Scene::SetIntroText(bool custom, std::array<std::wstring,3> fields, IntroFont font) {
    custom_intro_text_=custom;
    intro_text_=std::move(fields);
    intro_glyphs_=std::move(font.glyphs);
    intro_font_width_=font.width;intro_font_height_=font.height;
}

void Scene::SetStyle(uint32_t style) {
    if (style > 1) throw Failure{E_INVALIDARG};
    if (style_ == style) return;
    style_ = style;
    ResetTemporal();
    // A style switch is a new scene, never inherit a half-finished other intro.
    transition_phase_ = 0;
    exit_from_welcome_ = false;
}

void Scene::ResetLazer() {
    lazer_bars_.fill(0);
    lazer_sample_time_ = lazer_triangle_time_ = 0;
    lazer_beat_age_ = 1e9;
    lazer_amplitude_scale_ = lazer_hover_scale_ = lazer_hover_from_ = lazer_hover_target_ = 1;
    lazer_hover_age_ = 500;
    lazer_beat_strength_ = lazer_flash_ = 0;
    lazer_pending_velocity_ = 0;
    lazer_bounce_scale_ = lazer_bounce_from_ = 1;
    lazer_bounce_age_ = 1000;
    lazer_click_age_ = 1e9;
    lazer_button_down_ = lazer_press_armed_ = lazer_bounce_releasing_ = false;
    lazer_visualizer_alpha_ = .5f;
    lazer_velocity_ = .5f;
    lazer_offset_ = 0;
    lazer_over_ = false;
    lazer_parallax_x_ = lazer_parallax_y_ = lazer_background_x_ = lazer_background_y_ = 0;
    lazer_triangles_.clear();
    ++lazer_fill_revision_;
}

float Scene::LazerLogoScale() const noexcept {
    float beat_scale = 1;
    if (lazer_beat_age_ < 60)
        beat_scale = 1 - .02f * lazer_beat_strength_ * Out(static_cast<float>(lazer_beat_age_ / 60));
    else
        beat_scale = 1 - .02f * lazer_beat_strength_ *
            (1 - Quint(static_cast<float>((lazer_beat_age_ - 60) / std::max(1.0, beat_.length * 2))));
    const float hover = transition_phase_ == 2 ?
        lazer_hover_scale_ + (1-lazer_hover_scale_) * Quint(static_cast<float>(TransitionAge() / 350)) : lazer_hover_scale_;
    const float bounce = transition_phase_ == 2 ?
        lazer_bounce_scale_ + (1-lazer_bounce_scale_) * Quint(static_cast<float>(TransitionAge()/350)) : lazer_bounce_scale_;
    return hover * bounce * lazer_amplitude_scale_ * beat_scale;
}

void Scene::LazerBurst() {
    if (!(options_.flags & TTP_OSU_STARS)) return;
    // The original emits 240/s per side for 800 ms. Schedule the same particles
    // in bounded storage so output FPS cannot change the emitted count.
    constexpr size_t per_side = 192;
    if (particles_.size() + per_side * 2 > 768)
        particles_.erase(particles_.begin(), particles_.begin() +
            static_cast<ptrdiff_t>(particles_.size() + per_side * 2 - 768));
    const int direction = static_cast<int>(Random() % 3) - 1;
    for (size_t i = 0; i < per_side; ++i) {
        const double time = static_cast<double>(i) * 1000 / 240;
        for (unsigned side = 0; side < 2; ++side) {
            const float horizontal = direction * (side ? 1.f : -1.f) * 500 *
                (1 - 2 * static_cast<float>(time / 800));
            particles_.push_back({now_ + static_cast<uint64_t>(time), RandomRange(300,1000),
                horizontal + RandomRange(-60,60), -1400 + RandomRange(-100,100),
                2.2f + RandomRange(-.4f,.4f), RandomRange(-4,4),RandomRange(-2,2),side != 0});
        }
    }
    last_burst_ = now_; burst_valid_ = true; ++bursts_;
}

void Scene::UpdateLazer(double elapsed, const TtpOsuVisualFrame& frame) {
    const bool playing = frame.playback == 2;
    const double position = frame.playback == 0 || frame.playback == 4 ?
        static_cast<double>(now_ % 1000) : frame.position_ms;
    const auto timeline = frame.playback == 0 || frame.playback == 4 ?
        std::span<const TimingPoint>{} : std::span<const TimingPoint>(timing_);
    const double bpm = frame.playback == 0 || frame.playback == 4 ? 60 : options_.fallback_bpm;
    // OsuLogo fires 60 ms early, whereas Kiai fountains use the actual position.
    BeatState next = EvaluateTiming(timeline, position + 60, bpm);
    const BeatState actual = EvaluateTiming(timeline, position, bpm);
    const bool new_beat = !beat_valid_ || next.index != beat_.index || next.length != beat_.length;
    if (playing && actual.kiai && !beat_.kiai && std::abs(position - actual.marker) < 500 &&
        (!burst_valid_ || now_ - last_burst_ > 500)) LazerBurst();
    lazer_beat_age_ += elapsed;
    if (new_beat && next.index >= 0) {
        lazer_beat_age_ = next.phase * next.length;
        lazer_beat_strength_ = std::min(1.f, .4f + std::max(left_peak_, right_peak_));
        lazer_pending_velocity_ = playing ? lazer_beat_strength_ * (next.kiai ? 6 : 3) : 0;
        const auto flash = [&](Flash& target, float channel) {
            const float factor = next.kiai ? (1 - .25f*.95f)/.8f : (1-.25f)/.55f;
            const float peak = std::clamp(.1f+(channel-.25f)/factor,.1f,1.f);
            target = {now_,FlashAlpha(target),peak,next.length+65};
        };
        if (playing && (options_.flags & TTP_OSU_FLASH)) {
            if (next.kiai ? next.index % 2 == 0 : next.index % next.meter == 0) flash(left_flash_,left_peak_);
            if (next.kiai ? next.index % 2 != 0 : next.index % next.meter == 0) flash(right_flash_,right_peak_);
        }
    }
    beat_ = next;
    beat_.kiai = actual.kiai;
    beat_valid_ = true;
    // The logo contracts early, but OsuLogo schedules the triangle impulse at
    // the actual beat (60 ms later). Keep these two clocks separate.
    if(lazer_beat_age_>=60 && lazer_pending_velocity_>0) {
        lazer_velocity_+=lazer_pending_velocity_;lazer_pending_velocity_=0;
    }
    const float maximum = playing ? std::max(left_peak_,right_peak_) : 0;
    lazer_amplitude_scale_ = Damp(lazer_amplitude_scale_,1-std::max(0.f,maximum-.4f)*.04f,.9f,elapsed);
    lazer_velocity_ = Damp(lazer_velocity_,playing ? (actual.kiai ? 2.f : 1.f) : .5f,
        playing ? .995f : .9f,elapsed);
    const float beat_progress = Clamp(static_cast<float>(lazer_beat_age_ / 60));
    const float after_beat = Clamp(static_cast<float>((lazer_beat_age_-60)/std::max(1.0,next.length)));
    lazer_flash_ = actual.kiai ? .2f*lazer_beat_strength_*Out(beat_progress)*(1-after_beat) : 0;
    lazer_visualizer_alpha_ = actual.kiai ? .5f+(.9f*lazer_beat_strength_-.5f)*Out(beat_progress)*(1-after_beat) : .5f;
    // Original scheduler explicitly disables catch-up executions after stalls.
    lazer_sample_time_ += elapsed;
    if (lazer_sample_time_ >= 50) {
        lazer_sample_time_ = 0;
        for (size_t i=0;i<lazer_bars_.size();++i) {
            const float amplitude = playing && frame.count ? fft_[(i+lazer_offset_)%200]*(actual.kiai?1.f:.5f) : 0;
            lazer_bars_[i] = std::max(lazer_bars_[i],amplitude);
        }
        lazer_offset_ = (lazer_offset_+5)%200;
    }
    for (auto& bar:lazer_bars_) bar=std::max(0.f,bar-static_cast<float>(elapsed)*.0024f*(bar+.03f));
    const float dx=(pointer_x_-.5f)*logical_width_, dy=(pointer_y_-.5f)*surface_logical_height_;
    const float radius=CircleSize*.5f*LazerLogoScale();
    const bool over=pointer_inside_ && dx*dx+dy*dy < radius*radius;
    if (over!=lazer_over_) {
        lazer_over_=over; lazer_hover_from_=lazer_hover_scale_;
        lazer_hover_target_=over?1.1f:1.f; lazer_hover_age_=0;
    }
    lazer_hover_age_+=static_cast<float>(elapsed);
    lazer_hover_scale_=lazer_hover_from_+(lazer_hover_target_-lazer_hover_from_)*Elastic(lazer_hover_age_/500);
    const bool button=(frame.pointer_buttons&1)!=0;
    const bool interaction=(options_.flags&TTP_OSU_LOGO) && transition_phase_!=2 &&
        !(transition_phase_==1 && TransitionAge()<3000);
    lazer_bounce_age_+=static_cast<float>(elapsed);
    lazer_click_age_+=elapsed;
    if(!interaction && lazer_press_armed_) {
        lazer_press_armed_=false; lazer_bounce_releasing_=true;
        lazer_bounce_from_=lazer_bounce_scale_; lazer_bounce_age_=0;
    }
    if(interaction && button && !lazer_button_down_ && over) {
        lazer_press_armed_=true; lazer_bounce_releasing_=false;
        lazer_bounce_from_=lazer_bounce_scale_; lazer_bounce_age_=0;
    } else if(!button && lazer_button_down_ && lazer_press_armed_) {
        lazer_press_armed_=false; lazer_bounce_releasing_=true;
        lazer_bounce_from_=lazer_bounce_scale_; lazer_bounce_age_=0;
        if(interaction && over) lazer_click_age_=0;
    }
    lazer_button_down_=button;
    if(lazer_bounce_releasing_)
        lazer_bounce_scale_=lazer_bounce_from_+(1-lazer_bounce_from_)*Elastic(lazer_bounce_age_/500);
    else if(lazer_press_armed_)
        lazer_bounce_scale_=lazer_bounce_from_+(.9f-lazer_bounce_from_)*Out(lazer_bounce_age_/1000);
    const auto offset=[](float v,float half) {
        return std::copysign(1-std::pow(.999f,std::abs(v)),v)*half*.02f;
    };
    const float px=(options_.flags&TTP_OSU_PARALLAX) && pointer_inside_ ? offset(dx,logical_width_*.5f):0;
    const float py=(options_.flags&TTP_OSU_PARALLAX) && pointer_inside_ ? offset(dy,surface_logical_height_*.5f):0;
    const float smooth=Quint(static_cast<float>(elapsed/100));
    lazer_background_x_+=(px-lazer_background_x_)*smooth;
    lazer_background_y_+=(py-lazer_background_y_)*smooth;
    // TrianglesV2 count, Gaussian speed, size and upward movement follow the
    // original. Only rasterization is shared between software and D3D9 here.
    constexpr float triangleHeight=300*.866f/CircleSize;
    constexpr size_t triangleCount=13; // floor(481.28 * .02 * 1.4)
    const auto spawn=[&](bool random_y) {
        const float u1=std::max(.000001f,1-RandomRange(0,1)),u2=1-RandomRange(0,1);
        const float speed=std::max(.1f,.5f+.16f*std::sqrt(-2*std::log(u1))*std::sin(2*Pi*u2));
        return LazerTriangle{RandomRange(0,1),random_y?RandomRange(-triangleHeight,1):1,speed};
    };
    if (lazer_triangles_.empty()) for(size_t i=0;i<triangleCount;++i) lazer_triangles_.push_back(spawn(true));
    for(auto& triangle:lazer_triangles_) {
        triangle.y-=static_cast<float>(elapsed/1000)*lazer_velocity_*50/CircleSize*std::max(.5f,triangle.speed);
        if(triangle.y+triangleHeight<0) triangle=spawn(false);
    }
    if(elapsed>0) ++lazer_fill_revision_;
    std::erase_if(particles_,[&](const Particle& p){return now_>=p.start && now_-p.start>=p.lifetime;});
}

RenderFrame Scene::BuildLazerFrame(uint32_t width,uint32_t height) const {
    RenderFrame result{width,height,options_.background,{}};
    result.quads.reserve(1200+particles_.size());
    // DrawSizePreservingFillContainer default 1024x768. Wide and tall surfaces
    // extend the logical viewport; they do not distort or letterbox the logo.
    const float scale=std::min(width/1024.f,height/768.f);
    const float w=width/scale,h=height/scale,cx=w*.5f,cy=h*.5f;
    logical_width_=w;surface_logical_height_=h;
    const double age=TransitionAge();
    const bool entering=transition_phase_==1 && age<4000;
    const bool departing=transition_phase_==2;
    const bool intro=entering && age<3000;
    const float global_alpha=departing?1-Clamp(static_cast<float>(age/3000)):1;
    // The shipped 2026.1005.0 runtime keeps the glyph orientation fixed while
    // exiting. IntroScreen contains a RotateTo transform, but its screen-chain
    // execution does not render that turn (verified by two capture sequences).
    constexpr float rotation=0;
    const auto add=[&](Texture texture,float x,float y,float qwidth,float qheight,float rot,
        uint32_t color,bool additive=false,bool flip=false) -> Quad& {
        result.quads.push_back({x*scale,y*scale,qwidth*scale,qheight*scale,rot,color,texture,additive,flip});
        return result.quads.back();
    };
    if(!intro && !image_.pixels.empty()) {
        const float cover=std::max(w/image_.width,h/image_.height)*(1+100/w)*
            ((options_.flags&TTP_OSU_PARALLAX)?1.02f:1);
        const float background_alpha=departing?1-Clamp(static_cast<float>(age/100)):1;
        add(Texture::Background,cx+lazer_background_x_,cy+lazer_background_y_,image_.width*cover,
            image_.height*cover,0,Tint(RGB(255,255,255),background_alpha));
    }
    if(intro || departing) {
        add(Texture::White,cx,cy,w,h,0,Tint(RGB(0,0,0),intro?1:Clamp(static_cast<float>(age/100))));
    }
    if(intro) {
        // The default lazer Triangles sequence uses a monotonic visual clock,
        // not the user's song position. Seeking and pausing cannot strand it.
        if(age>=200 && age<1450) {
            const std::array<std::wstring,3> automatic{L"welcome",L"to",logo_kind_==0?L"TTPlayer":L"osu!"};
            const auto& fields=custom_intro_text_?intro_text_:automatic;
            std::wstring text=fields[0];
            if(age<400) {
                size_t offset=0;
                for(int i=0;i<3 && offset<text.size();++i)NextCodepoint(text,offset);
                text.resize(offset);
            } else {
                const int count=age<700?1:age<900?2:3;
                for(int i=1;i<count;++i) if(!fields[i].empty()) {
                    if(!text.empty())text+=L' ';
                    text+=fields[i];
                }
            }
            const float spacing=5+45*Clamp(static_cast<float>((age-900)/5000));
            constexpr float fontScale=42.f/100;
            struct Letter {const IntroGlyph* glyph;bool fallback;};
            std::vector<Letter> letters;
            float length=0;
            for(size_t offset=0;offset<text.size();) {
                const auto code=NextCodepoint(text,offset);
                const auto* g=FindTorusGlyph(code);
                const bool fallback=!g;
                if(!g)for(const auto& item:intro_glyphs_)if(item.code==code){g=&item;break;}
                if(!g)g=FindTorusGlyph('?');
                letters.push_back({g,fallback && g->code==code});
                length+=g->advance*fontScale+spacing;
            }
            if(!letters.empty())length-=spacing;
            // User text can be longer than the original phrase. Fit the whole
            // run in 90% of the viewport, retaining per-letter animated spacing.
            const float fit=length>0?std::min(1.f,w*.9f/length):1;
            float x=cx-length*fit*.5f;
            for(const auto& letter:letters) {
                const auto& g=*letter.glyph;
                if(g.code!=' ' && g.width>0 && g.height>0) {
                    auto& q=add(letter.fallback?Texture::LazerCustomFont:Texture::LazerFont,
                        x+(g.offset_x+g.width*.5f)*fontScale*fit,
                        cy+((g.offset_y+g.height*.5f-50)*fontScale-5)*fit,
                        g.width*fontScale*fit,g.height*fontScale*fit,0,0xffffffff);
                    const float tw=letter.fallback?static_cast<float>(intro_font_width_):1020;
                    const float th=letter.fallback?static_cast<float>(intro_font_height_):810;
                    q.u0=g.x/tw;q.v0=g.y/th;q.u1=(g.x+g.width)/tw;q.v1=(g.y+g.height)/th;
                }
                x+=(g.advance*fontScale+spacing)*fit;
            }
            if(age>=1060) {
                // Deterministic 22 ms spawns, 120 ms life. A local hash avoids
                // changing song particle RNG when CPU and GPU paint twice.
                const uint32_t last=static_cast<uint32_t>((age-1060)/22);
                for(uint32_t n=last>5?last-5:0;n<=last;++n) {
                    uint32_t hash=options_.seed ^ (n*0x9e3779b9u+17);
                    const auto rnd=[&]() { hash^=hash<<13;hash^=hash>>17;hash^=hash<<5;return (hash>>8)/16777216.f; };
                    const bool outlined=rnd()<.5f;
                    const float size=(rnd()+.2f)*80;
                    const float tx=cx+(.4f*w)*(rnd()-.5f),ty=cy+(.16f*h)*(rnd()-.5f);
                    add(outlined?Texture::OutlineTriangle:Texture::Triangle,tx,ty,size,size,0,
                        Tint(RGB(255,255,255),1-Clamp(static_cast<float>((age-1060-n*22)/120))),true);
                }
            }
        }
        if(age>=1450 && age<2080) {
            const float spacing=age<1650?200.f:age<1850?30.f:10.f;
            const float iconScale=age<1650?1.f:age<1850?2.f:4.f;
            const float containerScale=age<1850?1-.2f*Clamp(static_cast<float>((age-1450)/1000)):
                .92f+.38f*Clamp(static_cast<float>((age-1850)/1000));
            const float unit=iconScale*containerScale;
            constexpr Texture icons[]{Texture::RulesetOsu,Texture::RulesetTaiko,Texture::RulesetCatch,Texture::RulesetMania};
            for(unsigned i=0;i<4;++i) add(icons[i],cx+(static_cast<float>(i)-1.5f)*(30+spacing)*unit,cy,30*unit,30*unit,0,0xffffffff);
        }
        if(age>=2080 && (options_.flags&TTP_OSU_LOGO)) {
            const float progress=Clamp(static_cast<float>((age-2080)/920));
            const float inner=1.2f-.8f*std::pow(Clamp((progress-.7f)/.3f),5.f);
            const float outer=1.2f-.2f*progress*progress;
            const float size=960*inner*outer;
            if(logo_kind_==0 || logo_kind_==1) {
                result.lazer_intro_progress=progress;
                add(logo_kind_==0?Texture::LazerTTPlayerIntro:Texture::LazerIntro,
                    cx,cy,size,size,0,0xffffffff);
            } else {
                // Arbitrary custom images have no calibrated RG data. Their
                // colour channels must never be treated as reveal thresholds.
                const float factor=size/LogoSize;
                add(Texture::Logo,cx,cy,LogoSize*(logo_width_/337.5f)*factor,
                    LogoSize*(logo_height_/337.5f)*factor,0,Tint(RGB(255,255,255),Out(progress)));
            }
        }
        return result;
    }
    const float logoScale=LazerLogoScale();
    const float radius=CircleSize*.5f*logoScale;
    const float beatAge=static_cast<float>(lazer_beat_age_);
    const float rippleProgress=Clamp(beatAge/static_cast<float>(std::max(1.0,beat_.length)));
    if(options_.flags&TTP_OSU_LOGO) {
        const Texture tex=logo_kind_==1?Texture::LazerLogo:Texture::Logo;
        const float rippleScale=lazer_hover_scale_*lazer_bounce_scale_*lazer_amplitude_scale_*(1+.04f*lazer_beat_strength_*Quint(rippleProgress));
        const float aspectX=logo_kind_==1?1:logo_width_/337.5f,aspectY=logo_kind_==1?1:logo_height_/337.5f;
        if(!departing) add(tex,cx,cy,LogoSize*aspectX*rippleScale,LogoSize*aspectY*rippleScale,rotation,
            Tint(RGB(255,255,255),.15f*lazer_beat_strength_*(1-Quint(rippleProgress))),true);
    }
    if(options_.flags&TTP_OSU_SPECTRUM) {
        const float barWidth=CircleSize*std::sin(Pi/200)*logoScale;
        for(unsigned round=0;round<5;++round) for(size_t i=0;i<200;++i) {
            if(lazer_bars_[i]<1.f/600) continue;
            const float angle=2*Pi*(static_cast<float>(i)/200+round/5.f)+rotation;
            const float length=600*lazer_bars_[i]*logoScale;
            add(Texture::White,cx+(radius+length*.5f)*std::cos(angle),cy+(radius+length*.5f)*std::sin(angle),
                length,barWidth,angle,Tint(options_.accent,.2f*lazer_visualizer_alpha_*global_alpha),true);
        }
    }
    if(options_.flags&TTP_OSU_LOGO) {
        if(logo_kind_==1) {
            result.lazer_triangles=lazer_triangles_;
            result.lazer_fill_revision=lazer_fill_revision_;
            // On return to IntroScreen, Triangles and Ripple both fade out.
            const float fill_alpha=departing?1-Quint(static_cast<float>(age/300)):1;
            add(Texture::LazerFill,cx,cy,CircleSize*logoScale,CircleSize*logoScale,rotation,
                Tint(RGB(255,255,255),fill_alpha*global_alpha));
            const float clickFlash=lazer_click_age_<1500?
                .4f*std::pow(2.f,-10*static_cast<float>(lazer_click_age_/1500)):0;
            const float flash=std::max(lazer_flash_,clickFlash);
            if(flash>0 && !departing)
                add(Texture::LazerCircle,cx,cy,CircleSize*logoScale,CircleSize*logoScale,rotation,
                    Tint(RGB(255,255,255),flash),true);
        }
        const float logoAlpha=departing?1-Clamp(static_cast<float>((age-350)/3000)):1;
        add(logo_kind_==1?Texture::LazerLogo:Texture::Logo,cx,cy,
            LogoSize*(logo_kind_==1?1:logo_width_/337.5f)*logoScale,
            LogoSize*(logo_kind_==1?1:logo_height_/337.5f)*logoScale,rotation,
            Tint(RGB(255,255,255),logoAlpha*global_alpha));
    }
    // MainMenu draw order: logo proxy, side flashes, then the Kiai fountains.
    if(options_.flags&TTP_OSU_FLASH) {
        const float fade=departing?global_alpha:1;
        add(Texture::LazerFlash,0,cy,400,h*1.5f,0,Tint(RGB(102,204,255),FlashAlpha(left_flash_)*.6f*fade),true,true);
        add(Texture::LazerFlash,w,cy,400,h*1.5f,0,Tint(RGB(102,204,255),FlashAlpha(right_flash_)*.6f*fade),true);
    }
    if(options_.flags&TTP_OSU_STARS) for(const auto& particle:particles_) {
        if(now_<particle.start) continue;
        const float seconds=static_cast<float>(now_-particle.start)/1000;
        const float progress=Clamp(static_cast<float>(now_-particle.start)/particle.lifetime);
        const float x=(particle.right?w-250:250)+particle.vx*seconds;
        const float y=h+50+(particle.vy+800*seconds)*seconds;
        const float size=1+(particle.final_scale-1)*progress;
        add(Texture::LazerStar,x,y,33*size,34*size,
            -(particle.rotation_start+(particle.rotation_end-particle.rotation_start)*progress),
            Tint(RGB(255,255,255),(1-progress)*global_alpha),true);
    }
    if(entering && age>=3000) add(Texture::White,cx,cy,w,h,0,
        Tint(RGB(255,255,255),1-Out(static_cast<float>((age-3000)/1000))),true);
    return result;
}
}
