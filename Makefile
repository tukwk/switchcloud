.DEFAULT_GOAL := all

TARGET      := scswitch
BUILD       := build

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to devkitPro>")
endif

DEVKITA64   ?= $(DEVKITPRO)/tools
PREFIX      := $(DEVKITA64)/bin/aarch64-none-elf-

CC          := $(PREFIX)gcc
CXX         := $(PREFIX)g++
LD          := $(CXX)
ELF2NRO     := $(DEVKITA64)/bin/elf2nro
NACPTOOL    := $(DEVKITA64)/bin/nacptool

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57
DEFINES     := -D__SWITCH__ -D_GNU_SOURCE
INCLUDES    := -I$(DEVKITPRO)/libnx/include -I$(DEVKITPRO)/portlibs/switch/include -Isource
CFLAGS      := -g -O2 $(ARCH) -ffunction-sections -fdata-sections -fPIE -MMD -MP $(DEFINES) $(INCLUDES)
CXXFLAGS    := $(CFLAGS) -std=c++17 -fno-rtti -fno-exceptions
SYSBASE_SPECS := $(wildcard $(DEVKITPRO)/tools/aarch64-none-elf/lib/sysbase.specs)
ifneq ($(SYSBASE_SPECS),)
LDFLAGS_EXTRA := -specs=$(SYSBASE_SPECS)
endif

LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs $(LDFLAGS_EXTRA) \
               -g $(ARCH) -fPIE \
               -L$(DEVKITPRO)/libnx/lib -L$(DEVKITPRO)/portlibs/switch/lib
# The custom newlib needs libsysbase loaded eagerly (it provides __getreent and
# the syscall stubs that libc references from lazily-pulled members).
LIBS        := -lmbedtls -lmbedx509 -lmbedcrypto \
               -lavformat -lavcodec -lswresample -lavutil -lpthread \
               -lnx -lstdc++ -lm \
               -Wl,--undefined=__syscall_gettod_r -Wl,--undefined=__syscall_clock_gettime

SOURCES_C   := source/third_party/stb_vorbis.c
SOURCES_CPP := $(filter-out source/third_party/stb_vorbis.c,$(wildcard source/*.cpp))
OFILES      := $(patsubst source/%.c,$(BUILD)/%.c.o,$(SOURCES_C)) \
               $(patsubst source/%.cpp,$(BUILD)/%.cpp.o,$(SOURCES_CPP))

DEPS        := $(OFILES:.o=.d)
-include $(DEPS)

all: $(BUILD)/$(TARGET).nro

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.c.o: source/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.cpp.o: source/%.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OFILES)
	$(LD) $(LDFLAGS) $(OFILES) $(LIBS) -o $@
	python3 tools/check_relocs.py $(PREFIX)readelf $@

$(BUILD)/$(TARGET).nacp: | $(BUILD)
	$(NACPTOOL) --create "switchcloud" "scswitch" "0.1.0" $(BUILD)/$(TARGET).nacp --titleid=0100000000000000

$(BUILD)/$(TARGET).nro: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).nacp
	$(ELF2NRO) $< $@ --nacp=$(BUILD)/$(TARGET).nacp --icon=resources/icon.jpg

dist: all
	@mkdir -p dist
	cp $(BUILD)/$(TARGET).nro dist/
	cp README.md dist/
	cd dist && zip -q -r switchcloud-$(shell date +%Y%m%d).zip $(TARGET).nro README.md

test:
	@mkdir -p $(BUILD)/host
	g++ -std=c++17 -O2 -Isource tests/test_json.cpp source/json.cpp -o $(BUILD)/host/test_json
	g++ -std=c++17 -O2 -Isource tests/test_resampler.cpp -o $(BUILD)/host/test_resampler
	$(BUILD)/host/test_json
	$(BUILD)/host/test_resampler

clean:
	rm -rf $(BUILD) dist

.PHONY: all clean dist test
