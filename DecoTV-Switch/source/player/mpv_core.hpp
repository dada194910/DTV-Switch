// DecoTV 播放核心 —— 移植自 wiliwili 的 MPVCore（deko3d 硬件渲染）
// 视频直接渲染到 Switch deko3d 帧缓冲（GPU 硬件渲染），
// 音频由 libmpv_deko3d 自带的 Switch 后端自动处理，
// 从根本上规避旧 SW 软渲染路径导致的 mpv error -13 / Switch 2168-0001 崩溃。
#pragma once

#include <borealis.hpp>
#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_dk3d.h>
#include <string>
#include <vector>
#include <functional>

// libmpv 标准 C API 是 snake_case；这里映射成 wiliwili 风格的 PascalCase 别名，
// 与 mpv_core.cpp 中的调用保持一致。DecoTV 直接链接 -lmpv（不打包 DLL），故只取 else 分支。
#ifndef MPV_BUNDLE_DLL
#define mpvSetOptionString            mpv_set_option_string
#define mpvObserveProperty            mpv_observe_property
#define mpvCreate                     mpv_create
#define mpvInitialize                 mpv_initialize
#define mpvTerminateDestroy           mpv_terminate_destroy
#define mpvSetWakeupCallback          mpv_set_wakeup_callback
#define mpvCommandString              mpv_command_string
#define mpvErrorString                mpv_error_string
#define mpvWaitEvent                  mpv_wait_event
#define mpvGetProperty                mpv_get_property
#define mpvCommandAsync               mpv_command_async
#define mpvGetPropertyString          mpv_get_property_string
#define mpvFreeNodeContents           mpv_free_node_contents
#define mpvSetOption                  mpv_set_option
#define mpvFree                       mpv_free
#define mpvRenderContextCreate        mpv_render_context_create
#define mpvRenderContextSetUpdateCallback mpv_render_context_set_update_callback
#define mpvRenderContextRender        mpv_render_context_render
#define mpvRenderContextReportSwap    mpv_render_context_report_swap
#define mpvRenderContextUpdate        mpv_render_context_update
#define mpvRenderContextFree          mpv_render_context_free
#define mpvClientApiVersion           mpv_client_api_version
#endif

class MPVCore {
public:
    enum Event {
        LOADED = 1,  // 文件加载完成，开始解码
        ENDED  = 2,  // 播放结束
        ERROR  = 3,  // 出错
    };

    static MPVCore& instance() {
        static MPVCore inst;
        return inst;
    }

    void init();
    void setUrl(const std::string& url);
    void play();
    void pause();
    void togglePause();
    void stop();
    void draw();
    bool isPlaying() const { return m_playing; }
    void setEventCallback(std::function<void(int, const std::string&)> cb) { m_eventCb = std::move(cb); }
    void destroy();

private:
    MPVCore() = default;
    ~MPVCore() = default;
    MPVCore(const MPVCore&) = delete;
    MPVCore& operator=(const MPVCore&) = delete;

    void clean();
    static void on_wakeup(void* self);
    static void on_update(void* self);
    void eventMainLoop();

    void _command_async(const std::vector<std::string>& commands);
    void mpvCoreEventFire(int ev, const std::string& msg = "");
    template <typename... Args>
    void command_async(Args&&... args) {
        std::vector<std::string> commands{args...};
        _command_async(commands);
    }

    mpv_handle* m_mpv = nullptr;
    mpv_render_context* m_ctx = nullptr;

#ifdef BOREALIS_USE_DEKO3D
    DkFence m_doneFence{};
    DkFence m_readyFence{};
    mpv_deko3d_fbo m_fbo{ nullptr, &m_readyFence, &m_doneFence, 1280, 720, DkImageFormat_RGBA8_Unorm };
    mpv_render_param m_params[3] = {
        {MPV_RENDER_PARAM_DEKO3D_FBO, &m_fbo},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
#endif

    bool m_inited = false;
    bool m_playing = false;
    bool m_stopped = true;
    int m_errCode = 0;
    std::function<void(int, const std::string&)> m_eventCb;
};
