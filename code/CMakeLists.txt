cmake_minimum_required (VERSION 3.8)

#项目名称
project (DesktopTool LANGUAGES CXX)

# 生成pdb调试文件
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Zi")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")

# 查找Qt库
find_package(Qt6 COMPONENTS Widgets Core Gui Network sql LinguistTools REQUIRED )

# 设置资源文件
SET(RC_FILES ${PROJECT_SOURCE_DIR}/res/res.qrc)

# 链接头文件
include_directories(
./main/inc
./customizeObj/inc
./systemTool/inc
./widgets/inc
./itemWidgets/inc
)

# 待编译头文件
FILE(GLOB HEADERS 
"./main/inc/*.h"
"./customizeObj/inc/*.h"
"./systemTool/inc/*.h"
"./widgets/inc/*.h"
"./itemWidgets/inc/*.h"
)

# 待编译源代码
FILE(GLOB SOURCES 
"./main/src/*.cpp"
"./customizeObj/src/*.cpp"
"./systemTool/src/*.cpp"
"./widgets/src/*.cpp"
"./itemWidgets/src/*.cpp"
)

# ui文件
FILE(GLOB FORMS 
"./widgets/form/*.ui"
"./customizeObj/form/*.ui"
"./itemWidgets/form/*.ui"
)
FILE(GLOB APP_WINIRC 
"./res/qss/*.rc"
)

#通过ui文件生成对应的头文件
qt6_wrap_ui(WRAP_FILES ${FORMS})

qt6_create_translation(QM_FILES
    ${PROJECT_SOURCE_DIR}/inc
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/form
    #${TS_FILES}
    OPTIONS -source-language en_US -no-obsolete
)

#------------------------设置生成目录
SET(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/bin)
SET(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/bin)

SET(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/out/libs)

# 编译生成可执行程序
ADD_EXECUTABLE(${APP_NAME} WIN32  ${HEADERS} ${SOURCES} ${FORMS} ${RC_FILES} ${APP_WINIRC}) # ${app_WINIRC}

# 链接依赖的动态库
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Widgets Qt6::Core Qt6::Gui Qt6::Network Qt6::Sql)

