cmake_minimum_required(VERSION 3.12)
project(sim_joystick LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "sim_joystick requires Linux uinput")
endif()

add_executable(sim_joystick main.cpp)
target_compile_features(sim_joystick PRIVATE cxx_std_17)
target_compile_options(sim_joystick PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
  add_executable(test_layout test_layout.cpp)
  target_compile_features(test_layout PRIVATE cxx_std_17)
  target_compile_options(test_layout PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME joystick_layout COMMAND test_layout)
endif()
