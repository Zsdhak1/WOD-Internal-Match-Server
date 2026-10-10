cmake_minimum_required(VERSION 3.16)

project(Scompetition VERSION 0.2.0 LANGUAGES CXX)

# The GitHub Windows runner uses an English locale. Keep source/resource paths
# ASCII-safe and compile UTF-8 explicitly so regional settings cannot alter builds.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Development caches keep stable paths; exported artifacts carry a version and
# UTC build-session timestamp. The build wrapper supplies one timestamp for the
# configure/build/install/package sequence; manual configure creates a new one.
if(NOT DEFINED SCOMP_BUILD_TIMESTAMP OR SCOMP_BUILD_TIMESTAMP STREQUAL "")
    string(TIMESTAMP SCOMP_BUILD_TIMESTAMP "%Y%m%d-%H%M%S" UTC)
endif()
if(NOT SCOMP_BUILD_TIMESTAMP MATCHES "^[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]-[0-9][0-9][0-9][0-9][0-9][0-9]$")
    message(FATAL_ERROR "SCOMP_BUILD_TIMESTAMP must be yyyyMMdd-HHmmss (UTC)")
endif()
set(SCOMP_GIT_REVISION "unknown")
set(SCOMP_GIT_DIRTY "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE SCOMP_GIT_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _git_revision_result ERROR_QUIET)
    if(NOT _git_revision_result EQUAL 0)
        set(SCOMP_GIT_REVISION "unknown")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=normal
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _git_status RESULT_VARIABLE _git_status_result ERROR_QUIET)
    if(_git_status_result EQUAL 0)
        if(_git_status STREQUAL "")
            set(SCOMP_GIT_DIRTY "false")
        else()
            set(SCOMP_GIT_DIRTY "true")
        endif()
    endif()
endif()

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network Multimedia OpenGLWidgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network Multimedia OpenGLWidgets)

set(PROJECT_SOURCES
        main.cpp
        applogging.cpp
        applogging.h
        mainwindow.cpp
        mainwindow.h
        fieldcamera.cpp
        fieldcamera.h
        checkinmodel.cpp
        checkinmodel.h
        checkinpanel.cpp
        checkinpanel.h
        responsiveui.h
        broadcastwindow.cpp
        broadcastwindow.h
        videocanvas.cpp
        videocanvas.h
        matchprotocol.h
        matchreferee.cpp
        matchreferee.h
        matchserver.cpp
        matchserver.h
        protocol.h
        robotcommander.cpp
        robotcommander.h
        robotmanager.cpp
        robotmanager.h
        huddigitslabel.h
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
Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Multimedia Qt${QT_VERSION_MAJOR}::OpenGLWidgets)

if(WIN32)
    target_link_libraries(Scompetition PRIVATE dbghelp)
endif()

add_executable(ScompetitionClient
        client_main.cpp
        applogging.cpp
        applogging.h
        clientwindow.cpp
        clientwindow.h
        playercombatoverlay.cpp
        playercombatoverlay.h
        responsiveui.h
        broadcastwindow.cpp
        broadcastwindow.h
        videocanvas.cpp
        videocanvas.h
        robotmanager.cpp
        robotmanager.h
        matchprotocol.h
        protocol.h
        huddigitslabel.h
        broadcast_assets.qrc
)
target_link_libraries(ScompetitionClient PRIVATE Qt${QT_VERSION_MAJOR}::Widgets
Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Multimedia
Qt${QT_VERSION_MAJOR}::OpenGLWidgets)
if(WIN32)
    target_link_libraries(ScompetitionClient PRIVATE dbghelp)
endif()

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
        responsiveui.h
        protocol.h
)
target_link_libraries(RobotSimulator PRIVATE Qt${QT_VERSION_MAJOR}::Widgets
Qt${QT_VERSION_MAJOR}::Network)

add_executable(RobotProtocolSmokeTest
        tools/robot_protocol_smoke_test.cpp
        matchserver.cpp
        matchserver.h
        matchprotocol.h
        robotmanager.cpp
        robotmanager.h
        protocol.h
)
target_include_directories(RobotProtocolSmokeTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(RobotProtocolSmokeTest PRIVATE Qt${QT_VERSION_MAJOR}::Core
Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Network)

add_executable(AppLoggingSmokeTest
        tools/applogging_smoke_test.cpp
        applogging.cpp
        applogging.h
)
target_include_directories(AppLoggingSmokeTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(AppLoggingSmokeTest PRIVATE Qt${QT_VERSION_MAJOR}::Core)
if(WIN32)
    target_link_libraries(AppLoggingSmokeTest PRIVATE dbghelp)
endif()

add_executable(MatchRefereeTest
        tools/match_referee_test.cpp
        matchreferee.cpp
        matchreferee.h
        protocol.h
)
target_include_directories(MatchRefereeTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(MatchRefereeTest PRIVATE Qt${QT_VERSION_MAJOR}::Core
Qt${QT_VERSION_MAJOR}::Network)

add_executable(VideoCanvasSmokeTest
        tools/video_canvas_smoke_test.cpp
        videocanvas.cpp
        videocanvas.h
)
target_include_directories(VideoCanvasSmokeTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(VideoCanvasSmokeTest PRIVATE Qt${QT_VERSION_MAJOR}::Widgets
Qt${QT_VERSION_MAJOR}::OpenGLWidgets)

set_target_properties(ScompetitionClient RobotSimulator PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# Exercises the actual simulator, controller, TCP client and HUD offline.
add_executable(SimulatorIntegrationTest
    tools/simulator_integration_test.cpp
    simulatorwindow.cpp simulatorwindow.h
    mainwindow.cpp mainwindow.h
    fieldcamera.cpp fieldcamera.h
    checkinmodel.cpp checkinmodel.h checkinpanel.cpp checkinpanel.h
    clientwindow.cpp clientwindow.h
    playercombatoverlay.cpp playercombatoverlay.h
    broadcastwindow.cpp broadcastwindow.h huddigitslabel.h broadcast_assets.qrc
    videocanvas.cpp videocanvas.h
    robotmanager.cpp robotmanager.h
    robotcommander.cpp robotcommander.h
    matchreferee.cpp matchreferee.h
    matchserver.cpp matchserver.h)
target_include_directories(SimulatorIntegrationTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(SimulatorIntegrationTest PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network
    Qt${QT_VERSION_MAJOR}::Multimedia Qt${QT_VERSION_MAJOR}::OpenGLWidgets)
add_executable(PlayerCombatOverlayTest
    tools/player_combat_overlay_test.cpp
    playercombatoverlay.cpp playercombatoverlay.h)
target_include_directories(PlayerCombatOverlayTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(PlayerCombatOverlayTest PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)
add_executable(FieldCameraTest tools/field_camera_test.cpp tools/fake_field_camera.h
    fieldcamera.cpp fieldcamera.h)
target_include_directories(FieldCameraTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(FieldCameraTest PRIVATE Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Multimedia)
add_executable(CheckinModelTest tools/checkin_model_test.cpp checkinmodel.cpp checkinmodel.h)
target_include_directories(CheckinModelTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(CheckinModelTest PRIVATE Qt${QT_VERSION_MAJOR}::Core)
enable_testing()
add_test(NAME checkin_model COMMAND CheckinModelTest)
add_test(NAME field_camera COMMAND FieldCameraTest)
add_test(NAME player_combat_overlay COMMAND PlayerCombatOverlayTest)
set_tests_properties(player_combat_overlay PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
add_test(NAME robot_protocol COMMAND RobotProtocolSmokeTest)
add_test(NAME app_logging COMMAND AppLoggingSmokeTest)
add_test(NAME match_referee COMMAND MatchRefereeTest)
add_test(NAME simulator_integration COMMAND SimulatorIntegrationTest)
set_tests_properties(simulator_integration PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 180 RUN_SERIAL TRUE)

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
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md" DESTINATION .)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/SERVER_USB_CAMERA.md"
              "${CMAKE_CURRENT_SOURCE_DIR}/docs/CHECKIN_MODE.md" DESTINATION docs)

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

    # All development executables share one output directory. Deploy once,
    # after all links finish: concurrent POST_BUILD windeployqt/copies race on
    # Qt6Core.dll and other shared files on a clean GitHub runner.
    add_custom_target(development_runtime ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:Scompetition>/tools/ffmpeg"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFMPEG_RUNTIME_DIR}/ffmpeg.exe"
                "${FFMPEG_RUNTIME_DIR}/ffprobe.exe"
                "${FFMPEG_RUNTIME_DIR}/LICENSE.txt"
                "${FFMPEG_RUNTIME_DIR}/README.txt"
                "$<TARGET_FILE_DIR:Scompetition>/tools/ffmpeg"
        DEPENDS Scompetition ScompetitionClient RobotSimulator
        VERBATIM
    )
    if(WINDEPLOYQT_EXECUTABLE)
        foreach(target Scompetition ScompetitionClient RobotSimulator)
            add_custom_command(TARGET development_runtime POST_BUILD
                COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                        --no-translations "$<TARGET_FILE:${target}>"
                VERBATIM
            )
        endforeach()
    endif()

    # Keep the paired RGB/mask settlement clips beside development builds.
    # Both executables share one output directory; copying from two parallel
    # POST_BUILD steps raced on the same files and failed intermittently, so
    # a single target performs the copy and both programs depend on it.
    add_custom_target(settlement_assets
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:Scompetition>/Assets/gamefinishvideo"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${SETTLEMENT_ASSET_DIR}"
                "$<TARGET_FILE_DIR:Scompetition>/Assets/gamefinishvideo"
        VERBATIM
    )
    add_dependencies(Scompetition settlement_assets)
    add_dependencies(ScompetitionClient settlement_assets)

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
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(SCOMP_ARTIFACT_PLATFORM "win64")
else()
    set(SCOMP_ARTIFACT_PLATFORM "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(CMAKE_BUILD_TYPE)
    set(SCOMP_ARTIFACT_CONFIG "${CMAKE_BUILD_TYPE}")
else()
    set(SCOMP_ARTIFACT_CONFIG "multi-config")
endif()
set(SCOMP_BUILD_ID "v${PROJECT_VERSION}_${SCOMP_BUILD_TIMESTAMP}_${SCOMP_ARTIFACT_PLATFORM}_${SCOMP_ARTIFACT_CONFIG}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-info.json.in"
               "${CMAKE_CURRENT_BINARY_DIR}/build-info.json" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/build-info.json"
        DESTINATION "${CMAKE_INSTALL_BINDIR}")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}_${SCOMP_BUILD_ID}")
# A portable ZIP is available without requiring NSIS on the build machine.
set(CPACK_GENERATOR "ZIP")
include(CPack)

if(QT_VERSION_MAJOR EQUAL 6)
    qt_finalize_executable(Scompetition)
endif()
