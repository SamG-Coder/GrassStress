cmake_minimum_required(VERSION 3.24)
project(GrassStress VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(grass_core
    src/environment.cpp
    src/environment_simulation.cpp
    src/first_person_camera.cpp
    src/grass_field.cpp
    src/ground_texture.cpp
)
target_include_directories(grass_core PUBLIC src)
target_compile_options(grass_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(GrassStress WIN32
    src/main.cpp
    src/rtx_caps.cpp
    src/gpu_telemetry.cpp
    src/dxr_renderer.cpp
    src/streamline_host.cpp
    src/optix_denoise.cpp
)
target_include_directories(GrassStress PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/streamline/include"
    "${CMAKE_SOURCE_DIR}/third_party/streamline/external/ngx-sdk/include"
    "${CMAKE_SOURCE_DIR}/third_party/optix/optix-dev-9.0.0/include"
    "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.3/include"
)
target_link_libraries(GrassStress PRIVATE
    grass_core d3d12 dxgi d3dcompiler user32 gdi32 shell32
    windowscodecs ole32 uuid cfgmgr32
)
target_compile_definitions(GrassStress PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(GrassStress PRIVATE -Wall -Wextra -Wpedantic)
if(MINGW)
    target_link_options(GrassStress PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

add_executable(grass_field_tests src/grass_field_tests.cpp)
target_link_libraries(grass_field_tests PRIVATE grass_core)

enable_testing()
add_test(NAME grass_field_tests COMMAND grass_field_tests)

set_target_properties(GrassStress grass_field_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Streamline DLLs stay in bin/streamline so sl.interposer is not auto-injected.
