#pragma once

#include "../src/SearchInput.hpp"
#include "../src/Raster.hpp"

static void testSearchInput() {
    section("search input: insertion, Unicode deletion, selection and bounded queries");
    CSearchInput input;
    CHECK(input.text().empty() && input.cursor() == 0);
    CHECK(!input.backspace() && !input.eraseForward());
    CHECK(input.insert("gh"));
    input.left();
    CHECK(input.insert("o"));
    CHECK(input.text() == "goh" && input.cursor() == 2);
    CHECK(input.backspace());
    CHECK(input.text() == "gh" && input.cursor() == 1);
    CHECK(input.eraseForward());
    CHECK(input.text() == "g");
    input.selectAll();
    CHECK(input.insert("æøå"));
    input.left();
    CHECK(input.cursor() == 4);
    CHECK(input.backspace());
    CHECK(input.text() == "æå" && input.cursor() == 2);
    CHECK(input.eraseForward());
    CHECK(input.text() == "æ");
    input.home(true);
    CHECK((input.selection() == std::pair<size_t, size_t>(0, 2)));
    CHECK(input.insert("Ø"));
    CHECK(input.text() == "Ø" && input.cursor() == 2);
    input.setCursor(1); // Click positions may never split UTF-8.
    CHECK(input.cursor() == 0);
    input.end(true);
    input.left();
    CHECK(input.cursor() == 0 && input.selection().first == input.selection().second);
    input.selectAll();
    input.right();
    CHECK(input.cursor() == 2 && input.selection().first == input.selection().second);
    input.clear();
    CHECK(!input.insert("\xff"));
    CHECK(input.insert(std::string(CSearchInput::MAX_BYTES, 'W')));
    CHECK(!input.insert("æ"));
    input.selectAll();
    CHECK(input.insert("replacement"));
    CHECK(input.text() == "replacement");
    input.home();
    input.left();
    CHECK(input.cursor() == 0);
    input.end();
    input.right();
    CHECK(input.cursor() == input.text().size());

    section("search input: shaped caret, click positions and clipped text at fractional scales");
    for (const auto* font : {"Sans 14", "Monospace 18px"}) {
        for (double scale : {1.0, 1.2, 1.25, 1.5, 2.0}) {
            const std::string text  = "ghæøå";
            const auto        empty = measureTextCursor("", font, 0, scale);
            CHECK(empty.x == 0 && empty.caretHeight > 0);
            const auto end = measureTextCursor(text, font, text.size(), scale);
            CHECK_NEAR(end.x, end.width, 1.0);
            for (size_t position : {0U, 1U, 2U, 4U, 6U, 8U}) {
                const auto caret = measureTextCursor(text, font, position, scale);
                CHECK(caret.height == end.height && caret.caretHeight > 0);
                CHECK(textIndexAtX(text, font, caret.x / scale, scale) == position);
            }
            CHECK(textIndexAtX(text, font, -100, scale) == 0);
            CHECK(textIndexAtX(text, font, 10000, scale) == text.size());
            const auto plain    = renderInputText(text, font, {}, 80, 0, scale, 0, 0, {0.2, 0.4, 1, 0.28});
            const auto selected = renderInputText(text, font, {}, 80, 0, scale, 2, 6, {0.2, 0.4, 1, 0.28});
            CHECK(plain.ok() && selected.ok());
            CHECK(plain.w == static_cast<int>(80 * scale) && plain.h == end.height);
            CHECK(plain.data != selected.data);
            const std::string longText(CSearchInput::MAX_BYTES, 'W');
            const auto        longCaret = measureTextCursor(longText, font, longText.size(), scale);
            const auto        clipped   = renderInputText(longText, font, {}, 80, longCaret.x / scale - 79, scale, 0, 0, {});
            CHECK(clipped.ok() && clipped.w == static_cast<int>(80 * scale));
            CHECK(clipped.data.size() <= MAX_RASTER_BYTES);
        }
    }
    CHECK(!renderInputText("query", "Sans 14", {}, 80, 0, 0, 0, 0, {}).ok());
    CHECK(!renderInputText("query", "Sans 14", {}, 80, std::numeric_limits<double>::infinity(), 1, 0, 0, {}).ok());
    CHECK(!renderInputText("query", "Sans 4096", {}, 8192, 0, 1, 0, 0, {}).ok());
}
