PREFIX     ?= $(HOME)/.local
PLUGIN_DIR ?= $(PREFIX)/share/hyprspace

BUILD_DIR  ?= build
TARGET     := $(BUILD_DIR)/hyprspace.so
BUILD_CONFIG := $(BUILD_DIR)/.build-config
PROTOCOL_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
INTEGRATION_ARGS ?=
HEADLESS_PC = PKG_CONFIG_PATH="$(abspath $(BUILD_DIR))/test-tools/wlroots/usr/lib/pkgconfig:$${PKG_CONFIG_PATH:-}" pkg-config --define-prefix

SRCS := \
	src/main.cpp \
	src/Config.cpp \
	src/Capture.cpp \
	src/DesktopDb.cpp \
	src/Focus.cpp \
	src/Overview.cpp \
	src/OverviewSession.cpp \
	src/CompositorHooks.cpp \
	src/Launch.cpp \
	src/PassElements.cpp \
	src/Raster.cpp \
	src/Switcher.cpp \
	src/Texture.cpp

OBJS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

PKGS := pixman-1 libdrm hyprland pangocairo cairo gdk-pixbuf-2.0 librsvg-2.0 libinput libudev libeis-1.0 wayland-server xkbcommon

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

.PHONY: all clean install install-assets assets uninstall check test integration-test integration-fixtures companions format reload release-check dist FORCE

PLUGIN_SO := $(abspath $(PLUGIN_DIR))/hyprspace.so

all: $(TARGET) assets

assets:
	mkdir -p "$(BUILD_DIR)/launch-bin"
	bash scripts/atomic-output.sh "$(BUILD_DIR)/hyprspace-launch" install -m 0755 contrib/hyprspace-launch
	@for name in uwsm-app uwsm app2unit; do ln -sfn ../hyprspace-launch "$(BUILD_DIR)/launch-bin/$$name"; done

install-assets: assets
	mkdir -p "$(PLUGIN_DIR)/launch-bin" "$(PREFIX)/bin"
	bash scripts/atomic-output.sh "$(PLUGIN_DIR)/hyprspace-launch" install -m 0755 contrib/hyprspace-launch
	@for name in uwsm-app uwsm app2unit; do ln -sfn ../hyprspace-launch "$(PLUGIN_DIR)/launch-bin/$$name"; done
	ln -sfn "$(abspath $(PLUGIN_DIR))/hyprspace-launch" "$(PREFIX)/bin/hyprspace-launch"

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
install: check install-assets
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
reload: $(TARGET) install-assets
	bash scripts/reload.sh "$(abspath $(TARGET))" "$(PLUGIN_SO)"

uninstall:
	rm -f -- "$(PLUGIN_DIR)/hyprspace.so" "$(PLUGIN_DIR)/hyprspace-launch"
	@for name in uwsm-app uwsm app2unit; do rm -f -- "$(PLUGIN_DIR)/launch-bin/$$name"; done
	@if [ "$$(readlink "$(PREFIX)/bin/hyprspace-launch")" = "$(abspath $(PLUGIN_DIR))/hyprspace-launch" ]; then rm -f -- "$(PREFIX)/bin/hyprspace-launch"; fi
	-rmdir -- "$(PLUGIN_DIR)/launch-bin" "$(PLUGIN_DIR)" 2>/dev/null || true

test: assets
	$(MAKE) -C test run
	bash test/test_build.sh
	python3 test/test_launch_helper.py
	python3 test/test_integration_runner.py

$(BUILD_DIR)/test-headless: test/integration/headless.c scripts/atomic-output.sh Makefile
	@$(HEADLESS_PC) --exists wlroots-0.20 wayland-server pixman-1 || { echo "Background integration tests require wlroots-0.20 development files." >&2; exit 1; }
	mkdir -p "$(BUILD_DIR)"
	wayland-scanner server-header "$(PROTOCOL_DIR)/stable/xdg-shell/xdg-shell.xml" "$(BUILD_DIR)/xdg-shell-protocol.h"
	bash scripts/atomic-output.sh "$@" $(CC) -Wall -Wextra -I"$(BUILD_DIR)" test/integration/headless.c $$($(HEADLESS_PC) --cflags --libs wlroots-0.20 wayland-server pixman-1) -Wl,--disable-new-dtags -Wl,-rpath,"$$($(HEADLESS_PC) --variable=libdir wlroots-0.20)" -o

integration-fixtures: $(BUILD_DIR)/test-headless
	mkdir -p "$(BUILD_DIR)"
	wayland-scanner client-header test/integration/virtual-pointer.xml "$(BUILD_DIR)/virtual-pointer.h"
	wayland-scanner private-code test/integration/virtual-pointer.xml "$(BUILD_DIR)/virtual-pointer.c"
	wayland-scanner client-header test/integration/virtual-keyboard.xml "$(BUILD_DIR)/virtual-keyboard.h"
	wayland-scanner private-code test/integration/virtual-keyboard.xml "$(BUILD_DIR)/virtual-keyboard.c"
	$(CC) -I"$(BUILD_DIR)" test/integration/pointer.c "$(BUILD_DIR)/virtual-pointer.c" "$(BUILD_DIR)/virtual-keyboard.c" -lwayland-client -lxkbcommon -lm -o "$(BUILD_DIR)/test-pointer"
	wayland-scanner client-header test/integration/input-method.xml "$(BUILD_DIR)/input-method.h"
	wayland-scanner private-code test/integration/input-method.xml "$(BUILD_DIR)/input-method.c"
	$(CC) -I"$(BUILD_DIR)" test/integration/ime.c "$(BUILD_DIR)/input-method.c" -lwayland-client -o "$(BUILD_DIR)/test-ime"
	wayland-scanner client-header "$(PROTOCOL_DIR)/stable/xdg-shell/xdg-shell.xml" "$(BUILD_DIR)/xdg-shell.h"
	wayland-scanner private-code "$(PROTOCOL_DIR)/stable/xdg-shell/xdg-shell.xml" "$(BUILD_DIR)/xdg-shell.c"
	wayland-scanner client-header "$(PROTOCOL_DIR)/staging/xdg-activation/xdg-activation-v1.xml" "$(BUILD_DIR)/xdg-activation.h"
	wayland-scanner private-code "$(PROTOCOL_DIR)/staging/xdg-activation/xdg-activation-v1.xml" "$(BUILD_DIR)/xdg-activation.c"
	wayland-scanner client-header "$(PROTOCOL_DIR)/staging/ext-session-lock/ext-session-lock-v1.xml" "$(BUILD_DIR)/session-lock.h"
	wayland-scanner private-code "$(PROTOCOL_DIR)/staging/ext-session-lock/ext-session-lock-v1.xml" "$(BUILD_DIR)/session-lock.c"
	$(CC) -I"$(BUILD_DIR)" test/integration/activation.c "$(BUILD_DIR)/xdg-shell.c" "$(BUILD_DIR)/xdg-activation.c" "$(BUILD_DIR)/session-lock.c" -lwayland-client -o "$(BUILD_DIR)/test-activation"

integration-test: all integration-fixtures
	python3 test/integration/run.py $(INTEGRATION_ARGS)

companions:
	python3 scripts/build-companions.py

release-check:
	python3 scripts/check-release.py

dist: release-check
	bash scripts/dist.sh "$(TAG)"

clean:
	rm -f -- $(OBJS) $(DEPS) "$(TARGET)" "$(BUILD_CONFIG)"
	rm -f -- "$(BUILD_DIR)/hyprspace-launch" "$(BUILD_DIR)/test-pointer" "$(BUILD_DIR)/test-activation" "$(BUILD_DIR)/test-ime" "$(BUILD_DIR)/test-headless" "$(BUILD_DIR)/xdg-shell-protocol.h"
	@for name in uwsm-app uwsm app2unit; do rm -f -- "$(BUILD_DIR)/launch-bin/$$name"; done
	@for name in virtual-pointer virtual-keyboard input-method xdg-shell xdg-activation session-lock; do rm -f -- "$(BUILD_DIR)/$$name.h" "$(BUILD_DIR)/$$name.c"; done
	@rmdir -- "$(BUILD_DIR)/launch-bin" 2>/dev/null || true
	@rmdir -- "$(BUILD_DIR)" 2>/dev/null || true
	$(MAKE) -C test clean

format:
	find src test -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 clang-format -i
