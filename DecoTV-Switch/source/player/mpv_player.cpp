// DecoTV Switch —— 播放核心实现（libmpv 软渲染，参考 wiliwili MPVCore 精简）
// 全部 mpv API 走官方头文件（静态链接 switch-libmpv_deko3d）
#include "mpv_player.hpp"

#include <borealis/core/logger.hpp>

#include <cstdlib>
#include <cstring>

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
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "demuxer-max-bytes", "64MiB");
    mpv_set_option_string(m_mpv, "demuxer-max-back-bytes", "32MiB");

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
    brls::Logger::info("mpv: loading {}", url);
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
    // 只在 mpv 提示有新帧时渲染（省 CPU；无帧时画面保持）
    if (!m_needRender.exchange(false)) return false;

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
    return true;
}

void Player::processEvents() {
    if (!m_mpv) return;
    for (;;) {
        mpv_event* ev = mpv_wait_event(m_mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE) break;

        switch (ev->event_id) {
            case MPV_EVENT_LOG_MESSAGE: {
                auto* log = static_cast<mpv_event_log_message*>(ev->data);
                if (log->level <= MPV_LOG_LEVEL_ERROR) {
                    if (m_cb) m_cb(Event::ERROR, 0, std::string(log->prefix) + ": " + log->text);
                    else brls::Logger::error("mpv {}: {}", log->prefix, log->text);
                } else if (log->level <= MPV_LOG_LEVEL_WARN) {
                    brls::Logger::warning("mpv {}: {}", log->prefix, log->text);
                } else if (log->level <= MPV_LOG_LEVEL_INFO) {
                    brls::Logger::info("mpv {}: {}", log->prefix, log->text);
                }
                break;
            }
            case MPV_EVENT_FILE_LOADED:
                brls::Logger::info("mpv: file loaded");
                if (m_cb) m_cb(Event::LOADED, 0, "");
                break;
            case MPV_EVENT_END_FILE:
                brls::Logger::info("mpv: end file");
                if (m_cb) m_cb(Event::ENDED, 0, "");
                break;
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
