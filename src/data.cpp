#include "dashboard.hpp"
#include <curl/curl.h>
#include <iomanip>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>

namespace dash {
namespace {
using Params = std::map<std::string, std::string>;
struct HttpResponse {
    long status = 0;
    std::string body;
};
size_t receive(char *ptr, size_t size, size_t count, void *data) {
    auto &body = *static_cast<std::string *>(data);
    size_t n = size * count;
    if (n > 8 * 1024 * 1024 || body.size() + n > 8 * 1024 * 1024)
        return 0;
    body.append(ptr, n);
    return n;
}
int progress(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return stopping ? 1 : 0;
}
HttpResponse get(const std::string &url, const Params &params = {}, int timeout = 15,
                 const Params &headers = {}, const std::string &proxy = "") {
    static const int initialized = []() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT))
            throw std::runtime_error("curl initialization failed");
        return 1;
    }();
    (void)initialized;
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)
        throw std::runtime_error("Endpoint must use HTTP(S)");
    if (timeout < 1 || timeout > 60)
        throw std::runtime_error("Request timeout must be 1-60 seconds");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        throw std::bad_alloc();
    std::string target = url;
    for (auto &[key, value] : params) {
        char *escaped = curl_easy_escape(curl.get(), value.c_str(), int(value.size()));
        if (!escaped)
            throw std::bad_alloc();
        target += (target.find('?') == target.npos ? "?" : "&") + key + "=" + escaped;
        curl_free(escaped);
    }
    HttpResponse response;
    curl_slist *list = nullptr;
    for (auto &[k, v] : headers)
        list = curl_slist_append(list, (k + ": " + v).c_str());
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> holder(list, curl_slist_free_all);
    curl_easy_setopt(curl.get(), CURLOPT_URL, target.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "Mozilla/5.0 KindleDashboard/0.1");
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, long(timeout));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, long(std::min(8, timeout)));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_PROXY, proxy.c_str());
    if (!proxy.empty())
        curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
    if (const char *ca = std::getenv("CURL_CA_BUNDLE"))
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO, ca);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    // TLS peer/hostname verification remains enabled (libcurl defaults).
    auto code = curl_easy_perform(curl.get());
    if (code != CURLE_OK)
        throw std::runtime_error(std::string("HTTP transport: ") + curl_easy_strerror(code));
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status);
    return response;
}
Json payload(const HttpResponse &response) {
    if (response.status != 200)
        throw std::runtime_error("HTTP status " + std::to_string(response.status));
    return Json::parse(response.body);
}
std::pair<std::string, std::string> symbol_spec(std::string s) {
    s = trim(s);
    auto start = s.find('['), end = s.find(']');
    std::string note;
    if (start != s.npos && end == s.size() - 1) {
        note = ascii(s.substr(start + 1, end - start - 1));
        s = trim(s.substr(0, start));
    }
    if (s.find('(') != s.npos)
        throw std::runtime_error(
            "Configure each mixed-source symbol explicitly; fallback syntax is not supported");
    return {s, note};
}
Json quote(const std::string &symbol) {
    return Json{
        {"symbol", symbol},          {"price", "--"},          {"change", "--"}, {"status", "Loading"},
        {"intraday", Json::array()}, {"trading_progress", 1.0}};
}
std::string utc_day(std::time_t now) {
    std::tm tm{};
    gmtime_r(&now, &tm);
    char s[16];
    strftime(s, sizeof s, "%Y-%m-%d", &tm);
    return s;
}
Json session_spans(const fs::path &assets, const std::string &market, std::time_t now) {
    static std::mutex mutex;
    static std::map<std::string, Json> tables;
    std::lock_guard<std::mutex> lock(mutex);
    auto key = assets.string();
    if (!tables.count(key))
        tables[key] = read_json(assets / "calendars.json");
    const auto &table = tables[key].at(market == "us_cash" ? "XNYS" : "XSHG");
    auto day = utc_day(now);
    if (day < str(table, "start") || day > str(table, "end"))
        throw std::runtime_error("Exchange calendar expired; regenerate assets");
    auto it = table.at("sessions").find(day);
    return it == table.at("sessions").end() ? Json::array() : *it;
}
bool allowed(const Json &spans, std::time_t now) {
    for (auto &span : spans)
        if (now >= number(span[0]) && now <= number(span[1]))
            return true;
    return !spans.empty() && now > number(spans.back()[1]) && now <= number(spans.back()[1]) + 1800;
}
double trading_progress(const Json &spans, std::time_t now) {
    double total = 0, elapsed = 0;
    for (auto &span : spans) {
        double a = number(span[0]), b = number(span[1]);
        total += b - a;
        elapsed += std::clamp(double(now) - a, 0.0, b - a);
    }
    return total > 0 ? elapsed / total : 1;
}
std::vector<std::pair<int, int>> custom_sessions(const Json &c) {
    std::vector<std::pair<int, int>> result;
    auto list = obj(c, "sessions");
    if (!list.is_array())
        list = Json::array({"09:30-11:30", "13:00-15:15"});
    for (auto &item : list) {
        auto parts = split(item.get<std::string>(), '-');
        if (parts.size() != 2)
            throw std::runtime_error("Invalid custom sessions");
        auto minute = [](const std::string &s) {
            auto p = split(s, ':');
            if (p.size() != 2)
                throw std::runtime_error("Invalid session time");
            int h = std::stoi(p[0]), m = std::stoi(p[1]);
            if (h < 0 || h > 23 || m < 0 || m > 59)
                throw std::runtime_error("Invalid session time");
            return h * 60 + m;
        };
        int a = minute(parts[0]), b = minute(parts[1]);
        if (a >= b || (!result.empty() && a < result.back().second))
            throw std::runtime_error("Invalid session order");
        result.push_back({a, b});
    }
    if (result.empty())
        throw std::runtime_error("Missing custom sessions");
    return result;
}
Json price_quote(const std::string &symbol, const std::vector<double> &prices, double baseline, double last) {
    if (prices.empty() || !std::isfinite(last) || last <= 0 || !std::isfinite(baseline) || baseline <= 0)
        throw std::runtime_error("Invalid market prices");
    auto q = quote(symbol);
    q["status"] = "OK";
    q["price"] = format(last, 2, false, true);
    q["change"] = format((last / baseline - 1) * 100, 2, true) + "%";
    for (double p : prices)
        if (std::isfinite(p) && p > 0)
            q["intraday"].push_back(std::nearbyint((p / baseline - 1) * 1e6) / 1e4);
    return q;
}
Json custom_quote(const std::string &symbol, const Json &c, const Json &previous) {
    auto now = std::time(nullptr);
    auto sessions = custom_sessions(c);
    std::tm local{};
    localtime_r(&now, &local);
    int minute = local.tm_hour * 60 + local.tm_min;
    bool open = false;
    for (auto [a, b] : sessions)
        open |= minute >= a && minute <= b;
    if (str(previous, "status") == "OK" && (local.tm_wday == 0 || local.tm_wday == 6 || !open))
        return previous;
    Json rows;
    std::time_t day = now;
    int timeout = int(num(c, "timeout_seconds", 10)), lookback = int(num(c, "lookback_days", 30));
    if (lookback < 0 || lookback > 366)
        throw std::runtime_error("Invalid custom lookback_days");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    for (int back = 0; back <= lookback; ++back) {
        if (stopping || std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("Custom lookup cancelled or timed out");
        day = now - back * 86400;
        auto params = Params{{"product", str(c, "product")}, {"date", date_string(day, "%Y-%m-%d")}};
        auto response = get(str(c, "url"), params, timeout);
        if (response.status == 404)
            continue;
        rows = payload(response);
        if (rows.is_array() && !rows.empty())
            break;
    }
    if (!rows.is_array() || rows.empty())
        throw std::runtime_error("No custom data in lookback window");
    Json last;
    std::map<int, double> minutes;
    std::time_t day_start = parse_time(date_string(day, "%Y-%m-%d"));
    for (auto &row : rows) {
        double ret = num(row, "ret", NAN), timestamp = num(row, "time", NAN);
        if (!std::isfinite(ret) || !std::isfinite(timestamp) || timestamp / 1000 < day_start ||
            timestamp / 1000 >= day_start + 86400)
            continue;
        std::time_t sample = std::time_t(timestamp / 1000);
        last = row;
        std::tm local_sample{};
        localtime_r(&sample, &local_sample);
        minutes[local_sample.tm_hour * 60 + local_sample.tm_min] = std::nearbyint(ret * 1e6) / 1e4;
    }
    if (last.is_null())
        throw std::runtime_error("Invalid custom data");
    auto q = quote(symbol);
    q["status"] = "OK";
    double exposure = num(last, "exposure", NAN);
    q["price"] = std::isfinite(exposure) ? format(exposure * 100, 0) + "%" : "";
    q["change"] = format(num(last, "ret") * 100, 2, true) + "%";
    bool historical = date_string(day, "%Y-%m-%d") < date_string(now, "%Y-%m-%d");
    q["is_closed"] = historical || minute >= sessions.back().second;
    double total = 0, elapsed = 0;
    for (auto [a, b] : sessions) {
        total += b - a;
        elapsed += std::clamp(double(minute - a), 0.0, double(b - a));
        for (int m = a; m < b; ++m) {
            if (!historical && m > minute)
                break;
            auto it = minutes.find(m);
            q["intraday"].push_back(it == minutes.end() ? Json(nullptr) : Json(it->second));
        }
    }
    q["trading_progress"] = historical ? 1.0 : elapsed / total;
    return q;
}
Json market_quote(size_t index, const Json &config, const Json &previous, const fs::path &assets) {
    const auto &m = obj(config, "market");
    auto [symbol, note] = symbol_spec(m.at("mix_symbols").at(index));
    auto rule = obj(obj(m, "mix_sources"), symbol);
    auto custom = obj(obj(m, "custom_symbols"), symbol);
    Json q;
    auto market = str(rule, "market");
    auto now = std::time(nullptr);
    Json spans = Json::array();
    if (market == "a_share" || market == "us_cash") {
        spans = session_spans(assets, market, now);
        if (str(previous, "status") == "OK" && !allowed(spans, now)) {
            auto cached = previous;
            bool closed = spans.empty() || now < number(spans.front()[0]) || now > number(spans.back()[1]);
            cached["is_closed"] = closed;
            cached["trading_progress"] = closed ? 1 : trading_progress(spans, now);
            cached["delay_minutes"] = 0;
            return cached;
        }
    }
    int timeout = int(num(rule, "timeout_seconds", 15));
    auto provider = str(rule, "provider"), url = str(rule, "url");
    std::vector<double> prices;
    double baseline = NAN, last = NAN;
    std::time_t quoted_at = 0;
    if (!custom.empty())
        q = custom_quote(symbol, custom, previous);
    else if (provider == "binance" || symbol == "BTC-USD" || symbol == "BTCUSDT") {
        auto c = obj(m, "binance");
        auto ticker =
            payload(get(str(c, "ticker_url", "https://data-api.binance.vision/api/v3/ticker/24hr"),
                        {{"symbol", str(rule, "code", "BTCUSDT")}}, int(num(c, "timeout_seconds", 15))));
        baseline = num(ticker, "openPrice", NAN);
        last = num(ticker, "lastPrice", NAN);
        try {
            auto klines = payload(get(
                str(c, "klines_url", "https://data-api.binance.vision/api/v3/klines"),
                {{"symbol", str(rule, "code", "BTCUSDT")}, {"interval", "1m"}, {"limit", "1000"}}, timeout));
            for (auto &row : klines)
                if (row.is_array() && row.size() > 6) {
                    double p = number(row[4]);
                    if (p > 0 && std::isfinite(p)) {
                        prices.push_back(p);
                        quoted_at = std::time_t(number(row[6]) / 1000);
                    }
                }
        } catch (...) {
            if (stopping)
                throw;
        }
        if (prices.empty())
            prices.push_back(last);
        else
            prices.back() = last;
        q = price_quote(symbol, prices, baseline, last);
        q["is_24h"] = true;
        if (std::isfinite(num(ticker, "priceChangePercent", NAN)))
            q["change"] = format(num(ticker, "priceChangePercent"), 2, true) + "%";
    } else if (provider == "tencent") {
        auto code = str(rule, "code", symbol);
        auto p = payload(get(url.empty() ? "https://web.ifzq.gtimg.cn/appstock/app/minute/query" : url,
                             url.find("code=") == url.npos ? Params{{"code", code}} : Params{}, timeout));
        if (num(p, "code", -1) != 0)
            throw std::runtime_error("Tencent returned an error");
        auto entry = p.at("data").at(code);
        auto fields = entry.at("qt").at(code);
        if (!fields.is_array() || fields.size() < 5)
            throw std::runtime_error("Tencent quote missing");
        last = number(fields[3]);
        baseline = number(fields[4]);
        auto rows = obj(obj(entry, "data"), "data");
        if (rows.is_array())
            for (auto &row : rows) {
                if (!row.is_string())
                    continue;
                std::istringstream in(row.get<std::string>());
                std::string minute;
                double price;
                if (in >> minute >> price && price > 0 && std::isfinite(price))
                    prices.push_back(price);
            }
        if (prices.empty())
            prices.push_back(last);
        else
            prices.back() = last;
        if (fields.size() > 30 && fields[30].is_string()) {
            auto t = fields[30].get<std::string>();
            if (t.size() == 14)
                try {
                    quoted_at =
                        parse_time(t.substr(0, 4) + "-" + t.substr(4, 2) + "-" + t.substr(6, 2) + " " +
                                   t.substr(8, 2) + ":" + t.substr(10, 2) + ":" + t.substr(12, 2));
                } catch (...) {
                }
        }
        q = price_quote(symbol, prices, baseline, last);
    } else if (provider == "sina_futures") {
        auto p = payload(get(url, {}, timeout));
        auto rows = obj(p, "minLine_1d");
        if (!rows.is_array() || rows.empty())
            throw std::runtime_error("Sina futures minute data missing");
        if (rows[0].is_array() && rows[0].size() > 5)
            baseline = number(rows[0][5]);
        for (auto &row : rows) {
            if (!row.is_array() || row.size() < 2)
                continue;
            double price = number(row[1]);
            if (price > 0 && std::isfinite(price))
                prices.push_back(price);
            if (row.back().is_string())
                try {
                    quoted_at = parse_time(row.back().get<std::string>());
                } catch (...) {
                }
        }
        if (prices.empty())
            throw std::runtime_error("Sina futures prices missing");
        if (!std::isfinite(baseline) || baseline <= 0)
            baseline = prices.front();
        q = price_quote(symbol, prices, baseline, prices.back());
        q["is_24h"] = true;
    } else if (provider == "sina_us") {
        auto response = get(url, {}, timeout);
        if (response.status != 200)
            throw std::runtime_error("Sina US request failed");
        // A whole-day response can be large. libstdc++ regex recursively matching
        // (.*) overflows musl's small worker stack; parse the fixed JSONP wrapper.
        auto body = trim(response.body);
        auto begin = body.find("=(\"");
        if (!body.empty() && body.back() == ';')
            body.pop_back();
        if (begin == body.npos || body.size() < begin + 5 || body.substr(body.size() - 2) != "\")")
            throw std::runtime_error("Invalid Sina US JSONP");
        for (auto &row : split(body.substr(begin + 3, body.size() - begin - 5), ';')) {
            auto fields = split(row, ',');
            if (fields.size() < 4)
                continue;
            double price = number(fields[3]);
            if (price > 0 && std::isfinite(price))
                prices.push_back(price);
        }
        if (prices.empty())
            throw std::runtime_error("Sina US prices missing");
        q = price_quote(symbol, prices, prices.front(), prices.back());
    } else if (provider == "eastmoney") {
        auto p = payload(get(url, {}, timeout));
        auto d = obj(p, "data");
        baseline = num(d, "preClose", NAN);
        for (auto &row : obj(d, "trends")) {
            if (!row.is_string())
                continue;
            auto fields = split(row.get<std::string>(), ',');
            if (fields.size() < 3)
                continue;
            double price = number(fields[2]);
            if (price > 0 && std::isfinite(price))
                prices.push_back(price);
            try {
                quoted_at = parse_time(fields[0]);
            } catch (...) {
            }
        }
        if (prices.empty())
            throw std::runtime_error("Eastmoney prices missing");
        q = price_quote(symbol, prices, baseline, prices.back());
    } else
        throw std::runtime_error("Missing or unsupported mixed market provider");
    q["note"] = note;
    if (market == "a_share" || market == "us_cash") {
        bool closed = spans.empty() || now < number(spans.front()[0]) || now > number(spans.back()[1]) ||
                      (quoted_at && quoted_at < parse_time(date_string(now, "%Y-%m-%d")));
        q["is_closed"] = closed;
        q["trading_progress"] = closed ? 1 : trading_progress(spans, now);
    }
    if (quoted_at && !flag(q, "is_closed")) {
        int delay = int(std::nearbyint(std::difftime(now, quoted_at) / 60));
        q["delay_minutes"] = delay >= 2 ? delay : 0;
        if (market == "a_share" || market == "us_cash") {
            bool in_session = false;
            for (auto &span : spans)
                in_session |= now >= number(span[0]) && now <= number(span[1]);
            if (!in_session)
                q["delay_minutes"] = 0;
        }
    }
    auto conversion = obj(obj(m, "conversions"), symbol);
    if (!conversion.empty())
        try {
            double rate = num(conversion, "rate", 1);
            auto rateprovider = str(conversion, "rate_provider");
            if (rateprovider == "sina_fx_previous_close") {
                auto response =
                    get(str(conversion, "rate_url"), {}, int(num(conversion, "rate_timeout_seconds", 15)),
                        {{"Referer", "https://finance.sina.com.cn/"}});
                std::smatch match;
                if (response.status != 200 ||
                    !std::regex_search(response.body, match, std::regex("=\"([^\"]*)\"")))
                    throw std::runtime_error("FX quote missing");
                auto fields = split(match[1], ',');
                if (fields.size() < 4)
                    throw std::runtime_error("FX previous close missing");
                rate = number(fields[3]);
            } else if (!rateprovider.empty())
                throw std::runtime_error("Unsupported FX provider");
            auto text = str(q, "price");
            text.erase(std::remove(text.begin(), text.end(), ','), text.end());
            double price = number(text);
            double factor = str(conversion, "unit_conversion") == "troy_ounce_to_gram"
                                ? 1 / 31.1034768
                                : num(conversion, "factor", 1);
            if (!std::isfinite(price * rate) || rate <= 0)
                throw std::runtime_error("Invalid conversion");
            q["converted"] = format(price * rate * factor, int(num(conversion, "decimals", 2)), false, true) +
                             " " + str(conversion, "unit");
        } catch (...) {
            if (stopping)
                throw;
            if (!str(previous, "converted").empty())
                q["converted"] = previous["converted"];
        }
    return q;
}
Json weather(const Json &config) {
    const auto &c = obj(config, "weather");
    auto location = str(c, "location", "Hangzhou");
    double lat = num(c, "latitude", NAN), lon = num(c, "longitude", NAN);
    if (!std::isfinite(lat) || !std::isfinite(lon)) {
        auto geo =
            payload(get(str(c, "geocoding_url", "https://geocoding-api.open-meteo.com/v1/search"),
                        {{"name", location}, {"count", "1"}, {"language", "en"}, {"format", "json"}}, 10));
        auto list = obj(geo, "results");
        if (!list.is_array() || list.empty())
            throw std::runtime_error("Weather location not found");
        lat = num(list[0], "latitude", NAN);
        lon = num(list[0], "longitude", NAN);
        location = str(list[0], "name", location);
    }
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::abs(lat) > 90 || std::abs(lon) > 180)
        throw std::runtime_error("Invalid weather coordinates");
    auto p = payload(get(str(c, "forecast_url", "https://api.open-meteo.com/v1/forecast"),
                         {{"latitude", format(lat, 5)},
                          {"longitude", format(lon, 5)},
                          {"current", "temperature_2m,relative_humidity_2m,weather_code"},
                          {"hourly", "precipitation_probability,precipitation,uv_index"},
                          {"daily", "temperature_2m_max,temperature_2m_min"},
                          {"forecast_hours", "1"},
                          {"timezone", "auto"}},
                         10));
    auto first = [](const Json &j, const std::string &k) {
        const auto &v = obj(j, k);
        return v.is_array() && !v.empty() ? number(v[0]) : NAN;
    };
    const auto &cur = obj(p, "current");
    const auto &hourly = obj(p, "hourly");
    double temp = num(cur, "temperature_2m", NAN), lo = first(obj(p, "daily"), "temperature_2m_min"),
           hi = first(obj(p, "daily"), "temperature_2m_max");
    if (!std::isfinite(temp))
        throw std::runtime_error("Weather temperature missing");
    Json result = {
        {"title", location},
        {"temperature", format(temp, 1) + " C"},
        {"temperature_value_c", temp},
        {"temperature_min_c", lo},
        {"temperature_max_c", hi},
        {"temperature_min", format(lo, 1) + " C"},
        {"temperature_max", format(hi, 1) + " C"},
        {"weather_code", obj(cur, "weather_code")},
        {"precipitation_probability", "Rain " + format(first(hourly, "precipitation_probability"), 0) + "%"},
        {"precipitation_amount", "Precip " + format(first(hourly, "precipitation"), 1) + "mm"},
        {"uv_index", "UV " + format(first(hourly, "uv_index"), 1)},
        {"humidity", "Humidity " + format(num(cur, "relative_humidity_2m", NAN), 0) + "%"},
        {"aqi", "AQI --"},
        {"status", "OK"}};
    try {
        auto air =
            payload(get(str(c, "air_quality_url", "https://air-quality-api.open-meteo.com/v1/air-quality"),
                        {{"latitude", format(lat, 5)},
                         {"longitude", format(lon, 5)},
                         {"current", "us_aqi"},
                         {"timezone", "auto"}},
                        10));
        result["aqi"] = "AQI " + format(num(obj(air, "current"), "us_aqi", NAN), 0);
    } catch (...) {
        if (stopping)
            throw;
    }
    return result;
}
Json clouds(const Json &config) {
    const auto &c = obj(config, "cloud");
    double lat = num(c, "latitude", 30.2741), lon = num(c, "longitude", 120.1551);
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::abs(lat) > 90 || std::abs(lon) > 180)
        throw std::runtime_error("Invalid cloud coordinates");
    auto raw = payload(get(str(c, "url", "https://api.met.no/weatherapi/locationforecast/2.0/complete"),
                           {{"lat", format(lat, 4)}, {"lon", format(lon, 4)}}, 15, {},
                           str(c, "proxy")));
    const auto &series = obj(obj(raw, "properties"), "timeseries");
    if (!series.is_array())
        throw std::runtime_error("Invalid cloud forecast");
    std::map<std::time_t, double> aerosol;
    try {
        auto air = payload(get(str(c, "air_quality_url", "https://air-quality-api.open-meteo.com/v1/air-quality"),
                               {{"latitude", format(lat, 4)}, {"longitude", format(lon, 4)},
                                {"hourly", "aerosol_optical_depth"}, {"timeformat", "unixtime"},
                                {"forecast_days", "5"}}, 15, {}, str(c, "proxy")));
        const auto &hourly = obj(air, "hourly");
        const auto &times = obj(hourly, "time"), &values = obj(hourly, "aerosol_optical_depth");
        if (times.is_array() && values.is_array())
            for (size_t i = 0; i < std::min(times.size(), values.size()); ++i) {
                double t = number(times[i]), v = number(values[i]);
                if (std::isfinite(t) && std::isfinite(v) && v >= 0)
                    aerosol[std::time_t(t)] = v;
            }
    } catch (...) {
        if (stopping) throw;
        // Missing AOD must not hide the cloud forecast; render it as unavailable.
    }
    Json points = Json::array();
    auto now = std::time(nullptr);
    auto start = ((now + 8 * 3600 + 10799) / 10800) * 10800 - 8 * 3600;
    int valid = 0;
    for (int i = 0; i < 12; ++i) {
        auto target = start + i * 10800;
        const Json *best = nullptr;
        double distance = 5401;
        for (const auto &row : series) {
            double delta = std::abs(double(parse_time(str(row, "time"), true) - target));
            if (delta < distance) { distance = delta; best = &row; }
        }
        Json point = {{"time", target}};
        if (best && distance <= 5400) {
            const auto &details = obj(obj(obj(*best, "data"), "instant"), "details");
            for (auto key : {"cloud_area_fraction", "cloud_area_fraction_high", "cloud_area_fraction_medium",
                             "cloud_area_fraction_low", "air_temperature", "dew_point_temperature", "wind_speed"}) {
                double value = num(details, key, NAN);
                bool cloud = std::string(key).rfind("cloud_", 0) == 0;
                point[key] = std::isfinite(value) && (!cloud || (value >= 0 && value <= 100))
                                 ? Json(value) : Json(nullptr);
            }
            double rain = num(obj(obj(obj(*best, "data"), "next_6_hours"), "details"),
                              "precipitation_amount", NAN);
            point["precipitation_6h"] = std::isfinite(rain) && rain >= 0 ? Json(rain) : Json(nullptr);
            if (!point["cloud_area_fraction"].is_null()) ++valid;
        }
        auto aod = aerosol.find(target);
        point["aod"] = aod == aerosol.end() ? Json(nullptr) : Json(aod->second);
        points.push_back(point);
    }
    if (valid < 8) throw std::runtime_error("Insufficient cloud forecast points");
    return {{"status", "OK"}, {"fetched_at", now}, {"name", str(c, "name", "杭州")}, {"points", points}};
}
std::vector<std::string> sunset_codes(std::time_t now) {
    std::tm tm{};
    localtime_r(&now, &tm);
    return tm.tm_hour < 9    ? std::vector<std::string>{"rise_1", "set_1"}
           : tm.tm_hour < 20 ? std::vector<std::string>{"set_1", "rise_2"}
                             : std::vector<std::string>{"rise_2", "set_2"};
}
bool event_matches(const Json &event, const std::string &code, std::time_t now) {
    try {
        return str(event, "name").rfind(code.rfind("rise", 0) == 0 ? "Sunrise" : "Sunset", 0) == 0 &&
               date_string(parse_time(str(event, "time")), "%Y-%m-%d") ==
                   date_string(now + (code.back() == '2' ? 86400 : 0), "%Y-%m-%d");
    } catch (...) {
        return false;
    }
}
Json sunset(const Json &config, const Json &previous) {
    auto c = obj(config, "sunset");
    auto now = std::time(nullptr);
    Json result = {{"status", "Unavailable"}, {"events", Json::array()}, {"fetched_at", now}};
    if (!flag(c, "enabled", true)) {
        result["status"] = "Skipped";
        return result;
    }
    for (auto &code : sunset_codes(now)) {
        Json e = {
            {"code", code}, {"name", code.rfind("rise", 0) == 0 ? "Sunrise" : "Sunset"}, {"fetched_at", now}};
        for (auto model : {"EC", "GFS"})
            try {
                auto p = payload(get(str(c, "url", "https://sunsetbot.top/detailed/"),
                                     {{"query_id", std::to_string(1000000 + now % 9000000)},
                                      {"intend", "select_city"},
                                      {"query_city", str(c, "city", "杭州")},
                                      {"model", model},
                                      {"event_date", "None"},
                                      {"event", code},
                                      {"times", "None"}},
                                     int(num(c, "timeout_seconds", 10))));
                if (str(p, "status") != "ok")
                    throw std::runtime_error("Sunset API error");
                auto prefix = std::string(model) == "EC" ? "ec_" : "gfs_";
                for (auto key : {"aod", "quality"}) {
                    double value = num(p, std::string("local_") + key, num(p, std::string("tb_") + key, NAN));
                    e[std::string(prefix) + key] = format(value, 3);
                }
                if (!e.contains("time"))
                    e["time"] = str(p, "tb_event_time");
            } catch (...) {
                if (stopping)
                    throw;
            }
        if (event_matches(e, code, now))
            result["events"].push_back(e);
        else
            for (auto &old : obj(previous, "events")) {
                double age = now - num(old, "fetched_at", 0);
                if (event_matches(old, code, now) && age >= 0 &&
                    age < num(c, "cache_max_age_seconds", 1800)) {
                    auto cached = old;
                    cached["code"] = code;
                    result["events"].push_back(cached);
                    break;
                }
            }
    }
    if (!result["events"].empty())
        result["status"] = "OK";
    return result;
}
double work_seconds(std::time_t start, std::time_t end) {
    if (start >= end)
        return 0;
    auto day = parse_time(date_string(start, "%Y-%m-%d"));
    double total = 0;
    for (int i = 0; i < 10 && day <= end; ++i, day += 86400) {
        std::tm tm{};
        localtime_r(&day, &tm);
        if (tm.tm_wday == 0 || tm.tm_wday == 6)
            continue;
        for (auto span : std::vector<std::pair<int, int>>{{540, 690}, {780, 1080}})
            total += std::max(0.0, std::difftime(std::min(end, day + span.second * 60),
                                                 std::max(start, day + span.first * 60)));
    }
    return total;
}
Json usage_window(const Json &raw, std::string name, int limit, const Json &previous) {
    if (!raw.is_object() || !std::isfinite(num(raw, "used_percent", NAN))) {
        if (str(previous, "status") == "OK" && !flag(previous, "no_limit") &&
            (num(previous, "used_percent") > 0 || !str(previous, "reset_at_local").empty()))
            return previous;
        return Json{{"name", name}, {"used_percent", 0}, {"not_started", true}, {"status", "OK"}};
    }
    auto now = std::time(nullptr);
    std::time_t reset = 0;
    std::string reset_local = str(raw, "reset_at_local"), human = str(raw, "reset_after_human");
    try {
        auto text = reset_local;
        if (text.size() >= 5 && text[2] == '-')
            text = date_string(now, "%Y") + "-" + text;
        reset = parse_time(text);
        if (reset < now - 30 * 86400 && reset_local.size() > 2 && reset_local[2] == '-')
            reset = parse_time(std::to_string(std::stoi(date_string(now, "%Y")) + 1) + "-" + reset_local);
    } catch (...) {
    }
    if (!reset) {
        long seconds = 0;
        for (auto [unit, mult] :
             std::vector<std::pair<std::string, int>>{{"d", 86400}, {"h", 3600}, {"m", 60}, {"s", 1}}) {
            std::smatch match;
            if (std::regex_search(human, match, std::regex("([0-9]+)\\s*" + unit)))
                seconds += std::stol(match[1]) * mult;
        }
        if (seconds > 0)
            reset = now + seconds;
    }
    int t = reset ? std::clamp(int(std::nearbyint((limit - std::difftime(reset, now)) * 100 / limit)), 0, 100)
                  : 0,
        e = 0;
    if (reset) {
        double total = work_seconds(reset - limit, reset);
        if (total > 0)
            e = std::clamp(
                int(std::nearbyint(work_seconds(reset - limit, std::min(now, reset)) * 100 / total)), 0, 100);
    }
    double used = std::clamp(num(raw, "used_percent"), 0.0, 100.0);
    bool notstarted = used == 0 || (used == 1 && t <= 1);
    if (notstarted)
        used = 0;
    return Json{{"name", name},
                {"used_percent", used},
                {"remaining_percent", 100 - used},
                {"reset_after_human", human},
                {"reset_at_local", reset_local},
                {"t_progress", t},
                {"e_progress", e},
                {"not_started", notstarted},
                {"no_limit", false},
                {"status", "OK"}};
}
Json usage(const Json &config, const Json &previous) {
    auto c = obj(config, "agent_usage");
    auto url = str(c, "url");
    if (url.empty())
        throw std::runtime_error("agent_usage.url missing");
    auto path = url.find('/', url.find("://") + 3);
    if (path == url.npos)
        url += "/api/usage";
    else if (path == url.size() - 1)
        url += "api/usage";
    Params headers;
    if (!str(c, "password").empty())
        headers["Authorization"] = "Bearer " + str(c, "password");
    auto p = payload(get(url, {}, int(num(c, "timeout_seconds", 15)), headers));
    if (!p.is_object())
        throw std::runtime_error("Invalid usage response");
    auto codex = obj(p, "codex"), agy = obj(p, "agy");
    if (!flag(codex, "success", true))
        codex = Json::object();
    if (!flag(agy, "success", true))
        agy = Json::object();
    return Json{
        {"status", "OK"},
        {"fetched_at", std::time(nullptr)},
        {"codex_5h",
         usage_window(obj(codex, "primary_window"), "CODEX 5H", 18000, obj(previous, "codex_5h"))},
        {"codex_7d",
         usage_window(obj(codex, "secondary_window"), "CODEX 7D", 604800, obj(previous, "codex_7d"))},
        {"agy_5h", usage_window(obj(agy, "gemini_5h"), "AGY 5H", 18000, Json::object())},
         {"agy_7d", usage_window(obj(agy, "gemini_weekly"), "AGY 7D", 604800, obj(previous, "agy_7d"))}};
}
Json daily_news(const Json &config) {
    const auto &c = obj(config, "news");
    auto platform = str(c, "platform", "eastmoney");
    auto p = payload(get(str(c, "url", "https://news.orz.ai/api/v1/dailynews/"), {{"platform", platform}},
                         int(num(c, "timeout_seconds", 15))));
    const auto &rows = obj(p, "data");
    if (!rows.is_array())
        throw std::runtime_error("Invalid news response");
    Json items = Json::array();
    for (const auto &row : rows) {
        if (items.size() == 20)
            break;
        auto title = str(row, "title"), published = ascii(str(row, "publish_time"));
        if (title.empty())
            continue;
        if (title.size() > 512) {
            size_t end = 512;
            while (end && ((unsigned char)title[end] & 0xc0) == 0x80)
                --end;
            title.resize(end);
        }
        if (published.size() > 19)
            published.resize(19);
        items.push_back({{"title", title}, {"publish_time", published}});
    }
    if (items.empty())
        throw std::runtime_error("News response is empty");
    return Json{{"status", "OK"},
                {"platform", platform},
                {"fetched_at", std::time(nullptr)},
                {"items", std::move(items)}};
}
} // namespace
Json initial_data(const Json &config) {
    Json market = Json::array();
    for (auto &spec : obj(obj(config, "market"), "mix_symbols")) {
        auto [symbol, note] = symbol_spec(spec);
        auto q = quote(symbol);
        q["note"] = note;
        market.push_back(q);
    }
    return Json{{"generated_at", date_string(std::time(nullptr))},
                {"market", market},
                {"weather",
                 {{"title", str(obj(config, "weather"), "location", "WEATHER")},
                  {"temperature", "-- C"},
                  {"status", "Loading"}}},
                {"sunset", {{"status", "Loading"}, {"events", Json::array()}}},
                {"agent_usage", Json::object()},
                {"news",
                 {{"status", "Loading"},
                  {"platform", str(obj(config, "news"), "platform", "eastmoney")},
                  {"items", Json::array()}}}};
}
Json fetch_region(const std::string &name, const Json &config, const Json &previous, const fs::path &assets) {
    if (name == "cloud")
        return clouds(config);
    if (name == "weather")
        return weather(config);
    if (name == "sunset")
        return sunset(config, previous);
    if (name == "usage")
        return usage(config, previous);
    if (name == "news")
        return daily_news(config);
    if (name.rfind("market:", 0) == 0)
        return market_quote(std::stoul(name.substr(7)), config, previous, assets);
    throw std::runtime_error("Invalid region name");
}
void merge_region(Json &data, const std::string &name, const Json &result) {
    if (name.rfind("market:", 0) == 0)
        data["market"].at(std::stoul(name.substr(7))) = result;
    else
        data[name == "usage" ? "agent_usage" : name] = result;
}
void expire_sunset(Json &data, const Json &config, std::time_t now) {
    auto &forecast = data["sunset"];
    if (!obj(forecast, "events").is_array())
        return;
    Json valid = Json::array();
    for (auto &code : sunset_codes(now))
        for (auto &e : forecast["events"]) {
            double age = now - num(e, "fetched_at", num(forecast, "fetched_at", 0));
            if (event_matches(e, code, now) && age >= 0 &&
                age < num(obj(config, "sunset"), "cache_max_age_seconds", 1800)) {
                auto event = e;
                event["code"] = code;
                valid.push_back(event);
                break;
            }
        }
    if (!forecast["events"].empty() && valid.empty())
        forecast["status"] = "Unavailable";
    forecast["events"] = valid;
}
bool cloud_mode(const Json &sunset, bool current) {
    auto status = str(sunset, "status");
    if (status == "Loading") return current;
    const auto &events = obj(sunset, "events");
    if (status != "OK" || !events.is_array() || events.empty()) return true;
    int low = 0;
    for (size_t i = 0; i < std::min(size_t(2), events.size()); ++i) {
        bool available = false, below = true;
        for (auto key : {"ec_quality", "gfs_quality"}) {
            double q = num(events[i], key, NAN);
            if (!std::isfinite(q)) continue;
            if (q > .2) return false;
            available = true;
            below &= q < .2;
        }
        low += available && below;
    }
    return low == 2 ? true : current;
}
double interval(const std::string &name, const Json &c) {
    if (name == "cloud")
        return 10800;
    if (name == "news")
        return 900;
    auto r = obj(c, "refresh");
    if (obj(r, "regions").contains(name))
        return num(obj(r, "regions"), name, 60);
    if (name == "weather")
        return num(r, "weather_seconds", 600);
    if (name == "usage")
        return num(obj(c, "agent_usage"), "refresh_seconds", 60);
    if (name == "sunset")
        return std::max(1.0, std::min(num(obj(c, "sunset"), "refresh_seconds", 3600),
                                      num(obj(c, "sunset"), "cache_max_age_seconds", 1800)));
    return num(r, "market_seconds", 60);
}
} // namespace dash
