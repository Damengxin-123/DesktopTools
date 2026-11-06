cmake_minimum_required(VERSION 3.21)
project(DesktopToolGodotNative VERSION 2.2.1 LANGUAGES C CXX)

# Qt 安装位置由 CMAKE_PREFIX_PATH、Qt6_DIR 或用户本地预设提供。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# 保持便携版 exe 旁 data 的数据目录约定。
set(CMAKE_INSTALL_BINDIR ".")
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network WebChannel WebEngineWidgets Multimedia MultimediaWidgets)
find_package(LibtorrentRasterbar 2.1 CONFIG REQUIRED)

# 媒体支撑库只依赖原生 Qt，独立工具不链接主程序、WebEngine 或磁力引擎。
add_library(DesktopToolMediaCore STATIC
    src/services/MediaService.h src/services/MediaService.cpp)
target_include_directories(DesktopToolMediaCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(DesktopToolMediaCore PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_definitions(DesktopToolMediaCore PUBLIC DESKTOPTOOL_VERSION="${PROJECT_VERSION}" NOMINMAX WIN32_LEAN_AND_MEAN)
if(WIN32)
    target_link_libraries(DesktopToolMediaCore PRIVATE shell32 shlwapi advapi32)
endif()
if(MSVC)
    target_compile_options(DesktopToolMediaCore PUBLIC /utf-8 /W4)
endif()

# 独立工具只使用原生文案翻译，不引入网页运行环境。
add_library(DesktopToolMediaLanguage STATIC src/app/InterfaceLanguage.h src/app/InterfaceLanguage.cpp)
target_include_directories(DesktopToolMediaLanguage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(DesktopToolMediaLanguage PRIVATE DESKTOPTOOL_NATIVE_ONLY)
target_link_libraries(DesktopToolMediaLanguage PUBLIC DesktopToolMediaCore)
qt_add_executable(DesktopToolImageBrowser WIN32 src/tools/MediaToolMain.cpp
    src/app/ImageBrowserWindow.h src/app/ImageBrowserWindow.cpp resources/media-tools.qrc)
qt_add_executable(DesktopToolVideoPlayer WIN32 src/tools/MediaToolMain.cpp
    src/app/VideoPlayerWindow.h src/app/VideoPlayerWindow.cpp resources/media-tools.qrc)
target_compile_definitions(DesktopToolVideoPlayer PRIVATE DESKTOPTOOL_VIDEO_TOOL)
target_link_libraries(DesktopToolImageBrowser PRIVATE DesktopToolMediaLanguage)
target_link_libraries(DesktopToolVideoPlayer PRIVATE DesktopToolMediaLanguage Qt6::Multimedia Qt6::MultimediaWidgets)
foreach(media_tool IN ITEMS DesktopToolImageBrowser DesktopToolVideoPlayer)
    set_target_properties(${media_tool} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tools")
    if(WIN32)
        target_sources(${media_tool} PRIVATE resources/app.rc)
        target_include_directories(${media_tool} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources")
    endif()
endforeach()

# 显式维护后端源文件，界面资源由独立的 HTML 资源清单管理。
add_library(DesktopToolCore STATIC
    src/services/ServiceResult.h
    src/services/ShortcutService.h src/services/ShortcutService.cpp
    src/services/NoteService.h services/NoteService.cpp
    src/services/SettingsService.h src/services/SettingsService.cpp
    src/services/ClipboardService.h src/services/ClipboardService.cpp
    src/services/EmojiService.h src/services/EmojiService.cpp
    src/services/DownloadService.h src/services/DownloadService.cpp
    src/services/TorrentService.h src/services/TorrentService.cpp
    src/services/WallpaperService.h src/services/WallpaperService.cpp
    src/services/QrDecoder.h src/services/QrDecoder.cpp
    src/services/QrService.h src/services/QrService.cpp
    src/services/ScreenshotService.h src/services/ScreenshotService.cpp
    src/services/RecordingService.h src/services/RecordingService.cpp
    src/services/FileSearchIndex.h src/services/FileSearchIndex.cpp
    src/services/FileSearchService.h src/services/FileSearchService.cpp
    src/services/FileJournalClient.h src/services/FileJournalClient.cpp
    src/app/GlobalHotkey.h src/app/GlobalHotkey.cpp
    src/app/ScreenshotOverlay.h src/app/ScreenshotOverlay.cpp
    src/app/RecordingOverlay.h src/app/RecordingOverlay.cpp
    src/app/RecordingControlBar.h src/app/RecordingControlBar.cpp
    src/app/RecordingRegionFrame.h src/app/RecordingRegionFrame.cpp
    src/app/AudioCapturer.h src/app/AudioCapturer.cpp
    src/app/ScreenRecorder.h src/app/ScreenRecorder.cpp
    src/app/WallpaperWindow.h src/app/WallpaperWindow.cpp
    src/app/InterfaceLanguage.h src/app/InterfaceLanguage.cpp
    src/bridge/AppBridge.h src/bridge/AppBridge.cpp
)
target_include_directories(DesktopToolCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(DesktopToolCore PUBLIC DesktopToolMediaCore Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebChannel Qt6::WebEngineWidgets Qt6::Multimedia Qt6::MultimediaWidgets)
target_link_libraries(DesktopToolCore PRIVATE LibtorrentRasterbar::torrent-rasterbar)
if(WIN32)
    # 录屏的系统声音采集使用 WASAPI 环回（COM）。
    target_link_libraries(DesktopToolCore PRIVATE ole32 user32 gdi32)
    # 默认打开方式：SHChangeNotify 广播、SHOpenWithDialog 手动设置、SHLoadIndirectString 名称解析。
    target_link_libraries(DesktopToolCore PRIVATE shell32 shlwapi)
endif()
target_compile_definitions(DesktopToolCore PUBLIC DESKTOPTOOL_VERSION="${PROJECT_VERSION}" NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(DesktopToolCore PUBLIC /utf-8 /W4)
endif()
# 独立 NTFS 权限代理不链接 Qt，使用静态 CRT 以便从受保护服务目录启动。
if(WIN32)
    add_executable(DesktopToolIndexService src/services/FileIndexServiceMain.cpp)
    set_target_properties(DesktopToolIndexService PROPERTIES AUTOMOC OFF AUTORCC OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(DesktopToolIndexService PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(DesktopToolIndexService PRIVATE /utf-8 /W4)
    target_link_libraries(DesktopToolIndexService PRIVATE advapi32 shell32)
    install(TARGETS DesktopToolIndexService RUNTIME DESTINATION .)
endif()


# 当前文档编辑器使用独立的加密包存储。
target_sources(DesktopToolCore PRIVATE document_package.cpp document_package.h)
target_include_directories(DesktopToolCore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
# STORE 模式避免重复压缩图片；Windows 原生加密后端提供 WinZip AES。
set(MZ_COMPAT OFF CACHE BOOL "" FORCE)
foreach(option MZ_ZLIB MZ_BZIP2 MZ_LZMA MZ_ZSTD MZ_PKCRYPT MZ_FETCH_LIBS MZ_BUILD_TESTS MZ_BUILD_UNIT_TESTS)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
set(MZ_WZAES ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(third_party/minizip-ng-4.0.10 EXCLUDE_FROM_ALL)
target_link_libraries(DesktopToolCore PRIVATE MINIZIP::minizip)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
configure_file(runtime.qrc.in runtime.qrc @ONLY)
add_executable(DesktopToolNative WIN32 main.cpp document_editor.cpp document_editor.h "${CMAKE_CURRENT_BINARY_DIR}/runtime.qrc")
target_compile_features(DesktopToolNative PRIVATE cxx_std_17)
target_link_libraries(DesktopToolNative PRIVATE DesktopToolCore Qt6::Widgets Qt6::Network Qt6::WebChannel Qt6::WebEngineWidgets)
set_target_properties(DesktopToolNative PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_dependencies(DesktopToolNative DesktopToolImageBrowser DesktopToolVideoPlayer DesktopToolIndexService)
