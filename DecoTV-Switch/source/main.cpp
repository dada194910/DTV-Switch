// DecoTV Switch 客户端 —— 纯 TVBox 客户端（用户自管播放源）
// 首页：用户源列表 + 「搜索全部」；源内：搜索此源。标准 tvbox 协议搜出后直接播放。
#include <borealis.hpp>
#include <switch.h>
#include <switch/swkbd.h>
#include <functional>
#include <string>
#include <vector>

#include "platform/api_decotv.h"
#include "player/mpv_player.hpp"

static std::vector<decotv::TvboxSite> g_sites;
static std::string g_startupError;

// ---- 中文字体 fallback ----
static void setupChineseFont() {
    PlFontData font;
    if (R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_ChineseSimplified)))
        return;
    if (!brls::Application::loadFontFromMemory("chinese_simplified", font.address, font.size, false))
        return;
    int regular = brls::Application::getFont(brls::FONT_REGULAR);
    int cjk     = brls::Application::getFont("chinese_simplified");
    if (regular == brls::FONT_INVALID || cjk == brls::FONT_INVALID)
        return;
    nvgAddFallbackFontId(brls::Application::getNVGContext(), regular, cjk);
}

// ---- 可聚焦行（主文本 + 副文本，A 键触发）----
static brls::Box* makeRow(const std::string& mainText, const std::string& subText,
                          std::function<bool(brls::View*)> onClick) {
    auto* row = new brls::Box(brls::Axis::COLUMN);
    row->setFocusable(true);
    row->setHeight(64);

    auto* t = new brls::Label();
    t->setText(mainText);
    t->setFontSize(24);
    row->addView(t);

    if (!subText.empty()) {
        auto* s = new brls::Label();
        s->setText(subText);
        s->setFontSize(20);
        row->addView(s);
    }
    row->registerClickAction(onClick);
    return row;
}

// ---- tvbox 软键盘输入（libnx Swkbd，最可靠）----
static std::string showKeyboard(const std::string& hint) {
    SwkbdConfig kbd;
    swkbdInit(&kbd, SwkbdType_QWERTY, 1, -1);
    swkbdSetHint(&kbd, hint.c_str());
    swkbdConfigSetOkButtonText(&kbd, "搜索");
    char buf[256] = {0};
    SwkbdButton res = swkbdGetText(&kbd, buf, sizeof(buf));
    swkbdExit(&kbd);
    return (res == SWKBD_BUTTON_CONFIRM) ? std::string(buf) : std::string();
}

// ---- 视频画面视图（libmpv 软渲染帧 → nanovg 纹理）----
class PlayerView : public brls::View {
  public:
    void setPlayer(mpv_player::Player* p) { m_player = p; }

    void draw(NVGcontext* vg, float x, float y, float w, float h, brls::Style style,
              brls::FrameContext* ctx) override {
        if (!m_player) return;
        int dw = (int)(w * brls::Application::windowScale);
        int dh = (int)(h * brls::Application::windowScale);
        m_player->resizeSurface(dw, dh);
        if (m_player->renderFrame()) {
            if (m_tex && (m_texW != dw || m_texH != dh)) {
                nvgDeleteImage(vg, m_tex);
                m_tex = 0;
            }
            if (!m_tex) {
                m_tex = nvgCreateImageRGBA(vg, dw, dh, 0,
                                           (const unsigned char*)m_player->frameData());
                m_texW = dw;
                m_texH = dh;
            } else {
                nvgUpdateImage(vg, m_tex, (const unsigned char*)m_player->frameData());
            }
        }
        if (m_tex) {
            NVGpaint p = nvgImagePattern(vg, x, y, w, h, 0, m_tex, 1.0f);
            nvgBeginPath(vg);
            nvgRect(vg, x, y, w, h);
            nvgFillPaint(vg, p);
            nvgFill(vg);
        }
    }

    ~PlayerView() override {
        if (m_tex) nvgDeleteImage(brls::Application::getNVGContext(), m_tex);
    }

  private:
    mpv_player::Player* m_player = nullptr;
    int m_tex   = 0;
    int m_texW  = 0;
    int m_texH  = 0;
};

// ---- 播放器页 ----
class PlayerActivity : public brls::Activity {
  public:
    explicit PlayerActivity(std::string url) : m_url(std::move(url)) {}
    ~PlayerActivity() override { m_player.destroy(); }

    brls::View* createContentView() override {
        m_player.setEventCallback([this](mpv_player::Event ev, double, const std::string& msg) {
            switch (ev) {
                case mpv_player::Event::LOADED:
                    if (m_info) m_info->setText("播放中  A 暂停/播放  B 返回");
                    break;
                case mpv_player::Event::ENDED:
                    if (m_info) m_info->setText("播放结束  B 返回");
                    break;
                case mpv_player::Event::ERROR:
                    if (m_info) m_info->setText("错误: " + msg);
                    break;
            }
        });

        m_view = new PlayerView();
        m_view->setPlayer(&m_player);
        m_view->setHeightPercentage(78);

        m_info = new brls::Label();
        m_info->setFontSize(20);
        m_info->setFocusable(true);
        m_info->setText("加载中...");

        auto* layout = new brls::Box(brls::Axis::COLUMN);
        layout->addView(m_view);
        layout->addView(m_info);

        auto* frame = new brls::AppletFrame(layout);
        frame->setTitle("播放");
        frame->registerAction("返回", brls::ControllerButton::BUTTON_B, [this](brls::View*) {
            m_player.stop();
            m_player.destroy();
            brls::Application::popActivity();
            return true;
        });
        frame->registerAction("暂停/播放", brls::ControllerButton::BUTTON_A, [this](brls::View*) {
            m_player.togglePause();
            return true;
        });

        decotv::trailLog("PLAY open url=" + m_url.substr(0, 80));
        m_player.init();
        m_player.open(m_url);
        return frame;
    }

  private:
    std::string m_url;
    mpv_player::Player m_player;
    PlayerView* m_view = nullptr;
    brls::Label* m_info = nullptr;
};

// ---- 搜索页（swkbd 输入关键词 → 搜全部/单源 → 结果 → A 播）----
class SearchActivity : public brls::Activity {
  public:
    explicit SearchActivity(std::vector<decotv::TvboxSite> sites) : m_sites(std::move(sites)) {}

    brls::View* createContentView() override {
        m_info = new brls::Label();
        m_info->setFontSize(20);
        m_info->setFocusable(true);

        m_listBox = new brls::Box(brls::Axis::COLUMN);

        auto* layout = new brls::Box(brls::Axis::COLUMN);
        layout->addView(m_info);
        layout->addView(m_listBox);

        auto* frame = new brls::AppletFrame(layout);
        frame->setTitle(m_sites.size() == 1 ? ("DecoTV - " + m_sites[0].name)
                                            : "DecoTV - 搜索全部");
        frame->registerAction("返回", brls::ControllerButton::BUTTON_B, [](brls::View*) {
            brls::Application::popActivity();
            return true;
        });

        load();
        return frame;
    }

  private:
    void load() {
        brls::Application::giveFocus(m_info);
        for (auto* r : m_rows) m_listBox->removeView(r);
        m_rows.clear();

        std::string kw = showKeyboard("输入片名关键词");
        if (kw.empty()) {
            m_info->setText("未输入关键词，按 B 返回");
            return;
        }

        m_info->setText("搜索中: " + kw + " ...");
        decotv::trailLog("SEARCH kw=" + kw + " sites=" + std::to_string(m_sites.size()));

        auto hits = decotv::searchAllSources(m_sites, kw);
        if (hits.empty()) {
            auto* lbl = new brls::Label();
            lbl->setText("未找到可用播放源（或网络失败）");
            lbl->setFontSize(22);
            m_listBox->addView(lbl);
            m_rows.push_back(lbl);
            brls::Application::giveFocus(m_info);
        } else {
            for (auto& h : hits) {
                std::string sub = h.playUrl;
                if (sub.size() > 48) sub = sub.substr(0, 48) + "...";
                auto* row = makeRow(h.sourceName + "  " + h.vodName, sub, [h](brls::View*) {
                    brls::Application::pushActivity(new PlayerActivity(h.playUrl));
                    return true;
                });
                m_listBox->addView(row);
                m_rows.push_back(row);
            }
            m_info->setText("找到 " + std::to_string(m_rows.size()) +
                           " 个结果  A 播放  B 返回");
            brls::Application::giveFocus(m_rows[0]);
        }
    }

    std::vector<decotv::TvboxSite> m_sites;
    brls::Label* m_info = nullptr;
    brls::Box* m_listBox = nullptr;
    std::vector<brls::View*> m_rows;
};

// ---- 首页：用户源列表 + 搜索全部 ----
class HomeActivity : public brls::Activity {
  public:
    brls::View* createContentView() override {
        auto* box = new brls::Box(brls::Axis::COLUMN);

        if (!g_startupError.empty()) {
            auto* lbl = new brls::Label();
            lbl->setText("启动失败: " + g_startupError);
            lbl->setFontSize(22);
            box->addView(lbl);
        } else if (g_sites.empty()) {
            auto* lbl = new brls::Label();
            lbl->setText("未配置播放源。\n请在 SD 卡 sdmc:/switch/DecoTV/config.json 添加 sites。");
            lbl->setFontSize(20);
            box->addView(lbl);
        } else {
            box->addView(makeRow("🔍 搜索全部源", "A 输入关键词跨源搜索", [this](brls::View*) {
                brls::Application::pushActivity(new SearchActivity(g_sites));
                return true;
            }));
            for (auto& s : g_sites) {
                box->addView(makeRow(s.name, "A 搜索此源", [s](brls::View*) {
                    brls::Application::pushActivity(new SearchActivity({s}));
                    return true;
                }));
            }
        }

        auto* frame = new brls::AppletFrame(box);
        frame->setTitle("DecoTV");
        return frame;
    }
};

int main(int argc, char* argv[]) {
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);

    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    brls::Application::createWindow("DecoTV");
    brls::Application::setGlobalQuit(true);

    // 退出说明：borealis(xfangfang fork) 已在构造时 appletHook，
    // 收到 OnExitRequest 调 quit() -> clear()(删所有 Activity, ~PlayerActivity 释放 mpv)
    // -> exitNetwork() -> userAppExit()。无需自己注册钩子。
    setupChineseFont();

    decotv::trailLog("START initNetwork");
    if (!decotv::initNetwork()) {
        g_startupError = "Network init failed: " + std::to_string(decotv::g_netInitResult);
    } else {
        decotv::trailLog("initNetwork ok");
        g_sites = decotv::loadConfig();
        if (g_sites.empty()) g_startupError = "未配置播放源（请编辑 config.json）";
    }

    brls::Application::pushActivity(new HomeActivity());

    while (brls::Application::mainLoop())
        ;

    decotv::trailLog("EXIT mainLoop -> exitNetwork");
    decotv::exitNetwork();
    return EXIT_SUCCESS;
}
