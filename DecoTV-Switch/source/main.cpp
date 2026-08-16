// DecoTV Switch 客户端 —— 纯 TVBox 客户端（用户自管播放源）+ TVBox 风格界面
// 布局：左侧 TabFrame 侧栏（搜索 / 各源 / 设置），右侧海报网格；详情页选集；设置可清缓存。
#include <borealis.hpp>
#include <switch.h>
#include <switch/applets/swkbd.h>
#include <functional>
#include <string>
#include <vector>
#include <algorithm>

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

// ---- tvbox 软键盘输入（libnx Swkbd）----
static std::string showKeyboard(const std::string& hint) {
    SwkbdConfig kbd;
    swkbdCreate(&kbd, 0);
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, hint.c_str());
    swkbdConfigSetOkButtonText(&kbd, "搜索");
    char buf[256] = {0};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    std::string s(buf);
    return (R_SUCCEEDED(rc) && !s.empty()) ? s : std::string();
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

static const decotv::TvboxSite* findSite(const std::string& key) {
    for (auto& s : g_sites)
        if (s.key == key) return &s;
    return nullptr;
}

// ---- 海报卡片（TVBox 网格单元）----
static brls::Box* makePosterCard(const decotv::VodItem& item,
                                 std::function<void(const decotv::VodItem&)> onSelect) {
    auto* card = new brls::Box(brls::Axis::COLUMN);
    card->setFocusable(true);
    card->setWidth(200);
    card->setHeight(320);

    auto* img = new brls::Image();
    img->setScalingType(brls::ImageScalingType::FILL);
    img->setImageAlign(brls::ImageAlignment::CENTER);
    img->setWidth(200);
    img->setHeight(260);
    std::string p = decotv::cacheImage(item.pic);
    if (!p.empty()) img->setImageFromFile(p);
    card->addView(img);

    auto* title = new brls::Label();
    title->setText(item.vodName);
    title->setFontSize(18);
    title->setWidth(200);
    title->setHeight(60);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    card->addView(title);

    card->registerClickAction([item, onSelect](brls::View*) {
        if (onSelect) onSelect(item);
        return true;
    });
    return card;
}

// ---- 海报网格（每行 COLS 张）----
static brls::Box* buildPosterGrid(const std::vector<decotv::VodItem>& items,
                                  std::function<void(const decotv::VodItem&)> onSelect) {
    auto* grid = new brls::Box(brls::Axis::COLUMN);
    grid->setPadding(20, 20, 20, 20);
    const int COLS = 5;
    size_t n = std::min(items.size(), (size_t)30);  // 限制数量，避免卡顿
    for (size_t i = 0; i < n; i += COLS) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::FLEX_START);
        row->setMarginBottom(20);  // 行间距（替代 setSpacing）
        for (int c = 0; c < COLS && i + c < n; ++c) {
            auto* card = makePosterCard(items[i + c], onSelect);
            card->setMarginRight(20);  // 卡片间距（替代 setSpacing）
            row->addView(card);
        }
        grid->addView(row);
    }
    return grid;
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

// ---- 详情/选集页 ----
class DetailActivity : public brls::Activity {
  public:
    DetailActivity(decotv::VodItem item, decotv::TvboxSite site)
        : m_item(std::move(item)), m_site(std::move(site)) {}
    ~DetailActivity() override {}

    brls::View* createContentView() override {
        decotv::trailLog("DETAIL fetch vodId=" + m_item.vodId);
        // 搜索结果已带 playUrl；详情页再拉一次拿完整分集
        m_item = decotv::fetchDetail(m_site, m_item.vodId);

        auto* root = new brls::Box(brls::Axis::COLUMN);
        root->setPadding(20, 20, 20, 20);

        // 顶部：海报 + 标题
        auto* top = new brls::Box(brls::Axis::ROW);
        auto* img = new brls::Image();
        img->setScalingType(brls::ImageScalingType::FILL);
        img->setImageAlign(brls::ImageAlignment::CENTER);
        img->setWidth(180);
        img->setHeight(250);
        std::string p = decotv::cacheImage(m_item.pic);
        if (!p.empty()) img->setImageFromFile(p);
        top->addView(img);

        auto* info = new brls::Box(brls::Axis::COLUMN);
        auto* name = new brls::Label();
        name->setText(m_item.vodName);
        name->setFontSize(28);
        info->addView(name);
        auto* cnt = new brls::Label();
        cnt->setText("共 " + std::to_string(m_item.episodeNames.size()) + " 集");
        cnt->setFontSize(20);
        info->addView(cnt);
        top->addView(info);
        root->addView(top);

        // 选集网格
        auto* scroll = new brls::ScrollingFrame();
        auto* grid = new brls::Box(brls::Axis::COLUMN);
        const int COLS = 6;
        if (m_item.episodeNames.empty()) {
            auto* none = new brls::Label();
            none->setText("无分集信息");
            none->setFontSize(22);
            grid->addView(none);
        } else {
            for (size_t i = 0; i < m_item.episodeNames.size(); i += COLS) {
                auto* row = new brls::Box(brls::Axis::ROW);
                row->setJustifyContent(brls::JustifyContent::FLEX_START);
                row->setMarginBottom(12);  // 行间距（替代 setSpacing）
                for (int c = 0; c < COLS && i + c < (int)m_item.episodeNames.size(); ++c) {
                    int idx = (int)(i + c);
                    auto* ep = new brls::Box(brls::Axis::COLUMN);
                    ep->setFocusable(true);
                    ep->setWidth(150);
                    ep->setHeight(60);
                    ep->setMarginRight(12);  // 选集间距（替代 setSpacing）
                    auto* lbl = new brls::Label();
                    lbl->setText(m_item.episodeNames[idx]);
                    lbl->setFontSize(18);
                    lbl->setHorizontalAlign(brls::HorizontalAlign::CENTER);
                    ep->addView(lbl);
                    std::string url = m_item.episodeUrls[idx];
                    ep->registerClickAction([url](brls::View*) {
                        brls::Application::pushActivity(
                            new PlayerActivity(decotv::parsePlayUrl(url)));
                        return true;
                    });
                    row->addView(ep);
                }
                grid->addView(row);
            }
        }
        scroll->setContentView(grid);
        root->addView(scroll);

        auto* frame = new brls::AppletFrame(root);
        frame->setTitle(m_item.vodName);
        frame->registerAction("返回", brls::ControllerButton::BUTTON_B, [](brls::View*) {
            brls::Application::popActivity();
            return true;
        });
        return frame;
    }

  private:
    decotv::VodItem m_item;
    decotv::TvboxSite m_site;
};

// ---- 搜索结果 -> 详情页（TVBox 习惯：先看选集）----
static void onItemSelect(const decotv::VodItem& item) {
    const decotv::TvboxSite* s = findSite(item.sourceKey);
    if (!s) return;
    brls::Application::pushActivity(new DetailActivity(item, *s));
}

// ---- 搜索全部源 标签内容 ----
static brls::View* buildSearchTab() {
    auto* root = new brls::Box(brls::Axis::COLUMN);

    auto* header = new brls::Header();
    header->setTitle("搜索全部源");
    root->addView(header);

    auto* content = new brls::Box(brls::Axis::COLUMN);  // 结果容器（搜索后替换）
    auto* hint = new brls::Label();
    hint->setText("按下方按钮跨所有源搜索");
    hint->setFontSize(22);
    content->addView(hint);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setContentView(content);
    root->addView(scroll);

    auto* btn = new brls::Button();
    btn->setText("🔍 搜索");
    btn->setFontSize(24);
    btn->registerClickAction([content, scroll](brls::View*) {
        std::string kw = showKeyboard("输入片名关键词");
        if (kw.empty()) return true;
        auto hits = decotv::searchAllSources(g_sites, kw);
        if (hits.empty()) {
            auto* box = new brls::Box(brls::Axis::COLUMN);
            auto* lbl = new brls::Label();
            lbl->setText("未找到 / 网络失败");
            lbl->setFontSize(22);
            box->addView(lbl);
            scroll->setContentView(box);
        } else {
            scroll->setContentView(buildPosterGrid(hits, onItemSelect));
        }
        return true;
    });
    root->addView(btn);

    return root;
}

// ---- 设置标签内容 ----
static brls::View* buildSettingsTab() {
    auto* root = new brls::Box(brls::Axis::COLUMN);
    root->setPadding(20, 20, 20, 20);

    auto* header = new brls::Header();
    header->setTitle("设置");
    root->addView(header);

    // 源管理：添加订阅
    root->addView(makeRow("源管理（添加订阅）", "A 输入 tvbox 订阅地址，自动合并源", [](brls::View*) {
        std::string url = showKeyboard("输入 tvbox 订阅地址(https://...)");
        if (url.empty()) return true;
        // 读取 config.json -> 追加 subscriptions -> 写回 -> 重载
        const char* path = "sdmc:/switch/DecoTV/config.json";
        std::string content;
        FILE* f = fopen(path, "r");
        if (f) {
            char buf[1024];
            while (fgets(buf, sizeof(buf), f)) content += buf;
            fclose(f);
        }
        nlohmann::json j = content.empty() ? nlohmann::json::object()
                                           : nlohmann::json::parse(content, nullptr, false);
        if (!j.contains("subscriptions") || !j["subscriptions"].is_array())
            j["subscriptions"] = nlohmann::json::array();
        j["subscriptions"].push_back(url);
        FILE* w = fopen(path, "w");
        if (w) {
            std::string out = j.dump(2);
            fwrite(out.data(), 1, out.size(), w);
            fclose(w);
        }
        g_sites = decotv::loadConfig();
        auto* dlg = new brls::Dialog("已添加订阅，共 " + std::to_string(g_sites.size()) + " 个源。\n重启应用生效。");
        dlg->addButton("确定", [] {});
        dlg->open();
        return true;
    }));

    // 清除缓存
    std::string cacheStr = std::to_string(decotv::cacheSizeBytes() / 1024 / 1024) + " MB";
    root->addView(makeRow("清除缓存", "当前缓存 " + cacheStr + "（海报 + 视频），A 清理", [](brls::View*) {
        auto* dlg = new brls::Dialog("确认清除所有缓存？\n（海报与视频缓存将被删除）");
        dlg->addButton("清除", [] {
            decotv::clearCache();
        });
        dlg->addButton("取消", [] {});
        dlg->open();
        return true;
    }));

    // 关于
    root->addView(makeRow("关于", "DecoTV v2.01 · 纯 TVBox 客户端 · 用户自管源", [](brls::View*) {
        return true;
    }));

    return root;
}

// ---- 首页：TabFrame（TVBox 风格）----
class HomeActivity : public brls::Activity {
  public:
    brls::View* createContentView() override {
        auto* tab = new brls::TabFrame();

        if (!g_startupError.empty()) {
            tab->addTab("错误", [this]() -> brls::View* {
                auto* b = new brls::Box(brls::Axis::COLUMN);
                auto* l = new brls::Label();
                l->setText("启动失败: " + g_startupError);
                l->setFontSize(22);
                b->addView(l);
                return b;
            });
            return tab;
        }
        if (g_sites.empty()) {
            tab->addTab("未配置源", []() -> brls::View* {
                auto* b = new brls::Box(brls::Axis::COLUMN);
                auto* l = new brls::Label();
                l->setText("未配置播放源。\n请在 SD 卡 sdmc:/switch/DecoTV/config.json 添加 sites。");
                l->setFontSize(20);
                b->addView(l);
                return b;
            });
            return tab;
        }

        // 搜索全部
        tab->addTab("🔍 搜索", []() -> brls::View* {
            return buildSearchTab();
        });

        // 每个源一个标签
        for (auto& s : g_sites) {
            tab->addTab(s.name, [s]() -> brls::View* {
                // 单源搜索：复用搜索结果页，但只搜这一源
                auto* root = new brls::Box(brls::Axis::COLUMN);
                auto* header = new brls::Header();
                header->setTitle(s.name);
                root->addView(header);
                auto* content = new brls::Box(brls::Axis::COLUMN);
                auto* hint = new brls::Label();
                hint->setText("按下方按钮搜索此源");
                hint->setFontSize(22);
                content->addView(hint);
                auto* scroll = new brls::ScrollingFrame();
                scroll->setContentView(content);
                root->addView(scroll);
                auto* btn = new brls::Button();
                btn->setText("🔍 搜索 " + s.name);
                btn->setFontSize(24);
                btn->registerClickAction([s, content, scroll](brls::View*) {
                    std::string kw = showKeyboard("输入片名关键词");
                    if (kw.empty()) return true;
                    auto hits = decotv::searchSite(s, kw);
                    if (hits.empty()) {
                        auto* box = new brls::Box(brls::Axis::COLUMN);
                        auto* lbl = new brls::Label();
                        lbl->setText("未找到 / 网络失败");
                        lbl->setFontSize(22);
                        box->addView(lbl);
                        scroll->setContentView(box);
                    } else {
                        scroll->setContentView(buildPosterGrid(hits, onItemSelect));
                    }
                    return true;
                });
                root->addView(btn);
                return root;
            });
        }

        // 设置
        tab->addTab("⚙ 设置", []() -> brls::View* {
            return buildSettingsTab();
        });

        return tab;
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
    // -> exitNetwork()(清视频缓存) -> userAppExit()。无需自己注册钩子。
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
