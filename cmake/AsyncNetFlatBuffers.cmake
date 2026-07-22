# AsyncNetFlatBuffers.cmake — FlatBuffers code generation integration
#
# Usage:
#   include(cmake/AsyncNetFlatBuffers.cmake)
#   async_net_flatbuffers_generate(TARGET my_target FBS_FILES path/to/echo.fbs)
#
# This will:
#   1. Find flatc compiler
#   2. Generate C++ headers from .fbs files
#   3. Add generated headers to the target

function(async_net_flatbuffers_generate)
    cmake_parse_arguments(ARG "" "TARGET;OUT_DIR" "FBS_FILES;FBS_PATHS" ${ARGN})

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "async_net_flatbuffers_generate: TARGET is required")
    endif()

    if(NOT ARG_FBS_FILES)
        message(FATAL_ERROR "async_net_flatbuffers_generate: FBS_FILES is required")
    endif()

    # Find flatc - check vcpkg tools directory first
    if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        # vcpkg installs tools to <installed>/<triplet>/tools/<package>/
        set(VCPKG_TOOLS_DIR "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/flatbuffers")
        if(EXISTS "${VCPKG_TOOLS_DIR}/flatc")
            set(FLATC_EXECUTABLE "${VCPKG_TOOLS_DIR}/flatc")
        elseif(EXISTS "${VCPKG_TOOLS_DIR}/flatc.exe")
            set(FLATC_EXECUTABLE "${VCPKG_TOOLS_DIR}/flatc.exe")
        endif()
    endif()

    if(NOT FLATC_EXECUTABLE)
        find_program(FLATC_EXECUTABLE flatc)
    endif()

    if(NOT FLATC_EXECUTABLE)
        message(FATAL_ERROR "async_net_flatbuffers_generate: flatc not found. Install flatbuffers-compiler.")
    endif()

    # Default output directory
    if(NOT ARG_OUT_DIR)
        set(ARG_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    endif()

    file(MAKE_DIRECTORY ${ARG_OUT_DIR})

    set(GENERATED_HEADERS "")

    foreach(FBS_FILE ${ARG_FBS_FILES})
        # Get absolute path
        get_filename_component(FBS_ABS ${FBS_FILE} ABSOLUTE)
        get_filename_component(FBS_DIR ${FBS_ABS} DIRECTORY)
        get_filename_component(FBS_NAME ${FBS_FILE} NAME_WE)

        # Determine include path
        set(FBS_INCLUDE_ARGS "")
        if(ARG_FBS_PATHS)
            foreach(P ${ARG_FBS_PATHS})
                list(APPEND FBS_INCLUDE_ARGS "-I" "${P}")
            endforeach()
        else()
            set(FBS_INCLUDE_ARGS "-I" "${FBS_DIR}")
        endif()

        # Output header
        set(GENERATED_H "${ARG_OUT_DIR}/${FBS_NAME}_generated.h")

        # Add custom command to generate C++ from .fbs
        add_custom_command(
            OUTPUT ${GENERATED_H}
            COMMAND ${FLATC_EXECUTABLE}
                --cpp
                --gen-mutable
                --gen-object-api
                -o ${ARG_OUT_DIR}
                ${FBS_INCLUDE_ARGS}
                ${FBS_ABS}
            DEPENDS ${FBS_ABS}
            COMMENT "Generating FlatBuffers C++ code for ${FBS_FILE}"
            VERBATIM
        )

        list(APPEND GENERATED_HEADERS ${GENERATED_H})
    endforeach()

    # Add generated headers to target (INTERFACE for interface libraries)
    get_target_property(_target_type ${ARG_TARGET} TYPE)
    if(_target_type STREQUAL "INTERFACE_LIBRARY")
        target_sources(${ARG_TARGET} INTERFACE ${GENERATED_HEADERS})
        target_include_directories(${ARG_TARGET} INTERFACE ${ARG_OUT_DIR})
    else()
        target_sources(${ARG_TARGET} PRIVATE ${GENERATED_HEADERS})
        target_include_directories(${ARG_TARGET} PUBLIC ${ARG_OUT_DIR})
    endif()

    # Make generated headers visible to dependents
    set_source_files_properties(${GENERATED_HEADERS} PROPERTIES GENERATED TRUE)
endfunction()

# Helper: generate FlatBuffers code and create a library
# Usage: async_net_flatbuffers_library(NAME my_fbs_lib FBS_FILES echo.fbs)
function(async_net_flatbuffers_library)
    cmake_parse_arguments(ARG "" "NAME;OUT_DIR" "FBS_FILES;FBS_PATHS" ${ARGN})

    if(NOT ARG_NAME)
        message(FATAL_ERROR "async_net_flatbuffers_library: NAME is required")
    endif()

    if(NOT ARG_FBS_FILES)
        message(FATAL_ERROR "async_net_flatbuffers_library: FBS_FILES is required")
    endif()

    if(NOT ARG_OUT_DIR)
        set(ARG_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_${ARG_NAME}")
    endif()

    # Create an INTERFACE library (header-only, FlatBuffers is header-only)
    add_library(${ARG_NAME} INTERFACE)

    # Generate FlatBuffers code
    async_net_flatbuffers_generate(
        TARGET ${ARG_NAME}
        FBS_FILES ${ARG_FBS_FILES}
        OUT_DIR ${ARG_OUT_DIR}
        FBS_PATHS ${ARG_FBS_PATHS}
    )

    # Link FlatBuffers headers/library
    # Add platform-specific prefix paths so find_package can locate FlatBuffers config
    set(_FB_PREFIX_HINTS "")
    if(APPLE)
        if(EXISTS "/opt/homebrew")
            list(APPEND _FB_PREFIX_HINTS "/opt/homebrew")
        endif()
        if(EXISTS "/usr/local")
            list(APPEND _FB_PREFIX_HINTS "/usr/local")
        endif()
    endif()
    if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND _FB_PREFIX_HINTS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    endif()

    find_package(FlatBuffers QUIET HINTS ${_FB_PREFIX_HINTS})
    if(FlatBuffers_FOUND)
        if(TARGET FlatBuffers::FlatBuffers)
            target_link_libraries(${ARG_NAME} INTERFACE FlatBuffers::FlatBuffers)
        elseif(TARGET flatbuffers::flatbuffers)
            target_link_libraries(${ARG_NAME} INTERFACE flatbuffers::flatbuffers)
        elseif(TARGET flatbuffers)
            target_link_libraries(${ARG_NAME} INTERFACE flatbuffers)
        elseif(FLATBUFFERS_LIBRARIES)
            # Fallback: use variables set by Find module
            target_link_libraries(${ARG_NAME} INTERFACE ${FLATBUFFERS_LIBRARIES})
            if(FLATBUFFERS_INCLUDE_DIRS)
                target_include_directories(${ARG_NAME} INTERFACE ${FLATBUFFERS_INCLUDE_DIRS})
            endif()
        else()
            message(FATAL_ERROR "FlatBuffers found but no target or library variable defined")
        endif()
    else()
        message(FATAL_ERROR "FlatBuffers not found. Install via: apt install libflatbuffers-dev / brew install flatbuffers / vcpkg install flatbuffers")
    endif()
endfunction()
