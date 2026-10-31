cmake_minimum_required(VERSION 3.20)
project(GamePresenceMirror LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Game Presence Mirror currently supports Windows only.")
endif()

add_executable(game_presence_mirror src/main.cpp)
target_compile_features(game_presence_mirror PRIVATE cxx_std_17)
target_compile_definitions(game_presence_mirror PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(game_presence_mirror PRIVATE ws2_32 winhttp advapi32)

# The exe must not need any DLL that Windows doesn't already ship, so link the C++ runtime statically.
if(MINGW)
  target_link_options(game_presence_mirror PRIVATE -municode -static -static-libgcc -static-libstdc++)
elseif(MSVC)
  set_property(TARGET game_presence_mirror PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()
