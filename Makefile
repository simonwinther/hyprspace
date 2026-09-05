PREFIX     ?= $(HOME)/.local
PLUGIN_DIR ?= $(PREFIX)/share/hyprspace

BUILD_DIR  ?= build
TARGET     := $(BUILD_DIR)/hyprspace.so
BUILD_CONFIG := $(BUILD_DIR)/.build-config

SRCS := \
	src/main.cpp \
	src/Config.cpp \
	src/Capture.cpp \
	src/DesktopDb.cpp \
	src/Focus.cpp \
	src/Overview.cpp \
	src/PassElements.cpp \
	src/Raster.cpp \
	src/Switcher.cpp \
	src/Texture.cpp

OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

PKGS := pixman-1 libdrm hyprland pangocairo cairo gdk-pixbuf-2.0 librsvg-2.0 libinput libudev wayland-server xkbcommon

# Hyprland plugins must be built with the same compiler and flags as the
# compositor; -fno-gnu-unique matters for g++ so symbols can be unloaded.
ifneq (,$(findstring g++,$(shell $(CXX) --version 2>/dev/null)))
    EXTRA_FLAGS := -fno-gnu-unique
else
    EXTRA_FLAGS :=
endif

CXXFLAGS ?= -O2
override CXXFLAGS += -std=c++2b -fPIC $(EXTRA_FLAGS) -Wall -Wno-narrowing -Wno-unused-parameter
override CXXFLAGS += $(shell pkg-config --cflags $(PKGS))

override LDFLAGS += -shared
LDLIBS   += $(shell pkg-config --libs pangocairo cairo gdk-pixbuf-2.0 librsvg-2.0)

.PHONY: all clean install uninstall check test format reload release-check dist FORCE

PLUGIN_SO := $(abspath $(PLUGIN_DIR))/hyprspace.so

all: $(TARGET)

$(TARGET): $(OBJS) scripts/atomic-output.sh
	bash scripts/atomic-output.sh "$@" $(CXX) $(LDFLAGS) $(OBJS) $(LDLIBS) -o
	@echo "built $@"

# Track compiler, flags and dependency versions as well as source changes.
# -MD also tracks system headers, including Hyprland's ABI version headers.
shquote = '$(subst ','"'"',$(1))'

$(BUILD_CONFIG): FORCE
	@mkdir -p "$(@D)"
	@set -eu; \
		build_config_tmp=$$(mktemp "$@.XXXXXX"); \
		trap 'rm -f -- "$$build_config_tmp"' EXIT; \
		trap 'exit 130' INT; trap 'exit 143' HUP TERM; \
		{ printf '%s\n' $(call shquote,$(CXX)) $(call shquote,$(CPPFLAGS)) $(call shquote,$(CXXFLAGS)) $(call shquote,$(LDFLAGS)) $(call shquote,$(LDLIBS)); \
		  $(CXX) --version; pkg-config --modversion $(PKGS); } > "$$build_config_tmp"; \
		if ! cmp -s "$@" "$$build_config_tmp"; then mv -f -- "$$build_config_tmp" "$@"; fi

$(BUILD_DIR)/%.o: src/%.cpp $(BUILD_CONFIG) scripts/atomic-output.sh Makefile
	bash scripts/atomic-output.sh "$@" $(CXX) $(CPPFLAGS) $(CXXFLAGS) -MD -MP -MF "$(@:.o=.d)" -MT "$@" -c "$<" -o

-include $(DEPS)

# Refuse to install a plugin built against a different Hyprland than the one
# running, since the plugin ABI is tied to the exact commit hash.
check: $(TARGET)
	@bash scripts/check-abi.sh "$(TARGET)"

# Install by atomic rename, never by writing over the destination.
#
# A loaded plugin is dlopen'd, so Hyprland has this exact file mmap'd as
# executable pages. Truncating and rewriting it in place — which is what a
# plain `install` or `cp` does — swaps the code out from under the running
# compositor and takes it down with SIGBUS. rename(2) replaces the directory
# entry instead: the old inode stays alive and mapped until the plugin is
# properly unloaded, so installing over a live plugin is harmless.
install: check
	bash scripts/atomic-output.sh "$(PLUGIN_SO)" install -m 0755 "$(TARGET)"
	@echo ""
	@echo "Installed to $(PLUGIN_SO)"
	@echo ""
	@echo "Add these two lines to the END of ~/.config/hypr/hyprland.conf:"
	@echo ""
	@echo "  plugin = $(PLUGIN_SO)"
	@echo "  source = $(abspath .)/contrib/hyprspace.conf"
	@echo ""
	@echo "The plugin path must be absolute — Hyprland does not expand ~ for it."
	@echo "Then: make reload"

# Swap a running plugin for a freshly built one.
#
# `hyprctl reload` only re-reads the config; it does not re-dlopen anything, so
# a plugin already in memory stays in memory. The binary has to be unloaded
# before the new one is loaded, and the unload has to happen before the file is
# replaced so Hyprland closes the handle it actually opened.
reload: $(TARGET)
	bash scripts/reload.sh "$(abspath $(TARGET))" "$(PLUGIN_SO)"

uninstall:
	rm -f $(PLUGIN_DIR)/hyprspace.so
	-rmdir $(PLUGIN_DIR) 2>/dev/null || true

test:
	$(MAKE) -C test run
	bash test/test_build.sh

release-check:
	python3 scripts/check-release.py

dist: release-check
	bash scripts/dist.sh "$(TAG)"

clean:
	rm -f -- $(OBJS) $(DEPS) "$(TARGET)" "$(BUILD_CONFIG)"
	@rmdir -- "$(BUILD_DIR)" 2>/dev/null || true
	$(MAKE) -C test clean

format:
	find src test -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 clang-format -i
