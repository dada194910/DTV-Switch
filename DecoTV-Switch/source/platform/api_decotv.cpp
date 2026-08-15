// DecoTV Switch 客户端 —— 网络层实现
// 实测接口（2026-08-15 沙箱 curl 验证）：
//   POST /api/login  {"username","password"}  ->  {"ok":true} + Set-Cookie: auth=...
//   GET  /api/categories（带 cookie）         ->  {"version":1,"categories":[{key,label,...}]}
// 无 cookie 请求 /api/categories -> HTTP 401（空响应）
#include <switch.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <borealis/core/logger.hpp>

#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>

#include "api_decotv.h"

namespace decotv {

using json = nlohmann::json;

unsigned int g_netInitResult = 0;

// ---- curl 写回调：把响应体追加进 std::string ----
static size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    ((std::string*)userp)->append((char*)contents, total);
    return total;
}

// ---- 创建 curl easy handle ----
// cookie 采用 libcurl 推荐模式：COOKIEFILE + COOKIEJAR 指向同一文件，
// 每个请求都"读已有 cookie + 把新 Set-Cookie 写回"，保证登录态跨请求持久
static CURL* makeHandle(std::string* outBody) {
    CURL* curl = curl_easy_init();
    if (!curl) return nullptr;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, outBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);       // 30s 总超时
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DecoTV-Switch/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    // 读 + 写同一个 cookie jar（关键：登录后的 auth cookie 靠它在后续请求带上）
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, COOKIE_PATH);
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, COOKIE_PATH);

    return curl;
}

bool initNetwork() {
    // sdmc 挂载（cookie 持久化需要；libnx 4.x 用 fsdevMountSdmc，无对应 exit，进程结束自动清理）
    fsdevMountSdmc();
    mkdir("sdmc:/switch/DecoTV", 0777);  // cookie 目录（幂等）

    // socket：borealis 的 userAppInit 已初始化（switch_wrapper.c），二次调用会返回
    // AlreadyInitialized，此时 socket 已就绪，视为成功
    Result rc = socketInitializeDefault();
    g_netInitResult = rc;
    if (R_FAILED(rc) && rc != MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized))
        return false;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    return true;
}

void exitNetwork() {
    curl_global_cleanup();
    // socketExit 由 borealis 的 userAppExit 负责，这里不重复调用
}

bool hasSavedLogin() {
    struct stat st;
    return stat(COOKIE_PATH, &st) == 0;
}

// ---- 执行一次请求并填 HttpResponse；返回 curl 是否成功 ----
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

std::string httpGet(const std::string& url, bool withCookies, long* outStatus) {
    HttpResponse resp;
    std::string body;
    CURL* curl = makeHandle(&body);
    if (!curl) return "";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

    // 传输失败时重试一次（Switch 端偶发 DNS/连接失败，重试常可恢复）
    bool ok = perform(curl, &resp);
    if (!ok) {
        sleep(1);
        body.clear();  // 清掉第一次可能残留的部分响应
        brls::Logger::info("decotv: GET {} failed (curl err={}), retrying...", url, resp.curlError);
        ok = perform(curl, &resp);
    }
    curl_easy_cleanup(curl);

    if (outStatus) *outStatus = resp.status;

    brls::Logger::info("decotv: GET {} -> HTTP {} (curl={}, {} bytes): {}",
                       url, resp.status, resp.curlError, body.size(), body.substr(0, 120));
    if (!ok) return "";
    return body;
}

bool login(const std::string& username, const std::string& password, HttpResponse* out) {
    json reqBody;
    reqBody["username"] = username;
    reqBody["password"] = password;

    std::string body;
    CURL* curl = makeHandle(&body);
    if (!curl) return false;

    std::string postData = reqBody.dump();
    curl_easy_setopt(curl, CURLOPT_URL, (std::string(BASE_URL) + "/api/login").c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)postData.size());
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    HttpResponse resp;
    bool ok = perform(curl, &resp);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);  // 在此把 Set-Cookie 写入 COOKIE_PATH

    if (out) *out = resp;

    brls::Logger::info("decotv: login -> HTTP {} (curl={}, {} bytes): {}",
                       resp.status, resp.curlError, body.size(), body.substr(0, 80));
    if (!ok) return false;

    // 解析 {"ok":true}
    try {
        json j = json::parse(body);
        if (j.value("ok", false)) return true;
    } catch (...) {
        return false;
    }
    return false;
}

std::vector<Category> fetchCategories(HttpResponse* out) {
    std::vector<Category> result;
    HttpResponse resp;
    std::string body = httpGet(std::string(BASE_URL) + "/api/categories", true, &resp.status);
    resp.body = body;
    if (out) *out = resp;
    if (body.empty()) return result;

    try {
        json j = json::parse(body);
        if (!j.contains("categories") || !j["categories"].is_array()) return result;
        for (auto& c : j["categories"]) {
            Category cat;
            cat.key   = c.value("key", "");
            cat.label = c.value("label", "");
            if (!cat.key.empty()) result.push_back(std::move(cat));
        }
    } catch (...) {
        return result;
    }
    return result;
}

}  // namespace decotv
