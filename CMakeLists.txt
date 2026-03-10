# CMakeList.txt: DesktopTool 的 CMake 项目，在此处包括源代码并定义
# 项目特定的逻辑。
#
cmake_minimum_required (VERSION 3.8)

# 如果支持，请为 MSVC 编译器启用热重载。
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project ("DesktopTool")

# 指定qt库路径
set(CMAKE_PREFIX_PATH "J:\\TOOL\\Qt\\6.10.0\\msvc2022_64")


# 设置编译属性 不区分平台
SET(CMAKE_INCLUDE_CURRENT_DIR ON)

#SET(CMAKE_AUTOUIC ON)
SET(CMAKE_AUTOMOC ON)
SET(CMAKE_AUTORCC ON)

SET(CMAKE_CXX_STANDARD 11)
SET(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置生成的可执行文件名称
SET(APP_NAME "DesktopTool")



# 添加子项cmake
add_subdirectory("code" ${PROJECT_SOURCE_DIR}/out/code_build)

# 生成qt依赖拷贝脚本
set(QT_BIN_DIR "${CMAKE_PREFIX_PATH}\\bin")
set(WINDEPLOYQT_PATH "${QT_BIN_DIR}\\windeployqt.exe")
string(REPLACE "/" "\\" PACK_CMD "${WINDEPLOYQT_PATH} ${CMAKE_SOURCE_DIR}/out/bin/${APP_NAME}.exe ${CMAKE_SOURCE_DIR}/out/bin")



# 写入到 pack/copy_lib.bat
file(WRITE "${CMAKE_SOURCE_DIR}/pack/copy_lib.bat" "${PACK_CMD}\n")

# 生成qt库打包脚本
set(QT_BIN_DIR "${CMAKE_PREFIX_PATH}\\bin")
set(WINDEPLOYQT_PATH "${QT_BIN_DIR}\\windeployqt.exe")
set(PACK_CMD 
"cd ${CMAKE_SOURCE_DIR}/pack/ \n
mkdir app \n 
copy ${CMAKE_SOURCE_DIR}/out/bin/${APP_NAME}.exe ${CMAKE_SOURCE_DIR}/pack/app/\n
${WINDEPLOYQT_PATH} ${CMAKE_SOURCE_DIR}\\pack\\app\\${APP_NAME}.exe ${CMAKE_SOURCE_DIR}\\pack\\app")
string(REPLACE "/" "\\" PACK_CMD ${PACK_CMD})


# 写入到 pack/copy_lib.bat
file(WRITE "${CMAKE_SOURCE_DIR}/pack/pack_app.bat" "${PACK_CMD}\n")



# TODO: 如有需要，请添加测试并安装目标。
