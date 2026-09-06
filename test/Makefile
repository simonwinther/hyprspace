BIN  := test_hyprspace
PKGS := cairo pangocairo gdk-pixbuf-2.0 librsvg-2.0

CXXFLAGS ?= -O1 -g
CXXFLAGS += -std=c++23 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))

SRCS := test_hyprspace.cpp ../src/DesktopDb.cpp ../src/Raster.cpp

.PHONY: run clean asan

run: $(BIN)
	./$(BIN)

$(BIN): $(SRCS) ../src/Geometry.hpp ../src/OverviewLayout.hpp ../src/Input.hpp ../src/Interaction.hpp ../src/Scrolling.hpp ../src/SwitcherLayout.hpp ../src/ImageCache.hpp ../src/PreviewStyle.hpp ../src/DesktopDb.hpp ../src/Raster.hpp
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDLIBS)

# Same suite under AddressSanitizer, to catch out-of-bounds in the layout and
# pixel-conversion code.
asan:
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $(SRCS) -o $(BIN)-asan $(LDLIBS)
	LSAN_OPTIONS=suppressions=lsan.supp ./$(BIN)-asan

clean:
	rm -f $(BIN) $(BIN)-asan
