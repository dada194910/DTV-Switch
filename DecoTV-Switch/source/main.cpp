// DecoTV Switch 客户端 —— P3：首页分类浏览 + 影片列表（豆瓣驱动）
// 首页：4 分类（电影/电视剧/综艺/动漫）
// 列表页：主筛选标签 + 影片列表（标题/年份/评分），◀▶ 翻页，A 选中（详情 P4）
// [+] 退出：setGlobalQuit(true)；B 返回上一页
#include <borealis.hpp>
#include <switch.h>
#include <functional>
#include <string>
#include <vector>

#include "platform/api_decotv.h"

static std::vector<decotv::Category> g_categories;
static std::string g_startupError;

// ---- 中文字体 fallback（系统简体中文字体）----
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

// ---- 自定义可聚焦行（主文本 + 副文本，A 键触发）----
static brls::Box* makeRow(const std::string& mainText, const std::string& subText,
                          std::function<bool(brls::View*)> onClick) {
    auto* row = new brls::Box(brls::Axis::COLUMN);
    row->setFocusable(true);
    row->setHeight(64);

    auto* t = new brls::Label();
    t->setText(mainText);
    t->setFontSize(24);  // 主文本：保证可读
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

// ---- 主筛选标签（横排）----
// 安全设计：标签创建一次后只更新选中文本，绝不删除重建。
// 原因：若在点击回调里 removeView/delete 正在执行回调的按钮自身，
// 回调返回后框架再访问该 View 即为 use-after-free → data abort → 2168-0002 死机。
struct FilterTab {
    brls::Box* box     = nullptr;
    brls::Label* label = nullptr;
    std::string text;
};

static FilterTab makeFilterTab(const std::string& text,
                               std::function<bool(brls::View*)> onClick) {
    FilterTab ft;
    ft.text = text;
    ft.box = new brls::Box(brls::Axis::COLUMN);
    ft.box->setFocusable(true);
    ft.box->setHeight(46);
    ft.box->setWidth(170);
    ft.label = new brls::Label();
    ft.label->setFontSize(20);
    ft.label->setText("   " + text);
    ft.box->addView(ft.label);
    ft.box->registerClickAction(std::move(onClick));
    return ft;
}

// ---- 分类 → 豆瓣参数映射 ----
static void categoryToDouban(const decotv::Category& cat, int primaryIndex,
                             std::string& kind, std::string& category, std::string& type) {
    kind = "movie"; category = "全部"; type = "全部";
    if (cat.key == "movie") {
        kind = "movie";
        if (primaryIndex >= 0 && primaryIndex < (int)cat.primary.size())
            category = cat.primary[primaryIndex].value;
        type = "全部";
    } else if (cat.key == "show") {
        kind = "tv"; category = "show"; type = "show";
    } else {  // tv / anime
        kind = "tv"; category = "tv"; type = "tv";
    }
}

// ---- 影片列表页 ----
class VideoListActivity : public brls::Activity {
  public:
    explicit VideoListActivity(decotv::Category c) : m_cat(std::move(c)) {}

    brls::View* createContentView() override {
        auto* frame = new brls::AppletFrame();
        frame->setTitle("DecoTV - " + m_cat.label);
        frame->registerAction("返回", brls::ControllerButton::BUTTON_B, [](brls::View*) {
            brls::Application::popActivity();
            return true;
        });

        m_info = new brls::Label();
        m_info->setFontSize(20);

        m_primaryBar = new brls::Box(brls::Axis::ROW);

        m_scroll = new brls::ScrollingFrame();
        m_scroll->setPadding(20, 20, 20, 20);
        m_scroll->registerAction("上一页", brls::ControllerButton::BUTTON_LEFT, [this](brls::View*) {
            if (m_page > 0) { m_page--; loadPage(); }
            return true;
        });
        m_scroll->registerAction("下一页", brls::ControllerButton::BUTTON_RIGHT, [this](brls::View*) {
            m_page++;
            loadPage();
            return true;
        });

        // 主筛选标签：一次性创建，之后只更新选中态（绝不删除重建，防 use-after-free）
        for (size_t i = 0; i < m_cat.primary.size(); ++i) {
            FilterTab ft = makeFilterTab(m_cat.primary[i].label, [this, i](brls::View*) {
                m_primaryIndex = (int)i;
                m_page = 0;
                updatePrimarySelected();
                loadPage();
                return true;
            });
            m_primaryBar->addView(ft.box);
            m_barTabs.push_back(std::move(ft));
        }
        updatePrimarySelected();

        auto* layout = new brls::Box(brls::Axis::COLUMN);
        layout->addView(m_info);
        layout->addView(m_primaryBar);
        layout->addView(m_scroll);
        frame->setContentView(layout);

        loadPage();
        return frame;
    }

  private:
    void updatePrimarySelected() {
        for (size_t i = 0; i < m_barTabs.size(); ++i) {
            bool sel = ((int)i == m_primaryIndex);
            m_barTabs[i].label->setText((sel ? "▶ " : "   ") + m_barTabs[i].text);
        }
    }

    void loadPage() {
        std::string kind, category, type;
        categoryToDouban(m_cat, m_primaryIndex, kind, category, type);

        m_info->setText("第 " + std::to_string(m_page + 1) + " 页   ◀▶翻页  A选中  B返回");

        auto items = decotv::fetchDoubanList(kind, category, type, PAGE_SIZE, m_page * PAGE_SIZE);

        auto* box = new brls::Box(brls::Axis::COLUMN);
        if (items.empty()) {
            auto* lbl = new brls::Label();
            lbl->setText("暂无内容或加载失败");
            lbl->setFontSize(22);
            box->addView(lbl);
        } else {
            for (auto& v : items) {
                std::string sub = v.year;
                if (!sub.empty() && !v.rate.empty()) sub += "  ⭐" + v.rate;
                else if (!v.rate.empty()) sub = "⭐" + v.rate;
                box->addView(makeRow(v.title, sub, [v](brls::View*) {
                    brls::Application::notify("详情/选源/播放 将在 P4 实现");
                    return true;
                }));
            }
        }
        m_scroll->setContentView(box);
    }

    static constexpr int PAGE_SIZE = 20;
    decotv::Category m_cat;
    int m_primaryIndex = 0;
    int m_page = 0;
    brls::Label* m_info = nullptr;
    brls::Box* m_primaryBar = nullptr;
    brls::ScrollingFrame* m_scroll = nullptr;
    std::vector<FilterTab> m_barTabs;
};

// ---- 首页 ----
// v1.11：启动路径完全复刻 v1.08 已验证结构（AppletFrame + Box + Label 纯显示，
// 无 ScrollingFrame / 无 setFocusable / 无 registerClickAction），先恢复"能启动"。
// 元凶定位：若本版能启动 → 崩在 v1.09 新引入的上述 API；若仍崩 → 内存/环境问题（高内存模式 + crash report）
class HomeActivity : public brls::Activity {
  public:
    brls::View* createContentView() override {
        auto* frame = new brls::AppletFrame();
        frame->setTitle("DecoTV");

        auto* box = new brls::Box(brls::Axis::COLUMN);

        if (!g_startupError.empty()) {
            auto* lbl = new brls::Label();
            lbl->setText("启动失败: " + g_startupError);
            lbl->setFontSize(22);
            box->addView(lbl);
        } else if (g_categories.empty()) {
            auto* lbl = new brls::Label();
            lbl->setText("无分类数据（请检查网络）");
            lbl->setFontSize(22);
            box->addView(lbl);
        } else {
            for (auto& c : g_categories) {
                auto* lbl = new brls::Label();
                lbl->setText("▶ " + c.label);
                lbl->setFontSize(28);  // 分类名大字号，保证可读
                box->addView(lbl);
            }
        }

        frame->setContentView(box);
        return frame;
    }
};

int main(int argc, char* argv[]) {
    brls::Logger::setLogLevel(brls::LogLevel::INFO);

    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    brls::Application::createWindow("DecoTV");
    brls::Application::setGlobalQuit(true);   // [+] 全局退出
    setupChineseFont();

    // ---- 启动自检：登录 + 拉分类 ----
    if (!decotv::initNetwork()) {
        g_startupError = "Network init failed: " + std::to_string(decotv::g_netInitResult);
    } else {
        bool ok = decotv::hasSavedLogin() ? true
                                          : decotv::login(decotv::LOGIN_USER, decotv::LOGIN_PASS);
        if (!ok) {
            g_startupError = "Login failed";
        } else {
            g_categories = decotv::fetchCategories();
            if (g_categories.empty()) g_startupError = "Categories empty";
        }
    }

    brls::Application::pushActivity(new HomeActivity());

    while (brls::Application::mainLoop())
        ;

    decotv::exitNetwork();
    return EXIT_SUCCESS;
}
