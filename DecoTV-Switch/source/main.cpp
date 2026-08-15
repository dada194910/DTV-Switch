// DecoTV Switch 客户端 —— 最小可运行版（libnx-only）
// 目标：验证整条链路（交叉编译 -> .nro -> 真机显示 -> 按键 -> 退出）
// 屏幕打印服务器地址横幅；按 [+] 键退出
// 注意：libnx 4.x 已移除经典 HID API（hidScanInput/hidKeysDown/KEY_*），
//       使用新版 HidNpad API：
//       - hidInitialize() 只在启动时初始化基础服务+共享内存，**不激活 Npad**，
//         必须再显式调用 hidInitializeNpad() 才能读到按键
//       - 掌机模式按键 id 是 HidNpadIdType_Handheld(0x20)，不是 No1
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

    // 初始化 HID（新版 libnx API）
    hidInitialize();
    // 激活 Npad 子系统——hidInitialize() 不会激活它，缺了这行读不到任何按键
    hidInitializeNpad();

    // 主循环：[+] 退出
    // 兼容三种形态：Pro 手柄(FullKey@No1) / 掌机(Handheld@0x20) / 分离双 Joy-Con(JoyDual@No1)
    while (appletMainLoop()) {
        bool exitRequested = false;

        HidNpadFullKeyState fkState;
        if (hidGetNpadStatesFullKey(HidNpadIdType_No1, &fkState, 1) > 0 &&
            (fkState.buttons & HidNpadButton_Plus))
            exitRequested = true;

        HidNpadHandheldState hhState;
        if (hidGetNpadStatesHandheld(HidNpadIdType_Handheld, &hhState, 1) > 0 &&
            (hhState.buttons & HidNpadButton_Plus))
            exitRequested = true;

        HidNpadJoyDualState jdState;
        if (hidGetNpadStatesJoyDual(HidNpadIdType_No1, &jdState, 1) > 0 &&
            (jdState.buttons & HidNpadButton_Plus))
            exitRequested = true;

        if (exitRequested) break;
        consoleUpdate(nullptr);
    }

    consoleExit(nullptr);
    return 0;
}
