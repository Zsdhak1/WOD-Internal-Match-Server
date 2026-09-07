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
install(TARGETS Scompetition ScompetitionClient RobotSimulator
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(WIN32)
    set(FFMPEG_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg")

    # Keep the encoder beside each development executable as well as in the
    # install tree, so local runs exercise the same runtime dependency.
    foreach(target Scompetition ScompetitionClient RobotSimulator)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${target}>/tools/ffmpeg"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FFMPEG_RUNTIME_DIR}/ffmpeg.exe"
                    "$<TARGET_FILE_DIR:${target}>/tools/ffmpeg/ffmpeg.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FFMPEG_RUNTIME_DIR}/ffprobe.exe"
                    "$<TARGET_FILE_DIR:${target}>/tools/ffmpeg/ffprobe.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FFMPEG_RUNTIME_DIR}/LICENSE.txt"
                    "$<TARGET_FILE_DIR:${target}>/tools/ffmpeg/LICENSE.txt"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${FFMPEG_RUNTIME_DIR}/README.txt"
                    "$<TARGET_FILE_DIR:${target}>/tools/ffmpeg/README.txt"
            VERBATIM
        )
    endforeach()

    install(DIRECTORY "${FFMPEG_RUNTIME_DIR}/"
        DESTINATION "${CMAKE_INSTALL_BINDIR}/tools/ffmpeg"
        FILES_MATCHING
            PATTERN "ffmpeg.exe"
            PATTERN "ffprobe.exe"
            PATTERN "LICENSE.txt"
            PATTERN "README.txt"
    )
endif()

set(CPACK_PACKAGE_NAME "WOD-Internal-Match-Server")
set(CPACK_PACKAGE_VENDOR "WOD Internal Match")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "局域网赛事服务端、选手端和转播系统")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "WOD-Internal-Match-Server")
# A portable ZIP is available without requiring NSIS on the build machine.
set(CPACK_GENERATOR "ZIP")
include(CPack)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(Scompetition)
endif()
