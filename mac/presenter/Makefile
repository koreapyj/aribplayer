# libffpmetalpresenter (mac/presenter): the Metal FFPPresenter of libffplay (ffplay.h), and its test harness.
#
#   make            builds build/libffpmetalpresenter.dylib and build/ffp_metal_test
#   make test MEDIA=/path/to/clips   runs the harness (tests a-i)
#   make test-validate MEDIA=/path/to/clips   the same under the Metal debug layer + shader validation
#
# Needs: macOS arm64, Xcode with the Metal toolchain (DEVELOPER_DIR), the FFmpeg headers of the darwin build
# (FFINC) and its dylibs (FFLIB, rpath-based): ffmpeg/dist/darwin-arm64 (ffmpeg/darwin/build-ffplay.sh).
# MEDIA must be supplied as a make argument or environment variable; it has no default.

ROOT    := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
HERE    := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
FFLIB   ?= $(ROOT)/ffmpeg/dist/darwin-arm64
FFINC   ?= $(FFLIB)/include
BUILD   ?= $(HERE)/build

ifneq ($(filter test test-validate,$(MAKECMDGOALS)),)
ifeq ($(strip $(MEDIA)),)
$(error MEDIA is required; use make test MEDIA=/path/to/clips)
endif
endif

export DEVELOPER_DIR ?= /Applications/Xcode.app/Contents/Developer
CC      := xcrun -sdk macosx clang
METAL   := xcrun -sdk macosx metal
METALLIB:= xcrun -sdk macosx metallib
SDK     := $(shell xcrun -sdk macosx --show-sdk-path)
# The minimum macOS of the dylib, the harness and the embedded metallib: mac/cmake/MacDeploymentTarget.cmake's value.
MACOS_MIN := 13.3

CFLAGS  := -isysroot $(SDK) -mmacosx-version-min=$(MACOS_MIN) -arch arm64 -O2 -g -fobjc-arc \
           -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -Werror=unguarded-availability-new \
           -I$(HERE)/include -I$(FFLIB) -I$(FFINC)
LDFLAGS := -isysroot $(SDK) -mmacosx-version-min=$(MACOS_MIN) -arch arm64 -L$(FFLIB) -Wl,-rpath,$(FFLIB)
FRAMEWORKS := -framework Foundation -framework Metal -framework QuartzCore -framework CoreVideo \
              -framework CoreGraphics -framework IOSurface

LIB     := $(BUILD)/libffpmetalpresenter.dylib
TEST    := $(BUILD)/ffp_metal_test

all: $(LIB) $(TEST)

$(BUILD):
	mkdir -p $(BUILD)

# the shaders: .metal -> .air -> .metallib, embedded into the dylib as a byte array. The AIR targets MACOS_MIN; without
# -mmacosx-version-min the metal compiler targets the SDK's macOS.
$(BUILD)/ffp_metal_shaders.air: src/ffp_metal_shaders.metal | $(BUILD)
	$(METAL) -mmacosx-version-min=$(MACOS_MIN) -c -O2 -o $@ $<

$(BUILD)/ffp_metal_shaders.metallib: $(BUILD)/ffp_metal_shaders.air
	$(METALLIB) -o $@ $<

$(BUILD)/ffp_metal_shaders_blob.c: $(BUILD)/ffp_metal_shaders.metallib
	{ echo 'const unsigned char ffp_metal_shaders_metallib[] = {'; xxd -i < $<; echo '};'; \
	  echo 'const unsigned int ffp_metal_shaders_metallib_len = sizeof(ffp_metal_shaders_metallib);'; } > $@

$(BUILD)/ffp_metal_shaders_blob.o: $(BUILD)/ffp_metal_shaders_blob.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/ffp_metal_presenter.o: src/ffp_metal_presenter.m include/ffp_metal_presenter.h $(FFLIB)/ffplay.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIB): $(BUILD)/ffp_metal_presenter.o $(BUILD)/ffp_metal_shaders_blob.o
	$(CC) $(LDFLAGS) -dynamiclib -install_name @rpath/libffpmetalpresenter.dylib -o $@ $^ \
	    $(FRAMEWORKS) -lavutil

$(BUILD)/ffp_metal_test.o: test/ffp_metal_test.m include/ffp_metal_presenter.h $(FFLIB)/ffplay.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TEST): $(BUILD)/ffp_metal_test.o $(LIB)
	$(CC) $(LDFLAGS) -Wl,-rpath,@executable_path -o $@ $< -L$(BUILD) -lffpmetalpresenter \
	    $(FRAMEWORKS) -framework Cocoa -lavcodec -lavformat -lavutil -lswscale

test: all
	$(TEST) --media "$(MEDIA)"

test-validate: all
	MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_DEBUG_LAYER_ERROR_MODE=assert $(TEST) --media "$(MEDIA)"

clean:
	rm -rf $(BUILD)

.PHONY: all test test-validate clean
