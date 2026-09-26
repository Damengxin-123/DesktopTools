cmake_minimum_required(VERSION 3.21)
project(DesktopTool VERSION 2.1.1 LANGUAGES CXX)

# Qt 安装位置由 CMAKE_PREFIX_PATH、Qt6_DIR 或用户本地预设提供。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
# 保持便携版 exe 旁 data 的数据目录约定。
set(CMAKE_INSTALL_BINDIR ".")
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network WebChannel WebEngineWidgets)

# 显式维护后端源文件，界面资源由独立的 HTML 资源清单管理。
add_library(DesktopToolCore STATIC
    src/services/ServiceResult.h
    src/services/ShortcutService.h src/services/ShortcutService.cpp
    src/services/NoteService.h src/services/NoteService.cpp
    src/services/SettingsService.h src/services/SettingsService.cpp
    src/services/DownloadService.h src/services/DownloadService.cpp
    src/app/GlobalHotkey.h src/app/GlobalHotkey.cpp
    src/app/WebWindow.h src/app/WebWindow.cpp
    src/bridge/AppBridge.h src/bridge/AppBridge.cpp
)
target_include_directories(DesktopToolCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(DesktopToolCore PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::WebChannel Qt6::WebEngineWidgets)
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

# 官方部署流程包含 WebEngine 子进程、资源和语言包。
install(TARGETS DesktopTool RUNTIME DESTINATION .)
qt_generate_deploy_app_script(TARGET DesktopTool OUTPUT_SCRIPT desktoptool_deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT "${desktoptool_deploy_script}")

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(CatalogServiceTest tests/CatalogServiceTest.cpp)
    target_link_libraries(CatalogServiceTest PRIVATE DesktopToolCore)
    add_test(NAME catalog-services COMMAND CatalogServiceTest)
    add_executable(NoteServiceTest tests/NoteServiceTest.cpp)
    target_link_libraries(NoteServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME note-service COMMAND NoteServiceTest)
    add_executable(DownloadServiceTest tests/DownloadServiceTest.cpp)
    target_link_libraries(DownloadServiceTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME download-service COMMAND DownloadServiceTest)
    set_tests_properties(download-service PROPERTIES TIMEOUT 120)
    add_executable(WebSmokeTest tests/WebSmokeTest.cpp resources/app.qrc tests/test-resources.qrc)
    target_link_libraries(WebSmokeTest PRIVATE DesktopToolCore Qt6::Test)
    add_test(NAME web-integration COMMAND WebSmokeTest)
    set_tests_properties(web-integration PROPERTIES TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu")
endif()
