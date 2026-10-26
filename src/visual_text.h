#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ttp_osu::visual {
// 100 px em, baseline 84, matching the embedded Torus Light atlas.
struct IntroGlyph {
    uint32_t code;
    float x, y, width, height, offset_x, offset_y, advance;
};
inline uint32_t NextCodepoint(std::wstring_view text, size_t& offset) noexcept {
    uint32_t c=text[offset++];
    if(c>=0xd800 && c<=0xdbff) {
        if(offset<text.size() && text[offset]>=0xdc00 && text[offset]<=0xdfff)
            return 0x10000+((c-0xd800)<<10)+(text[offset++]-0xdc00);
        return 0xfffd;
    }
    return c>=0xdc00 && c<=0xdfff ? 0xfffd : c;
}
struct IntroFont {
    std::vector<IntroGlyph> glyphs;
    uint32_t width{}, height{};
    std::vector<uint8_t> pixels;
};
IntroFont MakeIntroFont(const std::array<std::wstring,3>& fields);
}
