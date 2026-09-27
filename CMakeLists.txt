cmake_minimum_required(VERSION 3.16)

project(GameEngine
        VERSION 0.1.0
        DESCRIPTION "A reusable 2D game engine built on top of SFML"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default single-config generators (Makefile, Ninja) to Debug, so a plain
# `cmake -S . -B build` gives a useful development build. Multi-config
# generators such as Ninja Multi-Config pick the configuration at build time.
get_property(ENGINE_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT ENGINE_IS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---------------------------------------------------------------------------
# Engine settings
#
# These are the single obvious place where the window is configured. They are
# forwarded into the generated header engine/EngineConfig.hpp, so any of them
# can be overridden without touching code, e.g.
#   cmake -S . -B build -DENGINE_WINDOW_WIDTH=1920 -DENGINE_WINDOW_HEIGHT=1080
# ---------------------------------------------------------------------------
set(ENGINE_WINDOW_WIDTH 1280 CACHE STRING "Width of the game window in pixels")
set(ENGINE_WINDOW_HEIGHT 720 CACHE STRING "Height of the game window in pixels")
set(ENGINE_WINDOW_TITLE "GameEngine" CACHE STRING "Title shown in the window title bar")
set(ENGINE_FRAMERATE_LIMIT 60 CACHE STRING "Frame rate cap in FPS (0 disables the cap)")
set(ENGINE_BACKGROUND_COLOR_RED 32 CACHE STRING "Red channel of the window background colour")
set(ENGINE_BACKGROUND_COLOR_GREEN 36 CACHE STRING "Green channel of the window background colour")
set(ENGINE_BACKGROUND_COLOR_BLUE 48 CACHE STRING "Blue channel of the window background colour")

option(ENGINE_BUILD_TESTS "Build the automated smoke tests" ${PROJECT_IS_TOP_LEVEL})

# ---------------------------------------------------------------------------
# SFML discovery
#
# Homebrew ships SFML 2.x as the keg-only formula `sfml@2`, so its CMake
# package files sit outside CMake's default search prefixes. Ask Homebrew where
# the formula lives rather than hard-coding a machine-specific path. This block
# is skipped on other platforms, and whenever SFML has already been located
# through -DSFML_ROOT=<path> or the SFML_ROOT environment variable.
# ---------------------------------------------------------------------------
if(APPLE AND NOT SFML_DIR AND NOT DEFINED ENV{SFML_ROOT})
    find_program(ENGINE_BREW_EXECUTABLE brew)

    if(ENGINE_BREW_EXECUTABLE)
        foreach(ENGINE_SFML_FORMULA IN ITEMS sfml@2 sfml)
            # `--installed` makes this fail when the formula is not installed.
            execute_process(
                COMMAND "${ENGINE_BREW_EXECUTABLE}" --prefix --installed "${ENGINE_SFML_FORMULA}"
                OUTPUT_VARIABLE ENGINE_SFML_PREFIX
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE ENGINE_SFML_LOOKUP_RESULT)

            if(ENGINE_SFML_LOOKUP_RESULT EQUAL 0
               AND EXISTS "${ENGINE_SFML_PREFIX}/lib/cmake/SFML/SFMLConfig.cmake")
                set(SFML_ROOT "${ENGINE_SFML_PREFIX}"
                    CACHE PATH "Root directory of the SFML installation" FORCE)
                message(STATUS "Using SFML from Homebrew formula '${ENGINE_SFML_FORMULA}' "
                               "at ${ENGINE_SFML_PREFIX}")
                break()
            endif()
        endforeach()
    endif()
endif()

find_package(SFML 2.6 REQUIRED COMPONENTS graphics window)

# ---------------------------------------------------------------------------
# Generated configuration header
# ---------------------------------------------------------------------------
configure_file("${PROJECT_SOURCE_DIR}/config/engine.config.cmake.in"
               "${PROJECT_BINARY_DIR}/generated/engine/EngineConfig.hpp"
               @ONLY)

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
if(MSVC)
    set(ENGINE_WARNING_FLAGS /W4)
else()
    set(ENGINE_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

# The asset configuration parser and the opaque resource handles, kept in their
# own library so that their isolation is a property of the build rather than a
# promise in a comment.
#
# These sources are deliberately SFML-free and filesystem-free: the parser reads
# a configuration string, and the handles are pointers to a private
# implementation. This target deliberately does not link SFML, so an
# `#include <SFML/...>` added to any of them would fail to compile, and a symbol
# from SFML would fail to link. Either way the boundary holds without anyone
# having to remember it. The test suites below rely on exactly that.
add_library(engine_assets STATIC
            src/engine/assets/AssetFile.cpp
            src/engine/assets/Font.cpp
            src/engine/assets/Texture.cpp)

target_include_directories(engine_assets PUBLIC
                           $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                           $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>)

target_compile_features(engine_assets PUBLIC cxx_std_17)
target_compile_options(engine_assets PRIVATE ${ENGINE_WARNING_FLAGS})

# The reusable engine library. It deliberately contains no main() so that the
# examples and the tests can each provide their own entry point.
add_library(engine STATIC
            src/engine/Application.cpp
            src/engine/Time.cpp
            src/engine/ecs/ComponentStorage.cpp
            src/engine/ecs/EntityManager.cpp
            src/engine/ecs/EntityView.cpp
            src/engine/ecs/SystemManager.cpp
            src/engine/graphics/SfmlRenderer.cpp
            src/engine/input/SfmlKeyMap.cpp
            src/engine/math/Vec2.cpp
            src/engine/systems/CameraSystem.cpp
            src/engine/systems/MovementSystem.cpp
            src/engine/systems/PhysicsSystem.cpp
            src/engine/systems/RenderSystem.cpp)

target_include_directories(engine PUBLIC
                           $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
                           $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>)

target_link_libraries(engine PUBLIC sfml-graphics sfml-window engine_assets)
target_compile_features(engine PUBLIC cxx_std_17)
target_compile_options(engine PRIVATE ${ENGINE_WARNING_FLAGS})

add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE engine)
target_compile_options(game PRIVATE ${ENGINE_WARNING_FLAGS})

# ---------------------------------------------------------------------------
# Tests
#
# Phase 1 had no logic worth unit testing, so the meaningful check there was that
# the application builds, launches, runs its loop and shuts down cleanly. Phase 2
# adds real logic in engine::Vec2, which is now covered directly.
# ---------------------------------------------------------------------------
if(ENGINE_BUILD_TESTS)
    enable_testing()

    # Library-level smoke test: drives engine::Application directly.
    add_executable(application_smoke_test tests/ApplicationSmokeTest.cpp)
    target_link_libraries(application_smoke_test PRIVATE engine)
    target_compile_options(application_smoke_test PRIVATE ${ENGINE_WARNING_FLAGS})
    add_test(NAME application.smoke COMMAND application_smoke_test)
    set_tests_properties(application.smoke PROPERTIES TIMEOUT 60)

    # End-to-end smoke test: runs the shipped executable through its own main().
    add_test(NAME application.end_to_end COMMAND game --frames 5)
    set_tests_properties(application.end_to_end PROPERTIES TIMEOUT 60)

    # Math test: engine::Vec2, which is SFML independent.
    add_executable(vec2_test tests/Vec2Test.cpp)
    target_link_libraries(vec2_test PRIVATE engine)
    target_compile_options(vec2_test PRIVATE ${ENGINE_WARNING_FLAGS})
    add_test(NAME math.vec2 COMMAND vec2_test)
    set_tests_properties(math.vec2 PROPERTIES TIMEOUT 60)

    # ECS test: entity lifetime, component storage, tags and deferred destruction.
    add_executable(ecs_test tests/EcsTest.cpp)
    target_link_libraries(ecs_test PRIVATE engine)
    target_compile_options(ecs_test PRIVATE ${ENGINE_WARNING_FLAGS})
    add_test(NAME ecs.core COMMAND ecs_test)
    set_tests_properties(ecs.core PROPERTIES TIMEOUT 60)

    # ECS systems and component queries.
    add_executable(ecs_systems_test tests/EcsSystemsTest.cpp)
    target_link_libraries(ecs_systems_test PRIVATE engine)
    target_compile_options(ecs_systems_test PRIVATE ${ENGINE_WARNING_FLAGS})
    add_test(NAME ecs.systems COMMAND ecs_systems_test)
    set_tests_properties(ecs.systems PROPERTIES TIMEOUT 60)

    # Runtime timing and the Transform component.
    add_executable(runtime_test tests/RuntimeTest.cpp)
    target_link_libraries(runtime_test PRIVATE engine)
    target_compile_options(runtime_test PRIVATE ${ENGINE_WARNING_FLAGS})
    add_test(NAME runtime.timing_transform COMMAND runtime_test)
    set_tests_properties(runtime.timing_transform PROPERTIES TIMEOUT 60)

    # Rendering: the transform-to-render mapping, RenderSystem, and a real
    # graphics integration check that reads back pixels.
    add_executable(render_test tests/RenderTest.cpp)
    target_link_libraries(render_test PRIVATE engine)
    target_compile_options(render_test PRIVATE ${ENGINE_WARNING_FLAGS})
    add_test(NAME render.foundation COMMAND render_test)
    set_tests_properties(render.foundation PROPERTIES TIMEOUT 60)

    # Input: the key state machine, the SFML key adapter, and MovementSystem.
    add_executable(input_test tests/InputTest.cpp)
    target_link_libraries(input_test PRIVATE engine)
    target_compile_options(input_test PRIVATE ${ENGINE_WARNING_FLAGS})
    add_test(NAME input.keyboard_movement COMMAND input_test)
    set_tests_properties(input.keyboard_movement PROPERTIES TIMEOUT 60)

    # Physics: AABB maths, collision detection and resolution.
    add_executable(physics_test tests/PhysicsTest.cpp)
    target_link_libraries(physics_test PRIVATE engine)
    target_compile_options(physics_test PRIVATE ${ENGINE_WARNING_FLAGS})
    add_test(NAME physics.collision COMMAND physics_test)
    set_tests_properties(physics.collision PROPERTIES TIMEOUT 60)

    # Camera: world/screen mapping, zoom, follow, and independence from physics.
    add_executable(camera_test tests/CameraTest.cpp)
    target_link_libraries(camera_test PRIVATE engine)
    target_compile_options(camera_test PRIVATE ${ENGINE_WARNING_FLAGS})
    add_test(NAME camera.world_to_screen COMMAND camera_test)
    set_tests_properties(camera.world_to_screen PROPERTIES TIMEOUT 60)

    # Asset configuration parser.
    #
    # This test deliberately links engine_assets rather than engine. Nothing in
    # it needs a window, a GPU or SFML, and linking the parser's own library is
    # what turns "the parser is SFML-free" from a comment into something the
    # linker enforces. The two source paths are injected so the suite can also
    # assert on the parser's own text, which is the only way to check that no
    # filesystem call has been added to code whose behaviour cannot reveal it.
    add_executable(asset_file_test tests/AssetFileTest.cpp)
    target_link_libraries(asset_file_test PRIVATE engine_assets)
    target_compile_options(asset_file_test PRIVATE ${ENGINE_WARNING_FLAGS})
    target_compile_definitions(asset_file_test PRIVATE
        ENGINE_ASSET_FILE_HEADER="${PROJECT_SOURCE_DIR}/include/engine/assets/AssetFile.hpp"
        ENGINE_ASSET_FILE_SOURCE="${PROJECT_SOURCE_DIR}/src/engine/assets/AssetFile.cpp")
    add_test(NAME assets.configuration_parser COMMAND asset_file_test)
    set_tests_properties(assets.configuration_parser PROPERTIES TIMEOUT 60)

    # The opaque texture and font handles.
    #
    # Like the parser test above, this links engine_assets rather than engine, so
    # the suite builds and links with no SFML anywhere on its link line. The four
    # source paths are injected so it can also assert on the headers' own text,
    # which is the only way to catch a public header quietly acquiring a graphics
    # dependency: such a header would still compile and still pass every runtime
    # check in the file.
    add_executable(asset_handle_test tests/AssetHandleTest.cpp)
    target_link_libraries(asset_handle_test PRIVATE engine_assets)
    target_compile_options(asset_handle_test PRIVATE ${ENGINE_WARNING_FLAGS})
    target_compile_definitions(asset_handle_test PRIVATE
        ENGINE_TEXTURE_HEADER="${PROJECT_SOURCE_DIR}/include/engine/assets/Texture.hpp"
        ENGINE_FONT_HEADER="${PROJECT_SOURCE_DIR}/include/engine/assets/Font.hpp"
        ENGINE_TEXTURE_SOURCE="${PROJECT_SOURCE_DIR}/src/engine/assets/Texture.cpp"
        ENGINE_FONT_SOURCE="${PROJECT_SOURCE_DIR}/src/engine/assets/Font.cpp")
    add_test(NAME assets.handles COMMAND asset_handle_test)
    set_tests_properties(assets.handles PROPERTIES TIMEOUT 60)
endif()
