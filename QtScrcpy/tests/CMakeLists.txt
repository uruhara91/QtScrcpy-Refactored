find_package(Qt${QT_DESIRED_VERSION} REQUIRED COMPONENTS Test)
# The parent CMakeLists.txt only calls find_package(Threads) inside its
# Linux-specific block, but render_state_machine_tests.cpp below uses
# std::thread on every platform - so this needs its own copy rather than
# assuming Threads::Threads is already available. find_package() is
# idempotent, so this is a harmless no-op on Linux where it was already
# found.
find_package(Threads REQUIRED)

add_executable(QtScrcpyProtocolTests
    protocol_tests.cpp
    ../QtScrcpyCore/src/device/controller/inputconvert/controlmsg.cpp
    ../QtScrcpyCore/src/device/controller/receiver/devicemsg.cpp
)

set_target_properties(QtScrcpyProtocolTests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(QtScrcpyProtocolTests PRIVATE
    Qt${QT_DESIRED_VERSION}::Test
)

target_include_directories(QtScrcpyProtocolTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/device/android
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/device/controller
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/device/controller/inputconvert
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/device/controller/receiver
)

qtscrcpy_enable_sanitizer(QtScrcpyProtocolTests)
add_test(NAME protocol COMMAND QtScrcpyProtocolTests)

# Headless tests for PboMailboxStateMachine (see
# render/pbomailboxstatemachine.h) - the triple-buffer mailbox bookkeeping
# extracted out of QYuvOpenGLWidget specifically so it could be exercised
# without a GL context/GPU/display, including with real concurrent
# producer/consumer threads standing in for the decoder and render
# threads. See the audit note in render/pbomailboxstatemachine.h for why.
add_executable(QtScrcpyRenderStateMachineTests
    render_state_machine_tests.cpp
)

set_target_properties(QtScrcpyRenderStateMachineTests PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

target_link_libraries(QtScrcpyRenderStateMachineTests PRIVATE
    Qt${QT_DESIRED_VERSION}::Test
    Threads::Threads
)

target_include_directories(QtScrcpyRenderStateMachineTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../render
)

qtscrcpy_enable_sanitizer(QtScrcpyRenderStateMachineTests)
add_test(NAME render_state_machine COMMAND QtScrcpyRenderStateMachineTests)
