#include "joystick_layout.h"
#include <iostream>

int main() {
    // joydev returns zero for counts, and positive byte counts for maps.
    auto reader = [](unsigned long request, void* data) {
        switch (request) {
        case JSIOCGAXES:
        case JSIOCGBUTTONS:
            *static_cast<unsigned char*>(data) = 8;
            return 0;
        case JSIOCGAXMAP: {
            const unsigned char map[] = {0, 1, 2, 3, 4, 5, 16, 17};
            std::memcpy(data, map, sizeof(map));
            return ABS_CNT;
        }
        case JSIOCGBTNMAP: {
            const unsigned short map[] = {304, 305, 307, 308, 310, 311, 314, 315};
            std::memcpy(data, map, sizeof(map));
            return static_cast<int>((KEY_MAX - BTN_MISC + 1) * sizeof(unsigned short));
        }
        default: throw std::runtime_error("Unexpected ioctl request");
        }
    };
    try {
        sim_joystick::verify_layout(reader);
        sim_joystick::verify_layout([&](unsigned long request, void* data) {
            reader(request, data);
            return 0;
        });
        auto expect_error = [](auto faulty_reader, const char* text) {
            try {
                sim_joystick::verify_layout(faulty_reader);
            } catch (const std::runtime_error& error) {
                if (std::string(error.what()).find(text) != std::string::npos) return;
                throw;
            }
            throw std::runtime_error("Invalid layout/ioctl unexpectedly accepted");
        };
        expect_error([&](unsigned long request, void* data) {
            if (request == JSIOCGAXMAP) { errno = EINVAL; return -1; }
            return reader(request, data);
        }, "JSIOCGAXMAP:");
        expect_error([&](unsigned long request, void* data) {
            const int result = reader(request, data);
            if (request == JSIOCGBTNMAP) static_cast<unsigned short*>(data)[7] = 316;
            return result;
        }, "Actual buttons (8): 304 305 307 308 310 311 314 316");
        expect_error([&](unsigned long request, void* data) {
            const int result = reader(request, data);
            if (request == JSIOCGAXES) *static_cast<unsigned char*>(data) = 7;
            return result;
        }, "Actual axes (7)");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "PASS: byte-count/zero success, ioctl errors, invalid layout diagnostics\n";
}
