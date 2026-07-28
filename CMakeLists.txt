cmake_minimum_required(VERSION 3.15.0)

project(ODM
        LANGUAGES C CXX
        VERSION 0.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Resolve the Ultralight SDK path relative to this file.
get_filename_component(UL_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/ultralight-sdk" REALPATH)

if(NOT EXISTS "${UL_SDK_PATH}/include/AppCore/App.h")
    message(FATAL_ERROR "Ultralight SDK not found at '${UL_SDK_PATH}'. "
                        "Please place the SDK under 'ultralight-sdk/'.")
endif()

# Reuse the SDK's own sample cmake macros (add_app, add_console_app, etc.)
list(PREPEND CMAKE_MODULE_PATH "${UL_SDK_PATH}/samples/cmake")
include(Common)

# --- libcurl (HTTP/multi-segment download engine) ---------------------------
# We support two ways to provide libcurl dev files:
#   1. vcpkg: pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   2. A pre-built curl tree: -DCURL_ROOT="C:/path/to/curl"
find_package(CURL REQUIRED COMPONENTS HTTP HTTPS)

# --- MediaInfoLib (media analysis for the Properties dialog) ----------------
# Provided by vcpkg: `vcpkg install libmediainfo:x64-windows`.
find_package(MediaInfoLib CONFIG REQUIRED)

# --- FFmpeg (libavformat/libavcodec: lossless mux of split HLS/CMAF tracks) -
# Provided by vcpkg: `vcpkg install ffmpeg[core,avcodec,avformat]:x64-windows`.
find_package(FFMPEG REQUIRED)

# ---------------------------------------------------------------------------
# Our app target. We use Ultralight's add_app() helper which handles linking
# Ultralight/AppCore/WebCore/UltralightCore as well as copying the runtime
# DLLs and resources into the install output.
# ---------------------------------------------------------------------------
set(APP_NAME ODM)

add_app(${APP_NAME}
    src/main.cpp
    src/ODMApp.h
    src/ODMApp.cpp
    src/Downloader.h
    src/Downloader.cpp
    src/HlsDownloader.h
    src/HlsDownloader.cpp
    src/DashDownloader.h
    src/DashDownloader.cpp
    src/YtDlp.h
    src/YtDlp.cpp
    src/YtDlpDownloader.h
    src/YtDlpDownloader.cpp
    src/Muxer.h
    src/Muxer.cpp
    src/BridgeServer.h
    src/BridgeServer.cpp
)

# Embed the application icon into the Windows executable's resource section.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/icon.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/app.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
        @ONLY)
    target_sources(${APP_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
endif()

# Pull libcurl, MediaInfoLib and FFmpeg into the link line. FFmpeg's vcpkg
# port exports plain variables (no imported target), hence the trio below.
target_link_libraries(${APP_NAME} PRIVATE CURL::libcurl mediainfo)
target_include_directories(${APP_NAME} PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(${APP_NAME} PRIVATE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(${APP_NAME} PRIVATE ${FFMPEG_LIBRARIES})

# WinSock for the browser bridge server (localhost HTTP, no external deps).
if(WIN32)
    target_link_libraries(${APP_NAME} PRIVATE ws2_32)
endif()

# Make file:// URLs resolve to our assets/ folder next to the executable.
# ODM_VERSION comes from project(VERSION) above so the number the bridge
# reports (and the extension popup shows) cannot drift from the one the
# release is cut with.
target_compile_definitions(${APP_NAME} PRIVATE
    ULTRALIGHT_FILESYSTEM_PATH="./assets/"
    ODM_VERSION="${PROJECT_VERSION}")

# Copy our UI assets next to the binary for local testing / when running
# straight out of the build dir (the add_app macro already installs assets/
# into the install path, but we also want them adjacent to the .exe in build).
add_custom_command(TARGET ${APP_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:${APP_NAME}>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${UL_SDK_PATH}/resources"
        "$<TARGET_FILE_DIR:${APP_NAME}>/assets/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${UL_SDK_PATH}/bin"
        "$<TARGET_FILE_DIR:${APP_NAME}>")

# yt-dlp.exe (YouTube extraction) is not in the repository -- it is a 17 MB
# binary that updates itself. tools/package.ps1 downloads it when building a
# release; for a local build, drop it in tools/ and it lands next to ODM.exe.
# Without it everything else still builds and runs, YouTube links aside.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/yt-dlp.exe")
    add_custom_command(TARGET ${APP_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/yt-dlp.exe"
            "$<TARGET_FILE_DIR:${APP_NAME}>/yt-dlp.exe")
endif()
