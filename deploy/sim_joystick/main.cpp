#include <linux/joystick.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include "joystick_layout.h"

namespace {
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }

// joydev enumerates axes/buttons by ascending Linux event code. These match
// unitree_mujoco/simulate/src/physics_joystick.h::XBoxJoystick exactly.
using sim_joystick::axes;
using sim_joystick::buttons;

void check(int result, const char* operation) {
    if (result < 0)
        throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}

class Terminal {
public:
    Terminal() {
        if (!isatty(STDIN_FILENO))
            throw std::runtime_error("Run in an interactive terminal (stdin must be a TTY).");
        check(tcgetattr(STDIN_FILENO, &saved_), "tcgetattr");
        auto raw = saved_;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        check(tcsetattr(STDIN_FILENO, TCSANOW, &raw), "tcsetattr");
    }
    ~Terminal() { tcsetattr(STDIN_FILENO, TCSANOW, &saved_); }
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;
private:
    termios saved_{};
};

class Gamepad {
public:
    explicit Gamepad(bool dry_run) : dry_run_(dry_run) {
        if (dry_run_) return;
        fd_ = open("/dev/uinput", O_WRONLY | O_CLOEXEC);
        if (fd_ < 0)
            throw std::runtime_error(std::string("Cannot open /dev/uinput: ") +
                std::strerror(errno) +
                ". Load uinput and run with permission to access /dev/uinput (see README.md).");
        try {
            check(ioctl(fd_, UI_SET_EVBIT, EV_KEY), "UI_SET_EVBIT KEY");
            for (int code : buttons)
                check(ioctl(fd_, UI_SET_KEYBIT, code), "UI_SET_KEYBIT");
            check(ioctl(fd_, UI_SET_EVBIT, EV_ABS), "UI_SET_EVBIT ABS");
            for (int code : axes) {
                check(ioctl(fd_, UI_SET_ABSBIT, code), "UI_SET_ABSBIT");
                uinput_abs_setup abs{};
                abs.code = code;
                abs.absinfo.minimum = -32767;
                abs.absinfo.maximum = 32767;
                abs.absinfo.value = (code == ABS_Z || code == ABS_RZ) ? -32767 : 0;
                check(ioctl(fd_, UI_ABS_SETUP, &abs), "UI_ABS_SETUP");
            }
            uinput_setup setup{};
            setup.id.bustype = BUS_USB;
            setup.id.vendor = 0x045e;
            setup.id.product = 0x028e;
            setup.id.version = 1;
            std::strcpy(setup.name, "Unitree Sim Keyboard Gamepad");
            check(ioctl(fd_, UI_DEV_SETUP, &setup), "UI_DEV_SETUP");
            check(ioctl(fd_, UI_DEV_CREATE), "UI_DEV_CREATE");
            created_ = true;
            char sysname[128]{};
            check(ioctl(fd_, UI_GET_SYSNAME(sizeof(sysname)), sysname), "UI_GET_SYSNAME");
            const auto base = std::filesystem::path("/sys/devices/virtual/input") / sysname;
            // Wait for joydev/udev to expose this specific device, not an unrelated js0.
            for (int attempt = 0; attempt < 60 && !stopped; ++attempt) {
                if (std::filesystem::exists(base)) {
                    for (const auto& child : std::filesystem::directory_iterator(base)) {
                        const auto name = child.path().filename().string();
                        if (name.rfind("js", 0) == 0 &&
                            std::filesystem::exists(std::filesystem::path("/dev/input") / name)) {
                            device_ = "/dev/input/" + name;
                            verify_layout();
                            std::cout << "Virtual gamepad: " << device_ << '\n'
                                      << "MuJoCo config: use_joystick: 1, joystick_type: xbox, "
                                      << "joystick_bits: 16, joystick_device: " << device_ << '\n';
                            return;
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            throw std::runtime_error("No /dev/input/jsN appeared. Load joydev: sudo modprobe joydev");
        } catch (...) {
            destroy();
            throw;
        }
    }
    ~Gamepad() { destroy(); }
    Gamepad(const Gamepad&) = delete;
    Gamepad& operator=(const Gamepad&) = delete;

    void emit(int type, int code, int value) {
        if (dry_run_) {
            if (type != EV_SYN)
                std::cout << (type == EV_KEY ? "KEY " : "ABS ")
                          << code << ' ' << value << std::endl;
            return;
        }
        input_event event{};
        event.type = type;
        event.code = code;
        event.value = value;
        ssize_t size;
        do { size = write(fd_, &event, sizeof(event)); } while (size < 0 && errno == EINTR);
        if (size != sizeof(event))
            throw std::runtime_error("Failed to write uinput event");
    }
    void sync() { emit(EV_SYN, SYN_REPORT, 0); }

private:
    void verify_layout() {
        int js = open(device_.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        check(js, "open virtual joystick");
        try {
            sim_joystick::verify_layout([js](unsigned long request, void* data) {
                return ioctl(js, request, data);
            });
        } catch (...) {
            close(js);
            throw;
        }
        close(js);
    }
    void destroy() noexcept {
        if (created_) ioctl(fd_, UI_DEV_DESTROY);
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
        created_ = false;
    }
    bool dry_run_, created_ = false;
    int fd_ = -1;
    std::string device_;
};

class Controls {
public:
    Controls(Gamepad& pad, double magnitude) : pad_(pad), value_(std::lround(32767 * magnitude)) {}

    void key(char key) {
        const auto now = Clock::now();
        if (key == '1' || key == '2' || key == '0') {
            // Ignore repeat while a pulse is active; emit a release before another press.
            if (pulse_ != 0) return;
            zero_motion();
            pulse_ = key;
            if (key != '2') pad_.emit(EV_ABS, ABS_Z, 32767); // LT
            pad_.emit(EV_KEY, key == '1' ? BTN_SOUTH : key == '0' ? BTN_EAST : BTN_START, 1);
            pad_.sync();
            pulse_end_ = now + std::chrono::milliseconds(200);
            std::cout << (key == '1' ? "Stand (LT+A)" : key == '2' ? "Policy (Start)" : "Damping (LT+B)")
                      << std::endl;
            return;
        }
        switch (key) {
        case 'w': motion_[1] = -value_; break;
        case 's': motion_[1] = value_; break;
        case 'a': motion_[0] = -value_; break;
        case 'd': motion_[0] = value_; break;
        case 'q': motion_[2] = -value_; break;
        case 'e': motion_[2] = value_; break;
        case ' ': reset(); return;
        default: return;
        }
        const int index = (key == 'w' || key == 's') ? 1 : (key == 'a' || key == 'd') ? 0 : 2;
        pad_.emit(EV_ABS, motion_axes_[index], motion_[index]);
        pad_.sync();
        motion_end_[index] = now + std::chrono::milliseconds(400);
    }

    void tick() {
        const auto now = Clock::now();
        if (pulse_ && now >= pulse_end_) release_buttons();
        for (std::size_t i = 0; i < motion_.size(); ++i) {
            if (motion_[i] && now >= motion_end_[i]) {
                motion_[i] = 0;
                pad_.emit(EV_ABS, motion_axes_[i], 0);
                pad_.sync();
            }
        }
    }

    void reset() {
        zero_motion();
        release_buttons();
    }
private:
    void zero_motion() {
        motion_.fill(0);
        for (int code : motion_axes_) pad_.emit(EV_ABS, code, 0);
        pad_.emit(EV_ABS, ABS_RY, 0);
        pad_.emit(EV_ABS, ABS_HAT0X, 0);
        pad_.emit(EV_ABS, ABS_HAT0Y, 0);
        pad_.sync();
    }
    void release_buttons() {
        for (int code : buttons) pad_.emit(EV_KEY, code, 0);
        pad_.emit(EV_ABS, ABS_Z, -32767);
        pad_.emit(EV_ABS, ABS_RZ, -32767);
        pad_.sync();
        pulse_ = 0;
    }
    Gamepad& pad_;
    int value_;
    char pulse_ = 0;
    Clock::time_point pulse_end_{};
    constexpr static std::array<int, 3> motion_axes_ = {ABS_X, ABS_Y, ABS_RX};
    std::array<int, 3> motion_{};
    std::array<Clock::time_point, 3> motion_end_{};
};
} // namespace

int main(int argc, char** argv) {
    try {
        bool dry_run = false;
        double magnitude = 0.5;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                std::cout << "Usage: sim_joystick [--axis-value 0..1] [--dry-run]\n"
                          << "1: LT+A stand; 2: Start policy; 0: LT+B damping\n"
                          << "w/s: forward/back; a/d: left/right; q/e: turn left/right\n"
                          << "Space: clear inputs; x or Ctrl+C: exit\n"
                          << "Motion expires after 400 ms without repeated input on that axis.\n"
                          << "--dry-run prints events without creating a device.\n";
                return 0;
            } else if (arg == "--dry-run") {
                dry_run = true;
            } else if (arg == "--axis-value" && i + 1 < argc) {
                const std::string value = argv[++i];
                std::size_t used = 0;
                magnitude = std::stod(value, &used);
                if (used != value.size() || !std::isfinite(magnitude) || magnitude < 0 || magnitude > 1)
                    throw std::runtime_error("--axis-value must be between 0 and 1");
            } else {
                throw std::runtime_error("Unknown or incomplete option: " + arg);
            }
        }
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        std::signal(SIGHUP, stop);
        Terminal terminal;
        Gamepad pad(dry_run);
        Controls controls(pad, magnitude);
        controls.reset();
        std::cout << "Focus this terminal. 1 stand | 2 policy | 0 damping | "
                  << "W/S A/D Q/E move | Space stop | x exit\n" << std::flush;
        // Terminals don't report key releases. Use repeated characters and per-axis expiry.
        // Consume escape sequences rather than interpreting arrow-key suffixes as commands.
        bool escape = false;
        auto escape_end = Clock::now();
        while (!stopped) {
            pollfd input{STDIN_FILENO, POLLIN, 0};
            const int result = poll(&input, 1, 20);
            if (result < 0 && errno != EINTR) check(result, "poll");
            if (result > 0 && (input.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
            if (result > 0 && (input.revents & POLLIN)) {
                char buffer[64];
                const auto count = read(STDIN_FILENO, buffer, sizeof(buffer));
                if (count == 0) break;
                if (count < 0 && errno != EINTR) check(-1, "read stdin");
                for (ssize_t i = 0; i < count; ++i) {
                    const char key = buffer[i];
                    if (key == '\033') {
                        escape = true;
                        escape_end = Clock::now() + std::chrono::milliseconds(100);
                    } else if (escape && Clock::now() < escape_end) {
                        // CSI/SS3 sequence finishes with a character in this range.
                        if ((key >= '@' && key <= '~') && key != '[' && key != 'O') escape = false;
                    } else {
                        escape = false;
                        if (key == 'x' || key == 'X') { stopped = 1; break; }
                        controls.key(key >= 'A' && key <= 'Z' ? key + ('a' - 'A') : key);
                    }
                }
            }
            controls.tick();
        }
        controls.reset();
        std::cout << "Inputs cleared; virtual gamepad removed.\n";
    } catch (const std::exception& error) {
        std::cerr << "sim_joystick: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
