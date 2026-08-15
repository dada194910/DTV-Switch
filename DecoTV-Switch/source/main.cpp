// DecoTV Switch 客户端 —— P1：borealis UI 框架启动页
// 暗色主题（borealis 默认），标题栏 + 居中服务器地址
// [+] 键退出由 borealis 内置处理（application.cpp 默认注册 PLUS -> quit）
#include <borealis.hpp>

int main(int argc, char* argv[]) {
    brls::Logger::setLogLevel(brls::LogLevel::LOG_INFO);

    // 初始化 borealis（默认暗色主题）
    if (!brls::Application::init("DecoTV")) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    // 启动页：顶部标题栏 + 居中显示服务器地址
    // 用纯 ASCII 文本，规避中文字体依赖（中文 UI + 字体留到后续阶段）
    brls::AppletFrame* root = new brls::AppletFrame(true, true);
    root->setTitle("DecoTV");

    brls::Label* label = new brls::Label(brls::LabelStyle::REGULAR,
        "DecoTV client  http://tv.2001002.xyz:11113");
    label->setHorizontalAlign(NVG_ALIGN_CENTER);
    label->setVerticalAlign(NVG_ALIGN_MIDDLE);
    label->setFontSize(24);

    root->setContentView(label);

    // 压入根视图并进入主循环（[+] 退出）
    brls::Application::pushView(root);

    while (brls::Application::mainLoop())
        ;

    return EXIT_SUCCESS;
}
