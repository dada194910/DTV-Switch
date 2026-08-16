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
#include "player/mpv_player.hpp"

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

// ---- 视频画面视图（P4b：libmpv 软渲染帧 → nanovg 纹理绘制）----
class PlayerView : public brls::View {
  public:
    void setPlayer(mpv_player::Player* p) { m_player = p; }

    void draw(NVGcontext* vg, float x, float y, float w, float h, brls::Style style,
              brls::FrameContext* ctx) override {
        if (m_player && m_player->hasFrame()) {
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
        brls::View::draw(vg, x, y, w, h, style, ctx);
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

// ---- 播放器页（P4b：libmpv 播放 m3u8/mp4，A 暂停/播放，B 退出）----
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
        m_info->setFocusable(true);  // 保活焦点锚点
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

// ---- 详情页（P4a：直调 TVBox 源搜索，显示各源播放地址；播放 P4b 实现）----
// 定义在 VideoListActivity 之前（后者行点击要 new 本类）
class DetailActivity : public brls::Activity {
  public:
    explicit DetailActivity(decotv::VideoItem v) : m_video(std::move(v)) {}

    brls::View* createContentView() override {
        m_info = new brls::Label();
        m_info->setFontSize(20);
        m_info->setFocusable(true);  // 永存活焦点锚点（v1.14 教训）

        m_listBox = new brls::Box(brls::Axis::COLUMN);

        auto* layout = new brls::Box(brls::Axis::COLUMN);
        layout->addView(m_info);
        layout->addView(m_listBox);

        auto* frame = new brls::AppletFrame(layout);
        frame->setTitle("DecoTV - " + m_video.title);
        frame->registerAction("返回", brls::ControllerButton::BUTTON_B, [](brls::View*) {
            brls::Application::popActivity();
            return true;
        });

        load();
        return frame;
    }

  private:
    void load() {
        // 焦点安全：删行前转移到保活锚点（v1.14 教训）
        brls::Application::giveFocus(m_info);
        for (auto* r : m_rows) m_listBox->removeView(r);
        m_rows.clear();

        m_info->setText("正在搜索各播放源...");

        // 拉源列表，搜前几个源（顺序同步请求，够用）
        auto sources = decotv::fetchTvboxSources();
        int count = 0;
        int limit = (int)sources.size() < 6 ? (int)sources.size() : 6;

        for (int i = 0; i < limit; ++i) {
            auto hits = decotv::searchTvboxSource(sources[i], m_video.title);
            for (auto& h : hits) {
                std::string sub = h.playUrl;
                if (sub.size() > 48) sub = sub.substr(0, 48) + "...";
                auto* row = makeRow(h.sourceName + "  " + h.vodName, sub, [h](brls::View*) {
                    brls::Application::pushActivity(new PlayerActivity(h.playUrl));
                    return true;
                });
                m_listBox->addView(row);
                m_rows.push_back(row);
                ++count;
            }
        }

        if (count == 0) {
            auto* lbl = new brls::Label();
            lbl->setText("未找到可用播放源（或网络失败）");
            lbl->setFontSize(22);
            m_listBox->addView(lbl);
            m_rows.push_back(lbl);
        }
        m_info->setText("找到 " + std::to_string(count) + " 个播放源  A 选中  B 返回");
        if (!m_rows.empty()) brls::Application::giveFocus(m_rows[0]);
    }

    decotv::VideoItem m_video;
    brls::Label* m_info = nullptr;
    brls::Box* m_listBox = nullptr;
    std::vector<brls::View*> m_rows;
};

// ---- 影片列表页 ----
class VideoListActivity : public brls::Activity {
  public:
    explicit VideoListActivity(decotv::Category c) : m_cat(std::move(c)) {}

    brls::View* createContentView() override {
        m_info = new brls::Label();
        m_info->setFontSize(20);
        // v1.14 修复：m_info 作为"永存活焦点锚点"（翻页/切筛选删行前先给它焦点，
        // 否则被删除行持有 currentFocus → 下一帧访问已删除 View → use-after-free → 2168-0001）
        m_info->setFocusable(true);

        m_primaryBar = new brls::Box(brls::Axis::ROW);

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

        m_listBox = new brls::Box(brls::Axis::COLUMN);  // 列表容器：Box 分页（无滚动）

        auto* layout = new brls::Box(brls::Axis::COLUMN);
        layout->addView(m_info);
        layout->addView(m_primaryBar);
        layout->addView(m_listBox);

        // wiliwili 分支 AppletFrame::setContentView 是 protected，改用构造传参
        auto* frame = new brls::AppletFrame(layout);
        frame->setTitle("DecoTV - " + m_cat.label);
        frame->registerAction("返回", brls::ControllerButton::BUTTON_B, [](brls::View*) {
            brls::Application::popActivity();
            return true;
        });
        // 翻页注册在 frame 上（v1.12 起不用 ScrollingFrame——它是 20e2d33 启动崩溃元凶；
        // 切 xfangfang fork 后 ScrollingFrame 可用，后续 P3 列表可升级为滚动）
        frame->registerAction("上一页", brls::ControllerButton::BUTTON_LEFT, [this](brls::View*) {
            if (m_page > 0) { m_page--; loadPage(); }
            return true;
        });
        frame->registerAction("下一页", brls::ControllerButton::BUTTON_RIGHT, [this](brls::View*) {
            m_page++;
            loadPage();
            return true;
        });

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

        // v1.14 关键修复：删除旧行前先把焦点转移到保活锚点（m_info）。
        // 原因：若当前焦点在某个将被删除的列表行上，removeView delete 该行后
        // currentFocus 成为悬垂指针，下一帧输入/渲染访问它 → 调用已释放对象的
        // 虚函数 → vtable 跳转无效地址 → Instruction Abort (2168-0001)。
        // 必须先转移焦点（giveFocus 会对旧焦点调 onFocusLost，此时旧焦点必须还活着），
        // 再删除旧行。
        brls::Application::giveFocus(m_info);

        // 清掉旧行：Box::removeView 会 delete 子视图（删除的不是正在执行回调的视图本身，安全）
        for (auto* r : m_rows) m_listBox->removeView(r);
        m_rows.clear();

        auto items = decotv::fetchDoubanList(kind, category, type, PAGE_SIZE, m_page * PAGE_SIZE);

        if (items.empty()) {
            auto* lbl = new brls::Label();
            lbl->setText("暂无内容或加载失败");
            lbl->setFontSize(22);
            m_listBox->addView(lbl);
            m_rows.push_back(lbl);
        } else {
            for (auto& v : items) {
                std::string sub = v.year;
                if (!sub.empty() && !v.rate.empty()) sub += "  ⭐" + v.rate;
                else if (!v.rate.empty()) sub = "⭐" + v.rate;
                auto* row = makeRow(v.title, sub, [v](brls::View*) {
                    brls::Application::pushActivity(new DetailActivity(v));
                    return true;
                });
                m_listBox->addView(row);
                m_rows.push_back(row);
            }
            // 重建后聚焦第一行（焦点安全 + 翻页体验：焦点直接在新列表上）
            brls::Application::giveFocus(m_rows[0]);
        }
    }

    static constexpr int PAGE_SIZE = 8;  // 小分页：一屏显示完，无需滚动
    decotv::Category m_cat;
    int m_primaryIndex = 0;
    int m_page = 0;
    brls::Label* m_info = nullptr;
    brls::Box* m_primaryBar = nullptr;
    brls::Box* m_listBox = nullptr;
    std::vector<FilterTab> m_barTabs;
    std::vector<brls::View*> m_rows;
};

// ---- 首页 ----
// v1.12：恢复分类点击进列表（makeRow: focusable + click），但不使用 ScrollingFrame
// （v1.09 启动崩溃元凶已确认），4 个分类用 Box 直接排布，无需滚动。
class HomeActivity : public brls::Activity {
  public:
    brls::View* createContentView() override {
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
                box->addView(makeRow(c.label, "A 进入浏览", [c](brls::View*) {
                    brls::Application::pushActivity(new VideoListActivity(c));
                    return true;
                }));
            }
        }

        // wiliwili 分支 AppletFrame::setContentView 是 protected，改用构造传参
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
