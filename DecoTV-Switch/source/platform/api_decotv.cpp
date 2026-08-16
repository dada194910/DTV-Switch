// DecoTV Switch 客户端 —— 网络层实现（纯 TVBox 客户端）
// 标准 tvbox 协议：GET {api}?wd=关键词&ac=detail 搜索，返回 vod_play_url 由 parsePlayUrl 解析。
#include <switch.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <borealis/core/logger.hpp>

#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <ctime>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <functional>

#include "api_decotv.h"

namespace decotv {

using json = nlohmann::json;

unsigned int g_netInitResult = 0;

// 缓存目录（MPV_DIR 在 exitNetwork 中用到，故前置声明/定义于此）
static const char* CACHE_DIR = "sdmc:/switch/DecoTV/cache";
static const char* IMG_DIR   = "sdmc:/switch/DecoTV/cache/img";
static const char* MPV_DIR   = "sdmc:/switch/DecoTV/cache/mpv";

// 前向声明：下面 exitNetwork 用到，定义在文件末尾缓存段
static void removeDirFiles(const char* dir);

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
std::string parsePlayUrl(const std::string& raw) {
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
    // 退出时清理视频缓存：mpv 磁盘缓存片段留在 SD 卡会无限堆积（Switch 不自动清），
    // 这里在进程结束前清掉 cache/mpv/*，避免占满卡（v2.00）。cache/img 海报保留（可复用）。
    removeDirFiles(MPV_DIR);
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
std::vector<VodItem> searchSite(const TvboxSite& src, const std::string& keyword,
                               HttpResponse* out) {
    std::vector<VodItem> result;
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
            VodItem item;
            item.sourceKey  = src.key;
            item.sourceName = src.name;
            item.vodId      = std::to_string(it.value("vod_id", 0));
            item.vodName    = it.value("vod_name", "");
            item.pic        = it.value("vod_pic", "");
            std::string pu  = it.value("vod_play_url", "");
            item.playUrl    = parsePlayUrl(pu);
            if (!item.vodName.empty() && !item.playUrl.empty())
                result.push_back(std::move(item));
        }
    } catch (...) {}
    return result;
}

// ---- 跨所有源搜索 ----
std::vector<VodItem> searchAllSources(const std::vector<TvboxSite>& sites,
                                      const std::string& keyword) {
    std::vector<VodItem> result;
    int limit = (int)sites.size() < 8 ? (int)sites.size() : 8;
    for (int i = 0; i < limit; ++i) {
        if (!sites[i].searchable) continue;
        auto hits = searchSite(sites[i], keyword);
        for (auto& h : hits) result.push_back(std::move(h));
    }
    return result;
}

// ---- 豆瓣推荐（仿 TVBox：首页用豆瓣热门榜单）----
// 豆瓣对 UA/Referer 有反爬，这里用浏览器 UA + Referer + 自动解压，超时也收紧避免卡首页
static void doubanHttpGet(const std::string& url, HttpResponse* out) {
    std::string body;
    CURL* curl = makeHandle(&body);
    if (!curl) { if (out) *out = HttpResponse(); return; }
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 6L);          // 首页只给 6s，失败立即转源兜底
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);
    struct curl_slist* hdr = nullptr;
    hdr = curl_slist_append(hdr, "Referer: https://movie.douban.com/");
    hdr = curl_slist_append(hdr, "Accept: application/json, text/plain, */*");
    hdr = curl_slist_append(hdr, "Cookie: bid=decotv1234");  // 无 cookie 时部分环境会被拦
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // 自动 gzip/deflate 解压
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    perform(curl, out);                 // 首页只试 1 次，失败立即转源兜底（避免卡太久）
    curl_slist_free_all(hdr);
    curl_easy_cleanup(curl);
    if (out) out->body = body;
}

// 拉取豆瓣「热门」榜单，合并成首页海报墙（上限 30 张）。失败/空返回空 vector。
std::vector<VodItem> fetchDoubanRecommend() {
    std::vector<VodItem> out;
    const char* tag = "热门";
    std::string u = "https://movie.douban.com/j/search_subjects?type=movie&tag="
                  + urlEncode(tag) + "&sort=recommend&page_limit=24&page_start=0";
    HttpResponse resp;
    doubanHttpGet(u, &resp);
    if (resp.status != 200 || resp.body.empty()) {
        trailLog("douban: status=" + std::to_string(resp.status) +
                 " curl=" + std::to_string(resp.curlError));
        return out;
    }
    try {
        json j = json::parse(resp.body, nullptr, false);
        if (!j.contains("subjects") || !j["subjects"].is_array()) return out;
        for (auto& s : j["subjects"]) {
            VodItem it;
            it.sourceKey  = "douban";
            it.sourceName = "豆瓣·热门";
            it.vodName    = s.value("title", "");
            it.vodId      = s.value("id", "");
            it.pic        = s.value("cover", "");
            it.playUrl    = "";   // 豆瓣无直链，点击转去源里搜片名
            if (!it.vodName.empty()) out.push_back(std::move(it));
            if (out.size() >= 30) break;
        }
    } catch (...) {}
    trailLog("douban: got " + std::to_string(out.size()) + " items");
    return out;
}

// 兜底：豆瓣不可达时，从用户已配置源拉 ac=list 热门列表当首页海报墙
// （tvbox 首页本质是源自身内容，比依赖豆瓣更稳、也更贴合 TVBox 习惯）
std::vector<VodItem> fetchSourceRecommend(const std::vector<TvboxSite>& sites) {
    std::vector<VodItem> out;
    int n = std::min((int)sites.size(), 2);   // 最多取 2 个源凑满一屏，控制首页加载耗时
    for (int i = 0; i < n && (int)out.size() < 30; ++i) {
        auto& s = sites[i];
        std::string url = s.api;
        url += (url.find('?') == std::string::npos ? "?" : "&");
        url += "ac=videolist&pg=1";   // videolist 才带 vod_pic 海报；ac=list 不带
        std::string body;
        CURL* c = makeHandle(&body);
        if (!c) continue;
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 6L);   // 首页每源最多 6s
        HttpResponse resp;
        perform(c, &resp);
        curl_easy_cleanup(c);
        resp.body = body;
        if (resp.body.empty()) {
            trailLog("srcRec: " + s.key + " empty (http=" + std::to_string(resp.status) + ")");
            continue;
        }
        try {
            json j = json::parse(resp.body);
            if (!j.contains("list") || !j["list"].is_array()) {
                trailLog("srcRec: " + s.key + " no list field");
                continue;
            }
            for (auto& it : j["list"]) {
                VodItem item;
                item.sourceKey = s.key;
                item.sourceName = s.name;
                item.vodId = std::to_string(it.value("vod_id", 0));
                item.vodName = it.value("vod_name", "");
                item.pic = it.value("vod_pic", "");
                if (item.vodName.empty() || item.pic.empty()) continue;
                std::string pu = it.value("vod_play_url", "");
                item.playUrl = parsePlayUrl(pu);
                out.push_back(std::move(item));
                if ((int)out.size() >= 30) break;
            }
        } catch (...) {
            trailLog("srcRec: " + s.key + " parse err");
        }
        if ((int)out.size() >= 12) break;   // 已够一屏，少等后续源
    }
    trailLog("srcRec: got " + std::to_string(out.size()) + " items");
    return out;
}

// ---- 详情：GET {api}?ac=detail&ids=<vodId> -> 解析分集 ----
VodItem fetchDetail(const TvboxSite& src, const std::string& vodId) {
    VodItem item;
    item.sourceKey = src.key;
    item.sourceName = src.name;
    item.vodId = vodId;

    std::string url = src.api;
    url += (url.find('?') == std::string::npos ? "?" : "&");
    url += "ac=detail&ids=" + urlEncode(vodId);

    HttpResponse resp;
    httpGet(url, false, &resp);
    if (resp.body.empty()) return item;

    try {
        json j = json::parse(resp.body);
        if (!j.contains("list") || !j["list"].is_array() || j["list"].empty())
            return item;
        auto& it = j["list"][0];
        item.vodName = it.value("vod_name", "");
        item.pic     = it.value("vod_pic", "");
        std::string pu = it.value("vod_play_url", "");
        item.playUrl  = parsePlayUrl(pu);
        parseEpisodes(pu, item.episodeNames, item.episodeUrls);
    } catch (...) {}
    return item;
}

// ---- 把 vod_play_url 拆成 name/url 分集 ----
void parseEpisodes(const std::string& raw, std::vector<std::string>& names,
                   std::vector<std::string>& urls) {
    names.clear();
    urls.clear();
    if (raw.empty()) return;
    // 先按 $$$ 拆块（不同清晰度/直链块），每块取第一集即可避免重复
    std::vector<std::string> blocks;
    size_t pos = 0;
    while (true) {
        size_t d = raw.find("$$$", pos);
        if (d == std::string::npos) { blocks.push_back(raw.substr(pos)); break; }
        blocks.push_back(raw.substr(pos, d - pos));
        pos = d + 3;
    }
    for (auto& blk : blocks) {
        size_t h = blk.find('#');
        std::string eps = (h != std::string::npos) ? blk.substr(0, h) : blk;
        size_t p = 0;
        while (true) {
            size_t nxt = eps.find('#', p);
            std::string ep = (nxt == std::string::npos) ? eps.substr(p)
                                                       : eps.substr(p, nxt - p);
            if (!ep.empty()) {
                size_t d = ep.find('$');
                std::string name = (d != std::string::npos) ? ep.substr(0, d) : ep;
                std::string url  = (d != std::string::npos) ? ep.substr(d + 1) : ep;
                if (!url.empty()) {
                    names.push_back(name);
                    urls.push_back(url);
                }
            }
            if (nxt == std::string::npos) break;
            p = nxt + 1;
        }
    }
}

// ---- 缓存管理 ----
// CACHE_DIR / IMG_DIR / MPV_DIR 已在文件顶部定义（供 exitNetwork 使用）

// djb2 哈希 -> 16 进制串，用作缓存文件名
static std::string hashHex(const std::string& s) {
    unsigned long hash = 5381;
    for (unsigned char c : s) hash = ((hash << 5) + hash) + c;
    char buf[32];
    snprintf(buf, sizeof(buf), "%08lX", hash);
    return std::string(buf);
}

static std::string extFromUrl(const std::string& url) {
    size_t q = url.find('?');
    std::string s = (q != std::string::npos) ? url.substr(0, q) : url;
    size_t dot = s.find_last_of('.');
    if (dot == std::string::npos) return ".jpg";
    std::string e = s.substr(dot);  // 含点，如 .jpg
    if (e == ".jpg" || e == ".jpeg" || e == ".png" || e == ".webp" ||
        e == ".gif" || e == ".bmp" || e == ".tga")
        return e;
    return ".jpg";
}

static bool fileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

// 把海报 URL 下载到 cache/img（已存在则复用），返回本地路径；失败返回空
std::string cacheImage(const std::string& url) {
    if (url.empty()) return "";
    std::string path = std::string(IMG_DIR) + "/" + hashHex(url) + extFromUrl(url);
    if (fileExists(path)) return path;

    HttpResponse resp;
    httpGet(url, false, &resp);
    if (resp.body.empty()) return "";

    mkdir(CACHE_DIR, 0777);
    mkdir(IMG_DIR, 0777);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return "";
    fwrite(resp.body.data(), 1, resp.body.size(), f);
    fclose(f);
    return path;
}

// 删除目录下所有文件（不递归子目录，安全）
static void removeDirFiles(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string p = std::string(dir) + "/" + e->d_name;
        unlink(p.c_str());
    }
    closedir(d);
}

static long dirSizeBytes(const char* dir) {
    long total = 0;
    DIR* d = opendir(dir);
    if (!d) return 0;
    struct dirent* e;
    struct stat st;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        std::string p = std::string(dir) + "/" + e->d_name;
        if (stat(p.c_str(), &st) == 0) total += st.st_size;
    }
    closedir(d);
    return total;
}

bool clearCache() {
    removeDirFiles(IMG_DIR);
    removeDirFiles(MPV_DIR);
    return true;
}

long cacheSizeBytes() {
    return dirSizeBytes(IMG_DIR) + dirSizeBytes(MPV_DIR);
}

}  // namespace decotv
