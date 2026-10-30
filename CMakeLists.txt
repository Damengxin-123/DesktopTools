cmake_minimum_required(VERSION 3.21)
project(DesktopTool VERSION 2.2.1 LANGUAGES CXX)

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

# 关卡矢量工具共享原生编辑库，独立进程不依赖 WebEngine。
add_library(DesktopToolLevelCore STATIC
    src/level/LevelDocument.h src/level/LevelDocument.cpp
    src/level/LevelCanvas.h src/level/LevelCanvas.cpp
    src/level/LevelWindow.h src/level/LevelWindow.cpp)
target_include_directories(DesktopToolLevelCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(DesktopToolLevelCore PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets)
if(MSVC)
    target_compile_options(DesktopToolLevelCore PUBLIC /utf-8 /W4)
endif()
qt_add_executable(DesktopToolLevelEditor WIN32 src/tools/LevelToolMain.cpp)
target_link_libraries(DesktopToolLevelEditor PRIVATE DesktopToolLevelCore)
if(WIN32)
    target_sources(DesktopToolLevelEditor PRIVATE resources/app.rc)
    target_include_directories(DesktopToolLevelEditor PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources")
endif()
install(TARGETS DesktopToolLevelEditor RUNTIME DESTINATION .)

# 显式维护后端源文件，界面资源由独立的 HTML 资源清单管理。
add_library(DesktopToolCore STATIC
    src/services/ServiceResult.h
    src/services/ShortcutService.h src/services/ShortcutService.cpp
    src/services/BookmarkService.h src/services/BookmarkService.cpp
    src/services/NoteService.h src/services/NoteService.cpp
    src/services/SettingsService.h src/services/SettingsService.cpp
    src/services/ClipboardService.h src/services/ClipboardService.cpp
    src/services/EmojiService.h src/services/EmojiService.cpp
    src/services/DownloadService.h src/services/DownloadService.cpp
    src/services/TorrentService.h src/services/TorrentService.cpp
    src/services/WallpaperService.h src/services/WallpaperService.cpp
    src/services/GridMapService.h src/services/GridMapService.cpp
    src/services/QrDecoder.h src/services/QrDecoder.cpp
    src/services/QrService.h src/services/QrService.cpp
    src/services/ScreenshotService.h src/services/ScreenshotService.cpp
    src/services/RecordingService.h src/services/RecordingService.cpp
    src/services/FileSearchIndex.h src/services/FileSearchIndex.cpp
    src/services/FileSearchService.h src/services/FileSearchService.cpp
    src/services/FileJournalClient.h src/services/FileJournalClient.cpp
    src/services/TerrainService.h src/services/TerrainService.cpp
    src/app/GlobalHotkey.h src/app/GlobalHotkey.cpp
    src/app/ScreenshotOverlay.h src/app/ScreenshotOverlay.cpp
    src/app/RecordingOverlay.h src/app/RecordingOverlay.cpp
    src/app/RecordingControlBar.h src/app/RecordingControlBar.cpp
    src/app/RecordingRegionFrame.h src/app/RecordingRegionFrame.cpp
    src/app/AudioCapturer.h src/app/AudioCapturer.cpp
    src/app/ScreenRecorder.h src/app/ScreenRecorder.cpp
    src/app/WallpaperWindow.h src/app/WallpaperWindow.cpp
    src/app/InterfaceLanguage.h src/app/InterfaceLanguage.cpp
    src/app/PaintCanvas.h src/app/PaintCanvas.cpp
    src/app/PaintWindow.h src/app/PaintWindow.cpp
    src/app/WebWindow.h src/app/WebWindow.cpp
    src/app/BookmarkWindow.h src/app/BookmarkWindow.cpp
    src/app/TerrainEditWindow.h src/app/TerrainEditWindow.cpp
    src/bridge/AppBridge.h src/bridge/AppBridge.cpp
    src/bridge/BookmarkBridge.h src/bridge/BookmarkBridge.cpp
    src/bridge/TerrainBridge.h src/bridge/TerrainBridge.cpp
)
target_include_directories(DesktopToolCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(DesktopToolCore PUBLIC DesktopToolLevelCore DesktopToolMediaCore Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebChannel Qt6::WebEngineWidgets Qt6::Multimedia Qt6::MultimediaWidgets)
target_link_libraries(DesktopToolCore PRIVATE LibtorrentRasterbar::torrent-rasterbar)
if(WIN32)
    # 录屏的系统声音采集使用 WASAPI 环回（COM）。
    target_link_libraries(DesktopToolCore PRIVATE ole32)
    # 默认打开方式：SHChangeNotify 广播、SHOpenWithDialog 手动设置、SHLoadIndirectString 名称解析。
    target_link_libraries(DesktopToolCore PRIVATE shell32 shlwapi)
endif()
target_compile_definitions(DesktopToolCore PUBLIC DESKTOPTOOL_VERSION="${PROJECT_VERSION}" NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(DesktopToolCore PUBLIC /utf-8 /W4)
endif()
qt_add_executable(DesktopTool WIN32 src/main.cpp resources/app.qrc)
if(WIN32)
    target_sources(DesktopTool PRIVATE resources/app.rc)
    target_include_directories(DesktopTool PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources")
endif()
target_link_libraries(DesktopTool PRIVATE DesktopToolCore Qt6::Network)
# 构建主程序时同步生成它调用的两个工具。
add_dependencies(DesktopTool DesktopToolImageBrowser DesktopToolVideoPlayer DesktopToolLevelEditor)

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

# 官方部署流程包含 WebEngine 子进程、资源和语言包。
install(TARGETS DesktopTool RUNTIME DESTINATION .)
qt_generate_deploy_app_script(TARGET DesktopTool OUTPUT_SCRIPT desktoptool_deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT "${desktoptool_deploy_script}")

# 工具安装到应用根目录的 tools，可单独部署此组件到项目根目录。
install(TARGETS DesktopToolImageBrowser DesktopToolVideoPlayer RUNTIME DESTINATION tools COMPONENT MediaTools)
qt_generate_deploy_script(TARGET DesktopToolImageBrowser OUTPUT_SCRIPT media_tools_deploy_script CONTENT [[
    set(QT_DEPLOY_BIN_DIR tools)
    set(QT_DEPLOY_LIB_DIR tools)
    set(QT_DEPLOY_PLUGINS_DIR tools/plugins)
    set(QT_DEPLOY_TRANSLATIONS_DIR tools/translations)
    qt_deploy_runtime_dependencies(
        EXECUTABLE "tools/$<TARGET_FILE_NAME:DesktopToolImageBrowser>"
        ADDITIONAL_EXECUTABLES "tools/$<TARGET_FILE_NAME:DesktopToolVideoPlayer>"
        GENERATE_QT_CONF)
]])
install(SCRIPT "${media_tools_deploy_script}" COMPONENT MediaTools)

# 静态 vcpkg 构建无需额外运行库；独立动态依赖可显式提供待部署的 DLL。
set(DESKTOPTOOL_EXTRA_RUNTIME_FILES "" CACHE STRING "随应用部署的额外运行库文件列表")
if(DESKTOPTOOL_EXTRA_RUNTIME_FILES)
    install(FILES ${DESKTOPTOOL_EXTRA_RUNTIME_FILES} DESTINATION .)
endif()
# 保留磁力引擎、Boost 和 OpenSSL 的完整分发许可证。
install(DIRECTORY third_party/licenses/ DESTINATION licenses)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    # 验证矢量对象往返保存、吸附、交互及导出语义。
    add_executable(LevelLayoutTest tests/LevelLayoutTest.cpp)
    target_link_libraries(LevelLayoutTest PRIVATE DesktopToolLevelCore Qt6::Test)
    add_test(NAME level-layout COMMAND LevelLayoutTest)
    set_tests_properties(level-layout PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # 验证画板门控、失焦断笔以及原生数位笔输入，不依赖实体硬件。
    add_executable(PaintCanvasTest tests/PaintCanvasTest.cpp)
    target_link_libraries(PaintCanvasTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME paint-canvas COMMAND PaintCanvasTest)
    set_tests_properties(paint-canvas PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(ClipboardServiceTest tests/ClipboardServiceTest.cpp)
    target_link_libraries(ClipboardServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME clipboard-service COMMAND ClipboardServiceTest)
    set_tests_properties(clipboard-service PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(EmojiServiceTest tests/EmojiServiceTest.cpp)
    target_link_libraries(EmojiServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME emoji-service COMMAND EmojiServiceTest)
    set_tests_properties(emoji-service PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(WallpaperServiceTest tests/WallpaperServiceTest.cpp)
    target_link_libraries(WallpaperServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME wallpaper-service COMMAND WallpaperServiceTest)
    set_tests_properties(wallpaper-service PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(GridMapServiceTest tests/GridMapServiceTest.cpp)
    target_link_libraries(GridMapServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME gridmap-service COMMAND GridMapServiceTest)
    set_tests_properties(gridmap-service PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(QrServiceTest tests/QrServiceTest.cpp)
    target_link_libraries(QrServiceTest PRIVATE DesktopToolCore Qt6::Test)
    target_compile_definitions(QrServiceTest PRIVATE TEST_QR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/media")
    add_test(NAME qr-service COMMAND QrServiceTest)
    set_tests_properties(qr-service PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(ScreenshotServiceTest tests/ScreenshotServiceTest.cpp)
    target_link_libraries(ScreenshotServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME screenshot-service COMMAND ScreenshotServiceTest)
    set_tests_properties(screenshot-service PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(RecordingServiceTest tests/RecordingServiceTest.cpp)
    target_link_libraries(RecordingServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME recording-service COMMAND RecordingServiceTest)
    set_tests_properties(recording-service PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(MediaServiceTest tests/MediaServiceTest.cpp)
    target_link_libraries(MediaServiceTest PRIVATE DesktopToolMediaCore Qt6::Test)
    add_test(NAME media-service COMMAND MediaServiceTest)
    set_tests_properties(media-service PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # 普通权限下验证实际管道收发与关闭，不安装系统服务。
    add_executable(FileJournalTransportTest tests/FileJournalTransportTest.cpp)
    target_include_directories(FileJournalTransportTest PRIVATE src)
    target_compile_definitions(FileJournalTransportTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(FileJournalTransportTest PRIVATE /utf-8 /W4)
    target_link_libraries(FileJournalTransportTest PRIVATE advapi32 shell32)
    add_test(NAME file-journal-transport COMMAND FileJournalTransportTest)
    set_tests_properties(file-journal-transport PROPERTIES TIMEOUT 30)
    # 手动运行的百万条索引性能基准，不影响普通回归测试时长。
    add_executable(FileSearchBenchmark tests/FileSearchBenchmark.cpp)
    target_link_libraries(FileSearchBenchmark PRIVATE DesktopToolCore psapi)
    add_executable(FileSearchServiceTest tests/FileSearchServiceTest.cpp)
    target_link_libraries(FileSearchServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME file-search COMMAND FileSearchServiceTest)
    set_tests_properties(file-search PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(RecordingPipelineSmokeTest tests/RecordingPipelineSmokeTest.cpp)
    target_link_libraries(RecordingPipelineSmokeTest PRIVATE Qt6::Multimedia Qt6::Gui Qt6::Test)
    add_test(NAME recording-pipeline COMMAND RecordingPipelineSmokeTest)
    set_tests_properties(recording-pipeline PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(WallpaperVideoTest tests/WallpaperVideoTest.cpp resources/app.qrc)
    target_link_libraries(WallpaperVideoTest PRIVATE DesktopToolCore Qt6::Test)
    target_compile_definitions(WallpaperVideoTest PRIVATE TEST_VIDEO_FILE="${CMAKE_CURRENT_SOURCE_DIR}/tests/media/wallpaper-h264.mp4")
    add_test(NAME wallpaper-video COMMAND WallpaperVideoTest)
    set_tests_properties(wallpaper-video PROPERTIES TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu")
    # 真实 Windows 图形会话的手动回归，验证 WARP WebGL2 与事件循环，不使用 offscreen。
    add_executable(RenderingCompatibilitySmokeTest tests/RenderingCompatibilitySmokeTest.cpp)
    target_include_directories(RenderingCompatibilitySmokeTest PRIVATE src)
    target_link_libraries(RenderingCompatibilitySmokeTest PRIVATE Qt6::Widgets Qt6::WebEngineWidgets)
    if(MSVC)
        target_compile_options(RenderingCompatibilitySmokeTest PRIVATE /utf-8 /W4)
    endif()
    # 真实桌面环境的动态壁纸冒烟程序，需手动运行验证挂载与媒体加载。
    add_executable(wallpaper-smoke tests/WallpaperSmoke.cpp resources/app.qrc)
    target_link_libraries(wallpaper-smoke PRIVATE DesktopToolCore)
    # 真实桌面环境的地形编辑冒烟程序：验证 WebGL 渲染、笔划、保存与导出。
    add_executable(terrain-smoke tests/TerrainSmoke.cpp resources/app.qrc)
    target_link_libraries(terrain-smoke PRIVATE DesktopToolCore)
    add_executable(CatalogServiceTest tests/CatalogServiceTest.cpp)
    target_link_libraries(CatalogServiceTest PRIVATE DesktopToolCore)
    add_test(NAME catalog-services COMMAND CatalogServiceTest)
    add_executable(BookmarkServiceTest tests/BookmarkServiceTest.cpp)
    target_link_libraries(BookmarkServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME bookmark-service COMMAND BookmarkServiceTest)
    add_executable(NoteServiceTest tests/NoteServiceTest.cpp)
    target_link_libraries(NoteServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME note-service COMMAND NoteServiceTest)
    add_executable(DownloadServiceTest tests/DownloadServiceTest.cpp)
    target_link_libraries(DownloadServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME download-service COMMAND DownloadServiceTest)
    set_tests_properties(download-service PROPERTIES TIMEOUT 120)
    add_executable(TorrentServiceTest tests/TorrentServiceTest.cpp)
    target_link_libraries(TorrentServiceTest PRIVATE DesktopToolCore Qt6::Test LibtorrentRasterbar::torrent-rasterbar)
    add_test(NAME torrent-service COMMAND TorrentServiceTest)
    set_tests_properties(torrent-service PROPERTIES TIMEOUT 120)
    add_executable(TerrainServiceTest tests/TerrainServiceTest.cpp)
    target_link_libraries(TerrainServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME terrain-service COMMAND TerrainServiceTest)
    set_tests_properties(terrain-service PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(BookmarkWindowSmokeTest tests/BookmarkWindowSmokeTest.cpp resources/app.qrc)
    target_link_libraries(BookmarkWindowSmokeTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME bookmark-window COMMAND BookmarkWindowSmokeTest)
    set_tests_properties(bookmark-window PROPERTIES TIMEOUT 180
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu")
    add_executable(TerrainWebSmokeTest tests/TerrainWebSmokeTest.cpp resources/app.qrc)
    target_link_libraries(TerrainWebSmokeTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME terrain-web COMMAND TerrainWebSmokeTest)
    set_tests_properties(terrain-web PROPERTIES TIMEOUT 240
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu --enable-unsafe-swiftshader")
    add_executable(WebSmokeTest tests/WebSmokeTest.cpp resources/app.qrc tests/test-resources.qrc)
    target_link_libraries(WebSmokeTest PRIVATE DesktopToolCore Qt6::Test LibtorrentRasterbar::torrent-rasterbar)
    add_dependencies(WebSmokeTest DesktopToolImageBrowser DesktopToolVideoPlayer)
    target_compile_definitions(WebSmokeTest PRIVATE TEST_QR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/media")
    add_test(NAME web-integration COMMAND WebSmokeTest)
    set_tests_properties(web-integration PROPERTIES TIMEOUT 240 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu")
endif()
