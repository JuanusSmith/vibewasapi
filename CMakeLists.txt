cmake_minimum_required(VERSION 3.15)
project(out_vibewasapi CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT

add_library(out_vibewasapi SHARED out_vibewasapi.cpp out_vibewasapi.rc)
set_target_properties(out_vibewasapi PROPERTIES PREFIX "" OUTPUT_NAME "out_vibewasapi")
target_link_libraries(out_vibewasapi PRIVATE ole32 user32)

if(MINGW)
  target_link_options(out_vibewasapi PRIVATE -static -static-libgcc -static-libstdc++)
endif()
