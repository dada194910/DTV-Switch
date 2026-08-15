// DecoTV Switch 客户端 —— 最小可运行版（libnx-only）
// 目标：验证整条链路（交叉编译 -> .nro -> 真机显示 -> 按键 -> 退出）
// 屏幕打印服务器地址横幅；按 [+] 键退出
// borealis UI 框架在下一步单独接入（依赖 switch-glfw 等，需从源码构建）
#include <switch.h>
#include <cstdio>

int main(int argc, char* argv[]) {
    // 初始化控制台输出（无需 framebuffer 初始化）
    consoleInit(nullptr);

    printf("\x1b[2J");  // 清屏
    printf("\x1b[16;12H");  // 光标移到中间
    printf("DecoTV Switch Client\n");
    printf("\x1b[17;8H");
    printf("Server: http://tv.2001002.xyz:11113\n");
    printf("\x1b[20;6H");
    printf("Press [+] to exit\n");

    // 主循环：扫按键，[+] 退出
    while (appletMainLoop()) {
        hidScanInput();
        u64 kDown = hidKeysDown(CONTROLLER_P1_AUTO);
        if (kDown & KEY_PLUS) break;
        consoleUpdate(nullptr);
    }

    consoleExit(nullptr);
    return 0;
}
