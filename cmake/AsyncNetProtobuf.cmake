# AsyncNetProtobuf.cmake — Protobuf code generation integration
#
# Usage:
#   include(cmake/AsyncNetProtobuf.cmake)
#   async_net_protobuf_generate(TARGET my_target PROTOS path/to/echo.proto)
#
# This will:
#   1. Find protoc compiler
#   2. Generate C++ code from .proto files
#   3. Add generated sources to the target

function(async_net_protobuf_generate)
    cmake_parse_arguments(ARG "" "TARGET;OUT_DIR" "PROTOS;PROTO_PATHS" ${ARGN})

    if(NOT ARG_TARGET)
        message(FATAL_ERROR "async_net_protobuf_generate: TARGET is required")
    endif()

    if(NOT ARG_PROTOS)
        message(FATAL_ERROR "async_net_protobuf_generate: PROTOS is required")
    endif()

    # Find protoc
    find_program(PROTOC_EXECUTABLE protoc)
    if(NOT PROTOC_EXECUTABLE)
        message(FATAL_ERROR "async_net_protobuf_generate: protoc not found. Install protobuf compiler.")
    endif()

    # Default output directory
    if(NOT ARG_OUT_DIR)
        set(ARG_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    endif()

    file(MAKE_DIRECTORY ${ARG_OUT_DIR})

    set(GENERATED_SOURCES "")
    set(GENERATED_HEADERS "")

    foreach(PROTO_FILE ${ARG_PROTOS})
        # Get absolute path
        get_filename_component(PROTO_ABS ${PROTO_FILE} ABSOLUTE)
        get_filename_component(PROTO_DIR ${PROTO_ABS} DIRECTORY)
        get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

        # Determine include path
        set(PROTO_INCLUDE_DIR ${PROTO_DIR})
        if(ARG_PROTO_PATHS)
            set(PROTO_INCLUDE_DIR "")
            foreach(P ${ARG_PROTO_PATHS})
                list(APPEND PROTO_INCLUDE_DIR "-I${P}")
            endforeach()
        else()
            set(PROTO_INCLUDE_DIR "-I${PROTO_DIR}")
        endif()

        # Output files
        set(GENERATED_CC "${ARG_OUT_DIR}/${PROTO_NAME}.pb.cc")
        set(GENERATED_H "${ARG_OUT_DIR}/${PROTO_NAME}.pb.h")

        # Add custom command to generate C++ from proto
        add_custom_command(
            OUTPUT ${GENERATED_CC} ${GENERATED_H}
            COMMAND ${PROTOC_EXECUTABLE}
                --cpp_out=${ARG_OUT_DIR}
                ${PROTO_INCLUDE_DIR}
                ${PROTO_ABS}
            DEPENDS ${PROTO_ABS}
            COMMENT "Generating protobuf C++ code for ${PROTO_FILE}"
            VERBATIM
        )

        list(APPEND GENERATED_SOURCES ${GENERATED_CC})
        list(APPEND GENERATED_HEADERS ${GENERATED_H})
    endforeach()

    # Add generated sources to target
    target_sources(${ARG_TARGET} PRIVATE ${GENERATED_SOURCES})

    # Add include directory for generated headers
    target_include_directories(${ARG_TARGET} PRIVATE ${ARG_OUT_DIR})

    # Make generated headers visible to dependents
    set_source_files_properties(${GENERATED_HEADERS} PROPERTIES GENERATED TRUE)
endfunction()

# Helper: generate protobuf and create a library
# Usage: async_net_protobuf_library(NAME my_proto_lib PROTOS echo.proto)
function(async_net_protobuf_library)
    cmake_parse_arguments(ARG "" "NAME;OUT_DIR" "PROTOS;PROTO_PATHS" ${ARGN})

    if(NOT ARG_NAME)
        message(FATAL_ERROR "async_net_protobuf_library: NAME is required")
    endif()

    if(NOT ARG_PROTOS)
        message(FATAL_ERROR "async_net_protobuf_library: PROTOS is required")
    endif()

    if(NOT ARG_OUT_DIR)
        set(ARG_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_${ARG_NAME}")
    endif()

    # Create a static library for the generated code
    add_library(${ARG_NAME} STATIC)

    # Generate protobuf code
    async_net_protobuf_generate(
        TARGET ${ARG_NAME}
        PROTOS ${ARG_PROTOS}
        OUT_DIR ${ARG_OUT_DIR}
        PROTO_PATHS ${ARG_PROTO_PATHS}
    )

    # Link protobuf
    find_package(protobuf CONFIG)
    if(protobuf_FOUND)
        target_link_libraries(${ARG_NAME} PUBLIC protobuf::libprotobuf)
    else()
        target_link_libraries(${ARG_NAME} PUBLIC ${Protobuf_LIBRARIES})
        target_include_directories(${ARG_NAME} PUBLIC ${Protobuf_INCLUDE_DIRS})
    endif()
endfunction()
