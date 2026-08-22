// DecoTV Switch 客户端 —— 网络层：纯 TVBox 客户端（用户自管播放源）
// 标准 tvbox 协议（json 接口）：GET {api}?wd=关键词&ac=detail（搜索 -> vod_play_url）
// 播放源来自用户 config.json（本地 sites + 远程 subscriptions 订阅）
#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace decotv {

// 播放源（标准 tvbox 订阅格式子集）
struct TvboxSite {
    std::string key;     // 源标识，如 "360zy"
    std::string name;    // 显示名，如 "TV-360资源"（搜索时会被改写为 "TV-360资源 (123ms)" 带延时）
    std::string api;     // 源接口基地址（标准 tvbox json 接口），如 https://360zy.com/api.php/provide/vod
    bool searchable = true;
    bool alive = true;       // 最近一次探测是否存活（接口可达且返回有效）
    int  latencyMs = -1;     // 最近一次探测延时（毫秒）；-1=未测/失败
};

// 视频条目（搜索/分类返回）。pic=海报，playUrl=直接可播地址（搜索即带），
// episodes 为分集（name/url 一一对应，详情页才有）。
struct VodItem {
    std::string sourceKey;
    std::string sourceName;
    std::string vodId;
    std::string vodName;
    std::string pic;       // 海报 URL
    std::string playUrl;   // 已解析的可播地址（搜索结果直接给）
    std::vector<std::string> episodeNames;
    std::vector<std::string> episodeUrls;  // 未解析时为 only playUrl
};

// HTTP 请求结果（用于界面排错显示）
struct HttpResponse {
    long status = 0;      // HTTP 状态码；0 = 未收到响应（传输失败）
    int curlError = 0;    // curl 错误码
    std::string body;
};

// 网络初始化：sdmc（日志目录）+ socket + curl 全局 + ssl。返回是否成功
bool initNetwork();
void exitNetwork();

// 最近一次网络初始化的详细结果码（用于界面排错显示）
extern unsigned int g_netInitResult;

// 加载用户源配置：sdmc:/switch/DecoTV/config.json（本地 sites + 远程订阅合并）
// 失败返回空（界面提示）。远程订阅拉取失败不阻断本地源。
std::vector<TvboxSite> loadConfig();

// 标准 tvbox 搜索：GET {api}?wd=关键词&ac=detail（ac=detail 才返回 vod_play_url）
std::vector<VodItem> searchSite(const TvboxSite& src, const std::string& keyword,
                                HttpResponse* out = nullptr);

// 跨所有源搜索（合并各源结果，最多取前 8 个源）。
// 注意：sites 以非 const 引用传入，函数内部会先对所有源做一次存活/延时探测
// （每次搜索都测，符合「每次搜索测速」需求），并把延时标注写回 site.name。
std::vector<VodItem> searchAllSources(std::vector<TvboxSite>& sites,
                                      const std::string& keyword);

// 对所有源做轻量存活+延时探测：GET {api}?ac=detail，读 CURLINFO_TOTAL_TIME。
// 并行探测（每个源一个线程），超时/非 200/解析失败 -> alive=false，latencyMs=-1。
// 探测后改写 site.name 为「原名 (123ms)」/「原名 (超时)」/「原名 (失败)」，
// 供侧栏/搜索结果直接显示。原 name 不含括号时才改写（避免重复累加）。
void probeSites(std::vector<TvboxSite>& sites);

// 豆瓣推荐（仿 TVBox 首页海报墙）：拉热门榜单，返回 VodItem（仅海报+片名，无直链，
// 点击时由界面拿片名去用户已配置源里搜）。失败返回空（界面显示降级提示）。
std::vector<VodItem> fetchDoubanRecommend();

// 源兜底：豆瓣不可达时，从用户已配置源拉 ac=videolist 热门列表当首页海报墙
// （tvbox 首页本质是源自身内容，比依赖豆瓣更稳、更贴合 TVBox 习惯）。失败返回空。
std::vector<VodItem> fetchSourceRecommend(const std::vector<TvboxSite>& sites);

// 详情：GET {api}?ac=detail&ids=<vodId> -> 解析 vod_play_url 为分集
VodItem fetchDetail(const TvboxSite& src, const std::string& vodId);

// vod_play_url 解析（优先真实媒体直链，避开 /share/、/play/ 页面地址）
std::string parsePlayUrl(const std::string& raw);

// 把 vod_play_url 拆成 name/url 分集列表（用于详情页选集）
void parseEpisodes(const std::string& raw, std::vector<std::string>& names,
                   std::vector<std::string>& urls);

// 崩溃轨迹日志：关键节点落 sdmc:/switch/DecoTV/trail.log，崩后看最后一行即知根因
void trailLog(const std::string& line);

// 内部 GET helper：结果写入 out；传输失败自动重试 2 次
void httpGet(const std::string& url, bool withCookies, HttpResponse* out);

// ---- 缓存管理（海报 cache/img + 视频缓存 cache/mpv，统一管、及时清）----
// 把海报 URL 下载到 cache/img（已存在则直接复用），返回本地路径；失败返回空。
std::string cacheImage(const std::string& url);
// 清空所有缓存（cache/img + cache/mpv），返回是否成功
bool clearCache();
// 缓存总大小（字节）
long cacheSizeBytes();

}  // namespace decotv
