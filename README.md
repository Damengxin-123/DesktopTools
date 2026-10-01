# DesktopTool 桌面工具

使用 **Qt 原生窗口 + HTML/CSS/JavaScript + QWebChannel + Qt 业务服务** 的 Windows 桌面工具。页面通过 Qt 资源系统随程序打包，离线运行，不需要 Node.js、npm 或本地 HTTP 服务。

## 功能与架构

- **快捷方式**：目录、网址、文件的分类管理、搜索、编辑、批量删除与排序，由 Qt 调用系统打开目标。
- **便签**：分类管理、富文本编辑与图片保存；Qt 负责内容过滤、文件访问及数据保存。
- **剪贴板**：勾选文本、图像、文件 / 文件夹、链接或其他格式信息后自动保存复制历史；支持完整文本搜索、类型筛选、详情、再次复制、置顶、批量删除及清空。
- **下载**：支持 HTTP/HTTPS 和磁力链接，查看进度、速度和历史记录；磁力任务获取文件清单后，由用户勾选并确认才下载内容，支持暂停、继续、取消及打开所在目录。
- **设置**：字号、全局热键、默认下载目录、开机自启和恢复默认设置。勾选“开机自启”并保存后，在当前用户登录 Windows 时启动；取消勾选并保存或恢复默认设置会移除自启项。自启使用当前程序路径与数据目录，无需管理员权限。热键注册、自启项修改及设置写入一起成功后生效。
- **系统集成**：托盘、默认 F8 唤醒、单实例唤醒及兼容旧版本的实例标识。有托盘时关闭窗口会隐藏，真正退出使用托盘菜单。

应用包含快捷方式、便签、剪贴板、下载和设置五个页面。旧版的登录和日志占位页面未纳入新版。

```text
HTML 页面 → web/bridge.js → QWebChannel → AppBridge → Qt 业务服务 → 数据文件
                                             └→ 文件选择、剪贴板、系统热键
```

| 目录 | 用途 |
| --- | --- |
| `src/app/` | 原生窗口、WebEngine 安全边界、托盘和全局热键 |
| `src/bridge/` | 唯一网页接口 `AppBridge`，协调系统操作和服务调用 |
| `src/services/` | 快捷方式、便签、下载任务和设置服务，校验数据并持久化 |
| `web/` | HTML、CSS、JavaScript 页面与异步桥接封装 |
| `resources/` | 应用图标、Windows 资源及将网页编译进应用的 Qt 资源清单 |
| `tests/` | 服务回归测试和真实 WebEngine / WebChannel 集成测试 |
| `pack/` | 本机更新、生成不含用户数据的便携 ZIP 压缩包 |

仓库以新版 `src/` 和 `web/` 为活动代码，不再包含旧版 `code/`、编译缓存或预先打包的 Qt 运行库。旧版源码可从 Git 历史查看；用户数据格式的兼容与迁移继续由新版服务提供。

页面只负责展示和交互；数据校验、读写、打开文件及系统集成由 Qt 处理。便签 HTML 经后端重建，资源请求和页面导航受限，外部网页不会加载到承载原生接口的页面中。

## 剪贴板历史

在“剪贴板”页顶部勾选需要保存的内容类型，勾选立即保存并在下次启动恢复；首次使用默认不记录，全部取消勾选即暂停。程序运行期间监听系统剪贴板变化，包括其他程序中的复制和剪切，隐藏到托盘后仍然有效；退出程序期间的复制无法补录，启动时也不会把已有剪贴板内容当作新事件。

聊天软件一次复制可能多次更新剪贴板格式。连续 1 秒内实际保存内容相同的通知只记一次，补充 HTML 格式或改变文本换行形式不会重复新增；原文保持不变。窗口从首次成功记录起计算，重复通知不会延长。不同内容立即保存，超过窗口再次复制相同内容也会新增；只记录格式名称的“其他”类型不作内容去重。

卡片显示内容类型和本地复制时间（精确到秒）。文本按纯文本保存，图片保存 PNG 内容并显示缩略图；普通文件和文件夹只保存名称、路径，不备份文件实体。复制的图像文件、截图，以及聊天工具提供的图像内容或可读取的本地图像缓存均归入“图像”；同时提供单张图像和临时路径时优先保存图像。按文件内容识别图像，兼容没有常见图片扩展名的缓存；读取前检查尺寸。一次复制多张图片或混合图片与普通文件时，分别按图像实体和文件引用保存，各自遵循类型勾选。无法解码或超出限制的图片会显示提示，不改归普通文件。带 URL 数据的其他复制归入“链接”，普通网址文本归入“文本”。其他应用专有格式仅保留格式名称，不保存二进制内容，不能恢复原内容；没有本地路径的虚拟文件也不会自动下载或实体化。

可搜索完整文本和文件路径，按类型筛选、查看详情、再次复制、置顶、全选当前结果、批量删除或确认清空。全选包含当前筛选结果的所有分页；清空包含置顶记录。再次复制文件会恢复系统文件引用供粘贴使用；原路径失效时会显示错误，历史中的名称和路径仍可查看或复制。删除历史不会删除引用的原文件。

历史和监听设置保存在数据目录的 `clipboard-history.v1.json`。最多保留 500 条、64 MB，达到上限自动移除最早的未置顶记录；置顶内容占满容量时会提示并跳过本次复制。单条文本 / 路径信息上限 2 MB，图片上限 2000 万像素且 PNG 不超过 8 MB，单次文件 / 链接上限 1000 项。索引损坏或保存失败会显示提示，不覆盖原数据。

## 下载任务

在下载页填写 HTTP/HTTPS 文件网址即可创建任务，也可为单次下载选择目录。设置中的默认下载目录初始为系统“下载”文件夹，清空后保存可恢复系统默认位置；自定义默认目录须已存在且可写。

输入 `magnet:?` 链接时自动进入磁力流程：先解析链接并连接节点获取文件清单，随后弹出文件选择窗口，支持全选、全不选及查看选中大小。至少勾选一个文件后点击“开始下载”，才传输文件内容。关闭弹窗会保留“等待选择文件”的任务，可从卡片重新打开；暂停、重启或重试不会跳过确认步骤。

磁力任务由本机 libtorrent 引擎处理，不使用第三方解析网站。每个任务保存到独立目录，避免覆盖已有文件；完成所选文件后停止传输。获取清单和下载速度取决于可用节点，暂时没有节点时可暂停后再继续。

不带 tracker 的磁力链接通过多个 DHT 引导入口寻找来源，单个入口的 DNS 或连接异常不会成为唯一阻塞点。节点网络建立后会补发文件来源查询；解析卡片显示等待时间，并区分正在连接资源网络、寻找来源和获取文件信息。网盘可能已缓存文件清单，本机解析仍需要能通信的在线来源。

任务可暂停后继续；服务器支持可靠的断点续传时，会接着已有内容下载，不支持时重新下载。任务所选目录不可用时，会回退到默认目录，并在任务中显示说明；默认目录也不可用时，依次尝试系统“下载”目录和应用数据目录中的 `downloads` 文件夹。

HTTP/HTTPS 连接连续 30 秒没有数据或暂时中断时，任务会在 1、2、4 秒后自动重试，单轮最多三次。有可靠断点标识时从已保存的位置继续，否则安全地重新下载；等待重试时仍可暂停或取消。超过次数后会显示失败，点击“重试”可重新开始一轮。

下载历史会保留，应用重启后不会自动发起下载：原下载中的任务变为暂停，暂停或失败的任务可通过“继续”或“重试”恢复。下载完成的文件不会被自动打开或执行，可以使用“打开目录”自行查看；移除完成记录会保留文件。

## 构建与运行

需要 Windows x64、MSVC x64 C++ 工具链、CMake 3.21 或更新版本、Ninja，以及 **Qt 6.8 或更新版本的 MSVC x64 套件**。Qt 组件需包含 `Core`、`Gui`、`Widgets`、`Network`、`WebChannel`、`WebEngineWidgets`；默认启用测试，还需 `Test`。项目使用 C++17；磁力下载依赖 libtorrent 2.1，仓库的 `vcpkg.json` 固定依赖基线并关闭不需要的 WebTorrent 功能。

先打开 Visual Studio 的 **x64 Native Tools Command Prompt（x64 原生工具命令提示符）**，进入克隆的仓库根目录。CMake 和 Ninja 需位于 `PATH`。以下使用已验证的 Qt 6.10.3 举例，请将 Qt 路径替换为自己的安装位置：

```bat
set "QT_ROOT=C:\Qt\6.10.3\msvc2022_64"
set "VCPKG_ROOT=C:\dev\vcpkg"
set "PATH=%QT_ROOT%\bin;%PATH%"
cmake --preset x64-release -DCMAKE_PREFIX_PATH="%QT_ROOT%" -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build --preset x64-release
ctest --preset x64-release
```

开发者命令提示符负责初始化 MSVC x64 编译器环境，也可手动调用 Visual Studio 安装目录下的 `Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64`。Qt 的 `bin` 加入 `PATH` 供开发运行及测试加载 DLL。若 Ninja 未加入 `PATH`，把其安装目录一并加入，或在配置命令中指定 `-DCMAKE_MAKE_PROGRAM=完整路径`。

项目不固定开发者机器上的 Qt 安装路径。使用 `-DCMAKE_PREFIX_PATH` 指定完整套件，或将个人配置写入已忽略的 `CMakeUserPresets.json`。

`VCPKG_ROOT` 指向已执行 `bootstrap-vcpkg.bat` 的官方 vcpkg 仓库。首次配置会构建磁力依赖；`x64-windows-static-md` 将 libtorrent 及依赖静态链接，同时使用与 Qt 一致的动态 MSVC 运行库。已有独立依赖安装也可通过 `CMAKE_PREFIX_PATH` 提供 `LibtorrentRasterbarConfig.cmake`。从旧构建切换到 vcpkg 工具链时使用新的构建目录，或在 CMake 3.24 及以上添加 `--fresh` 重新配置。

独立依赖还验证过 libtorrent 2.1.2、Boost 1.85 头文件与 OpenSSL 3.6.3 的组合。使用动态 OpenSSL 时，通过 `OPENSSL_ROOT_DIR` 指定同一版本的头文件和库，并将两个 DLL 的完整路径以分号分隔传入 `DESKTOPTOOL_EXTRA_RUNTIME_FILES`；安装流程会将它们放在程序旁。开发运行和测试时也需将其 `bin` 目录加入 `PATH`。标准 vcpkg 静态配置不需要额外 DLL。

调试版本将三个命令的 preset 改为 `x64-debug`。输出分别为：

- 发布构建：`out/build/web-x64-release/bin/DesktopTool.exe`
- 调试构建：`out/build/web-x64-debug/bin/DesktopTool.exe`

切换 Qt 套件时需清理旧配置缓存，例如 CMake 3.24 及以上可执行 `cmake --fresh --preset x64-release -DCMAKE_PREFIX_PATH="完整Qt套件路径"`，随后重新构建。头文件、导入库和运行时 DLL 必须来自同一完整套件。

从开发构建启动时，默认数据目录也在该可执行文件旁。要使用原来的数据，请指定其路径：

```bat
out\build\web-x64-release\bin\DesktopTool.exe --data-dir "D:\DesktopTool\data"
```

## 测试

`ctest --preset x64-release` 或 `ctest --preset x64-debug` 执行配置中的测试。测试使用独立临时目录，不读取真实便签与快捷方式数据。

- `catalog-services`：旧快捷方式导入、稳定标识、分类与排序、重复校验、损坏文件保护、设置兼容和下载目录校验。
- `note-service`：便签迁移、图片处理、内容过滤、路径边界、保存失败回滚和排序。
- `clipboard-service`：类型过滤、系统剪贴板事件、文本和图像恢复、文件引用保留、重启恢复、容量清理及写入失败保护。
- `download-service`：使用本地 HTTP 测试服务验证下载、暂停续传、异常响应、目录回退及历史恢复。
- `torrent-service`：使用本机做种端验证磁力元数据、确认前不下载内容、文件子集、暂停取消及历史恢复。
- `web-integration`：在真实 WebEngine 和 WebChannel 中验证页面与 Qt 后端调用，以及下载表单、路径回退、多任务卡片、暂停/继续/取消、磁力文件勾选弹窗和逐字节文件比对；测试关闭托盘和全局热键，使用离屏模式及本机 HTTP/BitTorrent 服务。

测试结果以当前构建运行时的 CTest 输出为准。真实系统的托盘、热键占用和最终发布目录运行仍需在目标 Windows 环境检查。

## 本机更新与便携发布

完成发布构建并退出正在运行的应用后执行：

```bat
pack\copy_lib.bat
```

该脚本使用 `cmake --install out/build/web-x64-release --prefix out/bin` 更新原有本机程序目录，保留原来 `out/bin/data`。之后从 `out/bin/DesktopTool.exe` 启动即可继续使用原数据。

生成不含用户数据的便携 ZIP：

```bat
pack\pack_app.bat
```

脚本调用 Windows 自带的 PowerShell 5.1，每次将当前 Release 构建通过 CMake 安装到全新的临时目录，再生成压缩包。默认输出为 `out/packages/DesktopTool-版本号-windows-x64-时间戳.zip`，同时生成同名 `.zip.sha256` 校验文件。解压后进入 `DesktopTool-版本号-windows-x64` 文件夹运行 `DesktopTool.exe`。

压缩包包含主程序、Qt 运行库和插件、WebEngine 子进程及资源、语言包、配置和第三方许可证；HTML/CSS/JavaScript 与应用图标已经编译进程序。Qt 部署流程附带 `vc_redist.x64.exe` 时一并保留，目标电脑缺少 MSVC 运行库时可先安装它。使用动态 OpenSSL 的构建仍须配置 `DESKTOPTOOL_EXTRA_RUNTIME_FILES`，确保两个 DLL 随安装部署。

打包不会读取或复制 `out/bin`、`pack/app` 或任意用户下载目录，因此不会带入便签、快捷方式、设置、下载记录、磁力缓存及已下载文件，也不会修改本地用户数据。脚本检查关键发布资源是否齐全，拒绝部署树中的用户数据目录、日志、调试文件和链接；失败返回非零退出码，已有 ZIP 保持不变。正常完成后自动清理本次临时目录。

可以指定构建目录和输出目录，路径含空格时加双引号：

```bat
pack\pack_app.bat -BuildDirectory "D:\DesktopTool\out\build\web-x64-release" -OutputDirectory "D:\Release Packages"
```

也可直接运行 `pack/package_release.ps1`，支持相同参数。打包脚本优先使用 `-CMakePath` 或 `DESKTOPTOOL_CMAKE` 指定的程序，未指定时使用构建缓存记录的 CMake，再回退到 `PATH`。本机更新脚本 `copy_lib.bat` 使用 `DESKTOPTOOL_CMAKE` 或 `PATH`。例如：

```bat
set "DESKTOPTOOL_CMAKE=C:\Program Files\CMake\bin\cmake.exe"
pack\pack_app.bat
```

磁力引擎及其依赖的完整许可证位于 `third_party/licenses`，会随包放入 `licenses` 目录。打包产物位于 Git 已忽略的 `out` 目录，不提交到源代码仓库。

## 数据与迁移

默认使用 **可执行文件旁的 `data` 目录**；通过 `--data-dir` 可指定其他位置。迁移前建议备份该目录。第一次运行新版时，仅在缺少新版索引的情况下导入旧数据：

| 数据 | 新版位置与兼容行为 |
| --- | --- |
| 快捷方式 | `data/shortcut/shortcuts.v2.json`，保存版本号、稳定 ID、分类及顺序；从旧 `shortcuts_config.txt` 导入并保留原文件 |
| 便签索引 | `data/note/notes.v2.json`，导入旧索引和便签目录；旧便签首次编辑时保存至按稳定 ID 命名的新目录，保留原目录 |
| 便签内容 | 保存为 HTML；合法图片由服务处理。删除便签仅移除索引记录，磁盘内容保留，可用于人工恢复 |
| 设置 | 继续使用 `data/setting/system_config.json`，兼容 `tree_view_font_size`、`hotkey_modifier`、`hotkey_key`，新增 `download_directory`；旧文件缺少该字段时使用系统下载目录，保留其他未知字段 |
| 下载历史 | `data/download-tasks.v1.json`，保存来源、目录、状态和续传标识；部分文件位于任务下载目录，取消时仅清理该任务自己的部分文件 |
| 磁力历史 | `data/magnet-tasks.v1.json`，独立保存元数据及文件选择；尚未确认的任务重启后仍等待选择，不自动下载内容 |

旧逗号分隔快捷方式存在无法无歧义识别的行时，会返回中文导入警告，不猜测字段，也不会更改原文件。新版 JSON 或索引损坏时，相关服务会报告错误并禁止覆盖；修复或从备份恢复文件后重启应用。删除过的便签不会在下次启动时自动重新导入。

## 扩展页面与接口

1. 在 `src/services/` 实现业务规则及存储，新增类型、函数和成员写中文注释；避免依赖具体网页控件。
2. 在 `AppBridge` 添加 `Q_INVOKABLE` 方法，返回 `ServiceResult::success(data)` 或 `ServiceResult::failure(中文错误)`。只传递可序列化的业务字段及稳定 ID。
3. 在 `web/` 通过 `await desktopBridge.call("方法名", 参数)` 调用。桥接封装会把失败结果转换为异常；服务成功保存后发出变更信号，页面通过 `desktopBridge.on("信号名", 回调)` 刷新数据。
4. 新增网页文件时更新 `resources/app.qrc`，新增 C++ 文件时更新 `CMakeLists.txt`。补充涉及数据迁移、保存或系统交互的回归验证。

不要把任意文件路径执行、命令执行或任意外部页面访问直接暴露成网页接口。数据读写继续放在 Qt 服务中，页面资源继续随应用打包。
