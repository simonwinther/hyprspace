BIN  := test_hyprspace
PKGS := cairo pangocairo gdk-pixbuf-2.0 librsvg-2.0

CXXFLAGS ?= -O1 -g
CXXFLAGS += -std=c++23 -Wall -Wextra -Wno-unused-parameter -pthread $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))

SRCS := test_hyprspace.cpp ../src/DesktopDb.cpp ../src/Raster.cpp ../src/SpaceStore.cpp ../src/SpaceIcons.cpp

.PHONY: run clean asan

run: $(BIN)
	./$(BIN)

$(BIN): test_input_model.hpp ../src/InputModel.hpp
$(BIN): test_help.hpp ../src/HelpModel.hpp
$(BIN): test_spaces.hpp ../src/SpaceModel.hpp ../src/SpaceStore.hpp ../src/SpaceAppearance.hpp
$(BIN): test_space_icons.hpp ../src/SpaceIcons.hpp ../src/SpaceIconPicker.hpp
$(BIN): test_space_style.hpp ../src/SpaceStyle.hpp
$(BIN): test_render_geometry.hpp ../src/RenderGeometry.hpp

$(BIN): $(SRCS) test_zoom_input.hpp test_zoom_geometry.hpp test_launch_geometry.hpp test_capture_geometry.hpp test_empty_workspace.hpp test_window_views.hpp test_search_input.hpp ../src/CursorFollowPreference.hpp ../src/SearchInput.hpp ../src/WindowViewModel.hpp ../src/EmptyWorkspace.hpp ../src/LaunchGeometry.hpp ../src/CaptureGeometry.hpp ../src/Geometry.hpp ../src/OverviewLayout.hpp ../src/Input.hpp ../src/Interaction.hpp ../src/Scrolling.hpp ../src/SwitcherLayout.hpp ../src/ImageCache.hpp ../src/PreviewStyle.hpp ../src/DesktopDb.hpp ../src/Raster.hpp
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDLIBS)

# Same suite under AddressSanitizer, to catch out-of-bounds in the layout and
# pixel-conversion code.
asan:
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $(SRCS) -o $(BIN)-asan $(LDLIBS)
	LSAN_OPTIONS=suppressions=lsan.supp ./$(BIN)-asan

clean:
	rm -f $(BIN) $(BIN)-asan
