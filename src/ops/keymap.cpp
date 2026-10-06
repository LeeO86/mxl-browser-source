// SPDX-License-Identifier: MIT
#include "ops/keymap.hpp"

#include <array>
#include <utility>

namespace mbs::ops
{
    namespace
    {
        struct Entry
        {
            std::string_view code;
            int vk;
        };

        // DOM KeyboardEvent.code → Windows virtual key (Chromium's dom_code_data and
        // keyboard_codes_win, the subset a US/European keyboard sends).
        constexpr std::array kTable{
            Entry{"Backspace", 0x08}, Entry{"Tab", 0x09}, Entry{"Enter", 0x0D}, Entry{"ShiftLeft", 0x10}, Entry{"ShiftRight", 0x10},
            Entry{"ControlLeft", 0x11}, Entry{"ControlRight", 0x11}, Entry{"AltLeft", 0x12}, Entry{"AltRight", 0x12}, Entry{"Pause", 0x13},
            Entry{"CapsLock", 0x14}, Entry{"Escape", 0x1B}, Entry{"Space", 0x20}, Entry{"PageUp", 0x21}, Entry{"PageDown", 0x22},
            Entry{"End", 0x23}, Entry{"Home", 0x24}, Entry{"ArrowLeft", 0x25}, Entry{"ArrowUp", 0x26}, Entry{"ArrowRight", 0x27},
            Entry{"ArrowDown", 0x28}, Entry{"PrintScreen", 0x2C}, Entry{"Insert", 0x2D}, Entry{"Delete", 0x2E}, Entry{"MetaLeft", 0x5B},
            Entry{"MetaRight", 0x5C}, Entry{"ContextMenu", 0x5D}, Entry{"Numpad0", 0x60}, Entry{"Numpad1", 0x61}, Entry{"Numpad2", 0x62},
            Entry{"Numpad3", 0x63}, Entry{"Numpad4", 0x64}, Entry{"Numpad5", 0x65}, Entry{"Numpad6", 0x66}, Entry{"Numpad7", 0x67},
            Entry{"Numpad8", 0x68}, Entry{"Numpad9", 0x69}, Entry{"NumpadMultiply", 0x6A}, Entry{"NumpadAdd", 0x6B},
            Entry{"NumpadSubtract", 0x6D}, Entry{"NumpadDecimal", 0x6E}, Entry{"NumpadDivide", 0x6F}, Entry{"NumpadEnter", 0x0D},
            Entry{"NumLock", 0x90}, Entry{"ScrollLock", 0x91}, Entry{"Semicolon", 0xBA}, Entry{"Equal", 0xBB}, Entry{"Comma", 0xBC},
            Entry{"Minus", 0xBD}, Entry{"Period", 0xBE}, Entry{"Slash", 0xBF}, Entry{"Backquote", 0xC0}, Entry{"BracketLeft", 0xDB},
            Entry{"Backslash", 0xDC}, Entry{"BracketRight", 0xDD}, Entry{"Quote", 0xDE}, Entry{"IntlBackslash", 0xE2},
        };

        bool startsWith(std::string_view text, std::string_view prefix)
        {
            return text.substr(0, prefix.size()) == prefix;
        }

        bool endsWith(std::string_view text, std::string_view suffix)
        {
            return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
        }
    }

    std::optional<int> virtualKey(std::string_view code)
    {
        if (code.size() == 4 && startsWith(code, "Key") && code[3] >= 'A' && code[3] <= 'Z')
        {
            return code[3]; // 0x41…0x5A
        }
        if (code.size() == 6 && startsWith(code, "Digit") && code[5] >= '0' && code[5] <= '9')
        {
            return code[5]; // 0x30…0x39
        }
        if (code.size() >= 2 && code.size() <= 3 && code[0] == 'F')
        {
            int n = 0;
            for (char c : code.substr(1))
            {
                if (c < '0' || c > '9')
                {
                    return std::nullopt;
                }
                n = n * 10 + (c - '0');
            }
            if (n >= 1 && n <= 24)
            {
                return 0x70 + n - 1; // VK_F1…VK_F24
            }
            return std::nullopt;
        }
        for (auto const& entry : kTable)
        {
            if (entry.code == code)
            {
                return entry.vk;
            }
        }
        return std::nullopt;
    }

    bool modifierFlags(std::vector<std::string> const& names, std::uint32_t& flags)
    {
        for (auto const& name : names)
        {
            if (name == "shift")
            {
                flags |= kShiftDown;
            }
            else if (name == "ctrl")
            {
                flags |= kControlDown;
            }
            else if (name == "alt")
            {
                flags |= kAltDown;
            }
            else if (name == "meta")
            {
                flags |= kCommandDown;
            }
            else if (name == "capslock")
            {
                flags |= kCapsLockOn;
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    std::u16string utf16(std::string_view utf8)
    {
        std::u16string out;
        std::size_t i = 0;
        while (i < utf8.size())
        {
            auto const c = static_cast<unsigned char>(utf8[i]);
            std::uint32_t cp = 0xFFFD;
            int need = 0;
            if (c < 0x80)
            {
                cp = c;
            }
            else if ((c & 0xE0) == 0xC0)
            {
                cp = c & 0x1F;
                need = 1;
            }
            else if ((c & 0xF0) == 0xE0)
            {
                cp = c & 0x0F;
                need = 2;
            }
            else if ((c & 0xF8) == 0xF0)
            {
                cp = c & 0x07;
                need = 3;
            }
            ++i;
            for (int n = 0; n < need; ++n, ++i)
            {
                if (i >= utf8.size() || (static_cast<unsigned char>(utf8[i]) & 0xC0) != 0x80)
                {
                    cp = 0xFFFD;
                    break;
                }
                cp = (cp << 6) | (static_cast<unsigned char>(utf8[i]) & 0x3F);
            }
            if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            {
                cp = 0xFFFD;
            }
            if (cp >= 0x10000)
            {
                cp -= 0x10000;
                out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
                out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
            }
            else
            {
                out.push_back(static_cast<char16_t>(cp));
            }
        }
        return out;
    }

    std::vector<KeyEvent> keyEvents(bool down, std::string_view code, std::string_view key, std::uint32_t modifiers, bool repeat)
    {
        std::vector<KeyEvent> events;
        auto const vk = virtualKey(code);
        if (startsWith(code, "Numpad"))
        {
            modifiers |= kIsKeyPad;
        }
        if (endsWith(code, "Left"))
        {
            modifiers |= kIsLeft;
        }
        else if (endsWith(code, "Right"))
        {
            modifiers |= kIsRight;
        }
        if (repeat)
        {
            modifiers |= kIsRepeat;
        }
        KeyEvent base;
        base.windowsKeyCode = vk.value_or(0);
        base.modifiers = modifiers;
        if (!down)
        {
            base.type = KeyEventType::KeyUp;
            events.push_back(base);
            return events;
        }
        base.type = KeyEventType::RawKeyDown;
        events.push_back(base);
        // The character: a printable key is one code point; Enter types a carriage return.
        // With Ctrl or Meta held the key is a shortcut, which Chromium's editing commands take
        // from the RAWKEYDOWN.
        if ((modifiers & (kControlDown | kCommandDown)) != 0)
        {
            return events;
        }
        std::u16string text;
        if (key == "Enter")
        {
            text = u"\r";
        }
        else
        {
            text = utf16(key);
            bool const single = text.size() == 1 || (text.size() == 2 && text[0] >= 0xD800 && text[0] <= 0xDBFF);
            if (!single || key.empty())
            {
                return events; // a named key ("Backspace", "ArrowLeft"): no character
            }
        }
        for (char16_t unit : text)
        {
            KeyEvent ch = base;
            ch.type = KeyEventType::Char;
            ch.character = unit;
            ch.unmodifiedCharacter = unit >= u'A' && unit <= u'Z' ? static_cast<char16_t>(unit - u'A' + u'a') : unit;
            events.push_back(ch);
        }
        return events;
    }
}
