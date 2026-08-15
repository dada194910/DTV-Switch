// DecoTV Switch 客户端 —— P2：borealis UI + 网络层自检
// 启动时：初始化网络 → 登录（或复用已有 cookie）→ 拉分类 → 界面显示结果
// [+] 退出：setGlobalQuit(true)
#include <borealis.hpp>
#include <string>
#include <vector>

#include "platform/api_decotv.h"

// 启动前网络自检结果（main 里填充，Activity 显示）
static std::string g_statusTitle;   // 主状态（大字号）
static std::string g_statusDetail;  // 详情（可读字号）

class StartupActivity : public brls::Activity {
  public:
    brls::View* createContentView() override {
        brls::AppletFrame* frame = new brls::AppletFrame();
        frame->setTitle("DecoTV");

        brls::Box* box = new brls::Box(brls::Axis::COLUMN);

        // 主状态：大字号（UI 约束：字体不要太小）
        brls::Label* title = new brls::Label();
        title->setText(g_statusTitle);
        title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        title->setFontSize(32.0f);

        // 详情：仍保证可读
        brls::Label* detail = new brls::Label();
        detail->setText(g_statusDetail);
        detail->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        detail->setFontSize(22.0f);

        box->addView(title);
        box->addView(detail);

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

    // ---- P2 网络自检：登录 + 拉分类 ----
    if (!decotv::initNetwork()) {
        g_statusTitle = "Network init failed";
        g_statusDetail = "init result: " + std::to_string(decotv::g_netInitResult);
    } else {
        bool ok = decotv::hasSavedLogin();
        if (!ok) {
            g_statusTitle = "Logging in...";
            ok = decotv::login(decotv::LOGIN_USER, decotv::LOGIN_PASS);
        }

        if (ok) {
            auto cats = decotv::fetchCategories();
            g_statusTitle = "Login OK, cookie saved";
            if (!cats.empty()) {
                std::string names;
                for (const auto& c : cats) names += c.label + "  ";
                g_statusDetail = "Categories(" + std::to_string(cats.size()) + "): " + names;
            } else {
                g_statusDetail = "Categories fetch failed";
            }
        } else {
            g_statusTitle = "Login failed";
            g_statusDetail = "Check server: " + std::string(decotv::BASE_URL);
        }
    }

    brls::Application::pushActivity(new StartupActivity());

    while (brls::Application::mainLoop())
        ;

    decotv::exitNetwork();
    return EXIT_SUCCESS;
}
