#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace dash {
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr int WIDTH = 1440, HEIGHT = 1080;
inline constexpr std::array<std::pair<std::string_view, std::string_view>, 22> NEWS_PLATFORMS{{
    {"baidu", "百度热搜"},       {"sspai", "少数派"},         {"weibo", "微博热搜"},
    {"zhihu", "知乎热榜"},      {"tskr", "36氪"},            {"ftpojie", "吾爱破解"},
    {"bilibili", "哔哩哔哩"},   {"douban", "豆瓣"},          {"hupu", "虎扑"},
    {"tieba", "百度贴吧"},      {"juejin", "掘金"},          {"douyin", "抖音"},
    {"vtex", "V2EX"},           {"jinritoutiao", "今日头条"}, {"stackoverflow", "Stack Overflow"},
    {"github", "GitHub Trending"}, {"hackernews", "Hacker News"}, {"sina_finance", "新浪财经"},
    {"eastmoney", "东方财富"},  {"xueqiu", "雪球"},          {"cls", "财联社"},
    {"tenxunwang", "腾讯网"},
}};
std::string news_platform_name(std::string_view code);
extern std::atomic<bool> stopping;
std::string str(const Json &j, const std::string &key, std::string fallback = "");
double num(const Json &j, const std::string &key, double fallback = 0);
bool flag(const Json &j, const std::string &key, bool fallback = false);
const Json &obj(const Json &j, std::string_view key);
std::string trim(std::string s);
std::string upper(std::string s);
std::string ascii(std::string s);
std::string format(double value, int decimals = 2, bool sign = false, bool comma = false);
std::vector<std::string> split(const std::string &s, char separator);
double number(const Json &value, double fallback = NAN);
std::string date_string(std::time_t time, const char *format = "%Y-%m-%dT%H:%M:%S");
std::time_t parse_time(std::string text, bool utc = false);
Json read_json(const fs::path &path);
void write_json(const fs::path &path, const Json &value);
Json load_config(const fs::path &path);
Json initial_data(const Json &config);
Json fetch_region(const std::string &name, const Json &config, const Json &previous, const fs::path &assets);
void merge_region(Json &data, const std::string &name, const Json &result);
void expire_sunset(Json &data, const Json &config, std::time_t now);
bool cloud_mode(const Json &sunset, bool current);
double interval(const std::string &name, const Json &config);

struct Rect {
    int x, y, w, h;
    bool operator==(const Rect &b) const {
        return x == b.x && y == b.y && w == b.w && h == b.h;
    }
};
inline constexpr Rect SETTINGS_WEATHER_ROW{60, 120, 1320, 90};
inline constexpr Rect SETTINGS_SUNSET_ROW{60, 225, 1320, 90};
inline constexpr Rect SETTINGS_MARKET_ROW{60, 350, 1320, 90};
inline constexpr Rect SETTINGS_WEATHER_REFRESH_ROW{60, 465, 1320, 90};
inline constexpr Rect SETTINGS_ROTATION_ROW{60, 580, 1320, 90};
inline constexpr Rect SETTINGS_WAVEFORM_ROW{60, 695, 1320, 90};
inline constexpr Rect SETTINGS_NEWS_ROW{60, 800, 1320, 80};
inline constexpr Rect SETTINGS_SAVE{60, 900, 380, 100};
inline constexpr Rect SETTINGS_CLOSE{530, 900, 380, 100};
inline constexpr Rect SETTINGS_EXIT{1000, 900, 380, 100};
inline constexpr Rect SETTINGS_KEY_CLEAR{60, 900, 190, 100};
inline constexpr Rect SETTINGS_KEY_SPACE{270, 900, 590, 100};
inline constexpr Rect SETTINGS_KEY_BACKSPACE{880, 900, 250, 100};
inline constexpr Rect SETTINGS_KEY_DONE{1150, 900, 230, 100};
inline constexpr Rect SETTINGS_NEWS_BACK{1200, 18, 180, 62};
inline Rect news_platform_rect(size_t index) {
    return {60 + int(index / 11) * 670, 100 + int(index % 11) * 84, 650, 72};
}
struct Image {
    int width, height;
    std::vector<uint8_t> pixels;
    Image(int w = WIDTH, int h = HEIGHT) : width(w), height(h), pixels(size_t(w) * h, 255) {}
    void pixel(int x, int y, uint8_t color);
    Image crop(Rect box) const;
    void paste(const Image &source, int x, int y);
    void save(const fs::path &path) const;
    static Image load(const fs::path &path);
};
std::map<std::string, Rect> regions(size_t rows);
std::pair<Image, Rect> rotate_patch(const Image &patch, Rect box, int degrees, int canvas_width = WIDTH,
                                    int canvas_height = HEIGHT);
Image device_frame(const Image &reference, int rotation);
Rect device_region(Rect reference, int rotation);

// Two FreeType faces are reused across draws; no GUI toolkit or browser runtime.
class Renderer {
    void *library_ = nullptr;
    void *faces_[3] = {nullptr, nullptr, nullptr};
    fs::path assets_;

  public:
    explicit Renderer(const fs::path &assets);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;
    Image draw(const Json &data, bool show_news = false);
    Image settings(const Json &config, int field, const std::string &editing, bool keyboard,
                   const std::string &message = "");
    void *face(int size, bool bold, bool cjk = false);
};

class Display {
    fs::path output_;
    bool simulate_, ready_ = false;
    std::string fbink_, waveform_;
    int rotation_;
    double full_seconds_, last_full_ = 0;
    size_t sequence_ = 0;
    Image previous_;
    std::map<std::string, Rect> layout_;

  public:
    Display(fs::path output, bool simulate, const Json &config);
    void configure(const Json &config);
    void full_refresh();
    std::vector<std::string> update(const Image &image, size_t count, double now);
};
} // namespace dash
