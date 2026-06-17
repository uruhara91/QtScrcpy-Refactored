find_package(Qt${QT_DESIRED_VERSION} REQUIRED COMPONENTS Test)

add_executable(QtScrcpyProtocolTests
    protocol_tests.cpp
)

target_link_libraries(QtScrcpyProtocolTests PRIVATE
    QtScrcpyCore
    Qt${QT_DESIRED_VERSION}::Test
)

target_include_directories(QtScrcpyProtocolTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/device/android
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/device/controller/inputconvert
    ${CMAKE_CURRENT_SOURCE_DIR}/../QtScrcpyCore/src/device/controller/receiver
)

if(COMMAND qtscrcpy_enable_sanitizer)
    qtscrcpy_enable_sanitizer(QtScrcpyProtocolTests)
endif()

add_test(NAME protocol COMMAND QtScrcpyProtocolTests)
