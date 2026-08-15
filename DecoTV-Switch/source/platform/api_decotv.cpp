// DecoTV Switch 客户端 —— 网络层实现
// 实测接口（2026-08-15 沙箱 curl 验证）：
//   POST /api/login  {"username","password"}  ->  {"ok":true} + Set-Cookie: auth=...
//   GET  /api/categories（带 cookie）         ->  {"version":1,"categories":[{key,label,primary,secondary,filters}]}
#include <switch.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <sys/stat.h>
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

// ---- 创建 curl easy handle（统一超时/写回调）----
static CURL* makeHandle(std::string* outBody, bool withCookies) {
    CURL* curl = curl_easy_init();
    if (!curl) return nullptr;

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, outBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);       // 15s 超时，防卡死
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "DecoTV-Switch/1.0");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    if (withCookies) {
        // 载入已有 cookie（登录后复用）
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, COOKIE_PATH);
    }
    return curl;
}

bool initNetwork() {
    // sdmc 挂载（cookie 持久化需要；libnx 4.x 用 fsdevMountSdmc，无对应 exit，进程结束自动清理）
    fsdevMountSdmc();

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

std::string httpGet(const std::string& url, bool withCookies) {
    std::string body;
    CURL* curl = makeHandle(&body, withCookies);
    if (!curl) return "";

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return "";
    return body;
}

bool login(const std::string& username, const std::string& password) {
    // 确保 cookie 目录存在
    mkdir("sdmc:/switch/DecoTV", 0777);

    json reqBody;
    reqBody["username"] = username;
    reqBody["password"] = password;

    std::string body;
    CURL* curl = makeHandle(&body, false);
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
    // 登录成功后把 Set-Cookie 存盘
    curl_easy_setopt(curl, CURLOPT_COOKIEJAR, COOKIE_PATH);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return false;

    // 解析 {"ok":true}
    try {
        json resp = json::parse(body);
        if (resp.value("ok", false)) return true;
    } catch (...) {
        return false;
    }
    return false;
}

std::vector<Category> fetchCategories() {
    std::vector<Category> out;
    std::string body = httpGet(std::string(BASE_URL) + "/api/categories", true);
    if (body.empty()) return out;

    try {
        json resp = json::parse(body);
        if (!resp.contains("categories") || !resp["categories"].is_array()) return out;
        for (auto& c : resp["categories"]) {
            Category cat;
            cat.key   = c.value("key", "");
            cat.label = c.value("label", "");
            if (!cat.key.empty()) out.push_back(std::move(cat));
        }
    } catch (...) {
        return out;
    }
    return out;
}

}  // namespace decotv
