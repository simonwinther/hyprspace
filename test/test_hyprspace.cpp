// hyprspace - host-side unit tests.
//
// Covers the parts that carry real logic and no Hyprland dependency: the
// overview layout/navigation maths, .desktop parsing, icon resolution and the
// cairo/pango rasteriser.

#include "../src/DesktopDb.hpp"
#include "../src/Geometry.hpp"
#include "../src/Raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace hyprspace;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond)                                                                                                                                            \
    do {                                                                                                                                                       \
        ++g_checks;                                                                                                                                            \
        if (!(cond)) {                                                                                                                                         \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                                                      \
            ++g_failures;                                                                                                                                      \
        }                                                                                                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                                                                                  \
    do {                                                                                                                                                       \
        ++g_checks;                                                                                                                                            \
        if (std::fabs((a) - (b)) > (eps)) {                                                                                                                    \
            std::printf("  FAIL %s:%d  %s (%.4f) != %s (%.4f)\n", __FILE__, __LINE__, #a, (double)(a), #b, (double)(b));                                        \
            ++g_failures;                                                                                                                                      \
        }                                                                                                                                                      \
    } while (0)

static void section(const char* name) {
    std::printf("%s\n", name);
}

// ---------------------------------------------------------------- layout ----

static void testLayoutPreservesAspect() {
    section("layout: tiles keep their exact aspect ratio");

    std::vector<STileInput> in = {
        {.key = 0, .aspect = 16.0 / 9.0, .workspaceId = 1},
        {.key = 1, .aspect = 1.0, .workspaceId = 1},
        {.key = 2, .aspect = 3.0 / 4.0, .workspaceId = 1},
    };

    SLayoutParams p;
    p.screenW = 1920;
    p.screenH = 1080;

    const auto out = layout(in, p);
    CHECK(out.tiles.size() == 3);
    CHECK(out.bands.size() == 1);

    for (const auto& t : out.tiles) {
        CHECK(t.box.w > 0 && t.box.h > 0);
        CHECK_NEAR(t.box.w / t.box.h, in[t.key].aspect, 1e-6);
    }
}

static void testLayoutStaysInsideScreen() {
    section("layout: nothing escapes the padded screen area");

    std::vector<STileInput> in;
    for (int i = 0; i < 11; ++i)
        in.push_back({.key = static_cast<size_t>(i), .aspect = (i % 3 == 0) ? 2.0 : 1.3, .workspaceId = 1 + i % 3});

    SLayoutParams p;
    p.screenW = 2560;
    p.screenH = 1440;
    p.padding = 50;

    const auto out = layout(in, p);
    CHECK(out.tiles.size() == in.size());
    CHECK(out.bands.size() == 3); // three distinct workspaces

    for (const auto& t : out.tiles) {
        CHECK(t.box.x >= p.padding - 1.0);
        CHECK(t.box.y >= p.padding - 1.0);
        CHECK(t.box.x + t.box.w <= p.screenW - p.padding + 1.0);
        CHECK(t.box.y + t.box.h <= p.screenH - p.padding + 1.0);
    }
}

static void testLayoutNoOverlap() {
    section("layout: tiles never overlap");

    std::vector<STileInput> in;
    for (int i = 0; i < 8; ++i)
        in.push_back({.key = static_cast<size_t>(i), .aspect = 1.6, .workspaceId = 1});

    SLayoutParams p;
    p.screenW = 1920;
    p.screenH = 1080;

    const auto out = layout(in, p);

    for (size_t i = 0; i < out.tiles.size(); ++i) {
        for (size_t j = i + 1; j < out.tiles.size(); ++j) {
            const auto& a = out.tiles[i].box;
            const auto& b = out.tiles[j].box;

            const bool separated = a.x + a.w <= b.x + 1e-6 || b.x + b.w <= a.x + 1e-6 || a.y + a.h <= b.y + 1e-6 || b.y + b.h <= a.y + 1e-6;
            CHECK(separated);
        }
    }
}

static void testLayoutGroupsByWorkspace() {
    section("layout: one band per workspace, sorted by id");

    std::vector<STileInput> in = {
        {.key = 0, .aspect = 1.6, .workspaceId = 3},
        {.key = 1, .aspect = 1.6, .workspaceId = 1},
        {.key = 2, .aspect = 1.6, .workspaceId = 3},
    };

    SLayoutParams p;
    const auto    out = layout(in, p);

    CHECK(out.bands.size() == 2);
    CHECK(out.bands[0].workspaceId == 1);
    CHECK(out.bands[1].workspaceId == 3);
    CHECK(out.bands[0].tileCount == 1);
    CHECK(out.bands[1].tileCount == 2);

    // Band 1 must sit above band 3 on screen.
    CHECK(out.bands[0].contentBox.y < out.bands[1].contentBox.y);
}

static void testLayoutEdgeCases() {
    section("layout: degenerate inputs are handled");

    SLayoutParams p;
    CHECK(layout({}, p).tiles.empty());

    std::vector<STileInput> one = {{.key = 0, .aspect = 1.6, .workspaceId = 1}};
    CHECK(layout(one, p).tiles.size() == 1);

    // A screen smaller than its own padding must not produce garbage.
    SLayoutParams tiny;
    tiny.screenW = 40;
    tiny.screenH = 40;
    tiny.padding = 56;
    CHECK(layout(one, tiny).tiles.empty());
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
}

static void testIconResolution() {
    section("desktop: icon theme path resolution");

    const auto root = fs::temp_directory_path() / "hyprspace-test-icons";
    fs::remove_all(root);

    fs::create_directories(root / "hicolor" / "48x48" / "apps");
    fs::create_directories(root / "hicolor" / "256x256" / "apps");
    fs::create_directories(root / "hicolor" / "scalable" / "apps");

    auto touch = [](const fs::path& p) { std::ofstream(p) << "x"; };
    touch(root / "hicolor" / "48x48" / "apps" / "sized-only.png");
    touch(root / "hicolor" / "256x256" / "apps" / "sized-only.png");
    touch(root / "hicolor" / "scalable" / "apps" / "vector.svg");

    CDesktopDb db;
    db.setIconRoots({root.string()});

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

    CHECK(!db.resolveIconPath("nothing-here", 64).has_value());
    CHECK(!db.resolveIconPath("", 64).has_value());

    // An absolute path in Icon= is used verbatim when it exists.
    const auto abs = (root / "hicolor" / "scalable" / "apps" / "vector.svg").string();
    CHECK(db.resolveIconPath(abs, 64) == abs);
    CHECK(!db.resolveIconPath("/nonexistent/icon.png", 64).has_value());

    fs::remove_all(root);
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

int main() {
    std::printf("hyprspace test suite\n\n");

    testLayoutPreservesAspect();
    testLayoutStaysInsideScreen();
    testLayoutNoOverlap();
    testLayoutGroupsByWorkspace();
    testLayoutEdgeCases();
    testNavigation();
    testHitTest();
    testDesktopParsing();
    testClassLookup();
    testClassCandidates();
    testWebAppLookup();
    testIconResolution();
    testTextRaster();
    testPlaceholderIcon();
    testIconLoading();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
