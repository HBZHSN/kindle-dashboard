#include "dashboard.hpp"
#include <iostream>
#include <unistd.h>

using namespace dash;
static void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    try {
        check(argc == 2, "source root required");
        fs::path root = argv[1];
        auto config = load_config(root / "config.example.yaml");
        char temp[] = "/tmp/kindle-dashboard-check-XXXXXX";
        auto *p = mkdtemp(temp);
        check(p, "mkdtemp failed");
        fs::path out = p;
        config["refresh"]["full_refresh_seconds"] = 30;
        Display display(out, true, config);
        Image frame;
        check(display.update(frame, 10, 0) == std::vector<std::string>{"full"}, "first frame must be full");
        check(display.update(frame, 10, 1).empty(), "identical frame refreshed");
        for (auto &[name, r] : regions(10)) {
            frame.pixel(r.x + 5, r.y + 5, 0);
            check(display.update(frame, 10, 2) == std::vector<std::string>{name}, "region update leaked");
            check(display.update(frame, 10, 3).empty(), "identical region refreshed");
        }
        check(display.update(frame, 10, 30) == std::vector<std::string>{"full"},
              "full refresh deadline missed");
        check(display.update(frame, 10, 31).empty(), "full refresh did not reset timer");
        check(display.update(frame, 9, 32) == std::vector<std::string>{"full"},
              "layout changes must clear stale rows");
        for (size_t count : {size_t(0), size_t(1), size_t(10), size_t(16)}) {
            std::vector<uint8_t> coverage(WIDTH * HEIGHT);
            for (auto &[name, r] : regions(count)) {
                (void)name;
                for (int y = r.y; y < r.y + r.h; ++y)
                    for (int x = r.x; x < r.x + r.w; ++x)
                        ++coverage[y * WIDTH + x];
            }
            check(std::all_of(coverage.begin(), coverage.end(), [](uint8_t n) { return n == 1; }),
                  "regions must tile framebuffer exactly");
        }
        Image small(3, 2);
        small.pixels = {1, 2, 3, 4, 5, 6};
        auto rotated = rotate_patch(small, {100, 200, 3, 2}, 90);
        check(rotated.second == Rect{878, 100, 2, 3}, "rotation coordinates incorrect");
        check(rotated.first.pixels == std::vector<uint8_t>({4, 1, 5, 2, 6, 3}), "rotation pixels incorrect");
        auto reverse = rotate_patch(small, {100, 200, 3, 2}, 270);
        check(reverse.second == Rect{200, 1337, 2, 3}, "reverse rotation incorrect");
        auto panel = device_frame(frame, 90);
        check(panel.width == 1072 && panel.height == 1448, "KPW3 dimensions");
        check(device_region({0, 0, 1440, 1080}, 90) == Rect{0, 0, 1072, 1448}, "KPW3 full refresh rectangle");
        for (int rotation : {90, 270}) {
            std::vector<uint8_t> cover(1072 * 1448);
            for (auto &[name, box] : regions(10)) {
                (void)name;
                auto r = device_region(box, rotation);
                check(r.x >= 0 && r.y >= 0 && r.x + r.w <= 1072 && r.y + r.h <= 1448,
                      "device rectangle out of bounds");
                for (int y = r.y; y < r.y + r.h; ++y)
                    for (int x = r.x; x < r.x + r.w; ++x)
                        ++cover[y * 1072 + x];
            }
            check(std::all_of(cover.begin(), cover.end(), [](uint8_t n) { return n == 1; }),
                  "physical regions must tile KPW3");
        }
        // A failed driver call cannot advance the display cache or full-refresh timer.
        config["display"]["fbink"] = "/bin/false";
        Display failing(out / "failed", false, config);
        bool failed = false;
        try {
            failing.update(frame, 10, 0);
        } catch (...) {
            failed = true;
        }
        check(failed, "FBInk error swallowed");
        failed = false;
        try {
            failing.update(frame, 10, 1);
        } catch (...) {
            failed = true;
        }
        check(failed, "failed frame was incorrectly marked displayed");
        setenv("TZ", "Asia/Shanghai", 1);
        tzset();
        auto data = initial_data(config);
        Renderer renderer(root / "assets");
        auto rendered = renderer.draw(data);
        check(rendered.pixels != Image().pixels, "renderer produced a blank image");
        auto cloud_data = data;
        cloud_data["show_cloud"] = true;
        cloud_data["cloud"] = {{"fetched_at", std::time(nullptr)}, {"points", Json::array()}};
        for (int i = 0; i < 24; ++i)
            cloud_data["cloud"]["points"].push_back({{"time", 1790640000 + i * 10800},
                {"cloud_area_fraction", i * 4}, {"cloud_area_fraction_high", 25},
                {"cloud_area_fraction_medium", 50}, {"cloud_area_fraction_low", 75},
                {"air_temperature", 20 + i % 5}, {"dew_point_temperature", 15},
                {"precipitation_6h", i % 3}, {"wind_speed", 3.5}, {"aod", .35}});
        auto cloud_frame = renderer.draw(cloud_data);
        check(cloud_frame.crop({0, 274, 600, 266}).pixels != rendered.crop({0, 274, 600, 266}).pixels,
              "cloud must replace weather region");
        check(cloud_frame.crop({0, 0, 600, 274}).pixels == rendered.crop({0, 0, 600, 274}).pixels &&
              cloud_frame.crop({0, 806, 600, 274}).pixels == rendered.crop({0, 806, 600, 274}).pixels,
              "cloud must stay within combined region");
        cloud_data["cloud"]["points"][0]["precipitation_6h"] = nullptr;
        check(renderer.draw(cloud_data).pixels != cloud_frame.pixels, "missing rain must not render as zero");
        Json news_items = Json::array();
        for (int i = 0; i < 10; ++i)
            news_items.push_back(
                {{"title", "这是一条用于验证固定字号自动换成两行的很长很长的测试财经新闻标题"},
                 {"publish_time", "2026-09-23 17:08:54"}});
        data["news"] = {{"status", "OK"}, {"platform", "eastmoney"}, {"items", news_items}};
        auto news = renderer.draw(data, true);
        check(news.pixels != rendered.pixels, "news view did not render");
        data["news"]["items"][0]["publish_time"] = "2026-09-23 18:59:59";
        check(renderer.draw(data, true).pixels == news.pixels, "news publish time was rendered");
        bool second_line = false;
        for (int y = 135; y < 180; ++y)
            for (int x = 630; x < 1390; ++x)
                second_line |= news.pixels[size_t(y) * WIDTH + x] < 128;
        check(second_line, "long news title did not wrap to a second line");
        auto settings = renderer.settings(config, -1, "", false);
        check(settings.width == WIDTH && settings.height == HEIGHT && settings.pixels != Image().pixels,
              "settings page produced a blank image");
        auto keyboard = renderer.settings(config, 0, "New York", true);
        check(keyboard.pixels != settings.pixels, "settings keyboard page did not render");
        auto platforms = renderer.settings(config, 2, "", false);
        check(platforms.pixels != settings.pixels, "news platform picker did not render");
        check(news_platform_rect(21).x + news_platform_rect(21).w <= WIDTH &&
                  news_platform_rect(21).y + news_platform_rect(21).h <= HEIGHT,
              "news platform picker is out of bounds");
        check(news_platform_name("vtex") == "V2EX" && interval("news", config) == 900,
              "news platform or interval mismatch");
        rendered.save(out / "render.png");
        check(Image::load(out / "render.png").pixels == rendered.pixels, "PNG grayscale round-trip failed");
        auto bad = config;
        bad["refresh"]["poll_seconds"] = -1;
        write_json(out / "bad.yaml", bad);
        failed = false;
        try {
            load_config(out / "bad.yaml");
        } catch (...) {
            failed = true;
        }
        check(failed, "negative poll interval accepted");
        bad = config;
        bad["news"]["platform"] = "missing";
        write_json(out / "bad-news.yaml", bad);
        failed = false;
        try {
            load_config(out / "bad-news.yaml");
        } catch (...) {
            failed = true;
        }
        check(failed, "invalid news platform accepted");
        Json sun = {{"status", "OK"}, {"events", Json::array({
            {{"ec_quality", "0.1"}, {"gfs_quality", "0.19"}},
            {{"ec_quality", "0.0"}, {"gfs_quality", "0.1"}}})}};
        check(cloud_mode(sun, false), "low QLT should select clouds");
        sun["events"][1]["gfs_quality"] = "0.201";
        check(!cloud_mode(sun, true), "high QLT should restore sunset");
        sun["events"][1]["gfs_quality"] = "0.2";
        check(!cloud_mode(sun, false) && cloud_mode(sun, true), "threshold should preserve mode");
        sun["status"] = "Unavailable";
        check(cloud_mode(sun, false), "unavailable sunset should select clouds");
        check(interval("cloud", config) == 10800, "cloud refresh interval");
        sun["status"] = "OK";
        sun["events"] = Json::array({{{"ec_quality", "--"}}, {{"gfs_quality", "0.1"}}});
        check(!cloud_mode(sun, false), "missing QLT must not count as zero");
        check(format(12345.678, 2, false, true) == "12,345.68", "price formatting");
        check(parse_time("2026-09-22T08:00:00+08:00") == parse_time("2026-09-22T00:00:00Z"),
              "timezone parsing");
        std::cout << "PASS: unchanged frames, all 14 independent regions, full refresh, layout changes, "
                     "rotations, failed FBInk retries, rendering, config, dates\n";
        fs::remove_all(out);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
