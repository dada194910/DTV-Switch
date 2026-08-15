// DecoTV Switch 客户端 —— P1：borealis UI 框架启动页（deko3d 版 API）
// 暗色主题（borealis 默认），标题栏 + 居中服务器地址
// [+] 键退出：setGlobalQuit(true) 全局注册 START 键退出
#include <borealis.hpp>

// 启动页 Activity：返回根视图（AppletFrame 标题栏 + 居中 Label）
class StartupActivity : public brls::Activity {
  public:
    brls::View* createContentView() override {
        brls::AppletFrame* frame = new brls::AppletFrame();
        frame->setTitle("DecoTV");

        brls::Label* label = new brls::Label();
        label->setText("DecoTV client  http://tv.2001002.xyz:11113");
        label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        label->setVerticalAlign(brls::VerticalAlign::CENTER);
        label->setFontSize(24.0f);

        frame->setContentView(label);
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
    brls::Application::pushActivity(new StartupActivity());

    while (brls::Application::mainLoop())
        ;

    return EXIT_SUCCESS;
}
