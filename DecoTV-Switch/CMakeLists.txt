cmake_minimum_required(VERSION 3.13)

# devkitA64 交叉编译工具链（由 devkitPro 提供）
include($ENV{DEVKITPRO}/cmake/Switch.cmake)

project(DecoTV LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- 源码 ----
add_executable(DecoTV
    source/main.cpp
)

# ---- 链接 libnx ----
# libnx 的导入目标固定为 libnx::libnx
target_link_libraries(DecoTV PRIVATE libnx::libnx)

# ---- NACP（元数据：名称/作者/版本，需 ASCII）----
nx_generate_nacp(DecoTV_nacp
    NAME "DecoTV"
    AUTHOR "XiaoMiu"
    VERSION "0.1.0"
)

# ---- 打包为 .nro ----
# ICON 省略：devkitPro 的 Switch.cmake 宏会使用内置默认图标
nx_generate_nro(DecoTV_nro
    OUTPUT "DecoTV.nro"
    NACP DecoTV_nacp
)
