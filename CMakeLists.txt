cmake_minimum_required(VERSION 3.21)

project(BlackKatRecomp
    VERSION 0.1.0
    DESCRIPTION "Native PC port scaffold for Pirates: The Legend of Black Kat (NTSC-U SLUS-20365)"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ps2xRuntime includes <smmintrin.h> directly. Upstream CI passes the same flag.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_compile_options(-msse4.1)
endif()

# The Vita toolchain probe in PS2Recomp switches the whole build if this is set.
if(DEFINED ENV{VITASDK} AND NOT CMAKE_TOOLCHAIN_FILE)
    message(STATUS "Unsetting VITASDK so the Black Kat host build stays native")
    unset(ENV{VITASDK})
endif()

# Studio is a separate GUI and the test suite is upstream's. The default build
# is the recompiler, the analyzer, the runtime, and the blackkat host.
set(PS2X_BUILD_RECOMP ON CACHE BOOL "Build ps2_recomp")
set(PS2X_BUILD_ANALYZER ON CACHE BOOL "Build ps2_analyzer")
set(PS2X_BUILD_RUNTIME ON CACHE BOOL "Build ps2xRuntime")
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "Build ps2xStudio (not required to port Black Kat)")
set(PS2X_BUILD_TEST OFF CACHE BOOL "Build ps2xTest")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/PreparePs2RecompOverlay.cmake")
bkr_prepare_ps2recomp_overlay("${CMAKE_CURRENT_SOURCE_DIR}/third_party/PS2Recomp")

add_subdirectory("${BKR_PS2RECOMP_OVERLAY}" "${CMAKE_BINARY_DIR}/ps2recomp")

# Upstream turns IPO on for every Release target. Linking this tree with LTO
# needs much more RAM than a plain -O3 link. Keep the default Linux path
# buildable; opt back in with -DBKR_ENABLE_LTO=ON.
option(BKR_ENABLE_LTO "Enable interprocedural optimization on PS2Recomp targets" OFF)
if(NOT BKR_ENABLE_LTO)
    bkr_disable_ipo_recursive("${BKR_PS2RECOMP_OVERLAY}")
endif()

foreach(bkr_tool IN ITEMS ps2_recomp ps2_analyzer)
    if(TARGET ${bkr_tool})
        set_target_properties(${bkr_tool} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
    endif()
endforeach()

set(BKR_GENERATED_DIR "${CMAKE_SOURCE_DIR}/build/generated" CACHE PATH
    "Where ps2_recomp writes C++ for SLUS_203.65")
set(BKR_REGISTER_CPP "${BKR_GENERATED_DIR}/register_functions.cpp")

if(NOT TARGET ps2_runtime)
    message(STATUS
        "BlackKatRecomp: ps2_runtime was not built, so the blackkat game target is skipped.")
else()
    add_executable(blackkat
        src/main.cpp
        src/overrides/blackkat_slus_20365.cpp
    )
    target_link_libraries(blackkat PRIVATE ps2_runtime)
    target_include_directories(blackkat PRIVATE "${BKR_GENERATED_DIR}")
    target_compile_features(blackkat PRIVATE cxx_std_20)
    set_target_properties(blackkat PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    if(EXISTS "${BKR_REGISTER_CPP}")
        file(GLOB BKR_GENERATED_SOURCES CONFIGURE_DEPENDS "${BKR_GENERATED_DIR}/*.cpp")
        target_sources(blackkat PRIVATE ${BKR_GENERATED_SOURCES})
        message(STATUS
            "BlackKatRecomp: compiling recompiled output from ${BKR_GENERATED_DIR}")
    else()
        target_sources(blackkat PRIVATE src/runtime/empty_function_table.cpp)
        message(STATUS
            "BlackKatRecomp: ${BKR_REGISTER_CPP} is not present, so blackkat is built with an empty function table. "
            "The game ELF is not required for this target. Extract game/SLUS_203.65, run ps2_recomp on config/slus_203.65.toml, then reconfigure to compile the generated C++.")
    endif()
endif()

if(TARGET ps2_recomp)
    add_custom_target(recompile
        COMMAND $<TARGET_FILE:ps2_recomp> "${CMAKE_SOURCE_DIR}/config/slus_203.65.toml"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        DEPENDS ps2_recomp
        COMMENT "Recompile SLUS_203.65 into build/generated"
        USES_TERMINAL
        VERBATIM
    )
endif()
