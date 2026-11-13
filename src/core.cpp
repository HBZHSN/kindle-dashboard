#include "dashboard.hpp"
#include <cstring>
#include <iomanip>
#include <iostream>
#include <png.h>
#include <regex>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <yaml.h>

namespace dash {
std::atomic<bool> stopping{false};
std::string news_platform_name(std::string_view code) {
    for (auto [value, name] : NEWS_PLATFORMS)
        if (value == code)
            return std::string(name);
    return {};
}
const Json &obj(const Json &j, std::string_view key) {
    static const Json empty = Json::object();
    auto it = j.find(key);
    return it == j.end() || it->is_null() ? empty : *it;
}
std::string str(const Json &j, const std::string &key, std::string fallback) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null())
        return fallback;
    return it->is_string() ? it->get<std::string>() : it->dump();
}
double number(const Json &v, double fallback) {
    try {
        size_t used = 0;
        double n = v.is_number() ? v.get<double>() : std::stod(v.get<std::string>(), &used);
        if (v.is_string() && used != v.get<std::string>().size())
            return fallback;
        return std::isfinite(n) ? n : fallback;
    } catch (...) {
        return fallback;
    }
}
double num(const Json &j, const std::string &key, double fallback) {
    return number(obj(j, key), fallback);
}
bool flag(const Json &j, const std::string &key, bool fallback) {
    const auto &v = obj(j, key);
    return v.is_boolean() ? v.get<bool>() : fallback;
}
std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    return a == s.npos ? "" : s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string upper(std::string s) {
    for (auto &c : s)
        if ((unsigned char)c < 128)
            c = std::toupper((unsigned char)c);
    return s;
}
std::string ascii(std::string s) {
    std::string out;
    for (unsigned char c : s)
        if (c >= 32 && c < 127)
            out += char(c);
    return trim(out);
}
std::string format(double n, int decimals, bool sign, bool comma) {
    if (!std::isfinite(n))
        return "--";
    std::ostringstream s;
    if (sign && n >= 0)
        s << '+';
    s << std::fixed << std::setprecision(decimals) << n;
    auto out = s.str();
    if (comma) {
        size_t end = out.find('.');
        if (end == out.npos)
            end = out.size();
        size_t start = (out[0] == '-' || out[0] == '+');
        for (int p = int(end) - 3; p > int(start); p -= 3)
            out.insert(size_t(p), ",");
    }
    return out;
}
std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string part;
    while (std::getline(in, part, sep))
        out.push_back(part);
    return out;
}
std::string date_string(std::time_t t, const char *fmt) {
    std::tm tm{};
    localtime_r(&t, &tm);
    char out[128];
    std::strftime(out, sizeof out, fmt, &tm);
    return out;
}
std::time_t parse_time(std::string s, bool utc) {
    if (s.size() < 10)
        throw std::runtime_error("Invalid date");
    std::replace(s.begin(), s.end(), 'T', ' ');
    std::tm tm{};
    std::istringstream input(s.substr(0, 19));
    input >> std::get_time(&tm, s.size() > 10 ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d");
    if (input.fail() && s.size() >= 16) {
        input.clear();
        input.str(s.substr(0, 16));
        input >> std::get_time(&tm, "%Y-%m-%d %H:%M");
    }
    if (input.fail())
        throw std::runtime_error("Invalid date");
    tm.tm_isdst = -1;
    if (s.size() >= 20 && (s[19] == '+' || s[19] == '-')) {
        int minutes = std::stoi(s.substr(20, 2)) * 60 + std::stoi(s.substr(23, 2));
        return timegm(&tm) - (s[19] == '+' ? 1 : -1) * minutes * 60;
    }
    return utc || (!s.empty() && s.back() == 'Z') ? timegm(&tm) : mktime(&tm);
}
Json read_json(const fs::path &p) {
    std::ifstream f(p);
    if (!f)
        throw std::runtime_error("Cannot read JSON: " + p.string());
    return Json::parse(f);
}
void write_json(const fs::path &p, const Json &j) {
    if (!p.parent_path().empty())
        fs::create_directories(p.parent_path());
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        f << j.dump(2);
        if (!f)
            throw std::runtime_error("Snapshot write failed");
    }
    fs::rename(tmp, p);
}
static Json yaml_node(yaml_document_t &doc, int id, int depth) {
    if (depth > 32)
        throw std::runtime_error("YAML nesting/alias limit exceeded");
    auto *node = yaml_document_get_node(&doc, id);
    if (!node)
        throw std::runtime_error("Invalid YAML node");
    if (node->type == YAML_SCALAR_NODE) {
        std::string s((char *)node->data.scalar.value, node->data.scalar.length);
        if (node->data.scalar.style != YAML_PLAIN_SCALAR_STYLE)
            return s;
        if (s == "null" || s == "Null" || s == "NULL" || s == "~" || s.empty())
            return nullptr;
        if (s == "true" || s == "True")
            return true;
        if (s == "false" || s == "False")
            return false;
        if (std::regex_match(s, std::regex("[-+]?[0-9]+"))) {
            try {
                return std::stoll(s);
            } catch (...) {
            }
        }
        if (std::regex_match(s, std::regex("[-+]?[0-9]*\\.[0-9]+([eE][-+]?[0-9]+)?"))) {
            try {
                return std::stod(s);
            } catch (...) {
            }
        }
        return s;
    }
    if (node->type == YAML_SEQUENCE_NODE) {
        Json j = Json::array();
        for (auto it = node->data.sequence.items.start; it != node->data.sequence.items.top; ++it)
            j.push_back(yaml_node(doc, *it, depth + 1));
        return j;
    }
    if (node->type == YAML_MAPPING_NODE) {
        Json j = Json::object();
        for (auto it = node->data.mapping.pairs.start; it != node->data.mapping.pairs.top; ++it) {
            auto key = yaml_node(doc, it->key, depth + 1);
            if (!key.is_string())
                throw std::runtime_error("YAML keys must be strings");
            auto k = key.get<std::string>();
            if (j.contains(k))
                throw std::runtime_error("Duplicate YAML key: " + k);
            j[k] = yaml_node(doc, it->value, depth + 1);
        }
        return j;
    }
    return nullptr;
}
Json load_config(const fs::path &path) {
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("Cannot open config.yaml");
    std::string text((std::istreambuf_iterator<char>(file)), {});
    if (text.size() > 1024 * 1024)
        throw std::runtime_error("Config too large");
    yaml_parser_t parser;
    yaml_document_t doc;
    if (!yaml_parser_initialize(&parser))
        throw std::bad_alloc();
    yaml_parser_set_input_string(&parser, (const unsigned char *)text.data(), text.size());
    if (!yaml_parser_load(&parser, &doc)) {
        yaml_parser_delete(&parser);
        throw std::runtime_error("Invalid YAML");
    }
    Json config;
    try {
        config = yaml_node(doc, 1, 0);
    } catch (...) {
        yaml_document_delete(&doc);
        yaml_parser_delete(&parser);
        throw;
    }
    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    if (!config.is_object())
        throw std::runtime_error("Configuration must be a map");
    auto defaults = Json{{"poll_seconds", 1},    {"full_refresh_seconds", 900},
                         {"market_seconds", 60}, {"weather_seconds", 600},
                         {"workers", 4},         {"regions", Json::object()}};
    if (config.contains("refresh") && !config["refresh"].is_object())
        throw std::runtime_error("refresh must be a map");
    defaults.update(obj(config, "refresh"));
    config["refresh"] = defaults;
    for (auto key :
         {"poll_seconds", "full_refresh_seconds", "market_seconds", "weather_seconds", "workers"}) {
        double v = num(defaults, key, -1);
        if (v < 0 || v > 86400 || (v == 0 && std::string(key) != "full_refresh_seconds"))
            throw std::runtime_error(std::string("Invalid refresh.") + key);
    }
    if (!defaults["workers"].is_number_integer() || num(defaults, "workers") > 8)
        throw std::runtime_error("workers must be 1-8");
    if (!obj(defaults, "regions").is_object())
        throw std::runtime_error("refresh.regions must be a map");
    for (auto &v : obj(defaults, "regions"))
        if (number(v, -1) <= 0 || number(v) > 86400)
            throw std::runtime_error("Invalid region interval");
    for (auto section : {"agent_usage", "sunset"})
        for (auto key : {"refresh_seconds", "cache_max_age_seconds"}) {
            const auto &settings = obj(config, section);
            if (settings.contains(key) && (num(settings, key, -1) <= 0 || num(settings, key) > 86400))
                throw std::runtime_error(std::string("Invalid ") + section + "." + key);
        }
    if (config.contains("news") && !config["news"].is_object())
        throw std::runtime_error("news must be a map");
    auto platform = str(obj(config, "news"), "platform", "eastmoney");
    if (news_platform_name(platform).empty())
        throw std::runtime_error("Invalid news.platform");
    config["news"]["platform"] = platform;
    if (str(obj(config, "market"), "source", "mix") != "mix")
        throw std::runtime_error("Use market.source: mix on Kindle");
    auto symbols = obj(obj(config, "market"), "mix_symbols");
    if (!symbols.is_array() || symbols.size() > 16)
        throw std::runtime_error("mix_symbols must be an array of at most 16 symbols");
    for (auto &s : symbols)
        if (!s.is_string() || s.get<std::string>().empty())
            throw std::runtime_error("Invalid market symbol");
    if (str(obj(config, "output"), "orientation", "landscape") != "landscape")
        throw std::runtime_error("Use landscape orientation to match HTML");
    auto display = Json{{"rotation", 90}, {"waveform", "GC16"}, {"fbink", "fbink"}};
    display.update(obj(config, "display"));
    config["display"] = display;
    if (num(display, "rotation") != 90 && num(display, "rotation") != 270)
        throw std::runtime_error("rotation must be 90 or 270");
    if (str(display, "waveform") != "GC16" && str(display, "waveform") != "GL16" &&
        str(display, "waveform") != "AUTO")
        throw std::runtime_error("Invalid waveform");
    config["snapshot_path"] = (fs::absolute(path).parent_path() /
                               str(obj(config, "cache"), "data_path", ".cache/dashboard-data.json"))
                                  .string();
    return config;
}
void Image::pixel(int x, int y, uint8_t c) {
    if (x >= 0 && y >= 0 && x < width && y < height)
        pixels[size_t(y) * width + x] = c;
}
Image Image::crop(Rect r) const {
    if (r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 || r.x + r.w > width || r.y + r.h > height)
        throw std::runtime_error("Invalid crop");
    Image out(r.w, r.h);
    for (int y = 0; y < r.h; ++y)
        std::copy_n(pixels.begin() + size_t(y + r.y) * width + r.x, r.w,
                    out.pixels.begin() + size_t(y) * r.w);
    return out;
}
void Image::paste(const Image &s, int x, int y) {
    if (x < 0 || y < 0 || x + s.width > width || y + s.height > height)
        throw std::runtime_error("Invalid paste");
    for (int row = 0; row < s.height; ++row)
        std::copy_n(s.pixels.begin() + size_t(row) * s.width, s.width,
                    pixels.begin() + size_t(y + row) * width + x);
}
void Image::save(const fs::path &p) const {
    if (p.extension() == ".pgm") {
        std::ofstream f(p, std::ios::binary);
        f << "P5\n" << width << ' ' << height << "\n255\n";
        f.write((const char *)pixels.data(), pixels.size());
        if (!f)
            throw std::runtime_error("PGM write failed");
        return;
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_GRAY;
    if (!png_image_write_to_file(&image, p.c_str(), 0, pixels.data(), 0, nullptr))
        throw std::runtime_error("PNG write failed");
}
Image Image::load(const fs::path &p) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, p.c_str()))
        throw std::runtime_error("PNG read failed");
    if (image.width > WIDTH || image.height > 1440) {
        png_image_free(&image);
        throw std::runtime_error("PNG dimensions too large");
    }
    image.format = PNG_FORMAT_GRAY;
    Image out(image.width, image.height);
    if (!png_image_finish_read(&image, nullptr, out.pixels.data(), 0, nullptr)) {
        png_image_free(&image);
        throw std::runtime_error("PNG decode failed");
    }
    png_image_free(&image);
    return out;
}
std::map<std::string, Rect> regions(size_t count) {
    if (count > 16)
        throw std::runtime_error("Too many market rows");
    std::map<std::string, Rect> out{{"clock", {0, 0, 600, 274}},
                                    {"weather", {0, 274, 600, 266}},
                                    {"sunset", {0, 540, 600, 266}},
                                    {"usage", {0, 806, 600, 274}}};
    count = std::max(size_t(1), count);
    int rh = std::max(56, 1048 / int(count));
    for (size_t i = 0; i < count; ++i) {
        int top = i ? 16 + int(i) * rh : 0, bottom = i == count - 1 ? 1080 : 16 + int(i + 1) * rh;
        out["market:" + std::to_string(i)] = {600, top, 840, bottom - top};
    }
    return out;
}
std::pair<Image, Rect> rotate_patch(const Image &p, Rect r, int d, int canvas_width, int canvas_height) {
    if (d != 90 && d != 270)
        throw std::runtime_error("Invalid rotation");
    Image out(p.height, p.width);
    for (int y = 0; y < p.height; ++y)
        for (int x = 0; x < p.width; ++x)
            out.pixel(d == 90 ? p.height - 1 - y : y, d == 90 ? x : p.width - 1 - x,
                      p.pixels[size_t(y) * p.width + x]);
    return {std::move(out), d == 90 ? Rect{canvas_height - r.y - r.h, r.x, r.h, r.w}
                                    : Rect{r.y, canvas_width - r.x - r.w, r.h, r.w}};
}
Image device_frame(const Image &reference, int rotation) {
    // KPW3 is 1072x1448. Remove only white margins; never resample content.
    Image landscape(1448, 1072);
    landscape.paste(reference.crop({0, 4, 1440, 1072}), 4, 0);
    return rotate_patch(landscape, {0, 0, 1448, 1072}, rotation, 1448, 1072).first;
}
Rect device_region(Rect r, int rotation) {
    int left = r.x == 0 ? 0 : r.x + 4;
    int right = r.x + r.w == 1440 ? 1448 : r.x + r.w + 4;
    int top = std::max(4, r.y) - 4, bottom = std::min(1076, r.y + r.h) - 4;
    return rotation == 90 ? Rect{1072 - bottom, left, bottom - top, right - left}
                          : Rect{top, 1448 - right, bottom - top, right - left};
}
Display::Display(fs::path out, bool simulate, const Json &c)
    : output_(std::move(out)), simulate_(simulate) {
    configure(c);
    fs::create_directories(output_);
}
void Display::configure(const Json &c) {
    fbink_ = str(obj(c, "display"), "fbink", "fbink");
    waveform_ = str(obj(c, "display"), "waveform", "GC16");
    rotation_ = int(num(obj(c, "display"), "rotation", 90));
    full_seconds_ = num(obj(c, "refresh"), "full_refresh_seconds", 900);
    ready_ = false;
}
void Display::full_refresh() {
    ready_ = false;
}
static void run_fbink(const std::vector<std::string> &command) {
    std::vector<char *> argv;
    for (const auto &s : command)
        argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0)
        throw std::runtime_error("fork failed");
    if (pid == 0) {
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
        auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid)
            break;
        if (result < 0 && errno != EINTR)
            throw std::runtime_error("waitpid failed");
        if (stopping || std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            throw std::runtime_error("FBInk timed out or stopped");
        }
        usleep(10000);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        throw std::runtime_error("FBInk failed; region will retry");
}
std::vector<std::string> Display::update(const Image &image, size_t count, double now) {
    if (image.width != WIDTH || image.height != HEIGHT)
        throw std::runtime_error("Invalid frame dimensions");
    auto layout = regions(count);
    bool full = !ready_ || layout != layout_ || (full_seconds_ > 0 && now - last_full_ >= full_seconds_);
    auto candidates = full ? std::map<std::string, Rect>{{"full", {0, 0, WIDTH, HEIGHT}}} : layout;
    std::vector<std::string> changed;
    Image device(1, 1);
    for (auto &[name, box] : candidates) {
        auto patch = image.crop(box);
        if (!full && patch.pixels == previous_.crop(box).pixels)
            continue;
        if (changed.empty())
            device = device_frame(image, rotation_);
        auto r = device_region(box, rotation_);
        auto physical = device.crop(r);
        if (!simulate_) {
            auto path = fs::absolute(output_ / "patch.pgm");
            physical.save(path);
            std::vector<std::string> cmd{
                fbink_,
                "--quiet",
                "--noviewport",
                "--wait",
                "--waveform",
                full ? "GC16" : waveform_,
                "--image",
                "file=" + path.string() + ",x=" + std::to_string(r.x) + ",y=" + std::to_string(r.y)};
            if (full)
                cmd.push_back("--flash");
            run_fbink(cmd);
        }
        previous_.paste(patch, box.x, box.y);
        changed.push_back(name);
        Json e = {{"sequence", ++sequence_},
                  {"region", name},
                  {"box", {box.x, box.y, box.w, box.h}},
                  {"physical", {r.x, r.y, r.w, r.h}},
                  {"full", full},
                  {"monotonic", now},
                  {"waveform", full ? "GC16" : waveform_}};
        std::ofstream log(output_ / "refresh.jsonl", std::ios::app);
        log << e.dump() << '\n';
    }
    if (full) {
        ready_ = true;
        layout_ = layout;
        last_full_ = now;
    }
    if (simulate_ && !changed.empty()) {
        previous_.save(output_ / "frame.tmp.png");
        fs::rename(output_ / "frame.tmp.png", output_ / "frame.png");
        device.save(output_ / "device.png");
        Json boxes;
        for (auto &[name, r] : layout)
            boxes[name] = {r.x, r.y, r.w, r.h};
        write_json(output_ / "regions.json", boxes);
    }
    return changed;
}
} // namespace dash
