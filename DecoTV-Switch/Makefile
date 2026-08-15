#---------------------------------------------------------------------------------
# DecoTV Switch 客户端 — 经典 devkitPro Makefile 构建（集成 borealis UI）
# 蓝本：switchbrew/nx-hbmenu 的 Makefile.nx + natinusala/borealis 示例 Makefile
# 依赖 $(DEVKITPRO)/libnx/switch_rules，自动完成编译/链接/nacp/nro 打包
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitPro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET: 输出名 | BUILD: 中间产物目录 | SOURCES: 源码目录
# DATA: 二进制数据目录 | INCLUDES: 头文件目录 | ROMFS: romfs 资源目录(可选)
#---------------------------------------------------------------------------------
TARGET		:=	DecoTV
BUILD		:=	build
SOURCES		:=	source
DATA		:=
INCLUDES	:=	include
# borealis 的 resources（字体/着色器/i18n）打进 romfs
ROMFS		:=	library/borealis/resources

APP_TITLE	:=	DecoTV
APP_AUTHOR	:=	XiaoMiu
# 版本号从 VERSION 文件读取（铁律：每次构建递增），格式如 1.00 / 1.01
APP_VERSION	:=	$(shell cat VERSION 2>/dev/null || echo "0.1.0")

# borealis 配置
# 用 $(TOPDIR) 而非 $(CURDIR)：子 make 阶段 CURDIR 指向 build/，TOPDIR 恒为项目根
BOREALIS_PATH		:=	$(TOPDIR)/library/borealis
BOREALIS_RESOURCES	:=	romfs:/

#---------------------------------------------------------------------------------
# 代码生成选项（arch 与 borealis 示例一致；borealis 需要 RTTI/异常，去掉 -fno-*）
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -O2 -ffunction-sections \
			$(ARCH) $(DEFINES)

CFLAGS	+=	$(INCLUDE) -D__SWITCH__

CXXFLAGS	:= $(CFLAGS) -std=c++1z -O2 -Wno-volatile \
			-DBOREALIS_RESOURCES="\"$(BOREALIS_RESOURCES)\""

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lnx -lm

#---------------------------------------------------------------------------------
# 库目录列表（顶层含 include 和 lib）；switch-glfw/switch-mesa 装到 $(PORTLIBS)
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# 引入 borealis（追加其源码/头文件目录和 glfw/EGL 链接；必须在 LIBDIRS 之后）
#---------------------------------------------------------------------------------
include $(TOPDIR)/library/borealis/library/borealis.mk

#---------------------------------------------------------------------------------
# 以下为标准规则，除非要加新文件扩展名，否则无需修改
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

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

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

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# 主目标
#---------------------------------------------------------------------------------
ifeq ($(strip $(NO_NACP)),)
all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp
else
all	:	$(OUTPUT).elf
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
