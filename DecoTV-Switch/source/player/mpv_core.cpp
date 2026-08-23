// DecoTV 播放核心实现（deko3d 硬件渲染）
// 移植自 wiliwili xfangfang fork 的 MPVCore，去除 B 站专属依赖，保留 deko3d 渲染主线。
#include "mpv_core.hpp"

#include <clocale>
#include <cmath>

#include <borealis/core/thread.hpp>
#include <borealis/core/application.hpp>
#include <borealis/platforms/switch/switch_video.hpp>  // brls::SwitchVideoContext (deko3d 设备/帧缓冲)

static inline void check_error(int status) {
    if (status < 0) {
        brls::Logger::error("MPV ERROR ====> {}", mpvErrorString(status));
    }
}

// ---- 单例访问 ----
MPVCore& core() { return MPVCore::instance(); }

void MPVCore::init() {
    if (m_inited) return;
    setlocale(LC_NUMERIC, "C");

    m_mpv = mpvCreate();
    if (!m_mpv) {
        brls::fatal("Error Create mpv Handle");
    }

    // misc：不写配置文件、关 yt-dl、立体声、idle 常驻
    mpvSetOptionString(m_mpv, "config", "no");
    mpvSetOptionString(m_mpv, "ytdl", "no");
    mpvSetOptionString(m_mpv, "audio-channels", "stereo");
    mpvSetOptionString(m_mpv, "idle", "yes");
    mpvSetOptionString(m_mpv, "osd-level", "0");
    mpvSetOptionString(m_mpv, "vo", "libmpv");
    mpvSetOptionString(m_mpv, "keep-open", "yes");
    mpvSetOptionString(m_mpv, "video-timing-offset", "0");

    // Switch 专用：4 线程解码，dr 关闭（避免随机崩溃）
#ifdef __SWITCH__
    mpvSetOptionString(m_mpv, "vd-lavc-dr", "no");
    mpvSetOptionString(m_mpv, "vd-lavc-threads", "4");
    mpvSetOptionString(m_mpv, "opengl-glfinish", "yes");
#endif

    if (mpvInitialize(m_mpv) < 0) {
        mpvTerminateDestroy(m_mpv);
        brls::fatal("Could not initialize mpv context");
    }

    // 监听关键属性
    check_error(mpvObserveProperty(m_mpv, 1, "core-idle", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(m_mpv, 2, "eof-reached", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(m_mpv, 3, "duration", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(m_mpv, 4, "playback-time", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(m_mpv, 12, "pause", MPV_FORMAT_FLAG));

    // 渲染上下文（deko3d 硬件渲染）
#ifdef BOREALIS_USE_DEKO3D
    int advanced_control{1};
    auto* switchPlatform = (brls::SwitchVideoContext*)brls::Application::getPlatform()->getVideoContext();
    mpv_deko3d_init_params deko_init_params{switchPlatform->getDeko3dDevice()};
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_DEKO3D)},
        {MPV_RENDER_PARAM_DEKO3D_INIT_PARAMS, &deko_init_params},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpvRenderContextCreate(&m_ctx, m_mpv, params) < 0) {
        mpvTerminateDestroy(m_mpv);
        brls::fatal("failed to initialize mpv deko3d context");
    }
#else
    // 非 deko3d 环境（如 PC 调试）退回 OpenGL，保证可编译
    int advanced_control{1};
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpvRenderContextCreate(&m_ctx, m_mpv, params) < 0) {
        mpvTerminateDestroy(m_mpv);
        brls::fatal("failed to initialize mpv render context");
    }
#endif

    mpvSetWakeupCallback(m_mpv, on_wakeup, this);
    mpvRenderContextSetUpdateCallback(m_ctx, on_update, this);

    m_inited = true;
    brls::Logger::info("DecoTV MPVCore initialized");
}

void MPVCore::setUrl(const std::string& url) {
    brls::Logger::debug("DecoTV load url: {}", url);
    command_async("loadfile", url, "replace");
    m_stopped = false;
}

void MPVCore::play() {
    command_async("set", "pause", "no");
    m_playing = true;
}

void MPVCore::pause() {
    command_async("set", "pause", "yes");
    m_playing = false;
}

void MPVCore::togglePause() {
    if (m_playing)
        pause();
    else
        play();
}

void MPVCore::stop() {
    command_async("stop");
    m_stopped = true;
    m_playing = false;
}

void MPVCore::draw() {
#ifdef BOREALIS_USE_DEKO3D
    if (!m_ctx) return;
    static auto* videoContext = (brls::SwitchVideoContext*)brls::Application::getPlatform()->getVideoContext();
    m_fbo.tex = videoContext->getFramebuffer();
    videoContext->queueSignalFence(&m_readyFence);
    videoContext->queueFlush();
    mpvRenderContextRender(m_ctx, m_params);
    videoContext->queueWaitFence(&m_doneFence);
    mpvRenderContextReportSwap(m_ctx);
#endif
}

void MPVCore::on_wakeup(void* self) {
    brls::sync([]() { MPVCore::instance().eventMainLoop(); });
}

void MPVCore::on_update(void* self) {
    brls::sync([]() {
        // deko3d 模式下每帧由 View::draw() 主动渲染，这里无需额外操作
        (void)mpvRenderContextUpdate(MPVCore::instance().m_ctx);
    });
}

void MPVCore::eventMainLoop() {
    if (!m_mpv) return;
    while (true) {
        auto event = mpvWaitEvent(m_mpv, 0);
        switch (event->event_id) {
            case MPV_EVENT_NONE:
                return;
            case MPV_EVENT_FILE_LOADED:
                brls::Logger::info("========> MPV_EVENT_FILE_LOADED");
                mpvCoreEventFire(MPVCore::LOADED);
                command_async("playlist-clear");
                play();
                break;
            case MPV_EVENT_START_FILE:
                brls::Logger::info("========> MPV_EVENT_START_FILE");
                break;
            case MPV_EVENT_PLAYBACK_RESTART:
                brls::Logger::info("========> MPV_EVENT_PLAYBACK_RESTART");
                m_stopped = false;
                break;
            case MPV_EVENT_END_FILE: {
                brls::Logger::info("========> MPV_STOP");
                m_playing = false;
                m_stopped = true;
                auto* node = (mpv_event_end_file*)event->data;
                if (node->reason == MPV_END_FILE_REASON_ERROR) {
                    m_errCode = node->error;
                    brls::Logger::error("========> MPV ERROR: {}", mpvErrorString(node->error));
                    mpvCoreEventFire(MPVCore::ERROR, mpvErrorString(node->error));
                } else {
                    mpvCoreEventFire(MPVCore::ENDED);
                }
                break;
            }
            case MPV_EVENT_PROPERTY_CHANGE: {
                auto* data = ((mpv_event_property*)event->data)->data;
                switch (event->reply_userdata) {
                    case 1:  // core-idle
                        if (data) m_playing = *(int*)data == 0;
                        break;
                    case 12:  // pause
                        if (data) m_playing = !(*(int*)data);
                        break;
                    default:
                        break;
                }
                break;
            }
            default:
                break;
        }
    }
}

void MPVCore::_command_async(const std::vector<std::string>& commands) {
    if (!m_mpv) return;
    std::vector<const char*> res;
    res.reserve(commands.size() + 1);
    for (auto& i : commands) res.emplace_back(i.c_str());
    res.emplace_back(nullptr);
    mpvCommandAsync(m_mpv, 0, res.data());
}

void MPVCore::mpvCoreEventFire(int ev, const std::string& msg) {
    if (m_eventCb) m_eventCb(ev, msg);
}

void MPVCore::destroy() {
    clean();
}

void MPVCore::clean() {
    if (!m_inited) return;
    if (m_mpv) {
        mpvCommandString(m_mpv, "quit");
    }
    if (m_ctx) {
        mpvRenderContextFree(m_ctx);
        m_ctx = nullptr;
    }
    if (m_mpv) {
        mpvTerminateDestroy(m_mpv);
        m_mpv = nullptr;
    }
    m_inited = false;
}
