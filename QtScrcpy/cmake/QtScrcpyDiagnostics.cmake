set(QTSCRCPY_SANITIZER "none" CACHE STRING
    "Runtime sanitizer for diagnostic builds: none, address, thread, or undefined")
set_property(CACHE QTSCRCPY_SANITIZER PROPERTY STRINGS
    none address thread undefined)

option(QTSCRCPY_BUILD_TESTS
    "Build QtScrcpy regression-test executables"
    OFF)

function(qtscrcpy_enable_sanitizer target)
    if(QTSCRCPY_SANITIZER STREQUAL "none")
        return()
    endif()

    if(NOT TARGET ${target})
        message(FATAL_ERROR "Sanitizer target does not exist: ${target}")
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(FATAL_ERROR
            "QTSCRCPY_SANITIZER requires a GCC- or Clang-compatible compiler")
    endif()

    if(QTSCRCPY_SANITIZER STREQUAL "address")
        set(_qtscrcpy_sanitizers "address,undefined")
    elseif(QTSCRCPY_SANITIZER STREQUAL "thread")
        set(_qtscrcpy_sanitizers "thread")
    elseif(QTSCRCPY_SANITIZER STREQUAL "undefined")
        set(_qtscrcpy_sanitizers "undefined")
    else()
        message(FATAL_ERROR
            "Unknown QTSCRCPY_SANITIZER value: ${QTSCRCPY_SANITIZER}")
    endif()

    target_compile_options(${target} PRIVATE
        -O1
        -g3
        -fno-lto
        -fno-omit-frame-pointer
        -fno-optimize-sibling-calls
        -fno-sanitize-recover=all
        -fsanitize=${_qtscrcpy_sanitizers}
    )

    get_target_property(_qtscrcpy_target_type ${target} TYPE)
    if(NOT _qtscrcpy_target_type STREQUAL "STATIC_LIBRARY")
        target_link_options(${target} PRIVATE
            -fno-lto
            -fno-sanitize-recover=all
            -fsanitize=${_qtscrcpy_sanitizers}
        )
    endif()
endfunction()

qtscrcpy_enable_sanitizer(QtScrcpyCore)
qtscrcpy_enable_sanitizer(${PROJECT_NAME})

if(QTSCRCPY_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(
        "${CMAKE_CURRENT_LIST_DIR}/../tests"
        "${CMAKE_BINARY_DIR}/qtscrcpy-tests"
    )
endif()
