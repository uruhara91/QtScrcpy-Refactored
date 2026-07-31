find_package(Qt${QT_DESIRED_VERSION} REQUIRED COMPONENTS Test)

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
