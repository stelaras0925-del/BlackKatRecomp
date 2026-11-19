# PS2Recomp's analyzer, recompiler, studio, and tests include headers and
# cmake modules through CMAKE_SOURCE_DIR. That variable is the top of the
# CMake tree, so add_subdirectory() on the submodule looks for those files
# in BlackKatRecomp instead of in the submodule.
#
# This writes a symlink tree under build/ps2recomp-src and rewrites the few
# CMakeLists that need it so ${CMAKE_SOURCE_DIR} becomes ${BKR_PS2RECOMP_ROOT}
# (the real submodule checkout). Sources stay symlinks; the submodule itself
# is not modified. Delete the overlay (or the whole build/ directory) after
# bumping the pin and configure again.

set(BKR_OVERLAY_LAYOUT "1" CACHE INTERNAL "Bump when the overlay layout changes")

function(bkr_symlink source dest)
    if(IS_SYMLINK "${dest}" OR EXISTS "${dest}")
        return()
    endif()
    file(CREATE_LINK "${source}" "${dest}" SYMBOLIC)
endfunction()

function(bkr_write_patched_cmakelists source dest)
    file(READ "${source}" contents)
    string(REPLACE "\${CMAKE_SOURCE_DIR}" "\${BKR_PS2RECOMP_ROOT}" contents "${contents}")
    set(banner
        "# Patched by BlackKatRecomp (cmake/PreparePs2RecompOverlay.cmake).\n# \${CMAKE_SOURCE_DIR} in this file is \${BKR_PS2RECOMP_ROOT}, the PS2Recomp checkout.\n# Do not edit this copy; edit the submodule or the overlay script.\n\n")
    set(rewritten "${banner}${contents}")
    if(EXISTS "${dest}")
        file(READ "${dest}" existing)
        if(existing STREQUAL rewritten)
            return()
        endif()
    endif()
    file(WRITE "${dest}" "${rewritten}")
endfunction()

function(bkr_overlay_project upstream dest)
    file(MAKE_DIRECTORY "${dest}")
    file(GLOB entries LIST_DIRECTORIES true "${upstream}/*")
    foreach(entry IN LISTS entries)
        get_filename_component(name "${entry}" NAME)
        set(patched OFF)
        if(name MATCHES "^ps2x(Recomp|Analyzer|Studio|Test)$")
            set(patched ON)
        endif()

        if(NOT patched)
            bkr_symlink("${entry}" "${dest}/${name}")
            continue()
        endif()

        file(MAKE_DIRECTORY "${dest}/${name}")
        file(GLOB children LIST_DIRECTORIES true "${entry}/*")
        foreach(child IN LISTS children)
            get_filename_component(child_name "${child}" NAME)
            if(child_name STREQUAL "CMakeLists.txt")
                bkr_write_patched_cmakelists("${child}" "${dest}/${name}/CMakeLists.txt")
            else()
                bkr_symlink("${child}" "${dest}/${name}/${child_name}")
            endif()
        endforeach()
    endforeach()
endfunction()

function(bkr_prepare_ps2recomp_overlay upstream)
    if(NOT EXISTS "${upstream}/CMakeLists.txt")
        message(FATAL_ERROR
            "PS2Recomp submodule is missing at ${upstream}. Run: git submodule update --init third_party/PS2Recomp")
    endif()

    execute_process(
        COMMAND git -C "${upstream}" rev-parse HEAD
        OUTPUT_VARIABLE pin
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE pin_result
    )
    if(NOT pin_result EQUAL 0 OR pin STREQUAL "")
        message(FATAL_ERROR "Could not read the PS2Recomp git pin in ${upstream}")
    endif()

    set(overlay "${CMAKE_SOURCE_DIR}/build/ps2recomp-src")
    set(stamp_file "${overlay}/.bkr-overlay-stamp")
    set(stamp "${pin} layout ${BKR_OVERLAY_LAYOUT}")
    set(current "")
    if(EXISTS "${stamp_file}")
        file(READ "${stamp_file}" current)
        string(STRIP "${current}" current)
    endif()
    if(NOT current STREQUAL stamp)
        file(REMOVE_RECURSE "${overlay}")
    endif()

    bkr_overlay_project("${upstream}" "${overlay}")
    file(WRITE "${stamp_file}" "${stamp}\n")

    set(BKR_PS2RECOMP_ROOT "${upstream}" PARENT_SCOPE)
    set(BKR_PS2RECOMP_OVERLAY "${overlay}" PARENT_SCOPE)
    message(STATUS "PS2Recomp ${pin} via overlay ${overlay}")
endfunction()

function(bkr_disable_ipo_recursive dir)
    get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        set_property(TARGET "${target}" PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
        set_property(TARGET "${target}" PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
    endforeach()
    get_property(subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subs)
        bkr_disable_ipo_recursive("${sub}")
    endforeach()
endfunction()
