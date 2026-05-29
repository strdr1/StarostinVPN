cmake_minimum_required(VERSION 3.21)

project(StarostinVPN VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick Network Concurrent QuickControls2 Widgets)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(StarostinVPN
    app.rc
    src/main.cpp
    src/ZapretController.h
    src/ZapretController.cpp
    src/NetworkScanner.h
    src/NetworkScanner.cpp
    src/VlessController.h
    src/VlessController.cpp
    src/Store.h
    src/Store.cpp
    src/StatsTracker.h
    src/StatsTracker.cpp
    src/TgController.h
    src/TgController.cpp
    src/DiagController.h
    src/DiagController.cpp
    src/UpdateChecker.h
    src/UpdateChecker.cpp
    src/LogController.h
    src/LogController.cpp
    src/AdminCheck.h
    src/CursorHelper.h
    src/TrayMenuHelper.h
    src/CfController.h
    src/CfController.cpp
    src/GatewayController.h
    src/GatewayController.cpp
)

qt_add_qml_module(StarostinVPN
    URI StarostinVPN
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/StarField.qml
        qml/GlowButton.qml
        qml/GlassPanel.qml
    RESOURCES
        assets/tray.png
        assets/appicon.png
        assets/fonts/Manrope-Regular.ttf
)

set_target_properties(StarostinVPN PROPERTIES
    WIN32_EXECUTABLE TRUE
)

if(MSVC)
    target_link_options(StarostinVPN PRIVATE
        "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
endif()

target_link_libraries(StarostinVPN PRIVATE
    Qt6::Quick
    Qt6::Network
    Qt6::Concurrent
    Qt6::QuickControls2
    Qt6::Widgets
)

add_custom_command(TARGET StarostinVPN POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/engine"
        "$<TARGET_FILE_DIR:StarostinVPN>/engine"
    COMMENT "Copying bundled zapret engine next to the executable"
)
