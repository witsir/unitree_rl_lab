#pragma once

#include <linux/input.h>
#include <linux/joystick.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace sim_joystick {
inline constexpr std::array<int, 8> axes = {
    ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
inline constexpr std::array<int, 8> buttons = {
    BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST,
    BTN_TL, BTN_TR, BTN_SELECT, BTN_START};

// Injectable reader permits regression tests without root/uinput.
// Map queries may return a positive byte count on success, or zero on older kernels.
template <typename Reader>
void verify_layout(Reader read) {
    std::array<unsigned char, ABS_CNT> axis_map{};
    std::array<unsigned short, KEY_MAX - BTN_MISC + 1> button_map{};
    unsigned char axis_count = 0, button_count = 0;
    auto checked_read = [&](unsigned long request, void* data, const char* name) {
        if (read(request, data) < 0)
            throw std::runtime_error(std::string(name) + ": " + std::strerror(errno));
    };
    checked_read(JSIOCGAXES, &axis_count, "JSIOCGAXES");
    checked_read(JSIOCGBUTTONS, &button_count, "JSIOCGBUTTONS");
    checked_read(JSIOCGAXMAP, axis_map.data(), "JSIOCGAXMAP");
    checked_read(JSIOCGBTNMAP, button_map.data(), "JSIOCGBTNMAP");
    if (axis_count == axes.size() && button_count == buttons.size() &&
        std::equal(axes.begin(), axes.end(), axis_map.begin()) &&
        std::equal(buttons.begin(), buttons.end(), button_map.begin())) return;

    std::ostringstream error;
    error << "Kernel joystick layout does not match MuJoCo Xbox mapping.\n"
          << "Actual axes (" << static_cast<int>(axis_count) << "):";
    for (unsigned int i = 0; i < axis_count && i < axis_map.size(); ++i)
        error << ' ' << static_cast<int>(axis_map[i]);
    error << "\nExpected axes:";
    for (int code : axes) error << ' ' << code;
    error << "\nActual buttons (" << static_cast<int>(button_count) << "):";
    for (unsigned int i = 0; i < button_count && i < button_map.size(); ++i)
        error << ' ' << button_map[i];
    error << "\nExpected buttons:";
    for (int code : buttons) error << ' ' << code;
    throw std::runtime_error(error.str());
}
} // namespace sim_joystick
