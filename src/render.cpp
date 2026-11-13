#include "dashboard.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include <array>
#include <climits>
#include <hb-ft.h>
#include <regex>
#include <sstream>

namespace dash {
Renderer::Renderer(const fs::path &assets) : assets_(assets) {
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library))
        throw std::runtime_error("FreeType initialization failed");
    library_ = library;
    for (int i = 0; i < 2; ++i) {
        FT_Face f = nullptr;
        auto p = assets / "fonts" / (i ? "DejaVuSans-Bold.ttf" : "DejaVuSans.ttf");
        if (FT_New_Face(library, p.c_str(), 0, &f))
            throw std::runtime_error("Missing bundled font: " + p.string());
        faces_[i] = f;
    }
    FT_Face cjk = nullptr;
    auto cjk_path = assets / "fonts" / "NotoSansSC-Regular.otf";
    if (FT_New_Face(library, cjk_path.c_str(), 0, &cjk))
        throw std::runtime_error("Missing bundled font: " + cjk_path.string());
    faces_[2] = cjk;
}
Renderer::~Renderer() {
    for (auto f : faces_)
        if (f)
            FT_Done_Face((FT_Face)f);
    if (library_)
        FT_Done_FreeType((FT_Library)library_);
}
void *Renderer::face(int size, bool bold, bool cjk) {
    auto f = (FT_Face)faces_[cjk ? 2 : bold];
    if (FT_Set_Pixel_Sizes(f, 0, size))
        throw std::runtime_error("Invalid font size");
    return f;
}
namespace {
int rnd(double n) {
    return int(std::nearbyint(n));
}
struct Font {
    int size;
    bool bold = false;
    bool cjk = false;
};
struct Metrics {
    int left = 0, top = 0, right = 0, bottom = 0, advance = 0, advance64 = 0, ascender = 0, descender = 0;
};
struct Glyph {
    unsigned index;
    int advance, xoffset, yoffset;
};
int pixel64(int value) {
    return (value + 32) >> 6;
}
std::vector<Glyph> shape(FT_Face face, const std::string &text) {
    auto *font = hb_ft_font_create_referenced(face);
    auto *buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, text.data(), int(text.size()), 0, int(text.size()));
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, nullptr, 0);
    unsigned count = 0;
    auto *info = hb_buffer_get_glyph_infos(buffer, &count);
    auto *positions = hb_buffer_get_glyph_positions(buffer, &count);
    std::vector<Glyph> glyphs;
    for (unsigned i = 0; i < count; ++i)
        glyphs.push_back(
            {info[i].codepoint, positions[i].x_advance, positions[i].x_offset, positions[i].y_offset});
    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
    return glyphs;
}
struct Draw {
    Image &image;
    Renderer &renderer;
    const fs::path &assets;
    Metrics metrics(const std::string &text, Font font) {
        auto face = (FT_Face)renderer.face(font.size, font.bold, font.cjk);
        Metrics m;
        m.top = 0;
        m.bottom = 0;
        m.ascender = int(face->size->metrics.ascender >> 6);
        m.descender = int(face->size->metrics.descender >> 6);
        int pen = 0;
        for (auto glyph : shape(face, text)) {
            FT_Load_Glyph(face, glyph.index, FT_LOAD_DEFAULT);
            auto &gm = face->glyph->metrics;
            int px = pixel64(pen + glyph.xoffset), py = pixel64(glyph.yoffset);
            m.left = std::min(m.left, px + int(gm.horiBearingX >> 6));
            m.right = std::max(m.right, px + int((gm.horiBearingX + gm.width + 63) >> 6));
            m.top = std::min(m.top, -py - int((gm.horiBearingY + 63) >> 6));
            m.bottom = std::max(m.bottom, -py + int((-gm.horiBearingY + gm.height + 63) >> 6));
            pen += glyph.advance;
        }
        m.advance64 = pen;
        m.advance = pixel64(pen);
        m.right = std::max(m.right, m.advance);
        return m;
    }
    int width(const std::string &s, Font f) {
        auto m = metrics(s, f);
        return m.right - m.left;
    }
    int height(const std::string &s, Font f) {
        auto m = metrics(s, f);
        return m.bottom - m.top;
    }
    Font fit(const std::string &s, int w, int size, bool bold = true, int minimum = 12) {
        Font f{size, bold};
        while (f.size > minimum && width(s, f) > w)
            --f.size;
        return f;
    }
    Font fit_cjk(const std::string &s, int w, int size, bool bold = true, int minimum = 12) {
        Font f{size, bold, true};
        while (f.size > minimum && width(s, f) > w)
            --f.size;
        return f;
    }
    std::vector<std::string> wrap_two(std::string s, Font font, int w) {
        s = trim(s);
        if (width(s, font) <= w)
            return {s};
        size_t end = 0, space = 0;
        for (size_t pos = 0; pos < s.size();) {
            size_t next = pos + 1;
            while (next < s.size() && ((unsigned char)s[next] & 0xc0) == 0x80)
                ++next;
            if (end && width(s.substr(0, next), font) > w)
                break;
            end = next;
            if (s[pos] == ' ')
                space = pos;
            pos = next;
        }
        size_t cut = space ? space : end;
        return {trim(s.substr(0, cut)), trim(s.substr(cut))};
    }
    std::string truncate(std::string s, Font f, int w) {
        if (width(s, f) <= w)
            return s;
        while (!s.empty() && width(s + "...", f) > w) {
            size_t start = s.size() - 1;
            while (start > 0 && ((unsigned char)s[start] & 0xc0) == 0x80)
                --start;
            s.resize(start);
            while (!s.empty() && s.back() == ' ')
                s.pop_back();
        }
        return s.empty() ? "..." : s + "...";
    }
    void text(int x, int y, std::string s, Font font, int color = 0, std::string anchor = "la",
              int maxwidth = 0) {
        if (maxwidth)
            s = truncate(s, font, maxwidth);
        auto m = metrics(s, font);
        if (anchor[0] == 'm')
            x -= pixel64(m.advance64 / 2);
        else if (anchor[0] == 'r')
            x -= m.advance;
        switch (anchor[1]) {
        case 'a':
            y += m.ascender;
            break;
        case 't':
            y -= m.top;
            break;
        case 'm':
            y += pixel64((m.ascender + m.descender) * 32);
            break;
        case 'b':
            y -= m.bottom;
            break;
        default:
            break;
        }
        auto face = (FT_Face)renderer.face(font.size, font.bold, font.cjk);
        int pen = 0;
        for (auto glyph : shape(face, s)) {
            FT_Load_Glyph(face, glyph.index, FT_LOAD_DEFAULT);
            FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
            auto &bm = face->glyph->bitmap;
            int gx = x + pixel64(pen + glyph.xoffset) + face->glyph->bitmap_left,
                gy = y - pixel64(glyph.yoffset) - face->glyph->bitmap_top;
            for (unsigned row = 0; row < bm.rows; ++row)
                for (unsigned col = 0; col < bm.width; ++col) {
                    int px = gx + int(col), py = gy + int(row);
                    if (px < 0 || py < 0 || px >= image.width || py >= image.height)
                        continue;
                    int a = bm.buffer[int(row) * bm.pitch + col];
                    auto &dst = image.pixels[size_t(py) * image.width + px];
                    dst = uint8_t((dst * (255 - a) + color * a + 127) / 255);
                }
            pen += glyph.advance;
        }
    }
    void rect(int x1, int y1, int x2, int y2, int color = 0, int border = 0) {
        for (int y = std::max(0, y1); y <= std::min(image.height - 1, y2); ++y)
            for (int x = std::max(0, x1); x <= std::min(image.width - 1, x2); ++x)
                if (!border || x < x1 + border || x > x2 - border || y < y1 + border || y > y2 - border)
                    image.pixel(x, y, color);
    }
    void polygon(const std::vector<std::pair<int, int>> &p, int color = 0) {
        int top = INT_MAX, bottom = INT_MIN;
        for (auto [x, y] : p) {
            (void)x;
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
        for (int y = top; y <= bottom; ++y) {
            std::vector<double> crosses;
            for (size_t i = 0; i < p.size(); ++i) {
                auto [x1, y1] = p[i];
                auto [x2, y2] = p[(i + 1) % p.size()];
                if (y1 == y2) {
                    if (y == y1)
                        rect(std::min(x1, x2), y, std::max(x1, x2), y, color);
                    continue;
                }
                if (y >= std::min(y1, y2) && y <= std::max(y1, y2))
                    crosses.push_back(x1 + double(y - y1) * (x2 - x1) / (y2 - y1));
            }
            if (crosses.size() > 1) {
                std::sort(crosses.begin(), crosses.end());
                rect(int(std::floor(crosses.front() + .5)), y, int(std::ceil(crosses.back() - .5)), y, color);
            }
        }
    }
    void line(int x1, int y1, int x2, int y2, int color = 0, int width = 1) {
        if (y1 == y2) {
            rect(std::min(x1, x2), y1 - (width - 1) / 2, std::max(x1, x2), y1 + width / 2, color);
            return;
        }
        if (x1 == x2) {
            rect(x1 - (width - 1) / 2, std::min(y1, y2), x1 + width / 2, std::max(y1, y2), color);
            return;
        }
        if (width > 1) {
            double dx = x2 - x1, dy = y2 - y1, n = std::hypot(dx, dy);
            auto down = [](double v) { return int(std::copysign(std::ceil(std::abs(v) - .5), v)); };
            double maximum = std::floor((width - 1) / 2.0 + .5) / n;
            double minimum = std::ceil((width - 1) / 2.0 - .5) / n;
            int dxmin = down(minimum * dy), dxmax = down(maximum * dy), dymin = down(minimum * dx),
                dymax = down(maximum * dx);
            polygon({{x1 - dxmin, y1 + dymax},
                     {x2 - dxmin, y2 + dymax},
                     {x2 + dxmax, y2 - dymin},
                     {x1 + dxmax, y1 - dymin}},
                    color);
            return;
        }
        int dx = std::abs(x2 - x1), sx = x1 < x2 ? 1 : -1, dy = -std::abs(y2 - y1), sy = y1 < y2 ? 1 : -1,
            err = dx + dy;
        for (;;) {
            image.pixel(x1, y1, color);
            if (x1 == x2 && y1 == y2)
                break;
            int e = 2 * err;
            if (e >= dy) {
                err += dy;
                x1 += sx;
            }
            if (e <= dx) {
                err += dx;
                y1 += sy;
            }
        }
    }
    void ellipse(int x1, int y1, int x2, int y2, int color = 0, int border = 0) {
        auto path = assets / ("circle-" + std::to_string((x2 - x1) / 2) + ".png");
        if (!border && x2 - x1 == y2 - y1 && fs::exists(path)) {
            auto mask = Image::load(path);
            for (int y = 0; y < mask.height; ++y)
                for (int x = 0; x < mask.width; ++x)
                    if (mask.pixels[size_t(y) * mask.width + x])
                        image.pixel(x1 + x, y1 + y, color);
            return;
        }
        double rx = (x2 - x1) / 2.0, ry = (y2 - y1) / 2.0, cx = x1 + rx, cy = y1 + ry;
        for (int y = y1; y <= y2; ++y)
            for (int x = x1; x <= x2; ++x) {
                double a = std::pow((x - cx) / (rx + 0.2), 2) + std::pow((y - cy) / (ry + 0.2), 2);
                bool inner = border && rx > border && ry > border &&
                             std::pow((x - cx) / (rx - border + 0.2), 2) +
                                     std::pow((y - cy) / (ry - border + 0.2), 2) <
                                 1;
                if (a <= 1 && !inner)
                    image.pixel(x, y, color);
            }
    }
    void dashed(int x1, int x2, int y, int color = 180, int width = 3) {
        for (int x = x1; x < x2; x += 23)
            line(x, y, std::min(x + 14, x2), y, color, width);
    }
};
std::string metric(std::string s, const std::string &prefix) {
    if (upper(s).rfind(upper(prefix), 0) == 0)
        s = s.substr(prefix.size());
    s = trim(s);
    return s.empty() ? "--" : upper(s);
}
void clock(Draw &d, const Json &data) {
    auto t = parse_time(str(data, "generated_at"));
    d.text(298, 48, upper(date_string(t, "%b %d, %Y  %A")), {34, true}, 0, "mt");
    d.text(298, 100, date_string(t, "%H:%M"), {156, true}, 0, "mt");
}
void weather(Draw &d, const Json &w, const fs::path &assets) {
    auto title = upper(ascii(str(w, "title", "WEATHER")));
    title.erase(std::remove(title.begin(), title.end(), ' '), title.end());
    d.text(35, 299, title, {29, true}, 0, "la", 215);
    static const std::map<int, int> icons{
        {0, 1},  {1, 2},  {2, 2},   {3, 3},   {45, 19}, {48, 19}, {51, 8},  {53, 9},  {55, 9},  {56, 7},
        {57, 7}, {61, 8}, {63, 9},  {65, 10}, {66, 20}, {67, 20}, {71, 15}, {73, 16}, {75, 17}, {77, 15},
        {80, 4}, {81, 4}, {82, 10}, {85, 14}, {86, 14}, {95, 5},  {96, 6},  {99, 6}};
    int code = num(w, "weather_code", -1);
    auto it = icons.find(code);
    auto icon =
        assets / "weather" / (it == icons.end() ? "fallback-74.png" : std::to_string(it->second) + "-74.png");
    if (fs::exists(icon))
        d.image.paste(Image::load(icon), 266, 288);
    d.text(35, 357, str(w, "temperature", "-- C"), {84, true}, 0, "la", 305);
    double low = num(w, "temperature_min_c", NAN), high = num(w, "temperature_max_c", NAN),
           current = num(w, "temperature_value_c", NAN);
    if (std::isfinite(low) && std::isfinite(high) && std::isfinite(current) && high > low) {
        auto label = [](double v) {
            auto s = format(v, 1);
            if (s.size() > 2 && s.substr(s.size() - 2) == ".0")
                s.resize(s.size() - 2);
            return s;
        };
        auto l = label(low), h = label(high);
        int x1 = 35 + d.width(l, {24, true}) + 7, x2 = 340 - d.width(h, {24, true}) - 7, y = 479;
        d.text(35, y, l, {24, true}, 70, "lm");
        d.text(340, y, h, {24, true}, 70, "rm");
        int x = rnd(x1 + std::clamp((current - low) / (high - low), 0.0, 1.0) * (x2 - x1));
        d.line(x1, y, x2, y, 180, 4);
        d.line(x1, y, x, y, 0, 4);
        d.ellipse(x - 9, y - 9, x + 9, y + 9, 255);
        d.ellipse(x - 6, y - 6, x + 6, y + 6);
    }
    d.line(357, 290, 357, 524, 180, 2);
    auto prec = metric(str(w, "precipitation_amount", "Precip --"), "Precip");
    if (prec.size() > 2 && prec.substr(prec.size() - 2) == "MM")
        prec.resize(prec.size() - 2);
    std::vector<std::pair<std::string, std::string>> values{
        {"RAIN", metric(str(w, "precipitation_probability", "Rain --"), "Rain")},
        {"PREC", prec},
        {"AQI", metric(str(w, "aqi", "AQI --"), "AQI")},
        {"HUMD", metric(str(w, "humidity", "Humidity --"), "Humidity")},
        {"UV", metric(str(w, "uv_index", "UV --"), "UV")}};
    for (int i = 0; i < 5; ++i) {
        int a = 290 + i * 234 / 5, b = 290 + (i + 1) * 234 / 5, y = (a + b) / 2;
        if (i)
            d.line(376, a, 561, a, 180);
        auto [name, value] = values[i];
        d.text(376, y, name, d.fit(name, 88, 30, true, 24), 70, "lm", 88);
        d.text(561, y, value, d.fit(value, 89, 30, true, 24), 0, "rm", 89);
    }
    if (str(w, "status", "OK") != "OK")
        d.text(376, 505, upper(str(w, "status")), {30, true}, 0, "la", 185);
}
std::string grade(double value, bool aod) {
    if (!std::isfinite(value))
        return "--";
    std::vector<double> levels = aod ? std::vector<double>{.1, .2, .3, .4, .6, .8}
                                     : std::vector<double>{.05, .2, .4, .6, .8, 1, 1.5, 2};
    int count = int(std::upper_bound(levels.begin(), levels.end(), value) - levels.begin());
    if (aod)
        count = int(levels.size()) - count;
    std::string out;
    for (int i = 0; i < int(levels.size()); ++i)
        out += i < count ? "★" : "☆";
    return out;
}
void sunset_metric(Draw &d, Rect r, std::string label, const Json &e, const char *key, bool aod) {
    auto lv = str(e, std::string("ec_") + key, "--"), rv = str(e, std::string("gfs_") + key, "--");
    double l = number(lv), v = number(rv);
    auto ls = std::isfinite(l) ? format(l, 1) : lv, rs = std::isfinite(v) ? format(v, 1) : rv;
    bool collapse = std::isfinite(l) && std::isfinite(v) && ls == rs;
    auto text = collapse ? ls : ls + "/" + rs;
    int top = std::max(24, int(r.h * .50)), labelw = std::min(64, std::max(50, int(r.w * .25))),
        valuew = std::max(42, r.w - labelw - 12), cx = r.x + labelw + 4 + valuew / 2;
    auto stars = grade(std::isfinite(l) ? l : v, aod);
    auto gf = d.fit(stars, r.w - 6, 28, true, 14);
    int offset = std::max(d.height(stars, gf) / 2 + 1, (r.h - top) / 2);
    d.text(r.x + 6, r.y + top / 2, label, d.fit(label, labelw - 6, 22, true, 16), 70, "lm", labelw - 6);
    d.text(cx, r.y + top / 2, text, d.fit(text, valuew, collapse ? 36 : 32, true, collapse ? 22 : 18), 0,
           "mm", valuew);
    d.text(r.x + r.w / 2, r.y + top + offset, stars, gf, 0, "mm", r.w - 6);
}
void sunset(Draw &d, const Json &s, const Json &data) {
    auto status = str(s, "status", "Skipped");
    if (status != "OK") {
        auto msg = status.size() > 24 ? "SUNSET UNAVAILABLE" : upper(status);
        d.text(298, 673, msg, {26, true}, 0, "mm", 526);
        return;
    }
    const auto &events = obj(s, "events");
    if (!events.is_array() || events.empty()) {
        d.text(298, 673, "NO DATA", {26, true}, 0, "mm", 526);
        return;
    }
    int count = std::min(2, int(events.size()));
    for (int i = 0; i < count; ++i) {
        auto &e = events[i];
        int x = 35 + i * 526 / count, w = 526 / count;
        auto name = upper(str(e, "name", "EVENT"));
        auto title = name.rfind("SUNSET", 0) == 0 ? "SET" : name.rfind("SUNRISE", 0) == 0 ? "RISE" : "EVENT";
        std::string heading = std::string(title) + " --";
        try {
            auto t = parse_time(str(e, "time"));
            auto ref = parse_time(str(data, "generated_at"));
            auto day = parse_time(date_string(t, "%Y-%m-%d")), rd = parse_time(date_string(ref, "%Y-%m-%d"));
            int offset = int(std::difftime(day, rd) / 86400);
            heading = std::string(title) + " " + date_string(t, "%H:%M");
            if (offset)
                heading += "(" + std::string(offset > 0 ? "+" : "") + std::to_string(offset) + ")";
        } catch (...) {
        }
        d.text(x + w / 2, 566, heading, d.fit(heading, w - 8, 28, true, 18), 0, "mm", w - 8);
        sunset_metric(d, {x, 584, w, 107}, "AOD", e, "aod", true);
        sunset_metric(d, {x, 691, w, 107}, "QLT", e, "quality", false);
    }
}
void clouds(Draw &d, const Json &c) {
    constexpr int top = 274, left = 70, right = 598;
    d.rect(0, top, 599, top + 531, 255);
    const auto &p = obj(c, "points");
    if (!p.is_array() || p.empty()) {
        d.text(300, top + 266, "CLOUD " + upper(str(c, "status", "Loading")), {22, true}, 0, "mm");
        return;
    }
    int n = std::min(12, int(p.size()));
    double cw = double(right - left) / n;
    auto x = [&](int i) { return rnd(left + i * cw); };
    auto local = [&](int i) {
        auto t = std::time_t(num(p[i], "time")) + 8 * 3600;
        std::tm tm{};
        gmtime_r(&t, &tm);
        return tm;
    };
    d.rect(0, top + 0, left, top + 24);
    d.text(35, top + 12, "D", {15, true}, 255, "mm");
    d.text(35, top + 36, "T", {15, true}, 0, "mm");
    int start = 0;
    for (int i = 0; i < n; ++i) {
        auto tm = local(i);
        if (i == n - 1 || local(i + 1).tm_yday != tm.tm_yday) {
            d.rect(x(start), top + 0, x(i + 1), top + 24, 0, 1);
            d.text((x(start) + x(i + 1)) / 2, top + 12,
                   std::to_string(tm.tm_mon + 1) + "/" + std::to_string(tm.tm_mday), {16, true}, 0, "mm");
            start = i + 1;
        }
        d.rect(x(i), top + 24, x(i + 1), top + 48, 0, 1);
        d.text(rnd(left + (i + .5) * cw), top + 36,
               (tm.tm_hour < 10 ? "0" : "") + std::to_string(tm.tm_hour), {18, true}, 0, "mm");
    }
    const int rows[] = {48, 112, 160, 208, 256};
    const char *labels[] = {"C", "H", "M", "L"};
    const char *keys[] = {"cloud_area_fraction", "cloud_area_fraction_high",
                          "cloud_area_fraction_medium", "cloud_area_fraction_low"};
    for (int row = 0; row < 4; ++row) {
        int y = top + rows[row], h = rows[row + 1] - rows[row];
        d.rect(0, y, left, y + h, 0, 1);
        d.text(35, y + h / 2, labels[row], {18, true}, 0, "mm");
        for (int i = 0; i < n; ++i) {
            double v = num(p[i], keys[row], NAN);
            if (!std::isfinite(v)) {
                d.text((x(i) + x(i + 1)) / 2, y + h / 2, "-", {15}, 0, "mm");
                continue;
            }
            int bar = rnd((h - 4) * std::clamp(v, 0.0, 100.0) / 100);
            if (bar > 0) d.rect(x(i) + 2, y + h - 2 - bar, x(i + 1) - 3, y + h - 3);
            if (!row) d.text((x(i) + x(i + 1)) / 2,
                             bar >= 24 ? y + h - 2 - bar / 2 : y + 14,
                             format(v, 0), {17, true}, bar >= 24 ? 255 : 0, "mm");
        }
    }
    for (int i = 0; i <= n; ++i)
        for (int y = top + 48; y < top + 256; y += 4) d.line(x(i), y, x(i), y + 1);
    for (int row : rows)
        for (int xx = left; xx < right; xx += 4) d.line(xx, top + row, xx + 1, top + row);
    d.rect(0, top + 256, left, top + 352, 0, 1);
    d.rect(left, top + 256, right, top + 352, 0, 1);
    d.text(35, top + 280, "T", {17, true}, 0, "mm");
    d.text(35, top + 328, "D", {17}, 0, "mm");
    double lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < n; ++i)
        for (auto key : {"air_temperature", "dew_point_temperature"}) {
            double v = num(p[i], key, NAN);
            if (std::isfinite(v)) { lo = std::min(lo, v); hi = std::max(hi, v); }
        }
    if (std::isfinite(lo)) {
        lo = std::floor(lo - 1); hi = std::ceil(hi + 1);
        if (hi - lo < 6) { double mid = (hi + lo) / 2; lo = std::floor(mid - 3); hi = std::ceil(mid + 3); }
        auto ty = [&](double v) { return rnd(top + 272 + (hi - v) / (hi - lo) * 64); };
        for (int i = 0; i < n; ++i)
            if (i == 0 || local(i).tm_hour == 0) d.line(x(i), top + 256, x(i), top + 352);
        for (auto key : {"air_temperature", "dew_point_temperature"}) {
            bool dew = std::string(key) == "dew_point_temperature";
            for (int i = 1; i < n; ++i) {
                double a = num(p[i - 1], key, NAN), b = num(p[i], key, NAN);
                if (!std::isfinite(a) || !std::isfinite(b)) continue;
                int x1 = rnd(left + (i - .5) * cw), x2 = rnd(left + (i + .5) * cw);
                for (int xx = x1; xx < x2; ++xx)
                    if (!dew || (xx - left) % 7 < 4)
                        d.line(xx, rnd(ty(a) + double(xx - x1) / (x2 - x1) * (ty(b) - ty(a))),
                               xx + 1, rnd(ty(a) + double(xx + 1 - x1) / (x2 - x1) * (ty(b) - ty(a))), 0, dew ? 1 : 2);
            }
        }
        int imin = -1, imax = -1;
        for (int i = 0; i < n; ++i) {
            double v = num(p[i], "air_temperature", NAN);
            if (!std::isfinite(v)) continue;
            if (imin < 0 || v < num(p[imin], "air_temperature")) imin = i;
            if (imax < 0 || v > num(p[imax], "air_temperature")) imax = i;
        }
        for (int i = 0; i < n; ++i) {
            double v = num(p[i], "air_temperature", NAN);
            if (!std::isfinite(v) || (i && i != n - 1 && i != imin && i != imax)) continue;
            int xx = rnd(left + (i + .5) * cw), yy = ty(v);
            d.rect(xx - 15, yy - 10, xx + 15, yy + 9, 255);
            d.text(xx, yy - 1, format(v, 0), {17, true}, 0, "mm");
        }
    }
    const char *extra_keys[] = {"precipitation_6h", "wind_speed", "aod"};
    const char *extra_labels[] = {"R", "W", "A"};
    for (int row = 0; row < 3; ++row) {
        int y = top + 352 + row * 60;
        double maximum = row == 0 ? 1 : row == 1 ? 5 : 1;
        for (int i = 0; i < n; ++i) {
            double value = num(p[i], extra_keys[row], NAN);
            if (std::isfinite(value)) maximum = std::max(maximum, value);
        }
        d.rect(0, y, left, y + 59, 0, 1);
        d.text(35, y + 30, extra_labels[row], {17, true}, 0, "mm");
        for (int i = 0; i < n; ++i) {
            double value = num(p[i], extra_keys[row], NAN);
            int cx = (x(i) + x(i + 1)) / 2;
            std::string label = "--";
            if (std::isfinite(value) && value >= 0) {
                label = format(value, row == 2 ? 2 : 1);
                if (label.size() > 4) label = format(value, 0);
                int bar = rnd(value / maximum * 29);
                if (bar > 0) d.rect(x(i) + 4, y + 57 - bar, x(i + 1) - 4, y + 56);
            }
            d.text(cx, y + 13, label, d.fit(label, x(i + 1) - x(i) - 4, 18, true, 15), 0, "mm");
            for (int yy = y; yy < y + 60; yy += 4) d.line(x(i), yy, x(i), yy + 1);
        }
        d.line(left, y + 59, right, y + 59);
    }
    if (std::time(nullptr) - num(c, "fetched_at") > 10800)
        d.text(596, top + 343, "STALE", {15, true}, 0, "rm");
    d.rect(0, top, 599, top + 531, 0, 2);
}
void meter(Draw &d, int x, int y, int w, int h, int percent, int t, int e) {
    d.rect(x, y, x + w, y + h, 0, 2);
    int total = w - 5, fill = rnd(total * std::clamp(percent, 0, 100) / 100.0);
    if (percent > 0 && !fill)
        fill = 1;
    if (fill)
        d.rect(x + 3, y + 3, x + 3 + fill - 1, y + h - 3);
    for (int i = 0; i < 2; ++i) {
        int p = i ? e : t, ax = std::clamp(x + int(w * std::clamp(p, 0, 100) / 100.0), x + 10, x + w - 10);
        int yy = i ? y + h + 3 : y - 3, base = yy + (i ? 14 : -14);
        std::vector<std::pair<int, int>> triangle{{ax - 10, base}, {ax + 10, base}, {ax, yy}};
        if (percent >= std::max(0, p)) {
            for (int j = 0; j < 3; ++j)
                d.line(triangle[j].first, triangle[j].second, triangle[(j + 1) % 3].first,
                       triangle[(j + 1) % 3].second, 0, 2);
        } else
            d.polygon(triangle);
    }
}
void usage_card(Draw &d, Rect r, const Json &v, const std::string &fallback) {
    auto name = upper(str(v, "name", fallback)), status = str(v, "status", "OK");
    bool ok = status == "OK" || status == "Skipped", nolimit = flag(v, "no_limit") || !ok,
         notstarted = flag(v, "not_started", true);
    int pct = rnd(num(v, "used_percent"));
    auto percent = std::to_string(pct) + "%";
    std::string note;
    if (!ok) {
        percent = "ERR";
        note = upper(status);
    } else if (nolimit) {
        percent = "NO LIMIT";
        note = "UNLIMITED";
    } else if (notstarted) {
        percent = "0%";
        note = "NOT STARTED";
        pct = 0;
    } else if (!str(v, "reset_after_human").empty())
        note = "RESET IN " + upper(str(v, "reset_after_human"));
    else if (!str(v, "reset_at_local").empty())
        note = "RESET " + str(v, "reset_at_local");
    else
        note = "ACTIVE";
    d.text(r.x + 10, r.y + 8, name, d.fit(name, int((r.w - 20) * .55), 22, true, 16), 0, "lt");
    d.text(r.x + r.w - 10, r.y + 8, percent, d.fit(percent, int((r.w - 20) * .42), 26, true, 20), 0, "rt");
    if (nolimit)
        d.text(r.x + r.w / 2, r.y + r.h / 2 + 6, note, d.fit(note, r.w - 20, 18, true, 12), 0, "mm",
               r.w - 20);
    else {
        meter(d, r.x + 10, r.y + 48, r.w - 20, 16, pct, num(v, "t_progress"), num(v, "e_progress"));
        d.text(r.x + r.w / 2, r.y + 92, note, d.fit(note, r.w - 20, 16, true, 11), 0, "mt", r.w - 20);
    }
}
void usage(Draw &d, const Json &u) {
    d.line(298, 816, 298, 1062, 180);
    d.line(26, 939, 570, 939, 180);
    usage_card(d, {26, 816, 263, 117}, obj(u, "codex_5h"), "CODEX 5H");
    usage_card(d, {307, 816, 263, 117}, obj(u, "codex_7d"), "CODEX 7D");
    usage_card(d, {26, 945, 263, 117}, obj(u, "agy_5h"), "AGY 5H");
    usage_card(d, {307, 945, 263, 117}, obj(u, "agy_7d"), "AGY 7D");
}
void curve(Draw &d, Rect r, const Json &values, double progress) {
    int end = r.x + std::max(1, rnd(std::clamp(progress, 0.0, 1.0) * r.w));
    std::vector<std::pair<int, double>> finite;
    double low = 0, high = 0;
    if (values.is_array())
        for (size_t i = 0; i < values.size(); ++i) {
            double n = number(values[i]);
            if (std::isfinite(n)) {
                finite.push_back({int(i), n});
                low = std::min(low, n);
                high = std::max(high, n);
            }
        }
    if (finite.size() < 2) {
        d.line(r.x, r.y + r.h / 2, end, r.y + r.h / 2, 180);
        return;
    }
    if (high == low) {
        high += .5;
        low -= .5;
    }
    double pad = std::max((high - low) * .08, .02);
    low -= pad;
    high += pad;
    auto point = [&](int i, double n) {
        return std::pair<int, int>{r.x + rnd(i * double(end - r.x) / (values.size() - 1)),
                                   r.y + r.h - rnd((n - low) * r.h / (high - low))};
    };
    int zero = point(0, 0).second;
    d.line(r.x, zero, r.x + r.w, zero, 180, 3);
    auto previous = point(finite.front().first, finite.front().second);
    for (size_t i = 1; i < finite.size(); ++i) {
        auto p = point(finite[i].first, finite[i].second);
        d.line(previous.first, previous.second, p.first, p.second, 0, 3);
        previous = p;
    }
}
void markets(Draw &d, const Json &rows) {
    Json empty = Json::array({Json{{"symbol", "No symbols"}, {"price", "--"}, {"change", "--"}}});
    const auto &quotes = rows.is_array() && !rows.empty() ? rows : empty;
    int rowh = std::max(56, 1048 / int(quotes.size()));
    for (size_t i = 0; i < quotes.size(); ++i) {
        const auto &q = quotes[i];
        int y = 16 + int(i) * rowh;
        if (i + 1 < quotes.size())
            d.dashed(658, 1386, y + rowh - 1);
        auto symbol = ascii(str(q, "symbol", "SYM")), price = ascii(str(q, "price", "--")),
             change = str(q, "status", "OK") == "OK" ? str(q, "change", "--") : "N/A";
        auto sf = d.fit(symbol, 188, 31), pf = d.fit(price, 126, 26, false), cf = d.fit(change, 124, 28);
        int baseline = y + (rowh - d.height(symbol, {31, true})) / 2 - 2,
            sy = y + (rowh - d.height(symbol, sf)) / 2 - 2;
        auto sub = str(q, "secondary_symbol");
        if (sub.empty())
            sub = ascii(str(q, "note"));
        if (!sub.empty())
            sy -= 16;
        d.text(658, sy, symbol, sf, 0, "la", 188);
        if (!sub.empty())
            d.text(658, sy + 38, sub, {19, true}, 0, "la", 188);
        int symbolh = d.height(symbol, sf), delay = num(q, "delay_minutes");
        bool all = flag(q, "is_24h");
        if (all && delay > 0) {
            auto top = std::string("24"), bottom = "-" + std::to_string(delay);
            auto tf = d.fit(top, 40, 18), bf = d.fit(bottom, 40, 18);
            int th = d.height(top, tf), bh = d.height(bottom, bf), ty = sy + (symbolh - th - 2 - bh) / 2;
            d.text(653 - d.width(top, tf), ty, top, tf);
            d.text(653 - d.width(bottom, bf), ty + th + 2, bottom, bf);
        } else {
            std::string mark;
            Font mf = sf;
            if (all) {
                mark = "24";
                mf = d.fit(mark, 40, 22);
            } else if (flag(q, "is_closed"))
                mark = "*";
            else if (delay > 0) {
                mark = "-" + std::to_string(delay);
                mf = d.fit(mark, 40, 22);
            }
            if (!mark.empty())
                d.text(653 - d.width(mark, mf), sy + (symbolh - d.height(mark, mf)) / 2, mark, mf);
        }
        int ch = std::min(78, rowh - 34);
        curve(d, {864, y + (rowh - ch) / 2, 236, ch}, obj(q, "intraday"), num(q, "trading_progress", 1));
        bool secondary = !str(q, "secondary_price").empty(), converted = !str(q, "converted").empty();
        int dy = secondary || converted ? -16 : 0;
        d.text(1244, baseline + 4 + dy, price, pf, 0, "ra");
        d.text(1386, baseline + 6 + dy, change, cf, 0, "ra");
        int suby = baseline + 6 + dy + 36;
        if (secondary) {
            d.text(1244, suby, str(q, "secondary_price"), {19, true}, 0, "ra", 126);
            d.text(1386, suby, str(q, "secondary_change"), {19, true}, 0, "ra", 124);
            suby += 36;
        }
        if (converted)
            d.text(1386, suby, str(q, "converted"), {19, true}, 0, "ra", 268);
    }
}
void news(Draw &d, const Json &value) {
    auto code = str(value, "platform", "eastmoney");
    auto heading = news_platform_name(code);
    if (heading.empty())
        heading = code;
    heading += "  " + code;
    d.text(630, 34, heading, d.fit_cjk(heading, 740, 34, true, 22), 0, "la", 740);
    d.line(620, 78, 1408, 78, 0, 2);
    if (str(value, "status") != "OK") {
        auto status = upper(str(value, "status", "LOADING"));
        d.text(1020, 540, "NEWS " + status, {30, true}, 0, "mm", 720);
        return;
    }
    const auto &items = obj(value, "items");
    if (!items.is_array() || items.empty()) {
        d.text(1020, 540, "NO NEWS", {30, true}, 0, "mm", 720);
        return;
    }
    int count = std::min(10, int(items.size())), rowh = (1000 / count);
    Font font{36, false, true};
    for (int i = 0; i < count; ++i) {
        int top = 80 + i * rowh, middle = top + rowh / 2;
        if (i)
            d.dashed(630, 1408, top, 190, 2);
        auto lines = d.wrap_two(str(items[i], "title"), font, 760);
        if (lines.size() == 1)
            d.text(630, middle, lines[0], font, 0, "lm", 760);
        else {
            d.text(630, middle - 22, lines[0], font, 0, "lm", 760);
            d.text(630, middle + 22, lines[1], font, 0, "lm", 760);
        }
    }
}
void settings_box(Draw &d, Rect r, bool active = false) {
    d.rect(r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, active ? 225 : 255);
    d.rect(r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, active ? 0 : 170, 2);
}
std::string setting_value(const Json &config, const char *section, const char *key, const char *fallback) {
    auto value = ascii(str(obj(config, section), key, fallback));
    return value.empty() ? fallback : value;
}
void setting_row(Draw &d, Rect r, const std::string &label, const std::string &value, bool active = false,
                 bool cjk = false) {
    settings_box(d, r, active);
    d.text(r.x + 28, r.y + r.h / 2, label, {25, true}, 70, "lm");
    auto font = cjk ? d.fit_cjk(value, 810, 28, true, 18) : d.fit(value, 810, 28, true, 18);
    d.text(r.x + 500, r.y + r.h / 2, value, font, 0, "lm", 810);
}
void setting_button(Draw &d, Rect r, const std::string &label, bool dark = false) {
    d.rect(r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, dark ? 0 : 235);
    d.rect(r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, dark ? 0 : 170, 2);
    d.text(r.x + r.w / 2, r.y + r.h / 2, label, d.fit(label, r.w - 24, 26, true, 16), dark ? 255 : 0, "mm",
           r.w - 24);
}
void keyboard_key(Draw &d, int x, int y, int width, const std::string &label) {
    setting_button(d, {x, y, width, 80}, label);
}
void settings_keyboard(Draw &d, const std::string &label, const std::string &editing) {
    settings_box(d, {60, 120, 1320, 90}, true);
    d.text(88, 165, label, {25, true}, 70, "lm");
    auto value = ascii(editing);
    d.text(500, 165, value.empty() ? "(empty)" : value, d.fit(value, 810, 30, true, 18), 0, "lm", 810);
    const std::vector<std::pair<std::string, int>> rows{{"1234567890", 450},
                                                         {"QWERTYUIOP", 550},
                                                         {"ASDFGHJKL", 650},
                                                         {"ZXCVBNM-._", 750}};
    for (auto &[keys, y] : rows) {
        int x = keys.size() == 9 ? 149 : 85;
        for (char key : keys) {
            keyboard_key(d, x, y, 118, std::string(1, key));
            x += 128;
        }
    }
    keyboard_key(d, SETTINGS_KEY_CLEAR.x, SETTINGS_KEY_CLEAR.y, SETTINGS_KEY_CLEAR.w, "CLEAR");
    keyboard_key(d, SETTINGS_KEY_SPACE.x, SETTINGS_KEY_SPACE.y, SETTINGS_KEY_SPACE.w, "SPACE");
    keyboard_key(d, SETTINGS_KEY_BACKSPACE.x, SETTINGS_KEY_BACKSPACE.y, SETTINGS_KEY_BACKSPACE.w, "BACK");
    keyboard_key(d, SETTINGS_KEY_DONE.x, SETTINGS_KEY_DONE.y, SETTINGS_KEY_DONE.w, "DONE");
}
void news_platform_picker(Draw &d, const Json &config) {
    d.text(60, 42, "NEWS PLATFORM", {42, true}, 0, "la");
    setting_button(d, SETTINGS_NEWS_BACK, "BACK");
    d.line(60, 82, 1380, 82, 0, 3);
    auto selected = str(obj(config, "news"), "platform", "eastmoney");
    for (size_t i = 0; i < NEWS_PLATFORMS.size(); ++i) {
        auto r = news_platform_rect(i);
        bool active = NEWS_PLATFORMS[i].first == selected;
        settings_box(d, r, active);
        auto label = std::string(NEWS_PLATFORMS[i].second) + "  " + std::string(NEWS_PLATFORMS[i].first);
        d.text(r.x + 20, r.y + r.h / 2, label, d.fit_cjk(label, r.w - 40, 24, true, 18), 0, "lm",
               r.w - 40);
    }
}
Image settings_image(Renderer &renderer, const fs::path &assets, const Json &config, int field,
                     const std::string &editing, bool keyboard, const std::string &message) {
    Image image;
    Draw d{image, renderer, assets};
    if (!keyboard && field == 2) {
        news_platform_picker(d, config);
        return image;
    }
    d.text(60, 42, keyboard ? "EDIT SETTING" : "SETTINGS", {42, true}, 0, "la");
    d.text(1380, 48, keyboard ? "TOUCH DONE TO RETURN" : "LONG PRESS DASHBOARD TO OPEN", {20, true}, 90, "ra");
    d.line(60, 82, 1380, 82, 0, 3);
    if (keyboard) {
        settings_keyboard(d, field == 0 ? "WEATHER CITY" : "SUNSET CITY", editing);
        return image;
    }
    setting_row(d, SETTINGS_WEATHER_ROW, "WEATHER CITY", setting_value(config, "weather", "location", "Hangzhou"),
                field == 0);
    setting_row(d, SETTINGS_SUNSET_ROW, "SUNSET CITY", setting_value(config, "sunset", "city", "Hangzhou"),
                field == 1);
    setting_row(d, SETTINGS_MARKET_ROW, "MARKET REFRESH", std::to_string(int(num(obj(config, "refresh"), "market_seconds", 60))) + " sec");
    setting_button(d, {1050, SETTINGS_MARKET_ROW.y + 10, 110, 70}, "-");
    setting_button(d, {1210, SETTINGS_MARKET_ROW.y + 10, 110, 70}, "+");
    setting_row(d, SETTINGS_WEATHER_REFRESH_ROW, "WEATHER REFRESH",
                std::to_string(int(num(obj(config, "refresh"), "weather_seconds", 600))) + " sec");
    setting_button(d, {1050, SETTINGS_WEATHER_REFRESH_ROW.y + 10, 110, 70}, "-");
    setting_button(d, {1210, SETTINGS_WEATHER_REFRESH_ROW.y + 10, 110, 70}, "+");
    setting_row(d, SETTINGS_ROTATION_ROW, "SCREEN ROTATION",
                std::to_string(int(num(obj(config, "display"), "rotation", 90))) + " degrees");
    setting_row(d, SETTINGS_WAVEFORM_ROW, "PARTIAL WAVEFORM", str(obj(config, "display"), "waveform", "GC16"));
    auto platform = str(obj(config, "news"), "platform", "eastmoney");
    setting_row(d, SETTINGS_NEWS_ROW, "NEWS PLATFORM", news_platform_name(platform) + "  " + platform, false,
                true);
    if (!message.empty())
        d.text(720, 895, message, d.fit(message, 1320, 20, true, 14), 0, "mb", 1320);
    setting_button(d, SETTINGS_SAVE, "SAVE & CLOSE", true);
    setting_button(d, SETTINGS_CLOSE, "CLOSE");
    setting_button(d, SETTINGS_EXIT, "EXIT PROGRAM");
    return image;
}
} // namespace
Image Renderer::draw(const Json &data, bool show_news) {
    Image image;
    Draw d{image, *this, assets_};
    clock(d, data);
    d.line(8, 274, 588, 274, 0, 2);
    if (flag(data, "show_cloud"))
        clouds(d, obj(data, "cloud"));
    else {
        weather(d, obj(data, "weather"), assets_);
        d.line(8, 540, 588, 540, 0, 2);
        sunset(d, obj(data, "sunset"), data);
    }
    d.line(8, 806, 588, 806, 0, 2);
    usage(d, obj(data, "agent_usage"));
    d.line(600, 8, 600, 1072, 0, 2);
    if (show_news)
        news(d, obj(data, "news"));
    else
        markets(d, obj(data, "market"));
    return image;
}
Image Renderer::settings(const Json &config, int field, const std::string &editing, bool keyboard,
                         const std::string &message) {
    return settings_image(*this, assets_, config, field, editing, keyboard, message);
}
} // namespace dash
