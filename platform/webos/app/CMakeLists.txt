cmake_minimum_required(VERSION 3.22)
# Relay for webOS: Relay's own code and screens (src/, qml/), with the TV's
# window (platform/webos/qpa-sdl) and player (TvVideo) in place of mpv's.
# Built by platform/webos/build-app.sh.

set(RELAY ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
file(READ ${RELAY}/CMakeLists.txt RELAY_CMAKE)
string(REGEX MATCH "project\\(Relay VERSION ([0-9.]+)" _ "${RELAY_CMAKE}")
set(RELAY_VERSION ${CMAKE_MATCH_1})
project(relay-webos VERSION ${RELAY_VERSION} LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

set(WEBOS_SYSROOT "" CACHE PATH "The webOS SDK's sysroot")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Network Concurrent OpenGL Sql)
qt_policy(SET QTP0001 NEW)

# Qt's window system: SDL (platform/webos/qpa-sdl).
set(QPA ${CMAKE_CURRENT_SOURCE_DIR}/../qpa-sdl)
qt_add_plugin(qsdl STATIC CLASS_NAME QSdlIntegrationPlugin PLUGIN_TYPE platforms)
target_sources(qsdl PRIVATE ${QPA}/qsdlintegration.cpp ${QPA}/qsdlintegration.h)
target_link_libraries(qsdl PRIVATE Qt6::Gui Qt6::GuiPrivate SDL2)

# Relay's code, all of it but what the TV does differently: mpv's player
# (TvVideo here) and the cast sound-track repackager (needs FFmpeg).
file(GLOB RELAY_SOURCES CONFIGURE_DEPENDS ${RELAY}/src/*/*.cpp ${RELAY}/src/*/*.h)
list(FILTER RELAY_SOURCES EXCLUDE REGEX "/src/player/MpvItem\\.|/src/cast/Remuxer\\.")
file(GLOB QRCODE CONFIGURE_DEPENDS ${RELAY}/src/third_party/qrcodegen/*.cpp)

qt_add_executable(relay ${RELAY}/src/main.cpp ${RELAY_SOURCES} ${QRCODE}
    TvPlatform.cpp TvPlatform.h TvLuna.cpp TvLuna.h TvRemote.cpp TvVideo.cpp TvVideo.h)
target_compile_definitions(relay PRIVATE RELAY_WEBOS RELAY_VERSION="${RELAY_VERSION}")
target_include_directories(relay PRIVATE ${RELAY}/src ${CMAKE_CURRENT_SOURCE_DIR} ${QPA}
    ${WEBOS_SYSROOT}/usr/include/glib-2.0 ${WEBOS_SYSROOT}/usr/lib/glib-2.0/include)

# Relay's screens, as in the computer build (the root CMakeLists.txt).
file(GLOB RELAY_QML CONFIGURE_DEPENDS ${RELAY}/qml/*.qml)
list(FILTER RELAY_QML EXCLUDE REGEX "/RoundedEffect\\.qml$")
set_source_files_properties(${RELAY}/qml/Theme.qml ${RELAY}/qml/Icons.qml ${RELAY}/qml/Keymap.qml
    PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
foreach(f ${RELAY_QML} ${RELAY}/qml/RoundedEffect.qml)
    get_filename_component(name ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()
qt_add_qml_module(relay URI Relay VERSION 1.0 QML_FILES ${RELAY_QML} RESOURCES ${RELAY}/qml/RoundedEffect.qml)

set_source_files_properties(${RELAY}/src/web/remote.html PROPERTIES QT_RESOURCE_ALIAS remote.html)
qt_add_resources(relay "relay_web" PREFIX "/web" FILES ${RELAY}/src/web/remote.html)
qt_add_resources(relay "relay_fonts" PREFIX "/fonts" BASE ${RELAY}/packaging/fonts/Inter FILES
    ${RELAY}/packaging/fonts/Inter/Inter-Regular.ttf ${RELAY}/packaging/fonts/Inter/Inter-Medium.ttf
    ${RELAY}/packaging/fonts/Inter/Inter-SemiBold.ttf ${RELAY}/packaging/fonts/Inter/Inter-Bold.ttf)
set_source_files_properties(${RELAY}/packaging/icons/relay.png PROPERTIES QT_RESOURCE_ALIAS relay.png)
qt_add_resources(relay "relay_icons" PREFIX "/icons" FILES ${RELAY}/packaging/icons/relay.png)

target_link_libraries(relay PRIVATE qsdl
    Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Network Qt6::Concurrent Qt6::OpenGL Qt6::Sql
    SDL2 luna-service2 glib-2.0 ${WEBOS_SYSROOT}/usr/lib/libssl.a ${WEBOS_SYSROOT}/usr/lib/libcrypto.a GLESv2 z m dl pthread)
# The TV's own libstdc++ is older than this compiler's; the package ships ours.
set_target_properties(relay PROPERTIES BUILD_RPATH "$ORIGIN/lib")
qt_import_qml_plugins(relay)
