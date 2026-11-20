cmake_minimum_required(VERSION 3.15)
project(out_vibewasapi CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT

# The plug-in DLL that Winamp loads.
add_library(out_vibewasapi SHARED
  src/config_dialog.cpp
  src/device_monitor.cpp
  src/playback_status.cpp
  src/plugin.cpp
  src/render_engine.cpp
  src/sample_convert.cpp
  src/settings.cpp
  src/wasapi_device.cpp
  src/out_vibewasapi.rc)
set_target_properties(out_vibewasapi PROPERTIES PREFIX "" OUTPUT_NAME "out_vibewasapi")
target_include_directories(out_vibewasapi PRIVATE src)
target_link_libraries(out_vibewasapi PRIVATE ole32 user32)

if(MINGW)
  target_link_options(out_vibewasapi PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# Automated tests that need no sound card. Run them with: ctest -C Release
enable_testing()
add_executable(vibe_unit_tests tests/unit_tests.cpp src/playback_status.cpp src/sample_convert.cpp src/settings.cpp)
target_include_directories(vibe_unit_tests PRIVATE src)
add_test(NAME unit_tests COMMAND vibe_unit_tests)

# A manual test that runs the finished DLL on a real audio device.
add_executable(vibe_smoke_host tests/smoke_host.cpp)
target_include_directories(vibe_smoke_host PRIVATE src)
