// SPDX-License-Identifier: MIT
// Keyboard input for the page (SPEC §7.1), as obs-browser sends it: a key press is a
// RAWKEYDOWN with the Windows virtual key of the DOM `code`, then a CHAR with the character
// when the key produces text and neither Ctrl nor Meta is held; a release is a KEYUP.
// No CEF types here: flags and event types carry CEF's values (checked in src/cef).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mbs::ops
{
    // cef_event_flags_t
    enum EventFlags : std::uint32_t
    {
        kCapsLockOn = 1U << 0,
        kShiftDown = 1U << 1,
        kControlDown = 1U << 2,
        kAltDown = 1U << 3,
        kLeftMouseButton = 1U << 4,
        kMiddleMouseButton = 1U << 5,
        kRightMouseButton = 1U << 6,
        kCommandDown = 1U << 7,
        kIsKeyPad = 1U << 9,
        kIsLeft = 1U << 10,
        kIsRight = 1U << 11,
        kIsRepeat = 1U << 13,
    };

    // cef_key_event_type_t
    enum class KeyEventType
    {
        RawKeyDown = 0,
        KeyDown = 1,
        KeyUp = 2,
        Char = 3,
    };

    struct KeyEvent
    {
        KeyEventType type = KeyEventType::RawKeyDown;
        int windowsKeyCode = 0;
        char16_t character = 0;
        char16_t unmodifiedCharacter = 0;
        std::uint32_t modifiers = 0;
    };

    /// Windows virtual key of a DOM `KeyboardEvent.code` ("KeyA" → 0x41); nullopt if unknown.
    std::optional<int> virtualKey(std::string_view code);

    /// Flags of the modifier names of the protocol ("shift", "ctrl", "alt", "meta", "capslock");
    /// false when a name is unknown.
    bool modifierFlags(std::vector<std::string> const& names, std::uint32_t& flags);

    /// The CEF events for one key message. `key` is KeyboardEvent.key: a printable key is one
    /// character ("a", "Ä", " "), named keys are longer ("Enter", "ArrowLeft").
    std::vector<KeyEvent> keyEvents(bool down, std::string_view code, std::string_view key, std::uint32_t modifiers, bool repeat);

    /// UTF-8 to UTF-16; invalid sequences become U+FFFD.
    std::u16string utf16(std::string_view utf8);
}
