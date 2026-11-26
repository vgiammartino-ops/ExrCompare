cmake_minimum_required(VERSION 3.25)
project(ExrCompareNative VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC)
    # Static vcpkg libraries + the DLL C++ runtime that the .NET host also uses.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

option(EXRC_BUILD_TESTS "Build unit tests" ON)

find_package(Imath CONFIG REQUIRED)
find_package(OpenEXR CONFIG REQUIRED)
if(WIN32)
    find_package(OpenColorIO CONFIG REQUIRED)
else()
    # Some Linux distributions ship a broken OpenColorIOConfig.cmake; locate it directly.
    find_path(OCIO_INCLUDE_DIR OpenColorIO/OpenColorIO.h REQUIRED)
    find_library(OCIO_LIBRARY NAMES OpenColorIO REQUIRED)
    add_library(OpenColorIO::OpenColorIO UNKNOWN IMPORTED)
    set_target_properties(OpenColorIO::OpenColorIO PROPERTIES
        IMPORTED_LOCATION "${OCIO_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${OCIO_INCLUDE_DIR}")
endif()
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ------------------------------------------------------------------ portable engine --
add_library(exrc_engine STATIC
    engine/src/Types.cpp
    engine/src/SequenceScanner.cpp
    engine/src/ExrReader.cpp
    engine/src/FrameCache.cpp
    engine/src/DecodeScheduler.cpp
    engine/src/PlaybackController.cpp
    engine/src/ViewTransform.cpp
    engine/src/CompareLayout.cpp
    engine/src/ColorPipeline.cpp
    engine/src/Analysis.cpp
    engine/src/Session.cpp
)
target_include_directories(exrc_engine PUBLIC engine/include)
target_link_libraries(exrc_engine PUBLIC OpenEXR::OpenEXR Imath::Imath OpenColorIO::OpenColorIO
                                         nlohmann_json::nlohmann_json Threads::Threads)
if(MSVC)
    target_compile_options(exrc_engine PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_compile_definitions(exrc_engine PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(exrc_engine PRIVATE -Wall -Wextra)
    set_target_properties(exrc_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# --------------------------------------------------- Windows: renderer + C API DLL --
if(WIN32)
    add_library(exrcore SHARED
        capi/exrcore.cpp
        render/src/Renderer.cpp
        render/src/Snapshot.cpp
    )
    target_include_directories(exrcore PRIVATE capi render/include render/src)
    target_compile_definitions(exrcore PRIVATE EXRCORE_BUILDING)
    target_compile_options(exrcore PRIVATE /W4 /permissive- /utf-8 /bigobj)
    target_link_libraries(exrcore PRIVATE exrc_engine d3d11 dxgi d3dcompiler dxguid d2d1 dwrite windowscodecs)
    install(TARGETS exrcore RUNTIME DESTINATION bin)
endif()

# -------------------------------------------------------------------------- tests --
if(EXRC_BUILD_TESTS)
    enable_testing()
    add_executable(exrc_tests tests/TestMain.cpp tests/EngineTests.cpp tests/SessionTests.cpp tests/OcioTests.cpp)
    target_link_libraries(exrc_tests PRIVATE exrc_engine)
    add_test(NAME exrc_tests COMMAND exrc_tests)
    if(WIN32)
        # Drives exrcore.dll headless on a WARP (software) D3D11 device: shader compile,
        # OCIO LUT binding, every compare mode, snapshot PNG.
        add_executable(exrcore_render_test tests/RenderTest.cpp)
        target_include_directories(exrcore_render_test PRIVATE capi)
        target_link_libraries(exrcore_render_test PRIVATE exrcore exrc_engine)
        add_test(NAME exrcore_render_test COMMAND exrcore_render_test)
    endif()
endif()
