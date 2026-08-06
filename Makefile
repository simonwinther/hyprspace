PREFIX     ?= $(HOME)/.local
PLUGIN_DIR ?= $(PREFIX)/share/hyprspace

BUILD_DIR  := build
TARGET     := $(BUILD_DIR)/hyprspace.so

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
# compositor; --no-gnu-unique matters for g++ so symbols can be unloaded.
ifeq ($(shell $(CXX) --version 2>/dev/null | head -1 | grep -c g++),1)
    EXTRA_FLAGS := -fno-gnu-unique
else
    EXTRA_FLAGS :=
endif

CXXFLAGS ?= -O2
CXXFLAGS += -std=c++2b -fPIC $(EXTRA_FLAGS) -Wall -Wno-narrowing -Wno-unused-parameter
CXXFLAGS += $(shell pkg-config --cflags $(PKGS))

LDFLAGS  += -shared
LDLIBS   += $(shell pkg-config --libs pangocairo cairo gdk-pixbuf-2.0 librsvg-2.0)

.PHONY: all clean install uninstall check test format

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)
	@echo "built $@"

$(BUILD_DIR)/%.o: src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

# Refuse to install a plugin built against a different Hyprland than the one
# running, since the plugin ABI is tied to the exact commit hash.
check: $(TARGET)
	@./scripts/check-abi.sh $(TARGET)

install: check
	@mkdir -p $(PLUGIN_DIR)
	install -m 0755 $(TARGET) $(PLUGIN_DIR)/hyprspace.so
	@echo ""
	@echo "Installed to $(PLUGIN_DIR)/hyprspace.so"
	@echo "Add to your Hyprland config:  plugin = $(PLUGIN_DIR)/hyprspace.so"

uninstall:
	rm -f $(PLUGIN_DIR)/hyprspace.so
	-rmdir $(PLUGIN_DIR) 2>/dev/null || true

test:
	$(MAKE) -C test run

clean:
	rm -rf $(BUILD_DIR)
	$(MAKE) -C test clean

format:
	find src test -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 | xargs -0 clang-format -i
