// DecoTV Switch —— 播放核心实现（libmpv 软渲染，参考 wiliwili MPVCore 精简）
// 全部 mpv API 走官方头文件（静态链接 switch-libmpv_deko3d）
#include "mpv_player.hpp"

#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>  // brls::sync（跨线程投递到 UI 线程）

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// 把关键 mpv 错误追加写入 sdmc 日志，便于真机排错（v1.22）
// v1.28：加 256KB 上限，超了就覆盖重来，避免日志无限增长吃满 SD 卡
static void appendMpvLog(const std::string& line) {
    const char* path = "sdmc:/switch/DecoTV/mpv.log";
    const char* mode = "a";
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 256 * 1024) mode = "w";
    FILE* f = fopen(path, mode);
    if (f) {
        fputs(line.c_str(), f);
        fputc('\n', f);
        fclose(f);
    }
}

namespace mpv_player {

Player::~Player() {
    destroy();
}

bool Player::init() {
    if (m_inited) return true;

    m_mpv = mpv_create();
    if (!m_mpv) {
        brls::Logger::error("mpv: mpv_create failed");
        return false;
    }

    // 选项（参考 wiliwili：软解、无磁盘缓存、内存缓存、不读配置文件）
    mpv_set_option_string(m_mpv, "config", "no");
    mpv_set_option_string(m_mpv, "ytdl", "no");
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", "no");           // 软解（MPV_SW_RENDER 配套）
    mpv_set_option_string(m_mpv, "vd-lavc-dr", "no");
    mpv_set_option_string(m_mpv, "vd-lavc-threads", "4");  // 4 线程解码（Switch 4 核）
    mpv_set_option_string(m_mpv, "audio-channels", "stereo");
    // Switch audren 只接受有限的音频格式/采样率。部分 tvbox 源的音频流（非常规 48k/s16/stereo）
    // 会让 audren 初始化失败 (code -13)。这里在初始化阶段（早于 mpv_initialize）就把输出格式
    // 统一约束为 audren 友好值，使所有源都以兼容格式送交 ao，从源头消除 -13（v1.25）。
    // （参考 wiliwili：它只设 audio-channels=stereo 即可，因为其音源格式本就统一；tvbox 源更杂，故收紧。）
    mpv_set_option_string(m_mpv, "audio-samplerate", "48000");
    mpv_set_option_string(m_mpv, "audio-format", "s16");
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "64MiB");
    mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "32MiB");
    // 防盗链：很多源站按 UA 白名单校验，用浏览器 UA（v1.18）
    mpv_set_option_string(m_mpv, "user-agent",
                          "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                          "(KHTML, like Gecko) Chrome/120.0 Safari/537.36");
    // 注意：不在初始化时全局禁用音频。经验证本构建里 audio=no 不生效，且对能出声的源
    // 反而可能误杀声音。音频初始化失败 (code -13) 改为「静默降级」处理：
    // audio-fallback-to-null=yes 让 ao 初始化失败时自动退到 ao_null（无声），
    // 不再以 -13 中断整段播放（这才是 mpv 处理 -13 的正解，v1.22 重新启用）。
    // 若个别源仍走到 END_FILE -13，processEvents() 会按源重试一次兜底。
    mpv_set_option_string(m_mpv, "audio-fallback-to-null", "yes");

    if (mpv_initialize(m_mpv) < 0) {
        brls::Logger::error("mpv: mpv_initialize failed");
        destroy();
        return false;
    }

    // 软渲染上下文（CPU 渲染 RGBA 帧到我们的缓冲）
    mpv_render_param init_params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&m_rc, m_mpv, init_params) < 0) {
        brls::Logger::error("mpv: render context create failed");
        destroy();
        return false;
    }
    mpv_render_context_set_update_callback(m_rc, renderUpdateCallback, this);
    mpv_set_wakeup_callback(m_mpv, wakeupCallback, this);

    m_inited = true;
    brls::Logger::info("mpv: initialized (sw render)");
    return true;
}

void Player::open(const std::string& url) {
    if (!m_mpv || url.empty()) return;
    m_lastUrl       = url;   // v1.21：记录用于 -13 自动重试
    m_audioRetryStage = 0;    // v1.25：新源重置音频重试阶段
    // 防盗链：源站 m3u8 多数校验 Referer 域名，从 URL 提取源站域名设置（v1.18）
    size_t schemePos = url.find("://");
    if (schemePos != std::string::npos) {
        size_t hostStart = schemePos + 3;
        size_t hostEnd   = url.find('/', hostStart);
        if (hostEnd == std::string::npos) hostEnd = url.find('?', hostStart);
        if (hostEnd == std::string::npos) hostEnd = url.length();
        std::string scheme = url.substr(0, schemePos);
        std::string host   = url.substr(hostStart, hostEnd - hostStart);
        std::string ref    = "Referer: " + scheme + "://" + host + "/";
        mpv_set_option_string(m_mpv, "http-header-fields", ref.c_str());
        brls::Logger::info("mpv: referer set -> {}", ref);
    }
    brls::Logger::info("mpv: loading {}", url);
    appendMpvLog("OPEN url=" + url);   // v1.26：落盘实际喂给 mpv 的地址，定位 -17（格式不可识别）
    const char* cmd[] = {"loadfile", url.c_str(), nullptr};
    mpv_command_async(m_mpv, 0, cmd);
}

void Player::togglePause() {
    if (!m_mpv) return;
    const char* args[] = {"cycle", "pause", nullptr};
    mpv_command_async(m_mpv, 0, args);
}

void Player::stop() {
    if (!m_mpv) return;
    const char* args[] = {"stop", nullptr};
    mpv_command_async(m_mpv, 0, args);
}

void Player::destroy() {
    if (m_rc) {
        mpv_render_context_free(m_rc);
        m_rc = nullptr;
    }
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
    if (m_pixels) {
        free(m_pixels);
        m_pixels = nullptr;
    }
    m_swW = m_swH = m_stride = 0;
    m_inited                    = false;
}

void Player::resizeSurface(int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (m_pixels && w == m_swW && h == m_swH) return;
    if (m_pixels) free(m_pixels);
    m_pixels = malloc((size_t)w * h * 4);
    m_swW    = w;
    m_swH    = h;
    m_stride = w * 4;
    m_needRender = true;
    brls::Logger::debug("mpv: surface {}x{}", w, h);
}

bool Player::renderFrame() {
    if (!m_rc || !m_pixels) return false;
    // 只在 mpv 提示有新帧时渲染（省 CPU；无帧时画面保持）。
    // 首帧特殊：软渲染模式下视频解码由 render 调用驱动，初始没有 update 回调，
    // 必须无条件渲染一次让 mpv 开始输出帧（否则永远无画面，只有声音）
    bool dirty = m_needRender.exchange(false) || !m_everRendered;
    if (!dirty) return false;

    int size[2] = {m_swW, m_swH};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char*>("rgb0")},
        {MPV_RENDER_PARAM_SW_STRIDE, &m_stride},
        {MPV_RENDER_PARAM_SW_POINTER, m_pixels},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_render(m_rc, params) < 0) return false;
    mpv_render_context_report_swap(m_rc);
    m_everRendered = true;
    return true;
}

void Player::processEvents() {
    if (!m_mpv) return;
    for (;;) {
        mpv_event* ev = mpv_wait_event(m_mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;

        switch (ev->event_id) {
            case MPV_EVENT_LOG_MESSAGE: {
                // libmpv 的 MPV_LOG_LEVEL_* 是字符串宏（"error"/"fatal"...），用 strcmp 判断
                auto* log = static_cast<mpv_event_log_message*>(ev->data);
                std::string lvl(log->level);
                // 音频输出初始化失败（code -13）已通过 audio-fallback-to-null 静默降级为静音，
                // 其日志（含 "audio"/"AO"/"fallback"）非致命，不该弹窗盖住画面（v1.19）
                bool isAudioLog = (strstr(log->text, "audio") != nullptr ||
                                   strstr(log->text, "AO:") != nullptr ||
                                   strstr(log->text, "fallback") != nullptr);
                if (lvl == "fatal" && !isAudioLog) {
                    std::string msg = std::string(log->prefix) + ": " + log->text;
                    brls::Logger::error("mpv fatal: {}", msg);
                    if (m_cb) m_cb(Event::ERROR, 0, msg);
                } else if (lvl == "error" || lvl == "fatal") {
                    // error 级（含音频降级）仅记录，不弹窗；真正的致命加载失败由 END_FILE 上报
                    brls::Logger::error("mpv {}: {}", log->prefix, log->text);
                    appendMpvLog("MPVLOG " + std::string(log->prefix) + ": " + log->text);  // v1.26
                } else {
                    brls::Logger::debug("mpv {}: {}", log->prefix, log->text);
                }
                break;
            }
            case MPV_EVENT_FILE_LOADED:
                brls::Logger::info("mpv: file loaded");
                if (m_cb) m_cb(Event::LOADED, 0, "");
                break;
            case MPV_EVENT_END_FILE: {
                // 区分结束原因：EOF 正常结束；ERROR 是加载/播放失败（v1.18）
                auto* end = static_cast<mpv_event_end_file*>(ev->data);
                if (end->reason == MPV_END_FILE_REASON_ERROR) {
                    std::string err = mpv_error_string(end->error);
                    brls::Logger::error("mpv: end file ERROR (code {}) {}", end->error, err);
                    // 落盘记录真实 ao 错误，便于后续按源对症（v1.22）
                    appendMpvLog("END_FILE ERROR code=" + std::to_string(end->error) +
                                 " (" + err + ") url=" + m_lastUrl);
                    // 音频输出初始化失败 (code -13 = MPV_ERROR_AO_INIT_FAILED)：与具体源的音频流有关，
                    // 即便上面已全局约束 48k/s16/stereo，极少数源仍可能让 audren 初始化失败。
                    // 兜底：先切到 ao=null（静音但必然能初始化的空音频后端）重试一次，保证至少能播；
                    // 若仍失败再上报错误，避免误杀能播的源（v1.25）。
                    if (end->error == MPV_ERROR_AO_INIT_FAILED && m_audioRetryStage < 2 && !m_lastUrl.empty()) {
                        m_audioRetryStage++;
                        if (m_audioRetryStage == 1) {
                            mpv_set_property_string(m_mpv, "ao", "null");
                            brls::Logger::info("mpv: AO init failed on this source; "
                                               "fallback to ao=null (silent) and retry...");
                        } else {
                            // 第二次仍失败：放弃，交给下方错误弹窗
                            break;
                        }
                        const char* cmd[] = {"loadfile", m_lastUrl.c_str(), "replace", nullptr};
                        mpv_command_async(m_mpv, 0, cmd);
                        break;  // 不发错误弹窗，等重试结果
                    }
                    if (m_cb) m_cb(Event::ERROR, 0,
                                   "加载失败: " + err + " (code " + std::to_string(end->error) + ")");
                } else if (end->reason == MPV_END_FILE_REASON_EOF) {
                    brls::Logger::info("mpv: end file EOF");
                    if (m_cb) m_cb(Event::ENDED, 0, "");
                } else {
                    brls::Logger::info("mpv: end file reason={}", (int)end->reason);
                    if (m_cb) m_cb(Event::ENDED, 0, "播放停止");
                }
                break;
            }
            default:
                break;
        }
    }
}

void Player::wakeupCallback(void* ctx) {
    // mpv 内部线程触发：投递到 UI 主线程处理事件
    Player* p = static_cast<Player*>(ctx);
    if (p && p->m_mpv) {
        brls::sync([p]() { p->processEvents(); });
    }
}

void Player::renderUpdateCallback(void* ctx) {
    static_cast<Player*>(ctx)->m_needRender = true;
}

}  // namespace mpv_player
