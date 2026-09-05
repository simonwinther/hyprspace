// hyprspace - host-side unit tests.
//
// Covers the parts that carry real logic and no Hyprland dependency: the
// overview layout/navigation maths, .desktop parsing, icon resolution and the
// cairo/pango rasteriser.

#include "../src/DesktopDb.hpp"
#include "../src/Geometry.hpp"
#include "../src/Input.hpp"
#include "../src/ImageCache.hpp"
#include "../src/OverviewLayout.hpp"
#include "../src/PreviewStyle.hpp"
#include "../src/Raster.hpp"
#include "../src/SwitcherLayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

namespace fs = std::filesystem;
using namespace hyprspace;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                                                                                                                      \
    do {                                                                                                                                                                 \
        ++g_checks;                                                                                                                                                      \
        if (!(cond)) {                                                                                                                                                   \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                                                                \
            ++g_failures;                                                                                                                                                \
        }                                                                                                                                                                \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                                                                            \
    do {                                                                                                                                                                 \
        ++g_checks;                                                                                                                                                      \
        if (std::fabs((a) - (b)) > (eps)) {                                                                                                                              \
            std::printf("  FAIL %s:%d  %s (%.4f) != %s (%.4f)\n", __FILE__, __LINE__, #a, (double)(a), #b, (double)(b));                                                 \
            ++g_failures;                                                                                                                                                \
        }                                                                                                                                                                \
    } while (0)

static void section(const char* name) {
    std::printf("%s\n", name);
}

// ---------------------------------------------------------------- layout ----

static std::vector<STileInput> makeInput(size_t n) {
    std::vector<STileInput> in;
    for (size_t i = 0; i < n; ++i)
        in.push_back({.key = i, .workspaceId = static_cast<long>(i + 1)});
    return in;
}

static void testGridUniformity() {
    section("layout: every workspace cell is the same size and monitor-shaped");

    SLayoutParams p;
    p.screenW = 1920;
    p.screenH = 1080;
    p.aspect  = 1920.0 / 1080.0;

    for (size_t n = 1; n <= 10; ++n) {
        const auto out = layout(makeInput(n), p);
        CHECK(out.tiles.size() == n);
        if (out.tiles.empty())
            continue;

        const auto& first = out.tiles.front().box;
        for (const auto& t : out.tiles) {
            CHECK_NEAR(t.box.w, first.w, 1e-6);
            CHECK_NEAR(t.box.h, first.h, 1e-6);
            CHECK_NEAR(t.box.w / t.box.h, p.aspect, 1e-6);
        }
    }
}

static void testGridFitsOnScreen() {
    section("layout: the grid never leaves the padded screen area");

    for (double sw : {1920.0, 2560.0, 3840.0, 1366.0}) {
        SLayoutParams p;
        p.screenW    = sw;
        p.screenH    = sw * 9.0 / 16.0;
        p.aspect     = 16.0 / 9.0;
        p.padding    = 56;
        p.labelSpace = 34;

        for (size_t n = 1; n <= 10; ++n) {
            const auto out = layout(makeInput(n), p);
            for (const auto& t : out.tiles) {
                CHECK(t.box.x >= p.padding - 1.0);
                CHECK(t.box.y >= p.padding - 1.0);
                CHECK(t.box.x + t.box.w <= p.screenW - p.padding + 1.0);
                // Room must remain under the last row for its label.
                CHECK(t.box.y + t.box.h + p.labelSpace <= p.screenH - p.padding + 1.0);
            }
        }
    }
}

static void testPortraitGridFitsOnScreen() {
    section("layout: portrait grids stay separated and leave room for labels");

    SLayoutParams p;
    p.screenW    = 1080;
    p.screenH    = 1920;
    p.padding    = 56;
    p.gap        = 28;
    p.labelSpace = 34;
    p.aspect     = 1056.0 / 1852.0; // 1080x1920 output minus a 56px top bar

    for (size_t n = 1; n <= 10; ++n) {
        const auto out = layout(makeInput(n), p);
        CHECK(out.tiles.size() == n);

        for (const auto& tile : out.tiles) {
            CHECK_NEAR(tile.box.w / tile.box.h, p.aspect, 1e-6);
            CHECK(tile.box.x >= p.padding - 1.0);
            CHECK(tile.box.y >= p.padding - 1.0);
            CHECK(tile.box.x + tile.box.w <= p.screenW - p.padding + 1.0);
            CHECK(tile.box.y + tile.box.h + p.labelSpace <= p.screenH - p.padding + 1.0);
        }

        for (size_t i = 0; i < out.tiles.size(); ++i) {
            for (size_t j = i + 1; j < out.tiles.size(); ++j) {
                const auto& a = out.tiles[i].box;
                const auto& b = out.tiles[j].box;

                // Include each tile's label band in its occupied height. A
                // later row must not cover either the cell or its label.
                const bool separated = a.x + a.w <= b.x + 1e-6 || b.x + b.w <= a.x + 1e-6 ||
                    a.y + a.h + p.labelSpace <= b.y + 1e-6 || b.y + b.h + p.labelSpace <= a.y + 1e-6;
                CHECK(separated);
            }
        }
    }

    const auto three = layout(makeInput(3), p);
    CHECK(three.cols == 2);
    CHECK(three.rows == 2);
    CHECK(three.tiles[2].box.x > three.tiles[0].box.x);
}

static void testGridNoOverlap() {
    section("layout: workspace cells never overlap, at any count up to ten");

    SLayoutParams p;
    p.screenW = 1920;
    p.screenH = 1080;
    p.aspect  = 16.0 / 9.0;

    for (size_t n = 1; n <= 10; ++n) {
        const auto out = layout(makeInput(n), p);

        for (size_t i = 0; i < out.tiles.size(); ++i) {
            for (size_t j = i + 1; j < out.tiles.size(); ++j) {
                const auto& a = out.tiles[i].box;
                const auto& b = out.tiles[j].box;

                const bool separated = a.x + a.w <= b.x + 1e-6 || b.x + b.w <= a.x + 1e-6 || a.y + a.h <= b.y + 1e-6 || b.y + b.h <= a.y + 1e-6;
                CHECK(separated);
            }
        }
    }
}

static void testGridShape() {
    section("layout: the grid stays close to square");

    SLayoutParams p;
    p.screenW = 1920;
    p.screenH = 1080;
    p.aspect  = 16.0 / 9.0;

    // A single workspace should fill most of the screen.
    {
        const auto out = layout(makeInput(1), p);
        CHECK(out.rows == 1 && out.cols == 1);
        CHECK(out.tiles[0].box.w > (p.screenW - 2 * p.padding) * 0.9);
    }

    // Five workspaces -> 3 over 2, the shape in the reference screenshot.
    {
        const auto out = layout(makeInput(5), p);
        CHECK(out.cols == 3);
        CHECK(out.rows == 2);
        // The trailing row is centred, not left-aligned.
        CHECK(out.tiles[3].box.x > out.tiles[0].box.x);
    }

    // Ten workspaces must still produce a sane, non-degenerate grid.
    {
        const auto out = layout(makeInput(10), p);
        CHECK(out.tiles.size() == 10);
        CHECK(out.rows >= 2 && out.rows <= 4);
        CHECK(out.tiles[0].box.w > 200); // still big enough to read
    }
}

static void testGridEdgeCases() {
    section("layout: degenerate inputs are handled");

    SLayoutParams p;
    CHECK(layout({}, p).tiles.empty());

    SLayoutParams tiny;
    tiny.screenW = 40;
    tiny.screenH = 40;
    tiny.padding = 56;
    CHECK(layout(makeInput(3), tiny).tiles.empty());

    // A zero aspect must fall back rather than divide by zero.
    SLayoutParams bad;
    bad.aspect = 0.0;
    CHECK(layout(makeInput(2), bad).tiles.size() == 2);
}

// ------------------------------------------------------------ navigation ----

static std::vector<STile> gridTiles() {
    // 3x2 grid of 100x100 tiles at 200px spacing.
    std::vector<STile> tiles;
    size_t             k = 0;
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            STile t;
            t.key   = k;
            t.order = k;
            t.box   = SBoxF{static_cast<double>(col) * 200, static_cast<double>(row) * 200, 100, 100};
            tiles.push_back(t);
            ++k;
        }
    }
    return tiles;
}

static void testNavigation() {
    section("navigate: arrows move to the geometrically nearest tile");

    const auto tiles = gridTiles();

    CHECK(navigate(tiles, 0, EDirection::RIGHT) == 1);
    CHECK(navigate(tiles, 1, EDirection::RIGHT) == 2);
    CHECK(navigate(tiles, 1, EDirection::LEFT) == 0);
    CHECK(navigate(tiles, 0, EDirection::DOWN) == 3);
    CHECK(navigate(tiles, 3, EDirection::UP) == 0);
    CHECK(navigate(tiles, 4, EDirection::UP) == 1);

    // Moving past the edge keeps the current selection rather than wrapping.
    CHECK(navigate(tiles, 2, EDirection::RIGHT) == 2);
    CHECK(navigate(tiles, 0, EDirection::LEFT) == 0);
    CHECK(navigate(tiles, 0, EDirection::UP) == 0);
    CHECK(navigate(tiles, 5, EDirection::DOWN) == 5);

    // Empty and out-of-range inputs must not crash or return nonsense.
    CHECK(navigate({}, 0, EDirection::LEFT) == -1);
    CHECK(navigate(tiles, 999, EDirection::LEFT) == 0);
    CHECK(navigate(tiles, -1, EDirection::LEFT) == 0);
}

static void testHitTest() {
    section("tileAt: pointer hit testing");

    const auto tiles = gridTiles();

    CHECK(tileAt(tiles, 50, 50) == 0);
    CHECK(tileAt(tiles, 250, 50) == 1);
    CHECK(tileAt(tiles, 50, 250) == 3);
    CHECK(tileAt(tiles, 150, 150) == -1); // in the gap
    CHECK(tileAt(tiles, -10, -10) == -1);
}

// ------------------------------------------------------------- workspaces ----

static void testInsetBox() {
    section("geometry: centered insets retain aspect ratio");

    const SBoxF CELL{100, 100, 400, 225};

    // A shrink of 1 (or nonsense) leaves the box alone.
    for (double k : {1.0, 0.0, -0.5, 1.5}) {
        const auto b = insetBox(CELL, k);
        CHECK_NEAR(b.x, CELL.x, 1e-9);
        CHECK_NEAR(b.w, CELL.w, 1e-9);
    }

    // A real shrink stays centred, inside the box, and keeps the aspect.
    {
        const auto b = insetBox(CELL, 0.62);
        CHECK(b.w < CELL.w && b.h < CELL.h);
        CHECK(b.x > CELL.x && b.y > CELL.y);
        CHECK(b.x + b.w < CELL.x + CELL.w);
        CHECK(b.y + b.h < CELL.y + CELL.h);
        CHECK_NEAR(b.cx(), CELL.cx(), 1e-9);
        CHECK_NEAR(b.cy(), CELL.cy(), 1e-9);
        CHECK_NEAR(b.w / b.h, CELL.w / CELL.h, 1e-9);
    }
}

static void testWorkspaceLabels() {
    section("workspace: tile labels");

    // Hyprland hands us the id's own digits as the name for numbered
    // workspaces, so that must not be mistaken for a user-set name.
    CHECK(workspaceLabel(1, "1") == "1");
    CHECK(workspaceLabel(7, "7") == "7");
    CHECK(workspaceLabel(3, "") == "3");

    // Workspace 10 is the 0 key, so it gets the 0 key's label.
    CHECK(workspaceLabel(10, "10") == "0");
    CHECK(workspaceLabel(10, "") == "0");

    // A real name always wins, including one that happens to look numeric but
    // does not match the id.
    CHECK(workspaceLabel(-99, "special:scratchpad") == "special:scratchpad");
    CHECK(workspaceLabel(2, "mail") == "mail");
    CHECK(workspaceLabel(10, "chat") == "chat");
    CHECK(workspaceLabel(4, "10") == "10");

    // Named workspaces past the number row keep their own identity.
    CHECK(workspaceLabel(11, "11") == "11");
    CHECK(workspaceLabel(100, "100") == "100");
}

static void testWorkspaceForDigit() {
    section("workspace: number-row key mapping");

    for (int d = 1; d <= 9; ++d)
        CHECK(workspaceForDigit(d) == d);

    // 0 is workspace 10 — the same mapping the labels use, so pressing the key
    // under a tile's label goes to that tile.
    CHECK(workspaceForDigit(0) == 10);

    // Round-trip: every number-row key names the workspace its label shows.
    for (int d = 0; d <= 9; ++d) {
        const long WS = workspaceForDigit(d);
        CHECK(workspaceLabel(WS, std::to_string(WS)) == std::to_string(d));
    }

    // Anything off the number row names nothing.
    CHECK(workspaceForDigit(-1) == 0);
    CHECK(workspaceForDigit(10) == 0);
    CHECK(workspaceForDigit(99) == 0);
}

// --------------------------------------------------------------- desktop ----

static void testDesktopParsing() {
    section("desktop: [Desktop Entry] parsing");

    const std::string file = R"([Desktop Entry]
Type=Application
Name=Firefox Web Browser
Name[de]=Firefox Webbrowser
Icon=firefox
StartupWMClass=firefox
NoDisplay=false

[Desktop Action new-window]
Name=Open a New Window
Icon=should-be-ignored
)";

    const auto e = parseDesktopEntry(file, "firefox");
    CHECK(e.name == "Firefox Web Browser"); // the localised variant must not win
    CHECK(e.icon == "firefox");             // the action group's Icon must not win
    CHECK(e.wmClass == "firefox");
    CHECK(e.id == "firefox");
    CHECK(e.noDisplay == false);

    const auto hidden = parseDesktopEntry("[Desktop Entry]\nName=X\nIcon=x\nNoDisplay=true\n", "x");
    CHECK(hidden.noDisplay == true);

    // Malformed input must not throw.
    const auto junk = parseDesktopEntry("not a desktop file\n\n===\n", "junk");
    CHECK(junk.icon.empty());
}

static void testClassLookup() {
    section("desktop: window class -> icon name lookup");

    CDesktopDb db;
    db.addEntry(parseDesktopEntry("[Desktop Entry]\nName=Nautilus\nIcon=org.gnome.Nautilus\nStartupWMClass=org.gnome.Nautilus\n", "org.gnome.Nautilus"));
    db.addEntry(parseDesktopEntry("[Desktop Entry]\nName=Alacritty\nIcon=Alacritty\n", "Alacritty"));

    CHECK(db.iconNameForClass("org.gnome.Nautilus") == "org.gnome.Nautilus");
    CHECK(db.iconNameForClass("ORG.GNOME.NAUTILUS") == "org.gnome.Nautilus"); // case-insensitive
    CHECK(db.iconNameForClass("Alacritty") == "Alacritty");
    CHECK(db.iconNameForClass("alacritty") == "Alacritty");
    CHECK(db.appNameForClass("alacritty") == "Alacritty");
    CHECK(db.iconNameForClass("does-not-exist").empty());
    CHECK(db.iconNameForClass("").empty());

    // Entries without an Icon= are useless to us and must be dropped.
    CDesktopDb empty;
    empty.addEntry(parseDesktopEntry("[Desktop Entry]\nName=NoIcon\n", "noicon"));
    CHECK(empty.entryCount() == 0);
}

static void testClassCandidates() {
    section("desktop: window class candidate keys");

    // Chromium/Edge web apps, which is how Omarchy ships its web apps.
    {
        const auto c = classCandidates("chrome-chatgpt.com__-Default");
        CHECK(std::ranges::find(c, "chrome-chatgpt.com__-default") != c.end());
        CHECK(std::ranges::find(c, "chatgpt.com") != c.end());
        CHECK(std::ranges::find(c, "chatgpt") != c.end());
        // Most specific first.
        CHECK(c.front() == "chrome-chatgpt.com__-default");
    }
    {
        const auto c = classCandidates("brave-app.slack.com__client-Default");
        CHECK(std::ranges::find(c, "app.slack.com") != c.end());
        CHECK(std::ranges::find(c, "app.slack") != c.end());
    }

    // Reverse-DNS ids.
    {
        const auto c = classCandidates("org.gnome.Nautilus");
        CHECK(c.front() == "org.gnome.nautilus");
        CHECK(std::ranges::find(c, "nautilus") != c.end());
    }

    // Plain classes yield exactly one key, and there are never duplicates.
    {
        const auto c = classCandidates("firefox");
        CHECK(c.size() == 1);
        CHECK(c.front() == "firefox");
    }

    CHECK(classCandidates("").empty());
    CHECK(classCandidates("   ").empty());

    // A chromium-ish class with no profile suffix must not produce garbage.
    for (const char* weird : {"chrome-", "chrome--", "chrome-.-", "-", "..."}) {
        const auto c = classCandidates(weird);
        for (const auto& k : c)
            CHECK(!k.empty());
    }
}

static void testWebAppLookup() {
    section("desktop: chromium web app resolves to its entry");

    CDesktopDb db;
    // Omarchy-style web app entry: named for the site, absolute icon path.
    db.addEntry(parseDesktopEntry("[Desktop Entry]\nName=ChatGPT\nIcon=/tmp/ChatGPT.png\n", "ChatGPT"));

    CHECK(db.iconNameForClass("chrome-chatgpt.com__-Default") == "/tmp/ChatGPT.png");
    CHECK(db.appNameForClass("chrome-chatgpt.com__-Default") == "ChatGPT");
    CHECK(db.iconNameForClass("chrome-example.org__-Default").empty());

    // A "www." host must still reach an entry named for the bare site, which
    // is how Omarchy writes them. Without stripping it the class only ever
    // offers "www.youtube", nothing matches, and the switcher falls back to a
    // letter placeholder.
    db.addEntry(parseDesktopEntry("[Desktop Entry]\nName=YouTube\nIcon=/tmp/YouTube.png\n", "YouTube"));

    CHECK(db.iconNameForClass("chrome-www.youtube.com__-Default") == "/tmp/YouTube.png");
    CHECK(db.appNameForClass("chrome-www.youtube.com__-Default") == "YouTube");

    // The bare host keeps working, and other browsers use the same scheme.
    CHECK(db.iconNameForClass("chrome-youtube.com__-Default") == "/tmp/YouTube.png");
    CHECK(db.iconNameForClass("brave-www.youtube.com__-Default") == "/tmp/YouTube.png");
    CHECK(db.iconNameForClass("chromium-www.youtube.com__-Default") == "/tmp/YouTube.png");

    // Still no false positives.
    CHECK(db.iconNameForClass("chrome-www.example.org__-Default").empty());

    const auto CANDS = classCandidates("chrome-www.youtube.com__-Default");
    CHECK(std::ranges::find(CANDS, "youtube") != CANDS.end());
    CHECK(std::ranges::find(CANDS, "youtube.com") != CANDS.end());
    CHECK(std::ranges::find(CANDS, "www.youtube.com") != CANDS.end());
}

static void testIconResolution() {
    section("desktop: icon theme path resolution");

    const auto root     = fs::temp_directory_path() / "hyprspace-test-icons";
    const auto fallback = fs::temp_directory_path() / "hyprspace-test-icons-fallback";
    fs::remove_all(root);
    fs::remove_all(fallback);

    fs::create_directories(root / "hicolor" / "48x48" / "apps");
    fs::create_directories(root / "hicolor" / "256x256" / "apps");
    fs::create_directories(root / "hicolor" / "scalable" / "apps");
    fs::create_directories(fallback / "hicolor" / "scalable" / "apps");

    auto touch = [](const fs::path& p) { std::ofstream(p) << "x"; };
    touch(root / "hicolor" / "48x48" / "apps" / "sized-only.png");
    touch(root / "hicolor" / "256x256" / "apps" / "sized-only.png");
    touch(root / "hicolor" / "scalable" / "apps" / "vector.svg");
    touch(root / "hicolor" / "48x48" / "apps" / "priority.png");
    touch(fallback / "hicolor" / "scalable" / "apps" / "priority.svg");

    CDesktopDb db;
    db.setIconRoots({root.string(), fallback.string()});

    // SVG is preferred because it scales to any tile size.
    const auto vec = db.resolveIconPath("vector", 64);
    CHECK(vec.has_value());
    CHECK(vec && vec->ends_with("vector.svg"));

    // Otherwise the smallest size at or above the request wins.
    const auto sized = db.resolveIconPath("sized-only", 48);
    CHECK(sized.has_value());
    CHECK(sized && sized->ends_with("48x48/apps/sized-only.png"));

    const auto bigger = db.resolveIconPath("sized-only", 128);
    CHECK(bigger.has_value());
    CHECK(bigger && bigger->ends_with("256x256/apps/sized-only.png"));

    // XDG root priority beats format/size preference. Replacing the roots must
    // also invalidate both the filename index and previously resolved paths.
    const auto priority = db.resolveIconPath("priority", 64);
    CHECK(priority && priority->starts_with(root.string()));
    CHECK(priority && priority->ends_with("priority.png"));

    db.setIconRoots({fallback.string()});
    const auto reprioritised = db.resolveIconPath("priority", 64);
    CHECK(reprioritised && reprioritised->starts_with(fallback.string()));
    CHECK(reprioritised && reprioritised->ends_with("priority.svg"));

    CHECK(!db.resolveIconPath("nothing-here", 64).has_value());
    CHECK(!db.resolveIconPath("", 64).has_value());

    // An absolute path in Icon= is used verbatim when it exists.
    const auto abs = (root / "hicolor" / "scalable" / "apps" / "vector.svg").string();
    CHECK(db.resolveIconPath(abs, 64) == abs);
    CHECK(!db.resolveIconPath("/nonexistent/icon.png", 64).has_value());

    fs::remove_all(root);
    fs::remove_all(fallback);
}

// ---------------------------------------------------------------- raster ----

static void testTextRaster() {
    section("raster: text rasterisation");

    const auto img = renderText("hyprspace", "Sans 14", SRgba{1, 1, 1, 1});
    CHECK(img.ok());
    CHECK(img.w > 0 && img.h > 0);
    CHECK(img.stride >= img.w * 4);
    CHECK(img.data.size() == static_cast<size_t>(img.stride) * img.h);

    // Something must actually have been drawn.
    bool anyInk = false;
    for (size_t i = 3; i < img.data.size(); i += 4) {
        if (img.data[i] != 0) {
            anyInk = true;
            break;
        }
    }
    CHECK(anyInk);

    CHECK(!renderText("", "Sans 14", SRgba{1, 1, 1, 1}).ok());

    // Ellipsising must respect the width cap.
    const auto wide   = renderText("a very long window title that will not fit", "Sans 14", SRgba{1, 1, 1, 1});
    const auto capped = renderText("a very long window title that will not fit", "Sans 14", SRgba{1, 1, 1, 1}, 120);
    CHECK(capped.ok());
    CHECK(capped.w <= 121);
    CHECK(capped.w < wide.w);
}

static void testPlaceholderIcon() {
    section("raster: placeholder icon fallback");

    const auto img = placeholderIcon("F", "Sans Bold 32", 64, SRgba{1, 1, 1, 1}, SRgba{0.3, 0.4, 0.9, 1});
    CHECK(img.ok());
    CHECK(img.w == 64 && img.h == 64);

    // The centre is inside the rounded rect and must be opaque.
    const size_t centre = static_cast<size_t>(32) * img.stride + 32 * 4;
    CHECK(img.data[centre + 3] > 200);

    // The very corner is outside the rounding and must be transparent.
    CHECK(img.data[3] < 40);

    CHECK(!placeholderIcon("F", "Sans 12", 0, SRgba{}, SRgba{}).ok());
}

static void testIconLoading() {
    section("raster: svg and png icon loading");

    const auto dir = fs::temp_directory_path() / "hyprspace-test-load";
    fs::create_directories(dir);

    const auto svg = dir / "t.svg";
    {
        std::ofstream f(svg);
        f << R"(<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16">)"
          << R"(<rect width="16" height="16" fill="#ff0000"/></svg>)";
    }

    const auto img = loadIcon(svg.string(), 64);
    CHECK(img.ok());
    CHECK(img.w == 64 && img.h == 64);

    const size_t centre = static_cast<size_t>(32) * img.stride + 32 * 4;
    CHECK(img.data[centre + 3] > 200); // opaque
    CHECK(img.data[centre + 2] > 200); // red channel (BGRA order)
    CHECK(img.data[centre + 0] < 60);  // blue channel

    CHECK(!loadIcon("", 64).ok());
    CHECK(!loadIcon("/nonexistent/x.png", 64).ok());
    CHECK(!loadIcon(svg.string(), 0).ok());

    fs::remove_all(dir);
}

static void testButtonCapture() {
    section("input: mouse releases follow their presses across overlay transitions");
    CButtonCapture capture;
    constexpr uint32_t LEFT = 0x110;
    constexpr uint32_t RIGHT = 0x111;

    // A click that dismisses the overlay must not leak a release to the app.
    CHECK(capture.consume(LEFT, true, true));
    CHECK(capture.consume(LEFT, false, false));
    CHECK(!capture.consume(LEFT, false, false));

    // A client drag started before opening the overlay still gets its release.
    CHECK(!capture.consume(LEFT, true, false));
    CHECK(!capture.consume(LEFT, false, true));

    // The screenshot hand-off can occur with several buttons held. Only the
    // presses captured by the overlay retain ownership of their releases.
    CHECK(capture.consume(LEFT, true, true));
    CHECK(!capture.consume(RIGHT, true, false));
    CHECK(capture.consume(LEFT, false, false));
    CHECK(!capture.consume(RIGHT, false, true));

    CHECK(capture.consume(LEFT, true, true));
    CHECK(capture.consume(RIGHT, true, true));
    CHECK(capture.consume(RIGHT, false, true));
    CHECK(capture.consume(LEFT, false, true));

    capture.clear();
    CHECK(!capture.consume(LEFT, true, false));
    CHECK(!capture.consume(LEFT, false, false));
}

static void testScrollAccumulator() {
    section("input: high-resolution scrolling preserves detents and gesture boundaries");
    CScrollAccumulator scroll;
    uint32_t           time   = 0;
    auto               wheel  = [&](int32_t value) { return scroll.steps({.value120 = value, .timeMs = time += 8}); };
    auto               finger = [&](double delta) { return scroll.steps({.delta = delta, .timeMs = time += 8, .wheel = false}); };

    CHECK(wheel(40) == 0);
    CHECK(wheel(40) == 0);
    CHECK(wheel(40) == 1);
    CHECK(wheel(240) == 2);
    CHECK(wheel(-240) == -2);
    for (int sign : {-1, 1}) {
        scroll.reset();
        for (int i = 0; i < 119; ++i)
            CHECK(wheel(sign) == 0);
        CHECK(wheel(sign) == sign);
    }

    scroll.reset();
    CHECK(wheel(80) == 0);
    CHECK(wheel(-40) == 0);
    CHECK(wheel(-80) == -1);
    CHECK(wheel(80) == 0);
    time += 301;
    CHECK(wheel(80) == 0);
    CHECK(wheel(40) == 1);

    scroll.reset();
    CHECK(finger(10) == 0);
    CHECK(finger(10) == 0);
    CHECK(finger(10) == 0);
    CHECK(finger(10) == 1);
    CHECK(finger(-80) == -2);
    CHECK(finger(30) == 0);
    CHECK(finger(0) == 0);
    CHECK(finger(30) == 0);
    CHECK(finger(10) == 1);

    CHECK(finger(30) == 0);
    CHECK(wheel(40) == 0);
    CHECK(wheel(80) == 1);
    CHECK(finger(30) == 0);
    CHECK(finger(std::numeric_limits<double>::quiet_NaN()) == 0);
    CHECK(finger(10) == 0);
    CHECK(finger(std::numeric_limits<double>::infinity()) == 0);
    CHECK(finger(40) == 1);

    scroll.reset();
    CHECK(scroll.steps({.delta = 5, .timeMs = 10}) == 0);
    CHECK(scroll.steps({.delta = 5, .timeMs = 20}) == 0);
    CHECK(scroll.steps({.delta = 5, .timeMs = 30}) == 1);

    scroll.reset();
    CHECK(scroll.steps({.value120 = 80, .timeMs = std::numeric_limits<uint32_t>::max() - 20}) == 0);
    CHECK(scroll.steps({.value120 = 40, .timeMs = 10}) == 1);
    CHECK(wheel(std::numeric_limits<int32_t>::max()) == 32);
    CHECK(wheel(std::numeric_limits<int32_t>::min()) == -32);
}

static void testSwitcherLayout() {
    section("switcher: bounded pages keep every selection reachable on landscape and portrait outputs");
    for (const auto& screen : std::vector<SBoxF>{{0, 0, 1920, 1080}, {0, 0, 1080, 1920}, {0, 0, 800, 600}, {0, 0, 320, 240}, {0, 0, 60, 45}}) {
        for (int count : {1, 3, 7, 17, 70, 200, 500}) {
            for (int selected : {0, count / 2, count - 1}) {
                for (double titleHeight : {0.0, 34.0}) {
                    for (bool large : {false, true}) {
                        SSwitcherLayoutParams p{
                            .screenW     = screen.w,
                            .screenH     = screen.h,
                            .iconSize    = large ? 256.0 : 96.0,
                            .padding     = large ? 128.0 : 24.0,
                            .gap         = large ? 128.0 : 12.0,
                            .titleWidth  = 10000,
                            .titleHeight = titleHeight,
                        };
                        const auto result = switcherLayout(count, selected, p);
                        CHECK(result.panel.x >= -1e-6 && result.panel.y >= -1e-6);
                        CHECK(result.panel.x + result.panel.w <= screen.w + 1e-6);
                        CHECK(result.panel.y + result.panel.h <= screen.h + 1e-6);
                        CHECK(result.panel.w > 0 && result.panel.h > 0);
                        CHECK(result.iconSize > 0);
                        CHECK(result.capacity > 0 && result.capacity <= count);
                        CHECK(result.tiles.size() <= static_cast<size_t>(result.capacity));
                        CHECK(result.page >= 0 && result.page < result.pages);
                        CHECK(std::ranges::any_of(result.tiles, [&](const STile& tile) { return tile.key == static_cast<size_t>(selected); }));
                        CHECK(result.pages <= 1 || result.pageLabel.h > 0);
                        for (size_t i = 0; i < result.tiles.size(); ++i) {
                            const auto& tile = result.tiles[i];
                            CHECK(tile.key == static_cast<size_t>(result.first) + i);
                            CHECK(tile.key < static_cast<size_t>(count));
                            CHECK(tile.box.x >= result.panel.x - 1e-6);
                            CHECK(tile.box.y >= result.panel.y - 1e-6);
                            CHECK(tile.box.x + tile.box.w <= result.panel.x + result.panel.w + 1e-6);
                            CHECK(tile.box.y + tile.box.h <= result.title.y + 1e-6);
                            CHECK_NEAR(tile.box.w, tile.box.h, 1e-6);
                            if (i > 0) {
                                const auto& previous = result.tiles[i - 1];
                                if (tile.row == previous.row)
                                    CHECK(tile.box.x >= previous.box.x + previous.box.w - 1e-6);
                                else
                                    CHECK(tile.box.y >= previous.box.y + previous.box.h - 1e-6);
                            }
                        }
                    }
                }
            }
        }
    }

    SSwitcherLayoutParams p;
    p.titleWidth     = 600;
    const auto small = switcherLayout(3, 1, p);
    CHECK(small.pages == 1 && small.columns == 3);
    CHECK_NEAR(small.panel.w, 648, 1e-6);
    CHECK_NEAR(small.panel.h, 178, 1e-6);
    CHECK_NEAR(small.iconSize, 96, 1e-6);
    CHECK_NEAR(small.gap, 12, 1e-6);

    const auto first = switcherLayout(1000, 0, p);
    const auto last  = switcherLayout(1000, 999, p);
    CHECK(first.pages > 1);
    CHECK(last.page == last.pages - 1);
    CHECK_NEAR(first.panel.w, last.panel.w, 1e-6);
    CHECK_NEAR(first.panel.h, last.panel.h, 1e-6);
    CHECK(last.tiles.back().key == 999);
    CHECK(switcherLayout(0, 0, p).tiles.empty());
    CHECK(switcherLayout(-1, 0, p).tiles.empty());
    CHECK(switcherLayout(3, -1, p).tiles.front().key == 0);
    p.screenW = 0;
    CHECK(switcherLayout(3, 0, p).tiles.empty());
    p.screenW = std::numeric_limits<double>::quiet_NaN();
    CHECK(switcherLayout(3, 0, p).tiles.empty());
    p.screenW = 1920;
    p.gap     = std::numeric_limits<double>::infinity();
    CHECK(switcherLayout(3, 0, p).tiles.empty());

    CHECK(switcherRowStep(10, 5, 4, 1) == 8);
    CHECK(switcherRowStep(10, 6, 4, 1) == 9);
    CHECK(switcherRowStep(10, 9, 4, -1) == 6);
    CHECK(switcherRowStep(10, 8, 4, -1) == 5);
    CHECK(switcherRowStep(10, 0, 4, -1) == 0);
    CHECK(switcherRowStep(10, 9, 4, 1) == 9);
    CHECK(switcherRowStep(10, 5, 0, 1) == 5);
    CHECK(switcherRowStep(0, 0, 4, 1) == -1);
}

static SImage cacheImage(int width, uint8_t value = 0) {
    SImage image{.w = width, .h = 1, .stride = width * 4, .data = {}};
    image.data.assign(static_cast<size_t>(image.stride), value);
    return image;
}

static void testImageCache() {
    section("icons: decoded images retain pixels and obey byte and LRU entry limits");
    CImageCache cache(32, 2);
    cache.put("a", cacheImage(4, 1));
    cache.put("b", cacheImage(4, 2));
    CHECK(cache.size() == 2 && cache.bytes() == 32);
    CHECK(cache.get("a") && cache.get("a")->data[0] == 1);
    cache.put("c", cacheImage(4, 3));
    CHECK(cache.get("b") == nullptr);
    CHECK(cache.get("a") && cache.get("c"));
    CHECK(cache.size() == 2 && cache.bytes() <= 32);

    cache.put("a", cacheImage(2, 4));
    CHECK(cache.size() == 2 && cache.bytes() == 24);
    CHECK(cache.get("a") && cache.get("a")->data[0] == 4);
    cache.put("large", cacheImage(9));
    CHECK(cache.get("large") == nullptr);
    CHECK(cache.size() == 2 && cache.bytes() == 24);
    cache.put("invalid", SImage{});
    CHECK(cache.get("invalid") == nullptr);
    CHECK(cache.size() == 2);

    cache.clear();
    CHECK(cache.size() == 0 && cache.bytes() == 0);
    CHECK(cache.get("a") == nullptr);
    cache.put("d", cacheImage(8, 5));
    CHECK(cache.bytes() == 32 && cache.size() == 1);
    CHECK(cache.get("d") && cache.get("d")->data[0] == 5);
    cache.put("e", cacheImage(1, 6));
    CHECK(cache.size() == 1 && cache.bytes() == 4);
    CHECK(cache.get("d") == nullptr);

    auto reserved = cacheImage(1);
    reserved.data.reserve(128);
    cache.put("reserved", std::move(reserved));
    CHECK(cache.get("reserved") == nullptr);
    CHECK(cache.bytes() == 4);

    CImageCache disabled(0, 0);
    disabled.put("a", cacheImage(1));
    CHECK(disabled.size() == 0 && disabled.bytes() == 0);

    CImageCache entries(1024, 2);
    entries.put("a", cacheImage(1));
    entries.put("b", cacheImage(1));
    CHECK(entries.get("a"));
    entries.put("c", cacheImage(1));
    CHECK(entries.get("b") == nullptr);
    CHECK(entries.size() == 2 && entries.bytes() == 8);
    entries.put("a", SImage{});
    CHECK(entries.get("a") && entries.get("a")->ok());

    auto valid = cacheImage(2);
    CHECK(valid.ok());
    valid.stride = 4;
    CHECK(!valid.ok());
    valid.stride = 8;
    valid.h      = 2;
    CHECK(!valid.ok());
    valid.data.resize(16);
    CHECK(valid.ok());
    valid.w = std::numeric_limits<int>::max();
    CHECK(!valid.ok());
    valid.w      = 2;
    valid.stride = -1;
    CHECK(!valid.ok());
}

static void testPreviewStyle() {
    section("preview: preserve window opacity and fade workspace plates out before the desktop hand-off");

    CHECK_NEAR(windowPreviewOpacity(0.85F, false, 1.F), 0.85F, 1e-6F);
    CHECK_NEAR(windowPreviewOpacity(0.8F, false, 0.9F), 0.72F, 1e-6F);
    CHECK_NEAR(windowPreviewOpacity(0.2F, true, 0.9F), 0.9F, 1e-6F);
    CHECK_NEAR(windowPreviewOpacity(0.F, false, 1.F), 0.F, 1e-6F);
    CHECK_NEAR(windowPreviewOpacity(1.F, false, 0.F), 0.F, 1e-6F);

    for (bool anchor : {false, true}) {
        for (bool selected : {false, true}) {
            const auto desktop = workspacePreviewStyle(anchor, selected, 0.F);
            CHECK_NEAR(desktop.visibility, anchor ? 1.F : 0.F, 1e-6F);
            CHECK_NEAR(desktop.windowVisibility, anchor ? 1.F : 0.F, 1e-6F);
            CHECK_NEAR(desktop.plateVisibility, 0.F, 1e-6F);
            CHECK_NEAR(windowPreviewOpacity(0.85F, false, desktop.windowVisibility), anchor ? 0.85F : 0.F, 1e-6F);

            const auto overview = workspacePreviewStyle(anchor, selected, 1.F);
            CHECK_NEAR(overview.visibility, 1.F, 1e-6F);
            CHECK_NEAR(overview.windowVisibility, selected ? 1.F : 0.9F, 1e-6F);
            CHECK_NEAR(overview.plateVisibility, 1.F, 1e-6F);

            auto previous = desktop;
            for (int step = 1; step <= 1000; ++step) {
                const auto style = workspacePreviewStyle(anchor, selected, step / 1000.F);
                CHECK(style.windowVisibility >= 0.F && style.windowVisibility <= 1.F);
                CHECK(style.plateVisibility >= 0.F && style.plateVisibility <= 1.F);
                CHECK(std::abs(style.windowVisibility - previous.windowVisibility) < 0.002F);
                CHECK(std::abs(style.plateVisibility - previous.plateVisibility) < 0.003F);
                previous = style;
            }
        }
    }

    for (float value : {-1.F, 2.F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        const float opacity = windowPreviewOpacity(value, false, value);
        CHECK(std::isfinite(opacity) && opacity >= 0.F && opacity <= 1.F);
        const auto style = workspacePreviewStyle(true, false, value);
        CHECK(std::isfinite(style.windowVisibility) && style.windowVisibility >= 0.F && style.windowVisibility <= 1.F);
        CHECK(std::isfinite(style.plateVisibility) && style.plateVisibility >= 0.F && style.plateVisibility <= 1.F);
    }
}

static void testOverviewWindowLayout() {
    section("overview: fullscreen and maximized workspaces expose every window without overlap");

    for (const auto usable : {SBoxF{0, 56, 1920, 1024}, SBoxF{0, 56, 1080, 1864}, SBoxF{80, 0, 1200, 720}, SBoxF{-40, 20, 320, 240}, SBoxF{5, 7, 4, 3}}) {
        for (size_t count = 1; count <= 32; ++count) {
            std::vector<SOverviewWindowInput> windows;
            for (size_t i = 0; i < count; ++i) {
                // All desktop windows overlap, including wide video surfaces,
                // portrait terminals and several fullscreen windows.
                const double aspect = i % 3 == 0 ? 16.0 / 9.0 : (i % 3 == 1 ? 0.7 : 3.0);
                windows.push_back({{usable.x, usable.y, 1000 * aspect, 1000}, i % 4 == 0});
            }
            const auto result = layoutOverviewWindows(windows, usable);
            CHECK(result.boxes.size() == count);
            CHECK(result.spread == (count > 1));
            CHECK(result.columns >= 1 && result.columns <= count);

            for (size_t i = 0; i < result.boxes.size(); ++i) {
                const auto& box = result.boxes[i];
                CHECK(box.w > 0 && box.h > 0);
                CHECK(box.x >= usable.x - 1e-6 && box.y >= usable.y - 1e-6);
                CHECK(box.x + box.w <= usable.x + usable.w + 1e-6);
                CHECK(box.y + box.h <= usable.y + usable.h + 1e-6);
                CHECK_NEAR(box.w / box.h, windows[i].desktop.w / windows[i].desktop.h, 1e-6);
                CHECK(previewContains({box, usable}, box.cx(), box.cy()));
                for (size_t j = 0; j < result.boxes.size(); ++j) {
                    if (i == j)
                        continue;
                    const auto&  other    = result.boxes[j];
                    const double overlapW = std::min(box.x + box.w, other.x + other.w) - std::max(box.x, other.x);
                    const double overlapH = std::min(box.y + box.h, other.y + other.h) - std::max(box.y, other.y);
                    CHECK(overlapW <= 1e-6 || overlapH <= 1e-6);
                    CHECK(!previewContains({other, usable}, box.cx(), box.cy()));
                }
            }
        }
    }

    const SBoxF                       usable{0, 56, 1920, 1024};
    std::vector<SOverviewWindowInput> pair{{{12, 68, 941, 1000}, false}, {{12, 68, 1896, 1000}, true}};
    const auto                        spread = layoutOverviewWindows(pair, usable);
    CHECK(spread.spread && spread.columns == 2);
    CHECK(spread.boxes[0].cx() < spread.boxes[1].cx());
    const double gapX = (spread.boxes[0].x + spread.boxes[0].w + spread.boxes[1].x) / 2.0;
    CHECK(!previewContains({spread.boxes[0], usable}, gapX, usable.cy()));
    CHECK(!previewContains({spread.boxes[1], usable}, gapX, usable.cy()));

    // Resizing holds the arrangement chosen when the gesture began.
    pair[0].desktop.w  = 4000;
    const auto resized = layoutOverviewWindows(pair, usable, spread.columns);
    CHECK(resized.columns == spread.columns);
    CHECK_NEAR(resized.boxes[0].cx(), spread.boxes[0].cx(), 1e-6);
    CHECK_NEAR(resized.boxes[1].cx(), spread.boxes[1].cx(), 1e-6);

    pair[1].fullscreen = false;
    const auto normal  = layoutOverviewWindows(pair, usable);
    CHECK(!normal.spread && normal.columns == 0);
    for (size_t i = 0; i < pair.size(); ++i) {
        CHECK_NEAR(normal.boxes[i].x, pair[i].desktop.x, 1e-6);
        CHECK_NEAR(normal.boxes[i].y, pair[i].desktop.y, 1e-6);
        CHECK_NEAR(normal.boxes[i].w, pair[i].desktop.w, 1e-6);
        CHECK_NEAR(normal.boxes[i].h, pair[i].desktop.h, 1e-6);
    }

    pair.erase(pair.begin());
    pair[0].fullscreen = true;
    const auto single  = layoutOverviewWindows(pair, usable);
    CHECK(!single.spread && single.boxes.size() == 1);
    CHECK_NEAR(single.boxes[0].w, usable.w, 1e-6);
    CHECK_NEAR(single.boxes[0].cx(), usable.cx(), 1e-6);
    CHECK_NEAR(single.boxes[0].cy(), usable.cy(), 1e-6);
    CHECK(layoutOverviewWindows({}, usable).boxes.empty());

    const std::vector<SOverviewWindowInput> wide{{{0, 0, 1600, 900}, true}, {{0, 0, 1600, 900}, false}};
    CHECK(layoutOverviewWindows(wide, {0, 56, 1080, 1864}).columns == 1);

    // Letterboxing and clipping do not introduce invisible clickable regions.
    const SWindowPreviewGeometry clipped{{0, 0, 100, 100}, {20, 30, 50, 60}};
    CHECK(!previewContains(clipped, 10, 50));
    CHECK(!previewContains(clipped, 50, 20));
    CHECK(previewContains(clipped, 50, 50));
    CHECK(!previewContains(clipped, 70, 50));

    CHECK_NEAR(overviewWindowVisibility(0.F, true, 0.F), 0.F, 1e-6F);
    CHECK_NEAR(overviewWindowVisibility(0.F, true, 1.F), 1.F, 1e-6F);
    CHECK_NEAR(overviewWindowVisibility(0.F, true, 0.5F), 0.5F, 1e-6F);
    CHECK_NEAR(overviewWindowVisibility(1.F, true, 0.F), 1.F, 1e-6F);
    CHECK_NEAR(overviewWindowVisibility(0.F, false, 0.F), 1.F, 1e-6F);
}

static void testFullscreenPreviewGeometry() {
    section("preview: fullscreen zoom covers reserved panel strips before handing back to the desktop");

    const auto checkBox = [](const SBoxF& actual, const SBoxF& expected) {
        CHECK_NEAR(actual.x, expected.x, 1e-6);
        CHECK_NEAR(actual.y, expected.y, 1e-6);
        CHECK_NEAR(actual.w, expected.w, 1e-6);
        CHECK_NEAR(actual.h, expected.h, 1e-6);
    };

    for (const auto monitor : {SBoxF{0, 0, 1920, 1080}, SBoxF{0, 0, 1080, 1920}, SBoxF{0, 0, 1280, 720}}) {
        // Top, bottom and side panels, plus a monitor with no reserved area.
        for (const auto usable : {SBoxF{0, 56, monitor.w, monitor.h - 56}, SBoxF{0, 0, monitor.w, monitor.h - 48}, SBoxF{80, 0, monitor.w - 80, monitor.h},
                                  SBoxF{0, 0, monitor.w - 80, monitor.h}, monitor}) {
            SLayoutParams params;
            params.screenW = monitor.w;
            params.screenH = monitor.h;
            params.aspect  = usable.w / usable.h;

            const auto tiles = layout(makeInput(3), params).tiles;
            CHECK(tiles.size() == 3);
            for (const auto& tile : tiles) {
                // True fullscreen covers the panel; maximized windows retain
                // their work-area bounds. Both must reach their real geometry.
                for (const auto desktopBox : {monitor, usable}) {
                    const SWindowPreviewGeometry desktop{desktopBox, monitor};
                    const SWindowPreviewGeometry overview{fitBox(insetBox(tile.box, 0.8), desktopBox.w / desktopBox.h), tile.box};

                    const auto opened = overviewWindowGeometry(overview, desktop, true, 1.F);
                    checkBox(opened.box, overview.box);
                    checkBox(opened.clip, overview.clip);

                    const auto closed = overviewWindowGeometry(overview, desktop, true, 0.F);
                    checkBox(closed.box, desktop.box);
                    checkBox(closed.clip, monitor);

                    auto previous = closed;
                    for (int step = 0; step <= 100; ++step) {
                        const float progress = step / 100.F;
                        const auto  geometry = overviewWindowGeometry(overview, desktop, true, progress);
                        CHECK_NEAR(geometry.box.w / geometry.box.h, desktopBox.w / desktopBox.h, 1e-6);
                        CHECK(geometry.clip.x <= geometry.box.x + 1e-6);
                        CHECK(geometry.clip.y <= geometry.box.y + 1e-6);
                        CHECK(geometry.clip.x + geometry.clip.w >= geometry.box.x + geometry.box.w - 1e-6);
                        CHECK(geometry.clip.y + geometry.clip.h >= geometry.box.y + geometry.box.h - 1e-6);
                        CHECK(std::abs(geometry.box.x - previous.box.x) <= monitor.w * 0.011);
                        CHECK(std::abs(geometry.clip.y - previous.clip.y) <= monitor.h * 0.011);

                        // A non-anchor tile must never expand across another
                        // workspace, even as the active one reaches fullscreen.
                        const auto other = overviewWindowGeometry(overview, desktop, false, progress);
                        checkBox(other.box, overview.box);
                        checkBox(other.clip, overview.clip);
                        previous = geometry;
                    }
                }
            }
        }
    }

    // Reproduce the reported 56px top-strip regression on a 1080p output.
    const SWindowPreviewGeometry desktop{{0, 0, 1920, 1080}, {0, 0, 1920, 1080}};
    const SWindowPreviewGeometry overview{{70, 5, 1750, 984.375}, {70, 56, 1750, 933.333}};
    const auto                   nearEnd = overviewWindowGeometry(overview, desktop, true, 0.01F);
    CHECK(nearEnd.box.contains(960, 28));
    CHECK(nearEnd.clip.contains(960, 28));

    for (float progress : {-1.F, 2.F, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        const auto  bounded  = overviewWindowGeometry(overview, desktop, true, progress);
        const auto& expected = progress > 1.F && std::isfinite(progress) ? overview : desktop;
        checkBox(bounded.box, expected.box);
        checkBox(bounded.clip, expected.clip);
    }
}

int main() {
    std::printf("hyprspace test suite\n\n");

    testGridUniformity();
    testGridFitsOnScreen();
    testPortraitGridFitsOnScreen();
    testGridNoOverlap();
    testGridShape();
    testGridEdgeCases();
    testNavigation();
    testHitTest();
    testInsetBox();
    testWorkspaceLabels();
    testWorkspaceForDigit();
    testDesktopParsing();
    testClassLookup();
    testClassCandidates();
    testWebAppLookup();
    testIconResolution();
    testTextRaster();
    testPlaceholderIcon();
    testIconLoading();
    testButtonCapture();
    testScrollAccumulator();
    testSwitcherLayout();
    testImageCache();
    testPreviewStyle();
    testOverviewWindowLayout();
    testFullscreenPreviewGeometry();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
