// DecoTV Switch 客户端 —— 最小可运行版（libnx-only）
// 目标：验证整条链路（交叉编译 -> .nro -> 真机显示 -> 按键 -> 退出）
// 屏幕打印服务器地址横幅；按 [+] 键退出
//
// 输入采用 libnx 官方推荐方式：pad.h 抽象层
// （参照 switch-examples/hid/read-controls 官方示例）
// - padInitialize 内部会自动激活 Npad 子系统（hidInitializeNpad）
// - padUpdate 内部自动处理掌机(Handheld)/Pro(FullKey)/分离双 Joy-Con(JoyDual/Left/Right) 全部形态
// - 直接戳 hidGetNpadStates* 底层 API 容易踩连接检查/激活时序的坑，官方 pad 层已封装
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

    // 配置输入：单玩家 + 标准手柄样式（FullKey/Handheld/JoyDual/JoyLeft/JoyRight）
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    // 默认手柄：自动涵盖掌机模式输入 + 第一台已连接手柄
    PadState pad;
    padInitializeDefault(&pad);

    // 主循环：[+] 退出
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        if (kDown & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
    }

    consoleExit(nullptr);
    return 0;
}
