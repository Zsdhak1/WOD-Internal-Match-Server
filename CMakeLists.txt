cmake_minimum_required(VERSION 3.16)

project(Scompetition VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network Multimedia)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network Multimedia)

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
        robotcommander.cpp
        robotcommander.h
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
Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Multimedia)

# The broadcast window reads its layout beside the executable. Keep a
# deterministic default next to every target that hosts that window after
# each rebuild; user edits made while the program is running are therefore
# never confused with the build-time defaults.
set(DEFAULT_BROADCAST_LAYOUT_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/broadcast_layout.ini")
foreach(target Scompetition ScompetitionClient)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
                "${DEFAULT_BROADCAST_LAYOUT_FILE}"
                "$<TARGET_FILE_DIR:${target}>/broadcast_layout.ini"
        VERBATIM
    )
endforeach()

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
set(SETTLEMENT_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Assets/gamefinishvideo")
install(TARGETS Scompetition ScompetitionClient RobotSimulator
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES "${DEFAULT_BROADCAST_LAYOUT_FILE}"
        DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32)
    set(FFMPEG_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/ffmpeg")

    # GUI applications otherwise fail silently from Explorer when qwindows
    # and the matching Qt runtime DLLs are missing beside the executable.
    find_program(WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt windeployqt.exe
        HINTS
            "${CMAKE_PREFIX_PATH}/bin"
            "$ENV{QTDIR}/bin"
    )
    if(NOT WINDEPLOYQT_EXECUTABLE)
        message(WARNING "windeployqt was not found; Windows packages may be missing Qt runtime files")
    endif()

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

        if(WINDEPLOYQT_EXECUTABLE)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                        --no-translations
                        "$<TARGET_FILE:${target}>"
                VERBATIM
            )
        endif()
    endforeach()

    # Keep the paired RGB/mask settlement clips beside development builds.
    foreach(target Scompetition ScompetitionClient)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:${target}>/Assets/gamefinishvideo"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${SETTLEMENT_ASSET_DIR}"
                    "$<TARGET_FILE_DIR:${target}>/Assets/gamefinishvideo"
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

if(WIN32 AND WINDEPLOYQT_EXECUTABLE)
    # CPack runs the install script, so deploy Qt into the staged package too.
    # Keep the install prefix as an install-time variable so --prefix works.
    set(_windeployqt_install_code [=[
        set(_qt_deploy_targets
            "${CMAKE_INSTALL_PREFIX}/@BINDIR@/Scompetition.exe"
            "${CMAKE_INSTALL_PREFIX}/@BINDIR@/ScompetitionClient.exe"
            "${CMAKE_INSTALL_PREFIX}/@BINDIR@/RobotSimulator.exe")
        foreach(_qt_deploy_target IN LISTS _qt_deploy_targets)
            execute_process(
                COMMAND "@WINDEPLOYQT@" --no-translations "${_qt_deploy_target}"
                RESULT_VARIABLE _qt_deploy_result)
            if(NOT _qt_deploy_result EQUAL 0)
                message(FATAL_ERROR
                    "windeployqt failed for ${_qt_deploy_target}: ${_qt_deploy_result}")
            endif()
        endforeach()
    ]=])
    string(REPLACE "@WINDEPLOYQT@" "${WINDEPLOYQT_EXECUTABLE}"
           _windeployqt_install_code "${_windeployqt_install_code}")
    string(REPLACE "@BINDIR@" "${CMAKE_INSTALL_BINDIR}"
           _windeployqt_install_code "${_windeployqt_install_code}")
    install(CODE "${_windeployqt_install_code}")
endif()

install(DIRECTORY "${SETTLEMENT_ASSET_DIR}/"
    DESTINATION "${CMAKE_INSTALL_BINDIR}/Assets/gamefinishvideo"
    FILES_MATCHING PATTERN "*.mp4"
)

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
