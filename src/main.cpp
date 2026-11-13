#ifdef DASHBOARD_MUSL_THREADING
// The generic musl SDK's ARMv5 libstdc++ omits ARMv7 futex fast-path symbols.
// Use its portable condition-variable implementation while keeping ARMv7/VFP codegen.
#include <bits/c++config.h>
#undef _GLIBCXX_HAVE_LINUX_FUTEX
#endif
#include "dashboard.hpp"
#ifdef DASHBOARD_WINDOW
#include "preview.hpp"
#endif
#include <fcntl.h>
#include <future>
#include <linux/input.h>
#include <iostream>
#include <memory>
#include <set>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

using namespace dash;
namespace {
void stop(int) {
    stopping = true;
}
double monotonic() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Job {
    std::string name;
    double due = 0;
    bool attempted = false;
    std::future<Json> future;
};
Json prior(const Json &data, const std::string &name) {
    return name.rfind("market:", 0) == 0 ? data.at("market").at(std::stoul(name.substr(7)))
                                         : obj(data, name == "usage" ? "agent_usage" : name);
}
struct TouchAction {
    enum class Type { Tap, LongPress } type;
    int x, y;
    bool logical = false;
};
class TouchInput {
    struct Device {
        int fd;
        int raw_x = 0, raw_y = 0;
        int min_x = 0, max_x = 1071, min_y = 0, max_y = 1447;
        bool pressed = false, moved = false, long_sent = false;
        bool have_x = false, have_y = false, positioned = false;
        bool grabbed = false;
        int start_x = 0, start_y = 0;
        std::chrono::steady_clock::time_point started;
    };
    std::vector<Device> devices_;
    static int normal(int value, int minimum, int maximum, int limit) {
        if (maximum <= minimum)
            return std::clamp(value, 0, limit);
        return std::clamp(int((int64_t(value - minimum) * limit) / (maximum - minimum)), 0, limit);
    }
    static bool read_abs(int fd, int code, int &minimum, int &maximum) {
        input_absinfo info{};
        if (ioctl(fd, EVIOCGABS(code), &info) < 0 || info.maximum <= info.minimum)
            return false;
        minimum = info.minimum;
        maximum = info.maximum;
        return true;
    }
    static int physical_x(const Device &d) { return normal(d.raw_x, d.min_x, d.max_x, 1071); }
    static int physical_y(const Device &d) { return normal(d.raw_y, d.min_y, d.max_y, 1447); }
    static void begin(Device &d) {
        d.pressed = true;
        d.moved = d.long_sent = false;
        d.have_x = d.have_y = d.positioned = false;
        d.started = std::chrono::steady_clock::now();
    }
    static void move(Device &d) {
        if (d.pressed && d.positioned &&
            (std::abs(physical_x(d) - d.start_x) > 45 || std::abs(physical_y(d) - d.start_y) > 45))
            d.moved = true;
    }
    static void coordinate(Device &d, bool x, int value) {
        if (x) {
            d.raw_x = value;
            d.have_x = true;
        } else {
            d.raw_y = value;
            d.have_y = true;
        }
        if (!d.pressed || !d.have_x || !d.have_y)
            return;
        if (!d.positioned) {
            d.start_x = physical_x(d);
            d.start_y = physical_y(d);
            d.positioned = true;
        } else {
            move(d);
        }
    }
    static void end(Device &d, std::vector<TouchAction> &actions) {
        move(d);
        if (!d.long_sent && !d.moved) {
            auto type = std::chrono::steady_clock::now() - d.started >= std::chrono::milliseconds(1200)
                            ? TouchAction::Type::LongPress
                            : TouchAction::Type::Tap;
            actions.push_back({type, physical_x(d), physical_y(d)});
        }
        d.pressed = false;
    }

  public:
    explicit TouchInput(bool enabled) {
        if (!enabled)
            return;
        for (int i = 0; i < 16; ++i) {
            auto path = "/dev/input/event" + std::to_string(i);
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0)
                continue;
            Device d{fd};
            bool has_x = read_abs(fd, ABS_X, d.min_x, d.max_x);
            if (!has_x)
                has_x = read_abs(fd, ABS_MT_POSITION_X, d.min_x, d.max_x);
            bool has_y = read_abs(fd, ABS_Y, d.min_y, d.max_y);
            if (!has_y)
                has_y = read_abs(fd, ABS_MT_POSITION_Y, d.min_y, d.max_y);
            if (has_x && has_y)
                d.grabbed = ioctl(fd, EVIOCGRAB, 1) == 0;
            devices_.push_back(d);
        }
    }
    ~TouchInput() {
        for (auto &d : devices_) {
            if (d.grabbed)
                ioctl(d.fd, EVIOCGRAB, 0);
            close(d.fd);
        }
    }
    TouchInput(const TouchInput &) = delete;
    TouchInput &operator=(const TouchInput &) = delete;
    std::vector<TouchAction> poll() {
        std::vector<TouchAction> actions;
        for (auto &d : devices_) {
            input_event event{};
            while (read(d.fd, &event, sizeof event) == sizeof event) {
                if (event.type == EV_ABS) {
                    if (event.code == ABS_X || event.code == ABS_MT_POSITION_X)
                        coordinate(d, true, event.value);
                    else if (event.code == ABS_Y || event.code == ABS_MT_POSITION_Y)
                        coordinate(d, false, event.value);
                    else if (event.code == ABS_MT_TRACKING_ID) {
                        if (event.value >= 0 && !d.pressed)
                            begin(d);
                        else if (event.value < 0 && d.pressed) {
                            end(d, actions);
                        }
                    }
                } else if (event.type == EV_KEY && event.code == BTN_TOUCH) {
                    if (event.value && !d.pressed)
                        begin(d);
                    else if (!event.value && d.pressed) {
                        end(d, actions);
                    }
                }
            }
            if (d.pressed) {
                move(d);
                if (!d.long_sent && !d.moved &&
                    std::chrono::steady_clock::now() - d.started >= std::chrono::milliseconds(1200)) {
                    d.long_sent = true;
                    actions.push_back({TouchAction::Type::LongPress, physical_x(d), physical_y(d)});
                }
            }
        }
        return actions;
    }
};
std::pair<int, int> logical_touch(int x, int y, int rotation) {
    if (rotation == 90)
        return {std::clamp(y - 4, 0, WIDTH - 1), std::clamp(1075 - x, 0, HEIGHT - 1)};
    return {std::clamp(1439 - y, 0, WIDTH - 1), std::clamp(x + 4, 0, HEIGHT - 1)};
}
bool inside(Rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
std::string keyboard_key_at(int x, int y) {
    const std::vector<std::pair<std::string, int>> rows{{"1234567890", 450},
                                                         {"QWERTYUIOP", 550},
                                                         {"ASDFGHJKL", 650},
                                                         {"ZXCVBNM-._", 750}};
    for (auto &[keys, top] : rows) {
        int left = keys.size() == 9 ? 149 : 85;
        for (char key : keys) {
            if (inside({left, top, 118, 80}, x, y))
                return std::string(1, key);
            left += 128;
        }
    }
    if (inside(SETTINGS_KEY_CLEAR, x, y))
        return "CLEAR";
    if (inside(SETTINGS_KEY_SPACE, x, y))
        return "SPACE";
    if (inside(SETTINGS_KEY_BACKSPACE, x, y))
        return "BACK";
    if (inside(SETTINGS_KEY_DONE, x, y))
        return "DONE";
    return {};
}
void adjust_refresh(Json &config, const char *key, int delta, int minimum, int maximum) {
    auto value = int(num(obj(config, "refresh"), key, minimum));
    config["refresh"][key] = std::clamp(value + delta, minimum, maximum);
}
std::string editable_value(const Json &config, int field) {
    auto section = field == 0 ? "weather" : "sunset";
    auto key = field == 0 ? "location" : "city";
    auto value = ascii(str(obj(config, section), key));
    return value.empty() ? "Hangzhou" : value;
}
void commit_edit(Json &config, int field, const std::string &value) {
    if (field == 0)
        config["weather"]["location"] = value;
    else
        config["sunset"]["city"] = value;
}
void cycle_waveform(Json &config) {
    auto &waveform = config["display"]["waveform"];
    auto value = str(config["display"], "waveform", "GC16");
    waveform = value == "GC16" ? "GL16" : value == "GL16" ? "AUTO" : "GC16";
}
} // namespace
int main(int argc, char **argv) {
    try {
        fs::path config_path = "config.yaml", assets = "assets", output, snapshot;
        bool simulate = false, once = false, offline = false, frozen = false, window = false, settings_preview = false;
        double duration = 0;
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&]() {
                if (i + 1 >= argc)
                    throw std::runtime_error("Missing value for " + a);
                return std::string(argv[++i]);
            };
            if (a == "--config")
                config_path = value();
            else if (a == "--assets")
                assets = value();
            else if (a == "--output")
                output = value();
            else if (a == "--snapshot")
                snapshot = value();
            else if (a == "--simulate")
                simulate = true;
            else if (a == "--window")
                window = simulate = true;
            else if (a == "--once")
                once = true;
            else if (a == "--offline")
                offline = true;
            else if (a == "--freeze-time")
                frozen = true;
            else if (a == "--settings")
                settings_preview = true;
            else if (a == "--duration") {
                duration = std::stod(value());
                if (!std::isfinite(duration) || duration < 0)
                    throw std::runtime_error("Invalid duration");
            } else if (a == "--help") {
                std::cout << "kindle-dashboard [--config config.yaml] [--assets assets] [--simulate] "
                             "[--window] [--offline]\n                 [--snapshot snapshot.json] [--once] "
                             "[--freeze-time] [--settings] [--duration seconds] [--output preview]\n";
                return 0;
            } else
                throw std::runtime_error("Unknown argument: " + a);
        }
        auto config = load_config(config_path);
#ifndef DASHBOARD_WINDOW
        if (window)
            throw std::runtime_error("Build with -DDASHBOARD_WINDOW=ON to enable the WSLg window");
#endif
        assets = fs::absolute(assets);
        std::string zone = str(obj(config, "schedule"), "timezone", "Asia/Shanghai");
        if (zone != "Asia/Shanghai" && zone != "UTC")
            throw std::runtime_error("Dashboard timezone must be Asia/Shanghai or UTC");
        // Kindle does not ship the IANA timezone database. China has no DST in the supported years.
        setenv("TZ", zone == "Asia/Shanghai" ? "CST-8" : "UTC0", 1);
        tzset();
        setlocale(LC_TIME, "C");
        // Custom and sunset wall-clock APIs in the supplied configuration use China time.
        for (auto &c : obj(obj(config, "market"), "custom_symbols"))
            if (str(c, "timezone", zone) != zone)
                throw std::runtime_error("Custom source timezone must match schedule.timezone");
        if (str(obj(config, "sunset"), "timezone", zone) != zone)
            throw std::runtime_error("Sunset timezone must match schedule.timezone");
        if (output.empty())
            output = simulate ? "preview" : "/tmp/kindle-dashboard";
        output = fs::absolute(output);
        fs::create_directories(output);
        auto lockpath = simulate ? output / "dashboard.lock" : fs::path("/tmp/kindle-dashboard.lock");
        int lock = open(lockpath.c_str(), O_CREAT | O_RDWR, 0600);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
            throw std::runtime_error("Dashboard already running or lock unavailable");
        fcntl(lock, F_SETFD, FD_CLOEXEC);
        signal(SIGINT, stop);
        signal(SIGTERM, stop);
        Json data = initial_data(config), cached;
        if (!snapshot.empty())
            cached = read_json(snapshot);
        else if (fs::exists(str(config, "snapshot_path")))
            try {
                cached = read_json(str(config, "snapshot_path"));
            } catch (...) {
                std::cerr << "Ignoring invalid snapshot\n";
            }
        if (cached.is_object()) {
            for (auto key : {"weather", "sunset", "cloud", "agent_usage"})
                if (obj(cached, key).is_object())
                    data[key] = obj(cached, key);
            if (obj(cached, "news").is_object() &&
                str(obj(cached, "news"), "platform") == str(obj(data, "news"), "platform"))
                data["news"] = obj(cached, "news");
            if (frozen && !str(cached, "generated_at").empty())
                data["generated_at"] = cached["generated_at"];
            for (auto &q : data["market"])
                for (auto &old : obj(cached, "market"))
                    if (str(q, "symbol") == str(old, "symbol") && str(old, "status") == "OK") {
                        auto note = str(q, "note");
                        q = old;
                        q["note"] = note;
                        break;
                    }
        }
        Renderer renderer(assets);
        dash::Display display(output, simulate, config);
        TouchInput touch(!simulate);
#ifdef DASHBOARD_WINDOW
        std::unique_ptr<Preview> preview;
        if (window)
            preview = std::make_unique<Preview>(int(num(obj(config, "display"), "rotation", 90)), offline);
#endif
        std::vector<Job> jobs;
        auto rebuild_jobs = [&]() {
            jobs.clear();
            for (auto name : {"weather", "sunset", "cloud", "usage", "news"}) {
                double due = 0;
                if (std::string(name) == "cloud") {
                    double age = std::time(nullptr) - num(obj(data, "cloud"), "fetched_at");
                    if (age >= 0 && age < 10800 && str(obj(data, "cloud"), "status") == "OK")
                        due = monotonic() + 10800 - age;
                }
                jobs.push_back(Job{name, due, false, {}});
            }
            for (size_t i = 0; i < data["market"].size(); ++i)
                jobs.push_back(Job{"market:" + std::to_string(i), 0, false, {}});
        };
        rebuild_jobs();
        data["show_cloud"] = cloud_mode(obj(data, "sunset"), false);
        std::string sunset_state = obj(data, "sunset").dump();
        bool settings_open = settings_preview, settings_keyboard = false, settings_dirty = settings_preview,
             show_news = false;
        int settings_field = -1;
        std::string settings_editing, settings_message;
        Json settings_draft = config;
        const auto start = monotonic();
        std::string last_render_key;
        Image frame;
        size_t ticks = 0;
        double rendering_seconds = 0;
        size_t renders = 0;
        auto wait_jobs = [&]() {
            for (auto &job : jobs)
                if (job.future.valid())
                    try {
                        job.future.get();
                    } catch (const std::exception &e) {
                        std::cerr << "FETCH " << job.name << " stopped while saving settings: " << e.what()
                                  << '\n';
                    }
        };
        while (!stopping) {
            double now = monotonic();
            auto actions = touch.poll();
#ifdef DASHBOARD_WINDOW
            if (preview) {
                preview->poll();
                for (auto [x, y] : preview->take_taps())
                    actions.push_back({TouchAction::Type::Tap, x, y, true});
            }
#endif
            if (!settings_open) {
                for (auto &action : actions) {
                    auto point = action.logical
                                     ? std::pair<int, int>{action.x, action.y}
                                     : logical_touch(action.x, action.y,
                                                     int(num(obj(config, "display"), "rotation", 90)));
                    if (action.type == TouchAction::Type::LongPress) {
                        (void)point;
                        settings_open = true;
                        settings_keyboard = false;
                        settings_field = -1;
                        settings_message.clear();
                        settings_draft = config;
                        settings_dirty = true;
                        break;
                    } else if (inside(flag(data, "show_cloud") ? Rect{0, 274, 600, 532} : Rect{0, 540, 600, 266},
                                      point.first, point.second)) {
                        data["show_cloud"] = !flag(data, "show_cloud");
                    } else if (point.first >= 600) {
                        show_news = !show_news;
                        last_render_key.clear();
                    }
                }
            }
            if (settings_open) {
                for (auto &action : actions) {
                    if (action.type != TouchAction::Type::Tap)
                        continue;
                    auto point = action.logical
                                     ? std::pair<int, int>{action.x, action.y}
                                     : logical_touch(action.x, action.y,
                                                     int(num(obj(config, "display"), "rotation", 90)));
                    int x = point.first, y = point.second;
                    if (settings_field == 2) {
                        if (inside(SETTINGS_NEWS_BACK, x, y)) {
                            settings_field = -1;
                            settings_dirty = true;
                        } else
                            for (size_t i = 0; i < NEWS_PLATFORMS.size(); ++i)
                                if (inside(news_platform_rect(i), x, y)) {
                                    settings_draft["news"]["platform"] = NEWS_PLATFORMS[i].first;
                                    settings_field = -1;
                                    settings_dirty = true;
                                    break;
                                }
                    } else if (settings_keyboard) {
                        auto key = keyboard_key_at(x, y);
                        if (key == "DONE") {
                            if (trim(settings_editing).empty())
                                settings_message = "Value cannot be empty";
                            else {
                                commit_edit(settings_draft, settings_field, trim(settings_editing));
                                settings_keyboard = false;
                                settings_field = -1;
                                settings_editing.clear();
                                settings_message.clear();
                                settings_dirty = true;
                            }
                        } else if (key == "CLEAR") {
                            settings_editing.clear();
                            settings_message.clear();
                            settings_dirty = true;
                        } else if (key == "BACK") {
                            if (!settings_editing.empty())
                                settings_editing.pop_back();
                            settings_dirty = true;
                        } else if (key == "SPACE") {
                            if (settings_editing.size() < 64)
                                settings_editing.push_back(' ');
                            settings_dirty = true;
                        } else if (!key.empty() && settings_editing.size() < 64) {
                            settings_editing += key;
                            settings_dirty = true;
                        }
                    } else if (inside(SETTINGS_WEATHER_ROW, x, y)) {
                        settings_field = 0;
                        settings_editing = editable_value(settings_draft, settings_field);
                        settings_keyboard = settings_dirty = true;
                    } else if (inside(SETTINGS_SUNSET_ROW, x, y)) {
                        settings_field = 1;
                        settings_editing = editable_value(settings_draft, settings_field);
                        settings_keyboard = settings_dirty = true;
                    } else if (inside({1050, SETTINGS_MARKET_ROW.y + 10, 110, 70}, x, y)) {
                        adjust_refresh(settings_draft, "market_seconds", -15, 15, 3600);
                        settings_dirty = true;
                    } else if (inside({1210, SETTINGS_MARKET_ROW.y + 10, 110, 70}, x, y)) {
                        adjust_refresh(settings_draft, "market_seconds", 15, 15, 3600);
                        settings_dirty = true;
                    } else if (inside({1050, SETTINGS_WEATHER_REFRESH_ROW.y + 10, 110, 70}, x, y)) {
                        adjust_refresh(settings_draft, "weather_seconds", -60, 60, 3600);
                        settings_dirty = true;
                    } else if (inside({1210, SETTINGS_WEATHER_REFRESH_ROW.y + 10, 110, 70}, x, y)) {
                        adjust_refresh(settings_draft, "weather_seconds", 60, 60, 3600);
                        settings_dirty = true;
                    } else if (inside(SETTINGS_ROTATION_ROW, x, y)) {
                        settings_draft["display"]["rotation"] =
                            num(obj(settings_draft, "display"), "rotation", 90) == 90 ? 270 : 90;
                        settings_dirty = true;
                    } else if (inside(SETTINGS_WAVEFORM_ROW, x, y)) {
                        cycle_waveform(settings_draft);
                        settings_dirty = true;
                    } else if (inside(SETTINGS_NEWS_ROW, x, y)) {
                        settings_field = 2;
                        settings_dirty = true;
                    } else if (inside(SETTINGS_SAVE, x, y)) {
                        try {
                            auto saved = settings_draft;
                            saved.erase("snapshot_path");
                            write_json(config_path, saved);
                            auto next = load_config(config_path);
                            wait_jobs();
                            config = std::move(next);
                            settings_draft = config;
                            data = initial_data(config);
                            rebuild_jobs();
                            display.configure(config);
                            settings_open = settings_keyboard = false;
                            settings_field = -1;
                            settings_editing.clear();
                            settings_message.clear();
                            settings_dirty = false;
                            last_render_key.clear();
                        } catch (const std::exception &e) {
                            settings_message = std::string("SAVE FAILED: ") + e.what();
                            settings_dirty = true;
                        }
                    } else if (inside(SETTINGS_CLOSE, x, y)) {
                        settings_open = settings_keyboard = false;
                        settings_field = -1;
                        settings_editing.clear();
                        settings_message.clear();
                        settings_dirty = false;
                        display.full_refresh();
                        last_render_key.clear();
                    } else if (inside(SETTINGS_EXIT, x, y)) {
                        stopping = true;
                    }
                }
            }
            if (settings_open) {
                if (settings_dirty) {
                    auto before = monotonic();
                    frame = renderer.settings(settings_draft, settings_field, settings_editing, settings_keyboard,
                                              settings_message);
                    rendering_seconds += monotonic() - before;
                    ++renders;
                    display.full_refresh();
                    settings_dirty = false;
#ifdef DASHBOARD_WINDOW
                    if (preview)
                        preview->show(frame);
#endif
                }
                try {
                    auto changed = display.update(frame, data["market"].size(), now);
                    if (!changed.empty())
                        std::cout << Json{{"refreshed", changed}}.dump() << std::endl;
                } catch (const std::exception &e) {
                    std::cerr << e.what() << '\n';
                    if (once)
                        throw;
                }
                ++ticks;
                if (duration > 0 && now - start >= duration)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            bool updated = false;
            for (auto &job : jobs)
                if (job.future.valid() &&
                    job.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    job.due = now + interval(job.name, config);
                    try {
                        merge_region(data, job.name, job.future.get());
                        updated = true;
                        std::cerr << "FETCH " << job.name << " OK\n";
                    } catch (const std::exception &e) {
                        std::cerr << "FETCH " << job.name << " failed: " << e.what()
                                  << "; keeping prior content\n";
                        auto failed = prior(data, job.name);
                        if (str(failed, "status") == "Loading" || failed.empty()) {
                            failed["status"] = "Unavailable";
                            merge_region(data, job.name, failed);
                            updated = true;
                        }
                    }
                }
            if (!frozen) {
                data["generated_at"] = date_string(std::time(nullptr), "%Y-%m-%dT%H:%M:00");
                expire_sunset(data, config, std::time(nullptr));
            }
            auto next_sunset = obj(data, "sunset").dump();
            if (next_sunset != sunset_state) {
                data["show_cloud"] = cloud_mode(obj(data, "sunset"), flag(data, "show_cloud"));
                sunset_state = std::move(next_sunset);
            }
            if (updated)
                write_json(str(config, "snapshot_path"), data);
            if (!offline) {
                size_t running = 0;
                for (auto &j : jobs)
                    running += j.future.valid();
                for (auto &job : jobs) {
                    if (running >= size_t(num(obj(config, "refresh"), "workers", 4)))
                        break;
                    if (job.future.valid() || now < job.due || (once && job.attempted))
                        continue;
                    auto previous = prior(data, job.name);
                    auto name = job.name;
                    job.attempted = true;
                    job.future = std::async(std::launch::async, [&, name, previous]() {
                        return fetch_region(name, config, previous, assets);
                    });
                    ++running;
                }
            }
            auto key = data.dump() + (show_news ? "\nnews" : "\nmarket");
            if (key != last_render_key) {
                auto before = monotonic();
                frame = renderer.draw(data, show_news);
                rendering_seconds += monotonic() - before;
                ++renders;
                last_render_key = std::move(key);
#ifdef DASHBOARD_WINDOW
                if (preview)
                    preview->show(frame);
#endif
            }
            try {
                auto changed = display.update(frame, data["market"].size(), now);
                if (!changed.empty())
                    std::cout << Json{{"refreshed", changed}}.dump() << std::endl;
            } catch (const std::exception &e) {
                std::cerr << e.what() << '\n';
                if (once)
                    throw;
            }
            ++ticks;
            bool complete = true;
            for (auto &j : jobs)
                complete &= j.attempted && !j.future.valid();
            if ((once && (offline || complete)) || (duration > 0 && now - start >= duration))
                break;
            double poll = num(obj(config, "refresh"), "poll_seconds", 1);
            for (double t = 0; t < poll && !stopping; t += .05) {
#ifdef DASHBOARD_WINDOW
                if (preview)
                    preview->poll();
#endif
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        stopping = true;
        for (auto &j : jobs)
            if (j.future.valid())
                j.future.wait();
        std::cout << Json{{"ticks", ticks},
                          {"renders", renders},
                          {"render_mean_ms", renders ? rendering_seconds * 1000 / renders : 0}}
                         .dump()
                  << std::endl;
        close(lock);
        return 0;
    } catch (const std::exception &e) {
        stopping = true;
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }
}
