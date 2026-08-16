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
    std::string name;    // 显示名，如 "TV-360资源"
    std::string api;     // 源接口基地址（标准 tvbox json 接口），如 https://360zy.com/api.php/provide/vod
    bool searchable = true;
};

// 源搜索结果（标准 tvbox：wd=关键词&ac=detail 返回 list，每个含 vod_play_url）
struct TvboxHit {
    std::string sourceKey;   // 命中源 key
    std::string sourceName;  // 命中源显示名
    std::string vodId;       // 资源站视频 id
    std::string vodName;     // 资源站片名
    std::string playUrl;     // 解析后的播放地址（m3u8/mp4）
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
std::vector<TvboxHit> searchSite(const TvboxSite& src, const std::string& keyword,
                                 HttpResponse* out = nullptr);

// 跨所有源搜索（合并各源结果，最多取前 8 个源）
std::vector<TvboxHit> searchAllSources(const std::vector<TvboxSite>& sites,
                                       const std::string& keyword);

// vod_play_url 解析（优先真实媒体直链，避开 /share/、/play/ 页面地址）
std::string parsePlayUrl(const std::string& raw);

// 崩溃轨迹日志：关键节点落 sdmc:/switch/DecoTV/trail.log，崩后看最后一行即知根因
void trailLog(const std::string& line);

// 内部 GET helper：结果写入 out；传输失败自动重试 2 次
void httpGet(const std::string& url, bool withCookies, HttpResponse* out);

}  // namespace decotv
