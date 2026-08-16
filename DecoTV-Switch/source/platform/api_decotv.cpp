// DecoTV Switch 客户端 —— 网络层实现（纯 TVBox 客户端）
// 标准 tvbox 协议：GET {api}?wd=关键词&ac=detail 搜索，返回 vod_play_url 由 parsePlayUrl 解析。
#include <switch.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <borealis/core/logger.hpp>

#include <sys/stat.h>
#include <unistd.h>
#include <ctime>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "api_decotv.h"

namespace decotv {

using json = nlohmann::json;

unsigned int g_netInitResult = 0;

// ---- URL 编码（中文参数如片名需要）----
static std::string urlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ---- curl 写回调 ----
static size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    ((std::string*)userp)->append((char*)contents, total);
    return total;
}

// 选源诊断日志（落 sdmc，便于真机排错）。append 模式会无限增长，超过 256KB 覆盖重写。
static const long LOG_MAX_BYTES = 256 * 1024;
static const char* SOURCE_LOG = "sdmc:/switch/DecoTV/source.log";

static const char* logOpenMode(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > LOG_MAX_BYTES) return "w";
    return "a";
}

static void decotvLog(const std::string& line) {
    FILE* f = fopen(SOURCE_LOG, logOpenMode(SOURCE_LOG));
    if (f) {
        fputs(line.c_str(), f);
        fputc('\n', f);
        fclose(f);
    }
}

// 崩溃轨迹日志：关键节点落盘，崩后读卡看最后一行即知崩在哪一步。超 256KB 覆盖。
static const char* TRAIL_LOG = "sdmc:/switch/DecoTV/trail.log";
void trailLog(const std::string& line) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    FILE* f = fopen(TRAIL_LOG, logOpenMode(TRAIL_LOG));
    if (f) {
        fprintf(f, "[%s] %s\n", ts, line.c_str());
        fclose(f);
    }
}

// 判断地址是否像可直接播放的媒体直链（而非 /share/、/play/ 页面地址）
static bool looksLikeMedia(const std::string& u) {
    std::string s = u;
    size_t q = s.find('?');
    if (q != std::string::npos) s = s.substr(0, q);
    const char* exts[] = {".m3u8", ".m3u", ".mp4", ".ts", ".flv",
                          ".mkv", ".webm", ".mov", ".aac", ".mp3"};
    for (const char* e : exts) {
        size_t n = strlen(e);
        if (s.size() >= n && s.compare(s.size() - n, n, e) == 0)
            return true;
    }
    if (s.find("/index.m3u8") != std::string::npos) return true;
    return false;
}

// 把 TVBox 的 vod_play_url 解析出第一个可播放的媒体地址。
// 用 # 分隔剧集、$ 分隔「名称$地址」、$$$ 分隔不同清晰度/直链块与分享页块：
//   正片$https://a.com/x.m3u8
//   720P$https://a.com/share/XXX$$$720P$https://a.com/x.m3u8
// 策略：按 $$$ 拆块，取第一集在各块的候选地址，优先真实媒体直链，挑不到退回首个候选。
static std::string parsePlayUrl(const std::string& raw) {
    if (raw.empty()) return "";

    std::vector<std::string> blocks;
    size_t pos = 0;
    while (true) {
        size_t d = raw.find("$$$", pos);
        if (d == std::string::npos) { blocks.push_back(raw.substr(pos)); break; }
        blocks.push_back(raw.substr(pos, d - pos));
        pos = d + 3;
    }

    std::vector<std::string> candidates;
    for (auto& blk : blocks) {
        std::string ep = blk;
        size_t h = blk.find('#');
        if (h != std::string::npos) ep = blk.substr(0, h);
        size_t d = ep.find('$');
        std::string url = (d != std::string::npos) ? ep.substr(d + 1) : ep;
        if (!url.empty()) {
            bool dup = false;
            for (auto& x : candidates) if (x == url) { dup = true; break; }
            if (!dup) candidates.push_back(url);
        }
    }
    if (candidates.empty()) return "";

    for (auto& c : candidates) if (looksLikeMedia(c)) return c;
    return candidates[0];
}

// ---- 创建 curl easy handle ----
// 某些 tvbox 源有防盗链 cookie，沿用 COOKIEFILE+COOKIEJAR 同一文件读写（无害）。
static CURL* makeHandle(std::string* outBody) {
    CURL* curl = curl_easy_init();
    if (!curl) return nullptr;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, outBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DecoTV-Switch/2.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // HTTPS 支持：跳过证书校验（很多 tvbox 源站证书不在 Switch 系统 CA 内，homebrew 可接受）
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    mkdir("sdmc:/switch/DecoTV", 0777);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "sdmc:/switch/DecoTV/cookies.txt");
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, "sdmc:/switch/DecoTV/cookies.txt");

    return curl;
}

bool initNetwork() {
    fsdevMountSdmc();
    mkdir("sdmc:/switch/DecoTV", 0777);

    Result rc = socketInitializeDefault();
    g_netInitResult = rc;
    if (R_FAILED(rc) && rc != MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized))
        return false;

    sslInitialize(8);   // curl 的 libnx TLS 后端需要；**退出时不要 sslExit()**（v1.28 教训）
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return true;
}

void exitNetwork() {
    curl_global_cleanup();
    // 注意：不调 sslExit()（v1.28 教训：脏状态跨进程残留 -> 再启动 HTTPS 全废/系统崩溃）。
    // socketExit 由 borealis 的 userAppExit 负责。
}

// ---- 执行一次请求并填 HttpResponse ----
static bool perform(CURL* curl, HttpResponse* out) {
    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (out) {
        out->curlError = (int)res;
        out->status = status;
    }
    return res == CURLE_OK;
}

void httpGet(const std::string& url, bool, HttpResponse* out) {
    HttpResponse resp;
    std::string body;
    CURL* curl = makeHandle(&body);
    if (!curl) {
        if (out) *out = resp;
        return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    bool ok = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        body.clear();
        ok = perform(curl, &resp);
        if (ok) break;
        if (attempt < 1) {
            brls::Logger::info("decotv: GET {} attempt {} failed (curl={}, http={}), retrying...",
                               url, attempt + 1, resp.curlError, resp.status);
            sleep(1);
        }
    }
    curl_easy_cleanup(curl);

    resp.body = body;
    if (out) *out = resp;
    brls::Logger::info("decotv: GET {} -> HTTP {} (curl={}, {} bytes)",
                       url, resp.status, resp.curlError, body.size());
}

// ---- 加载用户源配置 ----
// config.json 格式（标准 tvbox 订阅子集）：
// {
//   "sites": [ {"key":"360zy","name":"TV-360资源",
//              "api":"https://360zy.com/api.php/provide/vod","searchable":1}, ... ],
//   "subscriptions": [ "https://某订阅/tvbox.json", ... ]   // 可选，远程订阅合并 sites
// }
std::vector<TvboxSite> loadConfig() {
    std::vector<TvboxSite> result;
    const char* path = "sdmc:/switch/DecoTV/config.json";
    FILE* f = fopen(path, "r");
    if (!f) {
        trailLog("loadConfig: config.json NOT FOUND");
        return result;
    }
    std::string content;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) content += buf;
    fclose(f);

    try {
        json j = json::parse(content);
        auto addSite = [&](const json& s) {
            TvboxSite site;
            site.key  = s.value("key", "");
            site.name = s.value("name", "");
            site.api  = s.value("api", "");
            site.searchable = s.value("searchable", 1) != 0;
            if (!site.key.empty() && !site.api.empty()) result.push_back(site);
        };
        if (j.contains("sites") && j["sites"].is_array())
            for (auto& s : j["sites"]) addSite(s);

        if (j.contains("subscriptions") && j["subscriptions"].is_array()) {
            for (auto& sub : j["subscriptions"]) {
                std::string url = sub.get<std::string>();
                HttpResponse resp;
                httpGet(url, false, &resp);
                if (resp.body.empty()) continue;
                try {
                    json sj = json::parse(resp.body);
                    json sitesArr = sj.is_array() ? sj
                                    : (sj.contains("sites") ? sj["sites"] : json::array());
                    if (sitesArr.is_array())
                        for (auto& s : sitesArr) addSite(s);
                } catch (...) {}
            }
        }
    } catch (...) {
        trailLog("loadConfig: parse failed");
    }
    trailLog("loadConfig: loaded " + std::to_string(result.size()) + " sites");
    return result;
}

// ---- 标准 tvbox 搜索：GET {api}?wd=关键词&ac=detail ----
std::vector<TvboxHit> searchSite(const TvboxSite& src, const std::string& keyword,
                                 HttpResponse* out) {
    std::vector<TvboxHit> result;
    std::string url = src.api;
    url += (url.find('?') == std::string::npos ? "?" : "&");
    url += "wd=" + urlEncode(keyword) + "&ac=detail";

    HttpResponse resp;
    httpGet(url, false, &resp);
    if (out) *out = resp;
    if (resp.body.empty()) return result;

    try {
        json j = json::parse(resp.body);
        if (!j.contains("list") || !j["list"].is_array()) return result;
        for (auto& it : j["list"]) {
            TvboxHit hit;
            hit.sourceKey  = src.key;
            hit.sourceName = src.name;
            hit.vodId      = std::to_string(it.value("vod_id", 0));
            hit.vodName    = it.value("vod_name", "");
            std::string pu = it.value("vod_play_url", "");
            hit.playUrl = parsePlayUrl(pu);
            if (!hit.vodName.empty() && !hit.playUrl.empty())
                result.push_back(std::move(hit));
        }
    } catch (...) {}
    return result;
}

// ---- 跨所有源搜索 ----
std::vector<TvboxHit> searchAllSources(const std::vector<TvboxSite>& sites,
                                       const std::string& keyword) {
    std::vector<TvboxHit> result;
    int limit = (int)sites.size() < 8 ? (int)sites.size() : 8;
    for (int i = 0; i < limit; ++i) {
        if (!sites[i].searchable) continue;
        auto hits = searchSite(sites[i], keyword);
        for (auto& h : hits) result.push_back(std::move(h));
    }
    return result;
}

}  // namespace decotv
