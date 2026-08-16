#---------------------------------------------------------------------------------
# DecoTV Switch 客户端 — classic devkitPro Makefile（集成 borealis）
# borealis = xfangfang fork（wiliwili 分支 @5f08b286, deko3d 后端）
# 该 fork 的 ScrollingFrame 稳定可用（wiliwili 真机验证），替代官方 20e2d33
# 集成文件：mk/borealis_wiliwili.mk（因 fork 无自带 borealis.mk，按 CMake 配置适配）
# borealis 克隆到 library/borealis（switch-libpulsar 等 vendored，无需子模块）
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitPro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
TARGET		:=	DecoTV
BUILD		:=	build
SOURCES		:=	source source/platform source/player
DATA		:=
INCLUDES	:=	include
# borealis 的 resources（字体/着色器/i18n）打进 romfs
ROMFS		:=	library/borealis/resources

APP_TITLE	:=	DecoTV
APP_AUTHOR	:=	XiaoMiu
# 版本号从 VERSION 文件读取（铁律：每次构建递增），格式如 1.00 / 1.01
APP_VERSION	:=	$(shell cat VERSION 2>/dev/null || echo "0.1.0")

# borealis 配置：BOREALIS_PATH 必须为相对 TOPDIR 的路径
BOREALIS_PATH	:=	library/borealis
# deko3d 着色器编译产物输出目录（相对 ROMFS）
OUT_SHADERS	:=	shaders

#---------------------------------------------------------------------------------
# 代码生成选项（arch 与 borealis 示例一致；borealis 需要 RTTI/异常，去掉 -fno-*）
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

# -include optional: nanovg-deko3d 的 dk_renderer.hpp 用了 std::optional 但没 #include <optional>
# （borealis 自身 CI 靠传递包含碰巧可用，GCC 15 下必现编译错误，这里强制预包含兜底）
CXXFLAGS	:= $(CFLAGS) -std=c++1z -O2 -Wno-volatile -include optional

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# 网络层：libcurl（switch-curl，SSL 后端 libnx）+ zlib
# P4b 播放：libmpv（switch-libmpv_deko3d 预编译包）静态依赖链很长
# （ffmpeg/placebo/libarchive/lua/jpeg/dav1d...），直接用 mpv.pc 的 Libs.private
# 完整链接参数（pkg-config --static），避免手写漏库
LIBS	:= -lcurl $(shell pkg-config --static --libs mpv 2>/dev/null) -lz -lm

#---------------------------------------------------------------------------------
# 库目录列表（顶层含 include 和 lib）；switch-deko3d 装到 $(PORTLIBS)
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# 引入 borealis（xfangfang fork 适配版；追加源码/头文件/链接；必须在 LIBDIRS 之后）
#---------------------------------------------------------------------------------
include $(TOPDIR)/mk/borealis_wiliwili.mk

#---------------------------------------------------------------------------------
# 以下为标准规则（含 borealis 的 GLSL 着色器编译），无需修改
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
GLSLFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.glsl)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# 用 CXX 链接 C++ 项目，CC 链接纯 C 项目
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

ifneq ($(strip $(ROMFS)),)
	ROMFS_TARGETS :=
	ROMFS_FOLDERS :=
	ifneq ($(strip $(OUT_SHADERS)),)
		ROMFS_SHADERS := $(ROMFS)/$(OUT_SHADERS)
		ROMFS_TARGETS += $(patsubst %.glsl, $(ROMFS_SHADERS)/%.dksh, $(GLSLFILES))
		ROMFS_FOLDERS += $(ROMFS_SHADERS)
	endif

	export ROMFS_DEPS := $(foreach file,$(ROMFS_TARGETS),$(CURDIR)/$(file))
endif

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean

#---------------------------------------------------------------------------------
all: $(ROMFS_TARGETS) | $(BUILD)
	@MSYS2_ARG_CONV_EXCL="-D;$(MSYS2_ARG_CONV_EXCL)" $(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

ifneq ($(strip $(ROMFS_TARGETS)),)

$(ROMFS_TARGETS): | $(ROMFS_FOLDERS)

$(ROMFS_FOLDERS):
	@mkdir -p $@

$(ROMFS_SHADERS)/%_vsh.dksh: %_vsh.glsl
	@echo {vert} $(notdir $<)
	@uam -s vert -o $@ $<

$(ROMFS_SHADERS)/%_tcsh.dksh: %_tcsh.glsl
	@echo {tess_ctrl} $(notdir $<)
	@uam -s tess_ctrl -o $@ $<

$(ROMFS_SHADERS)/%_tesh.dksh: %_tesh.glsl
	@echo {tess_eval} $(notdir $<)
	@uam -s tess_eval -o $@ $<

$(ROMFS_SHADERS)/%_gsh.dksh: %_gsh.glsl
	@echo {geom} $(notdir $<)
	@uam -s geom -o $@ $<

$(ROMFS_SHADERS)/%_fsh.dksh: %_fsh.glsl
	@echo {frag} $(notdir $<)
	@uam -s frag -o $@ $<

$(ROMFS_SHADERS)/%.dksh: %.glsl
	@echo {comp} $(notdir $<)
	@uam -s comp -o $@ $<

endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
ifeq ($(strip $(APP_JSON)),)
	@rm -fr $(BUILD) $(ROMFS_FOLDERS) $(TARGET).nro $(TARGET).nacp $(TARGET).elf
else
	@rm -fr $(BUILD) $(ROMFS_FOLDERS) $(TARGET).nsp $(TARGET).nso $(TARGET).npdm $(TARGET).elf
endif

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# 主目标
#---------------------------------------------------------------------------------
ifeq ($(strip $(APP_JSON)),)

all	:	$(OUTPUT).nro

ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp $(ROMFS_DEPS)
else
$(OUTPUT).nro	:	$(OUTPUT).elf $(ROMFS_DEPS)
endif

else

all	:	$(OUTPUT).nsp

$(OUTPUT).nsp	:	$(OUTPUT).nso $(OUTPUT).npdm

$(OUTPUT).nso	:	$(OUTPUT).elf

endif

$(OUTPUT).elf	:	$(OFILES)

$(OFILES_SRC)	: $(HFILES_BIN)

#---------------------------------------------------------------------------------
# 二进制数据文件规则（如需要扩展请自行添加）
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
