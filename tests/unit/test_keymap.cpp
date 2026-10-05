// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include "ops/keymap.hpp"

using namespace mbs::ops;

TEST_CASE("DOM codes map to Windows virtual keys")
{
    CHECK(virtualKey("KeyA") == 0x41);
    CHECK(virtualKey("KeyZ") == 0x5A);
    CHECK(virtualKey("Digit0") == 0x30);
    CHECK(virtualKey("F1") == 0x70);
    CHECK(virtualKey("F12") == 0x7B);
    CHECK(virtualKey("Enter") == 0x0D);
    CHECK(virtualKey("ArrowLeft") == 0x25);
    CHECK(virtualKey("NumpadEnter") == 0x0D);
    CHECK(virtualKey("Semicolon") == 0xBA);
    CHECK_FALSE(virtualKey("F25"));
    CHECK_FALSE(virtualKey("Fx"));
    CHECK_FALSE(virtualKey("Nonsense"));
}

TEST_CASE("modifier names become CEF flags")
{
    std::uint32_t flags = 0;
    CHECK(modifierFlags({"shift", "ctrl"}, flags));
    CHECK(flags == (kShiftDown | kControlDown));
    flags = 0;
    CHECK_FALSE(modifierFlags({"hyper"}, flags));
}

TEST_CASE("a printable key is RAWKEYDOWN then CHAR; release is KEYUP")
{
    auto events = keyEvents(true, "KeyA", "a", 0, false);
    REQUIRE(events.size() == 2);
    CHECK(events[0].type == KeyEventType::RawKeyDown);
    CHECK(events[0].windowsKeyCode == 0x41);
    CHECK(events[1].type == KeyEventType::Char);
    CHECK(events[1].character == u'a');

    events = keyEvents(true, "KeyA", "A", kShiftDown, false);
    REQUIRE(events.size() == 2);
    CHECK(events[1].character == u'A');
    CHECK(events[1].unmodifiedCharacter == u'a');
    CHECK((events[1].modifiers & kShiftDown) != 0);

    events = keyEvents(false, "KeyA", "a", 0, false);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == KeyEventType::KeyUp);
}

TEST_CASE("shortcuts, named keys, Enter and non-ASCII characters")
{
    // Ctrl+A is a shortcut: no character.
    auto events = keyEvents(true, "KeyA", "a", kControlDown, false);
    CHECK(events.size() == 1);
    // Named keys have no character.
    CHECK(keyEvents(true, "Backspace", "Backspace", 0, false).size() == 1);
    CHECK(keyEvents(true, "ArrowLeft", "ArrowLeft", 0, false).size() == 1);
    // Enter types a carriage return.
    events = keyEvents(true, "Enter", "Enter", 0, false);
    REQUIRE(events.size() == 2);
    CHECK(events[1].character == u'\r');
    // A Swiss-German umlaut and a character outside the BMP (two UTF-16 units).
    events = keyEvents(true, "Quote", "ä", 0, false);
    REQUIRE(events.size() == 2);
    CHECK(events[1].character == u'ä');
    events = keyEvents(true, "KeyX", "😀", 0, false);
    REQUIRE(events.size() == 3);
    CHECK(events[1].character == 0xD83D);
    CHECK(events[2].character == 0xDE00);
    // Repeat, keypad and side flags.
    events = keyEvents(true, "Numpad5", "5", 0, true);
    CHECK((events[0].modifiers & kIsKeyPad) != 0);
    CHECK((events[0].modifiers & kIsRepeat) != 0);
    CHECK((keyEvents(true, "ShiftLeft", "Shift", kShiftDown, false)[0].modifiers & kIsLeft) != 0);
}

TEST_CASE("UTF-8 to UTF-16, invalid sequences become U+FFFD")
{
    CHECK(utf16("Grüezi") == u"Grüezi");
    CHECK(utf16("你好") == u"你好");
    CHECK(utf16("\xff") == u"�");
    CHECK(utf16("a\xc3") == u"a�");
}
