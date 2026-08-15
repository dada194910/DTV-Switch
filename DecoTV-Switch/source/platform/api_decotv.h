// DecoTV Switch 客户端 —— 网络层：DecoTV LunaTV API 封装
// 使用 libcurl（switch-curl 包，无外部 SSL，HTTP 直连）+ nlohmann/json
#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace decotv {

// 服务器地址（写死，评审已确认）
constexpr const char* BASE_URL = "http://tv.2001002.xyz:11113";
// 登录凭据（写死：用户 docker-compose 中的账号）
constexpr const char* LOGIN_USER = "admin";
constexpr const char* LOGIN_PASS = "hjf@82611";
// Cookie 持久化路径（sdmc 需要 fsdevMountSdmc）
constexpr const char* COOKIE_PATH = "sdmc:/switch/DecoTV/cookies.txt";

// 分类主筛选标签（如 movie 的 全部/热门电影/最新电影...）
struct PrimaryTab {
    std::string label;  // 显示名，如 "热门电影"
    std::string value;  // 接口值，如 "热门"
};

struct Category {
    std::string key;                 // 如 "movie"
    std::string label;               // 如 "电影"
    std::vector<PrimaryTab> primary; // 主筛选标签
};

// HTTP 请求结果（用于界面排错显示）
struct HttpResponse {
    long status = 0;      // HTTP 状态码；0 = 未收到响应（传输失败）
    int curlError = 0;    // curl 错误码（CURLE_OK=0 无错误；6=DNS 失败 7=连接拒绝 28=超时 56=接收错误）
    std::string body;
};

// 影片列表项（豆瓣驱动，字段：标题/年份/评分/海报/豆瓣id）
struct VideoItem {
    std::string id;
    std::string title;
    std::string poster;
    std::string rate;
    std::string year;
};

// TVBox 播放源（GET /api/search/resources 返回；api 为源站提供接口，多为 https）
struct TvboxSource {
    std::string key;   // 源标识，如 "360zy"
    std::string name;  // 显示名，如 "TV-360资源"
    std::string api;   // 源 API 地址，如 https://360zy.com/api.php/provide/vod
};

// TVBox 源搜索结果（直调源 API：GET {api}?wd=关键词&ac=detail）
struct TvboxHit {
    std::string sourceKey;   // 命中源 key
    std::string sourceName;  // 命中源显示名
    std::string vodId;       // 资源站视频 id（/api/detail 需要，P4b 用）
    std::string vodName;     // 资源站片名（可能含年份）
    std::string playUrl;     // 第一个播放地址（m3u8/mp4）
};

// 网络初始化：sdmc（cookie 持久化）+ socket + curl 全局。返回是否成功
// 注意：borealis 的 userAppInit 已初始化过 socket，二次调用会返回
//       AlreadyInitialized——此时视为成功（socket 已就绪）
bool initNetwork();
void exitNetwork();

// 最近一次网络初始化的详细结果码（用于界面排错显示）
extern unsigned int g_netInitResult;

// 是否已存在登录 cookie 文件（下次启动可静默复用）
bool hasSavedLogin();

// 登录：POST /api/login → 下发 auth cookie 持久化到 COOKIE_PATH
// *out 可空：传出登录请求的 HTTP 结果（排错用）
bool login(const std::string& username, const std::string& password, HttpResponse* out = nullptr);

// GET /api/categories（带 cookie）→ 分类树；*out 可空：传出请求结果（排错用）
std::vector<Category> fetchCategories(HttpResponse* out = nullptr);

// GET /api/douban/categories（豆瓣驱动影片列表）
// kind=movie/tv；category=热门/豆瓣高分/最新/冷门佳片/tv/show；type=全部/tv/show
std::vector<VideoItem> fetchDoubanList(const std::string& kind, const std::string& category,
                                       const std::string& type, int limit, int start,
                                       HttpResponse* out = nullptr);

// GET /api/search/resources（带 cookie）→ 全部 TVBox 播放源
std::vector<TvboxSource> fetchTvboxSources(HttpResponse* out = nullptr);

// 直调 TVBox 源 API 搜索（https）：GET {api}?wd={keyword}&ac=detail
// 一次拿到 资源站id + 播放地址（m3u8/mp4）。PlayUrl 为空 = 该源未收录或无地址
std::vector<TvboxHit> searchTvboxSource(const TvboxSource& src, const std::string& keyword,
                                        HttpResponse* out = nullptr);

// 内部 GET helper：结果写入 out（body/status/curlError）；传输失败自动重试 3 次
void httpGet(const std::string& url, bool withCookies, HttpResponse* out);

}  // namespace decotv
