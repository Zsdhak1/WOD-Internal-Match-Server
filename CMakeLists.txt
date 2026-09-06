cmake_minimum_required(VERSION 3.16)

project(Scompetition VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        broadcastwindow.cpp
        broadcastwindow.h
        matchprotocol.h
        matchserver.cpp
        matchserver.h
        protocol.h
        robotmanager.cpp
        robotmanager.h
        broadcast_assets.qrc
)

if(${QT_VERSION_MAJOR} GREATER_EQUAL 6)
    qt_add_executable(Scompetition
        MANUAL_FINALIZATION
        ${PROJECT_SOURCES}
    )
# Define target properties for Android with Qt 6 as:
#    set_property(TARGET Scompetition APPEND PROPERTY QT_ANDROID_PACKAGE_SOURCE_DIR
#                 ${CMAKE_CURRENT_SOURCE_DIR}/android)
# For more information, see https://doc.qt.io/qt-6/qt-add-executable.html#target-creation
else()
    if(ANDROID)
        add_library(Scompetition SHARED
            ${PROJECT_SOURCES}
        )
# Define properties for Android with Qt 5 after find_package() calls as:
#    set(ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android")
    else()
        add_executable(Scompetition
            ${PROJECT_SOURCES}
        )
    endif()
endif()

target_link_libraries(Scompetition PRIVATE Qt${QT_VERSION_MAJOR}::Widgets
Qt${QT_VERSION_MAJOR}::Network)

add_executable(ScompetitionClient
        client_main.cpp
        clientwindow.cpp
        clientwindow.h
        broadcastwindow.cpp
        broadcastwindow.h
        robotmanager.cpp
        robotmanager.h
        matchprotocol.h
        protocol.h
        broadcast_assets.qrc
)
target_link_libraries(ScompetitionClient PRIVATE Qt${QT_VERSION_MAJOR}::Widgets
Qt${QT_VERSION_MAJOR}::Network)

add_executable(RobotSimulator
        simulator_main.cpp
        simulatorwindow.cpp
        simulatorwindow.h
        protocol.h
)
target_link_libraries(RobotSimulator PRIVATE Qt${QT_VERSION_MAJOR}::Widgets
Qt${QT_VERSION_MAJOR}::Network)

set_target_properties(ScompetitionClient RobotSimulator PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# Qt for iOS sets MACOSX_BUNDLE_GUI_IDENTIFIER automatically since Qt 6.1.
# If you are developing for iOS or macOS you should consider setting an
# explicit, fixed bundle identifier manually though.
if(${QT_VERSION} VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.Scompetition)
endif()
set_target_properties(Scompetition PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS Scompetition
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(Scompetition)
endif()
