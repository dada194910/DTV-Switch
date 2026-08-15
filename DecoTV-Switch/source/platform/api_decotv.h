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
// Cookie 持久化路径（sdmc 需要 sdmcInit）
constexpr const char* COOKIE_PATH = "sdmc:/switch/DecoTV/cookies.txt";

struct Category {
    std::string key;    // 如 "movie"
    std::string label;  // 如 "电影"
};

// 网络初始化：sdmc（cookie 持久化）+ socket + curl 全局。返回是否成功
bool initNetwork();
void exitNetwork();

// 是否已存在登录 cookie 文件（下次启动可静默复用）
bool hasSavedLogin();

// 登录：POST /api/login → 下发 auth cookie 持久化到 COOKIE_PATH
bool login(const std::string& username, const std::string& password);

// GET /api/categories（带 cookie）→ 分类列表
std::vector<Category> fetchCategories();

// 内部 GET helper：返回响应 body（空串表示失败）
std::string httpGet(const std::string& url, bool withCookies);

}  // namespace decotv
