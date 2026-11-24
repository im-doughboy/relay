cmake_minimum_required(VERSION 3.22)
project(relay-spike LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

set(WEBOS_SYSROOT "" CACHE PATH "The webOS SDK's sysroot")
set(FFMPEG_PREFIX "" CACHE PATH "FFmpeg built for the TV (build-ffmpeg.sh)")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
# QML modules under :/qt/qml, where the QML engine looks for them.
qt_policy(SET QTP0001 NEW)

# Qt's window system: SDL (platform/webos/qpa-sdl).
set(QPA ${CMAKE_CURRENT_SOURCE_DIR}/../qpa-sdl)
qt_add_plugin(qsdl STATIC CLASS_NAME QSdlIntegrationPlugin PLUGIN_TYPE platforms)
target_sources(qsdl PRIVATE ${QPA}/qsdlintegration.cpp ${QPA}/qsdlintegration.h)
target_link_libraries(qsdl PRIVATE Qt6::Gui Qt6::GuiPrivate SDL2)

set(RELAY ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
qt_add_executable(relay-spike main.cpp feed.cpp luna.cpp ui.cpp ndl.c ${RELAY}/src/app/Fonts.cpp)
qt_add_qml_module(relay-spike URI RelaySpike VERSION 1.0 QML_FILES Main.qml)
# Relay's bundled font, the same files the Windows build uses.
qt_add_resources(relay-spike "relay_fonts" PREFIX "/fonts" BASE ${RELAY}/packaging/fonts/Inter
    FILES ${RELAY}/packaging/fonts/Inter/Inter-Regular.ttf ${RELAY}/packaging/fonts/Inter/Inter-Medium.ttf
          ${RELAY}/packaging/fonts/Inter/Inter-SemiBold.ttf ${RELAY}/packaging/fonts/Inter/Inter-Bold.ttf)
target_include_directories(relay-spike PRIVATE ${QPA} ${RELAY}/src/app
    ${WEBOS_SYSROOT}/usr/include/glib-2.0 ${WEBOS_SYSROOT}/usr/lib/glib-2.0/include
    ${WEBOS_SYSROOT}/usr/include/starfish-media-pipeline ${FFMPEG_PREFIX}/include)
target_link_libraries(relay-spike PRIVATE qsdl Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    SDL2 SDL2_ttf umedia_api playerAPIs NDL_directmedia luna-service2 glib-2.0
    ${FFMPEG_PREFIX}/lib/libavformat.a ${FFMPEG_PREFIX}/lib/libavcodec.a
    ${FFMPEG_PREFIX}/lib/libswresample.a ${FFMPEG_PREFIX}/lib/libavutil.a
    ${WEBOS_SYSROOT}/usr/lib/libssl.a ${WEBOS_SYSROOT}/usr/lib/libcrypto.a
    GLESv2 z m dl pthread)
# The TV's own libstdc++ is older than this compiler's; the package ships ours.
set_target_properties(relay-spike PROPERTIES BUILD_RPATH "$ORIGIN/lib")
qt_import_qml_plugins(relay-spike)
