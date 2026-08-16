// DecoTV Switch —— 播放核心（libmpv 软渲染，参考 wiliwili MPVCore 精简）
// 软渲染：mpv 用 CPU 渲染 RGBA 帧到内部缓冲，UI 线程取帧传 nanovg 显示；
// 音频/网络/同步由 mpv 内部处理（无需我们碰 audout/解封装）。
// 依赖：switch-libmpv_deko3d + switch-ffmpeg（wiliwili 预编译包）
#pragma once

#include <mpv/client.h>
#include <mpv/render.h>

#include <atomic>
#include <functional>
#include <string>

namespace mpv_player {

// 事件（均在 UI 主线程回调）
enum class Event {
    LOADED,   // 文件加载完成（可播放）
    ENDED,    // 播放结束
    ERROR,    // 出错（msg 为 mpv 日志）
};

// 事件回调：ev 事件；(进度类事件用 value，其余为 0)；msg 附加信息
using EventCallback = std::function<void(Event, double, const std::string&)>;

class Player {
  public:
    Player()  = default;
    ~Player();

    Player(const Player&)            = delete;
    Player& operator=(const Player&) = delete;

    // 创建 mpv 句柄 + 初始化 + 创建软渲染上下文；失败返回 false
    bool init();

    // 播放 url（m3u8/mp4/http(s)）；非阻塞（mpv 后台加载）
    void open(const std::string& url);

    void togglePause();
    void stop();             // 停止播放（保留 mpv 实例，可再次 open）
    void destroy();          // 释放全部资源

    void setEventCallback(EventCallback cb) { m_cb = std::move(cb); }

    // ---- 渲染（UI 线程每帧调用）----
    // 设置软渲染目标尺寸（物理像素，= 显示尺寸 × windowScale），重分配缓冲
    void resizeSurface(int w, int h);
    // 渲染一帧到内部缓冲；返回是否有新帧（true 时应重绘）
    bool renderFrame();
    // 内部缓冲（RGBA8）
    void* frameData() const { return m_pixels; }
    int frameWidth() const { return m_swW; }
    int frameHeight() const { return m_swH; }
    bool hasFrame() const { return m_pixels != nullptr && m_swW > 0 && m_swH > 0; }

    // 非阻塞处理 mpv 事件（wakeup 回调触发后、UI 线程调用）
    void processEvents();

    bool isActive() const { return m_mpv != nullptr; }

  private:
    static void wakeupCallback(void* ctx);   // mpv 线程触发（用 brls::sync 投递到 UI 线程）
    static void renderUpdateCallback(void* ctx);  // mpv 有新帧时触发

    mpv_handle* m_mpv        = nullptr;
    mpv_render_context* m_rc = nullptr;

    void* m_pixels = nullptr;
    int m_swW = 0, m_swH = 0, m_stride = 0;
    std::atomic<bool> m_needRender{false};  // mpv 渲染线程置位，UI 线程消费
    bool m_everRendered = false;            // 首帧强制渲染（软渲染初始无 update 回调，必须主动渲染驱动解码）

    EventCallback m_cb;
    bool m_inited = false;

    std::string m_lastUrl;        // 当前播放 URL（v1.21：用于 -13 自动重试）
    int m_audioRetryStage = 0;    // -13 音频重试阶段：0 未重试，1 已切 ao=null 重试，2 放弃（防止死循环）
};

}  // namespace mpv_player
