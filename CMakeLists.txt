cmake_minimum_required(VERSION 3.21)

project(Relay VERSION 0.2.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS
    Core Gui Quick QuickControls2 Network Concurrent OpenGL Sql)
find_package(ZLIB REQUIRED)

if(COMMAND qt_policy)          # Qt 6.5+: modern QML resource layout
    qt_policy(SET QTP0001 NEW)
endif()

# ---- libmpv -----------------------------------------------------------------
# Linux / Homebrew: found through pkg-config.
# Windows: point MPV_DIR at an extracted mpv-dev package
#          (folder containing include/mpv/*.h and libmpv.dll.a / mpv.lib).
set(MPV_DIR "" CACHE PATH "Folder with libmpv headers and import library (Windows)")

add_library(mpv_lib INTERFACE)
if(MPV_DIR)
    target_include_directories(mpv_lib INTERFACE "${MPV_DIR}/include")
    find_library(MPV_LIBRARY NAMES mpv libmpv libmpv.dll mpv-2
                 PATHS "${MPV_DIR}" "${MPV_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(mpv_lib INTERFACE "${MPV_LIBRARY}")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MPV QUIET IMPORTED_TARGET mpv)
    endif()
    if(TARGET PkgConfig::MPV)
        target_link_libraries(mpv_lib INTERFACE PkgConfig::MPV)
    else()
        # pkg-config can turn mpv down over a dependency only static builds
        # need (MSYS2's LuaJIT standing in for Lua 5.1, say); Relay links the
        # shared libmpv, so its header and library are enough.
        find_path(MPV_INCLUDE_DIR mpv/client.h)
        find_library(MPV_LIBRARY NAMES mpv libmpv mpv-2 libmpv-2)
        if(NOT MPV_INCLUDE_DIR OR NOT MPV_LIBRARY)
            message(FATAL_ERROR "libmpv wasn't found. Install it (MSYS2: mingw-w64-ucrt-x86_64-mpv, "
                                "Homebrew: mpv, Debian/Ubuntu: libmpv-dev) or set MPV_DIR.")
        endif()
        message(STATUS "Found libmpv: ${MPV_LIBRARY} (without pkg-config)")
        target_include_directories(mpv_lib INTERFACE "${MPV_INCLUDE_DIR}")
        target_link_libraries(mpv_lib INTERFACE "${MPV_LIBRARY}")
    endif()
endif()

# ---- core library (everything except main) so tests can link it --------------
add_library(relay_core STATIC
    src/sources/Channel.h
    src/sources/M3uParser.h        src/sources/M3uParser.cpp
    src/sources/XtreamParser.h     src/sources/XtreamParser.cpp
    src/sources/PlaylistService.h  src/sources/PlaylistService.cpp
    src/sources/Catchup.h          src/sources/Catchup.cpp
    src/sources/PortalClient.h     src/sources/PortalClient.cpp
    src/sources/CatchupService.h   src/sources/CatchupService.cpp
    src/models/ChannelModel.h      src/models/ChannelModel.cpp
    src/player/PlayerController.h  src/player/PlayerController.cpp
    src/player/MpvItem.h           src/player/MpvItem.cpp
    src/app/AppSettings.h          src/app/AppSettings.cpp
    src/app/Session.h              src/app/Session.cpp
    src/app/Trakt.h               src/app/Trakt.cpp
    src/app/Updater.h             src/app/Updater.cpp
    src/app/RemoteInput.h         src/app/RemoteInput.cpp
    src/app/Gamepads.h            src/app/Gamepads.cpp
    src/app/PhoneRemote.h         src/app/PhoneRemote.cpp
    src/third_party/qrcodegen/qrcodegen.hpp src/third_party/qrcodegen/qrcodegen.cpp
    src/cast/TsSegmenter.h        src/cast/TsSegmenter.cpp
    src/cast/CastRelay.h          src/cast/CastRelay.cpp
    src/cast/HlsJoiner.h          src/cast/HlsJoiner.cpp
    src/cast/CastDevices.h        src/cast/CastDevices.cpp
    src/cast/CastService.h        src/cast/CastService.cpp
    src/app/Log.h                  src/app/Log.cpp
    src/app/UserLibrary.h          src/app/UserLibrary.cpp
    src/app/SecretStore.h          src/app/SecretStore.cpp
    src/app/Parental.h             src/app/Parental.cpp
    src/app/OtherPlaylists.h       src/app/OtherPlaylists.cpp
    src/app/Backup.h               src/app/Backup.cpp
    src/epg/Programme.h
    src/epg/XmltvParser.h          src/epg/XmltvParser.cpp
    src/epg/EpgMatcher.h           src/epg/EpgMatcher.cpp
    src/epg/EpgStore.h             src/epg/EpgStore.cpp
    src/epg/EpgService.h           src/epg/EpgService.cpp
    src/vod/VodParser.h            src/vod/VodParser.cpp
    src/record/HlsPlaylist.h       src/record/HlsPlaylist.cpp
    src/record/Recorder.h          src/record/Recorder.cpp
    src/record/ScheduleService.h   src/record/ScheduleService.cpp
    src/vod/VodService.h           src/vod/VodService.cpp
    src/vod/Tmdb.h               src/vod/Tmdb.cpp
)
target_include_directories(relay_core PUBLIC src)
target_link_libraries(relay_core PUBLIC
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Network Qt6::Concurrent Qt6::OpenGL Qt6::Sql
    ZLIB::ZLIB mpv_lib)
# System credential store for the playlist login (SecretStore).
# The phone remote's page.
set_source_files_properties(src/web/remote.html PROPERTIES QT_RESOURCE_ALIAS remote.html)
qt_add_resources(relay_core "relay_web" PREFIX "/web" FILES src/web/remote.html)

# Game controllers (Xbox, PlayStation, Switch…) through SDL2; without it Relay
# builds, but doesn't see controllers.
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
endif()
if(TARGET PkgConfig::SDL2)
    target_link_libraries(relay_core PUBLIC PkgConfig::SDL2)
    target_compile_definitions(relay_core PUBLIC RELAY_HAVE_SDL)
    message(STATUS "Game controllers: SDL2 ${SDL2_VERSION}")
else()
    message(STATUS "Game controllers: off (SDL2 not found; pacman -S mingw-w64-ucrt-x86_64-SDL2)")
endif()

if(WIN32)
    target_link_libraries(relay_core PUBLIC advapi32 dwmapi)
elseif(APPLE)
    target_link_libraries(relay_core PUBLIC "-framework Security" "-framework CoreFoundation")
endif()

# ---- application ---------------------------------------------------------------
# App icon: Windows resource (icon + version details), macOS bundle icon.
set(RELAY_PLATFORM_SOURCES "")
if(WIN32)
    set(RELAY_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/relay.ico")
    configure_file(packaging/windows-relay.rc.in "${CMAKE_CURRENT_BINARY_DIR}/relay.rc" @ONLY)
    list(APPEND RELAY_PLATFORM_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/relay.rc")
elseif(APPLE)
    set(RELAY_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/relay.icns")
    set_source_files_properties(${RELAY_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    list(APPEND RELAY_PLATFORM_SOURCES ${RELAY_ICNS})
endif()

qt_add_executable(relay WIN32 MACOSX_BUNDLE src/main.cpp ${RELAY_PLATFORM_SOURCES})
target_compile_definitions(relay PRIVATE RELAY_VERSION="${PROJECT_VERSION}")

# Window/taskbar icon (Linux; Windows and macOS also get it from the files above).
set_source_files_properties(packaging/icons/relay.png PROPERTIES QT_RESOURCE_ALIAS relay.png)
qt_add_resources(relay "relay_icons" PREFIX "/icons" FILES packaging/icons/relay.png)

set(RELAY_QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/ChannelPanel.qml
    qml/InfoBar.qml
    qml/NumberEntry.qml
    qml/SetupScreen.qml
    qml/Toast.qml
    qml/FieldInput.qml
    qml/ActionButton.qml
    qml/ChannelLogo.qml
    qml/Guide.qml
    qml/RelayMenu.qml
    qml/RelayMenuItem.qml
    qml/HiddenManager.qml
    qml/Search.qml
    qml/RenameDialog.qml
    qml/Poster.qml
    qml/VodBrowser.qml
    qml/VodDetails.qml
    qml/VodOsd.qml
    qml/TimeshiftBar.qml
    qml/Recordings.qml
    qml/ReminderPopup.qml
    qml/MultiView.qml
    qml/PipView.qml
        qml/PlayerOptions.qml
        qml/GuidePicker.qml
        qml/ListPicker.qml
        qml/Keymap.qml
        qml/KeysDialog.qml
        qml/CastPicker.qml
        qml/UpdateBanner.qml
        qml/CastBar.qml
    qml/PinDialog.qml
    qml/PlaylistPicker.qml
    qml/Icons.qml
    qml/Icon.qml
    qml/KeyCap.qml
    qml/BarButton.qml
    qml/CloseButton.qml
    qml/ControlBar.qml
    qml/ShortcutsPanel.qml
)
set_source_files_properties(qml/Theme.qml qml/Icons.qml qml/Keymap.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
# Place QML files next to the module's qmldir (qrc:/Relay/Main.qml) so the
# Theme singleton resolves with every Qt 6 version.
foreach(f ${RELAY_QML_FILES})
    get_filename_component(name ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()

qt_add_qml_module(relay
    URI Relay
    VERSION 1.0
    QML_FILES ${RELAY_QML_FILES}
)

target_link_libraries(relay PRIVATE relay_core Qt6::QuickControls2)

# Windows: copy the mpv DLL next to relay.exe so it runs from the build folder.
if(WIN32 AND MPV_DIR)
    file(GLOB MPV_DLLS "${MPV_DIR}/*mpv*.dll" "${MPV_DIR}/bin/*mpv*.dll")
    foreach(dll ${MPV_DLLS})
        add_custom_command(TARGET relay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:relay>")
    endforeach()
endif()

set_target_properties(relay PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER app.relay.player
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_NAME Relay
    MACOSX_BUNDLE_ICON_FILE relay.icns
)

# ---- install / packaging ---------------------------------------------------------
# `cmake --install build --prefix <folder>` puts relay.exe (or Relay.app) there;
# packaging/package-windows.sh and package-macos.sh add Qt and mpv beside it.
install(TARGETS relay
    RUNTIME DESTINATION .
    BUNDLE DESTINATION .)

# ---- tests ---------------------------------------------------------------------
option(RELAY_BUILD_TESTS "Build unit tests" ON)
if(RELAY_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    add_executable(test_sources tests/test_sources.cpp)
    target_link_libraries(test_sources PRIVATE relay_core Qt6::Test)
    add_test(NAME test_sources COMMAND test_sources)
    add_executable(test_epg tests/test_epg.cpp)
    target_link_libraries(test_epg PRIVATE relay_core Qt6::Test)
    add_test(NAME test_epg COMMAND test_epg)
endif()
