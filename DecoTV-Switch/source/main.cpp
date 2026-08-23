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
#include "player/mpv_core.hpp"

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
// 注意：当前 CI 工具链（devkitPro devkita64:20251117 的 libnx）尚未暴露
// SwkbdLanguageType_ChineseSimplified / swkbdConfigSetLanguage 等中文拼音 API，
// 直接调用会编译失败。故此处仅用跨版本稳定的基础接口；中文拼音输入将在
// 升级 libnx 后的下一版本（v2.11）单独启用。
static std::string showKeyboard(const std::string& hint) {
    SwkbdConfig kbd;
    swkbdCreate(&kbd, 0);
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, hint.c_str());
    swkbdConfigSetOkButtonText(&kbd, "搜索");
    swkbdConfigSetStringLenMax(&kbd, 64);
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
    void draw(NVGcontext* vg, float x, float y, float w, float h, brls::Style style,
              brls::FrameContext* ctx) override {
        // deko3d 模式下视频由 MPVCore 直接渲染到屏幕帧缓冲（GPU 硬件渲染），
        // 这里每帧触发一次渲染，文字信息由下方 Label 叠加在上层。
        MPVCore::instance().draw();
    }
};

// ---- 播放器页 ----
class PlayerActivity : public brls::Activity {
  public:
    explicit PlayerActivity(std::string url) : m_url(std::move(url)) {}
    ~PlayerActivity() override { MPVCore::instance().stop(); }

    brls::View* createContentView() override {
        auto& player = MPVCore::instance();
        player.init();
        player.setEventCallback([this](int ev, const std::string& msg) {
            if (ev == MPVCore::LOADED)
                m_info->setText("播放中  A 暂停/播放  B 返回");
            else if (ev == MPVCore::ENDED)
                m_info->setText("播放结束  B 返回");
            else if (ev == MPVCore::ERROR)
                m_info->setText("错误: " + msg);
        });

        m_view = new PlayerView();

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
            MPVCore::instance().stop();
            brls::Application::popActivity();
            return true;
        });
        frame->registerAction("暂停/播放", brls::ControllerButton::BUTTON_A, [this](brls::View*) {
            MPVCore::instance().togglePause();
            return true;
        });

        decotv::trailLog("PLAY open url=" + m_url.substr(0, 80));
        player.setUrl(m_url);
        return frame;
    }

  private:
    std::string m_url;
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

// ---- 推荐搜索结果页（点豆瓣推荐后，拿片名去各源搜出的结果）----
class ResultsActivity : public brls::Activity {
  public:
    ResultsActivity(std::string title, std::vector<decotv::VodItem> items)
        : m_title(std::move(title)), m_items(std::move(items)) {}
    brls::View* createContentView() override {
        auto* root = new brls::Box(brls::Axis::COLUMN);
        auto* header = new brls::Header();
        header->setTitle("搜索：" + m_title);
        root->addView(header);
        auto* scroll = new brls::ScrollingFrame();
        scroll->setContentView(buildPosterGrid(m_items, onItemSelect));
        root->addView(scroll);
        auto* frame = new brls::AppletFrame(root);
        frame->setTitle(m_title);
        frame->registerAction("返回", brls::ControllerButton::BUTTON_B, [](brls::View*) {
            brls::Application::popActivity();
            return true;
        });
        return frame;
    }
  private:
    std::string m_title;
    std::vector<decotv::VodItem> m_items;
};

// 点推荐条目：源兜底条目自带源+直链，直接进详情/选集；豆瓣条目用片名去各源搜
static void onDoubanSelect(const decotv::VodItem& item) {
    if (item.sourceKey != "douban" && !item.playUrl.empty()) {
        const decotv::TvboxSite* s = findSite(item.sourceKey);
        if (s) {
            brls::Application::pushActivity(new DetailActivity(item, *s));
            return;
        }
    }
    auto hits = decotv::searchAllSources(g_sites, item.vodName);
    if (hits.empty()) {
        auto* dlg = new brls::Dialog("未找到《" + item.vodName + "》\n（已配置源里搜不到该片）");
        dlg->addButton("确定", [] {});
        dlg->open();
        return;
    }
    brls::Application::pushActivity(new ResultsActivity(item.vodName, hits));
}

// ---- 内置静态分类墙（不联网，开箱即有内容）----
// 每个分类点击后用分类名当关键词去各源搜索，导向真实结果。
struct CatItem { std::string name; std::string icon; };
static const std::vector<CatItem> kCategories = {
    {"热播剧",   "🔥"}, {"电影",     "🎬"}, {"电视剧",   "📺"},
    {"动漫",     "🌸"}, {"综艺",     "🎤"}, {"纪录片",   "🌍"},
    {"少儿",     "🧒"}, {"港剧",     "🀄"}, {"韩剧",     "💝"},
    {"美剧",     "🍿"}, {"国产剧",   "🇨🇳"}, {"恐怖",     "👻"},
    {"动作",     "🥋"}, {"喜剧",     "😄"}, {"爱情",     "💕"},
    {"科幻",     "🚀"}, {"悬疑",     "🕵"}, {"经典",     "🏆"},
};

static brls::Box* makeCatCard(const CatItem& cat) {
    auto* card = new brls::Box(brls::Axis::COLUMN);
    card->setFocusable(true);
    card->setWidth(200);
    card->setHeight(110);
    card->setMarginRight(20);
    card->setMarginBottom(20);
    auto* ic = new brls::Label();
    ic->setText(cat.icon + " " + cat.name);
    ic->setFontSize(24);
    ic->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    card->addView(ic);
    card->registerClickAction([cat](brls::View*) {
        auto hits = decotv::searchAllSources(g_sites, cat.name);
        if (hits.empty()) {
            auto* dlg = new brls::Dialog("「" + cat.name + "」未找到 / 网络失败\n（用上方搜索框可手动输入）");
            dlg->addButton("确定", [] {});
            dlg->open();
        } else {
            brls::Application::pushActivity(new ResultsActivity(cat.name, hits));
        }
        return true;
    });
    return card;
}

// ---- 首页推荐 tab：内置分类墙（不依赖联网，永远有内容）----
static brls::View* buildRecommendTab() {
    auto* root = new brls::Box(brls::Axis::COLUMN);
    auto* header = new brls::Header();
    header->setTitle("推荐 · 分类直达");
    root->addView(header);

    auto* scroll = new brls::ScrollingFrame();
    auto* wrap = new brls::Box(brls::Axis::COLUMN);
    wrap->setPadding(20, 20, 20, 20);
    auto* tip = new brls::Label();
    tip->setText("点分类直接搜 · 上方搜索框可输入中/英文片名");
    tip->setFontSize(18);
    wrap->addView(tip);

    const int COLS = 5;
    for (size_t i = 0; i < kCategories.size(); i += COLS) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setJustifyContent(brls::JustifyContent::FLEX_START);
        for (int c = 0; c < COLS && i + c < kCategories.size(); ++c)
            row->addView(makeCatCard(kCategories[i + c]));
        wrap->addView(row);
    }

    scroll->setContentView(wrap);
    root->addView(scroll);
    return root;
}

// ---- 搜索全部源 标签内容（常驻搜索框 + 中/英文键盘）----
static brls::View* buildSearchTab() {
    auto* root = new brls::Box(brls::Axis::COLUMN);

    auto* header = new brls::Header();
    header->setTitle("搜索全部源");
    root->addView(header);

    // 常驻搜索框（可聚焦，点击弹键盘）
    auto* box = new brls::Box(brls::Axis::ROW);
    box->setFocusable(true);
    box->setHeight(64);
    box->setMarginBottom(12);
    box->setBackgroundColor(brls::Application::getTheme().getColor("brls/background"));
    auto* sIcon = new brls::Label();
    sIcon->setText("🔍");
    sIcon->setFontSize(26);
    sIcon->setMarginRight(12);
    box->addView(sIcon);
    auto* sText = new brls::Label();
    sText->setText("点击此处输入片名（中文 / 英文）…");
    sText->setFontSize(24);
    box->addView(sText);
    root->addView(box);

    // 结果容器 + 滚动区
    auto* scroll = new brls::ScrollingFrame();
    auto* content = new brls::Box(brls::Axis::COLUMN);
    auto* hint = new brls::Label();
    hint->setText("在上方搜索框输入关键词后回车即可搜索");
    hint->setFontSize(22);
    content->addView(hint);
    scroll->setContentView(content);
    root->addView(scroll);

    // 点击搜索框：弹中文键盘，回车即搜
    box->registerClickAction([sText, content, scroll](brls::View*) {
        std::string kw = showKeyboard("输入片名关键词（中文 / 英文）");
        if (kw.empty()) return true;
        sText->setText("🔍 " + kw);
        auto hits = decotv::searchAllSources(g_sites, kw);
        if (hits.empty()) {
            auto* box2 = new brls::Box(brls::Axis::COLUMN);
            auto* lbl = new brls::Label();
            lbl->setText("未找到 / 网络失败");
            lbl->setFontSize(22);
            box2->addView(lbl);
            scroll->setContentView(box2);
        } else {
            scroll->setContentView(buildPosterGrid(hits, onItemSelect));
        }
        return true;
    });

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
    root->addView(makeRow("关于", "DecoTV v2.11 · 纯 TVBox 客户端 · deko3d 硬件渲染播放", [](brls::View*) {
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

        // 豆瓣推荐（仿 TVBox 首页海报墙）—— 放在首位
        tab->addTab("🏠 推荐", []() -> brls::View* {
            return buildRecommendTab();
        });

        // 搜索全部
        tab->addTab("🔍 搜索", []() -> brls::View* {
            return buildSearchTab();
        });

        // 每个源一个标签
        for (auto& s : g_sites) {
            tab->addTab(s.name, [s]() -> brls::View* {
                // 单源搜索：常驻搜索框，点弹中文键盘
                auto* root = new brls::Box(brls::Axis::COLUMN);
                auto* header = new brls::Header();
                header->setTitle(s.name);
                root->addView(header);

                auto* box = new brls::Box(brls::Axis::ROW);
                box->setFocusable(true);
                box->setHeight(64);
                box->setMarginBottom(12);
                auto* sIcon = new brls::Label();
                sIcon->setText("🔍");
                sIcon->setFontSize(26);
                sIcon->setMarginRight(12);
                box->addView(sIcon);
                auto* sText = new brls::Label();
                sText->setText("点击此处输入片名（中文 / 英文）…");
                sText->setFontSize(24);
                box->addView(sText);
                root->addView(box);

                auto* scroll = new brls::ScrollingFrame();
                auto* content = new brls::Box(brls::Axis::COLUMN);
                auto* hint = new brls::Label();
                hint->setText("在上方搜索框输入关键词后回车即可搜索此源");
                hint->setFontSize(22);
                content->addView(hint);
                scroll->setContentView(content);
                root->addView(scroll);

                box->registerClickAction([s, sText, content, scroll](brls::View*) {
                    std::string kw = showKeyboard("输入片名关键词（中文 / 英文）");
                    if (kw.empty()) return true;
                    sText->setText("🔍 " + kw);
                    auto hits = decotv::searchSite(s, kw);
                    if (hits.empty()) {
                        auto* box2 = new brls::Box(brls::Axis::COLUMN);
                        auto* lbl = new brls::Label();
                        lbl->setText("未找到 / 网络失败");
                        lbl->setFontSize(22);
                        box2->addView(lbl);
                        scroll->setContentView(box2);
                    } else {
                        scroll->setContentView(buildPosterGrid(hits, onItemSelect));
                    }
                    return true;
                });
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
        // 允许空源启动：未配置播放源不再拦截，进入主界面后用户可在「设置」里添加
    }

    brls::Application::pushActivity(new HomeActivity());

    while (brls::Application::mainLoop())
        ;

    decotv::trailLog("EXIT mainLoop -> exitNetwork");
    decotv::exitNetwork();
    return EXIT_SUCCESS;
}
