#---------------------------------------------------------------------------------
# borealis (xfangfang fork — wiliwili 分支 @5f08b286, deko3d 后端) 集成
#
# 背景：natinusala 官方 20e2d33（deko3d 重构中）的 ScrollingFrame 在本项目环境
# 启动即崩（2168-0002）。wiliwili 长期真机使用 xfangfang fork（同 API 形态：
# init()/createWindow/pushActivity/setGlobalQuit/Box(Axis)），其 ScrollingFrame
# 稳定可用。该 fork 无自带 borealis.mk（wiliwili 用 CMake），此处按官方 20e2d33
# borealis.mk 结构 + wiliwili 分支 library/CMakeLists.txt 的 Switch 配置适配。
#
# 与 20e2d33 的关键差异：
#   - nanovg 路径：lib/extern/nanovg/{nanovg.c,deko3d/,deko3d/framework/,deko3d/shaders/}
#   - 需要 libromfs（i18n 用 romfs:: 读取）
#   - LIBS 含 -lnx
#   - dk_renderer.hpp 已自带 <optional>；switch_ime 不再用 swkbdConfigSetStringLenMaxExt
#---------------------------------------------------------------------------------
# borealis 仓库根在 $(BOREALIS_PATH)（= library/borealis），其源码在仓库的
# library/ 子目录（lib/ + include/）—— 所以 current_dir 要带 /library
current_dir := $(BOREALIS_PATH)/library

LIBS := -ldeko3d -lnx -lm $(LIBS)

include $(TOPDIR)/$(BOREALIS_PATH)/lib/extern/switch-libpulsar/deps.mk

SOURCES := $(SOURCES) \
	$(current_dir)/lib/core \
	$(current_dir)/lib/views \
	$(current_dir)/lib/platforms/switch \
	$(current_dir)/lib/extern/nanovg \
	$(current_dir)/lib/extern/nanovg/deko3d \
	$(current_dir)/lib/extern/nanovg/deko3d/framework \
	$(current_dir)/lib/extern/nanovg/deko3d/shaders \
	$(current_dir)/lib/extern/libretro-common/compat \
	$(current_dir)/lib/extern/libretro-common/encodings \
	$(current_dir)/lib/extern/libretro-common/features \
	$(current_dir)/lib/extern/nxfmtwrapper \
	$(current_dir)/lib/extern/yoga/src/yoga/event \
	$(current_dir)/lib/extern/yoga/src/yoga \
	$(current_dir)/lib/extern/tinyxml2 \
	$(current_dir)/lib/extern/libromfs/lib/source \
	$(addprefix $(current_dir)/lib/extern/switch-libpulsar/, $(PLSR_SOURCES))

INCLUDES := $(INCLUDES) \
	$(current_dir)/include \
	$(current_dir)/lib/extern/fmt/include \
	$(current_dir)/lib/extern/yoga/src \
	$(current_dir)/lib/extern/tweeny/include \
	$(current_dir)/lib/extern/libromfs/lib/include \
	$(current_dir)/include/borealis/extern \
	$(current_dir)/include/borealis/extern/tinyxml2 \
	$(addprefix $(current_dir)/lib/extern/switch-libpulsar/, $(PLSR_INCLUDES))

CXXFLAGS := $(CXXFLAGS) -DYG_ENABLE_EVENTS -fdata-sections -DBRLS_RESOURCES="\"romfs:/\""
