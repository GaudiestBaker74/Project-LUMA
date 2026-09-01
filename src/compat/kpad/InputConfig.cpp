// =============================================================================
// Platform::CompatInput::InputConfig — INI parsing + defaults (see header).
// =============================================================================

#include "compat/kpad/InputConfig.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace Platform::CompatInput {

namespace {

constexpr int kMaxConfigPath = 512;
char gOverridePath[kMaxConfigPath] = {};

void trim(char* s) {
    char* start = s;
    while (*start != '\0' && std::isspace(static_cast<unsigned char>(*start))) {
        ++start;
    }
    char* end = start + std::strlen(start);
    while (end > start && std::isspace(static_cast<unsigned char>(end[-1]))) {
        --end;
    }
    *end = '\0';
    if (start != s) {
        std::memmove(s, start, end - start + 1);
    }
}

bool startsWith(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

// Parses "key:space", "mouse:left", "mouse:right", "mouse:middle" or "none".
Platform::Key parseBindValue(const char* value) {
    if (startsWith(value, "key:")) {
        return Platform::keyFromName(value + 4);
    }
    if (std::strcmp(value, "mouse:left") == 0) {
        return Platform::Key::MouseLeft;
    }
    if (std::strcmp(value, "mouse:right") == 0) {
        return Platform::Key::MouseRight;
    }
    if (std::strcmp(value, "mouse:middle") == 0) {
        return Platform::Key::MouseMiddle;
    }
    return Platform::Key::None;  // "none" and anything unknown = unbound
}

} // namespace

const char* wiiActionName(WiiAction action) {
    switch (action) {
        case WiiAction::Up: return "up";
        case WiiAction::Down: return "down";
        case WiiAction::Left: return "left";
        case WiiAction::Right: return "right";
        case WiiAction::A: return "a";
        case WiiAction::B: return "b";
        case WiiAction::One: return "one";
        case WiiAction::Two: return "two";
        case WiiAction::Plus: return "plus";
        case WiiAction::Minus: return "minus";
        case WiiAction::Home: return "home";
        case WiiAction::C: return "c";
        case WiiAction::Z: return "z";
        case WiiAction::Shake: return "shake";
        case WiiAction::Count: break;
    }
    return nullptr;
}

WiiAction wiiActionFromName(const char* name) {
    for (int i = 0; i < static_cast<int>(WiiAction::Count); ++i) {
        const char* n = wiiActionName(static_cast<WiiAction>(i));
        if (n != nullptr && std::strcmp(n, name) == 0) {
            return static_cast<WiiAction>(i);
        }
    }
    return WiiAction::Count;  // not found
}

InputConfig InputConfig::defaults() {
    InputConfig cfg;
    for (int i = 0; i < 4; ++i) {
        cfg.channels[i].source = Source::None;
    }

    // Channel 0: keyboard + mouse.
    ChannelConfig& c0 = cfg.channels[0];
    c0.source = Source::KeyboardMouse;
    c0.bind[static_cast<int>(WiiAction::Up)] = Platform::Key::Up;
    c0.bind[static_cast<int>(WiiAction::Down)] = Platform::Key::Down;
    c0.bind[static_cast<int>(WiiAction::Left)] = Platform::Key::Left;
    c0.bind[static_cast<int>(WiiAction::Right)] = Platform::Key::Right;
    c0.bind[static_cast<int>(WiiAction::A)] = Platform::Key::Space;
    c0.bind[static_cast<int>(WiiAction::B)] = Platform::Key::MouseLeft;
    c0.bind[static_cast<int>(WiiAction::C)] = Platform::Key::ShiftL;
    c0.bind[static_cast<int>(WiiAction::Z)] = Platform::Key::MouseRight;
    c0.bind[static_cast<int>(WiiAction::One)] = Platform::Key::Q;
    c0.bind[static_cast<int>(WiiAction::Two)] = Platform::Key::E;
    c0.bind[static_cast<int>(WiiAction::Plus)] = Platform::Key::Enter;
    c0.bind[static_cast<int>(WiiAction::Minus)] = Platform::Key::Backspace;
    c0.bind[static_cast<int>(WiiAction::Home)] = Platform::Key::Escape;
    c0.bind[static_cast<int>(WiiAction::Shake)] = Platform::Key::K;

    // Channels 1-3: the first three SDL gamepads (if present).
    cfg.channels[1].source = Source::Gamepad;
    cfg.channels[1].gamepadIndex = 0;
    cfg.channels[2].source = Source::Gamepad;
    cfg.channels[2].gamepadIndex = 1;
    cfg.channels[3].source = Source::Gamepad;
    cfg.channels[3].gamepadIndex = 2;

    return cfg;
}

void InputConfig::setOverridePath(const char* path) {
    if (path == nullptr) {
        gOverridePath[0] = '\0';
        return;
    }
    std::strncpy(gOverridePath, path, kMaxConfigPath - 1);
    gOverridePath[kMaxConfigPath - 1] = '\0';
}

InputConfig InputConfig::load() {
    InputConfig cfg = InputConfig::defaults();

    const char* path = gOverridePath[0] != '\0' ? gOverridePath : "config/input.ini";
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) {
        return cfg;  // no config file → defaults
    }

    char line[256];
    int currentChannel = -1;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char* s = line;
        // Strip trailing newline.
        char* nl = std::strchr(s, '\n');
        if (nl != nullptr) {
            *nl = '\0';
        }
        trim(s);
        if (*s == '\0' || *s == '#' || *s == ';') {
            continue;
        }
        if (*s == '[') {
            char* close = std::strchr(s, ']');
            if (close == nullptr) {
                continue;
            }
            *close = '\0';
            const char* section = s + 1;
            currentChannel = -1;
            for (int i = 0; i < 4; ++i) {
                char name[16];
                std::snprintf(name, sizeof(name), "channel%d", i);
                if (std::strcmp(section, name) == 0) {
                    currentChannel = i;
                    break;
                }
            }
            continue;
        }
        if (currentChannel < 0) {
            continue;
        }
        char* eq = std::strchr(s, '=');
        if (eq == nullptr) {
            continue;
        }
        *eq = '\0';
        char key[64];
        std::strncpy(key, s, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
        char* value = eq + 1;
        trim(key);
        trim(value);

        ChannelConfig& ch = cfg.channels[currentChannel];
        if (std::strcmp(key, "source") == 0) {
            if (std::strcmp(value, "keyboard_mouse") == 0) {
                ch.source = Source::KeyboardMouse;
            } else if (std::strcmp(value, "gamepad") == 0) {
                ch.source = Source::Gamepad;
            } else if (std::strcmp(value, "none") == 0) {
                ch.source = Source::None;
            }
        } else if (std::strcmp(key, "gamepad") == 0) {
            ch.gamepadIndex = std::atoi(value);
        } else if (std::strcmp(key, "use_classic") == 0) {
            ch.useClassic = std::strcmp(value, "1") == 0 || std::strcmp(value, "true") == 0;
        } else if (std::strcmp(key, "stick") == 0) {
            ch.stickUsesArrows = std::strcmp(value, "arrows") == 0;
        } else if (startsWith(key, "bind_") || std::strcmp(key, "shake") == 0) {
            // "bind_<action>=..." plus the shorthand "shake=..." (an alias for
            // bind_shake, matching the documented config format).
            const char* actionName = startsWith(key, "bind_") ? key + 5 : "shake";
            WiiAction action = wiiActionFromName(actionName);
            if (action != WiiAction::Count) {
                ch.bind[static_cast<int>(action)] = parseBindValue(value);
            }
        } else if (std::strcmp(key, "pointer_sensitivity") == 0) {
            ch.pointerSensitivity = std::strtof(value, nullptr);
        } else if (std::strcmp(key, "sensor_height") == 0) {
            ch.sensorHeight = std::strtof(value, nullptr);
        } else if (std::strcmp(key, "shake_strength") == 0) {
            ch.shakeStrength = std::strtof(value, nullptr);
        }
    }

    std::fclose(f);
    return cfg;
}

} // namespace Platform::CompatInput
