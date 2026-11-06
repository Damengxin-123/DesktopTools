# DesktopTool · Godot 工作台

这是独立的 Windows Godot 4 工程。原有的左侧入口顺序和页面操作区布局保留，新增深色主题、蓝紫选中状态、线框图标、真实数量徽章，功能内容直接显示于工作区。

## 启动

在 PowerShell 7 中运行。

```powershell
& 'I:\PROJECT\Godot\DesktopTool\启动.ps1'
& 'I:\PROJECT\Godot\DesktopTool\启动.ps1' -Editor
```

也可以在 Godot 项目管理器中导入本目录的 `project.godot`，按 F6/F5 运行主场景。当前机器已部署后台运行库。使用 `-Rebuild` 可重新编译和部署，脚本参数允许替换 Godot、Qt、Visual Studio、依赖路径。运行要求 Windows x64 和 Godot 4.6 或更新版本。

## 功能与数据

| 页面 | 主要操作 |
| --- | --- |
| 快捷方式、文档、表情 | 分类、搜索、增改和批量删除，文档支持富文本排版、密码锁及 .DaDocH 导入导出 |
| 下载、剪贴板 | HTTP/HTTPS、磁力文件确认、任务进度，剪贴板类型监听、历史筛选、复制及置顶 |
| 壁纸、二维码 | 多显示器壁纸和显示方式，二维码图片选择、粘贴、离线识别及历史 |
| 截图、录屏 | 框选与历史，截图标注，录屏分辨率、鼠标与声音设置 |
| 资源浏览器、文件检索 | 图片视频入口、目录浏览、文件关联，C++ 文件索引和分页检索 |
| 设置 | 外观、系统、下载和关于四个分页，可切换暗色和金色亮色主题 |

在「设置 → 外观」切换主题会立即生效，并保留到下次启动。默认主题为暗色。

默认使用 Godot 的独立用户数据目录。要载入旧库，先关闭旧版，将旧版 `data` 目录复制到一个独立位置，再用 `启动.ps1 -DataDirectory <副本目录>` 启动。快捷方式、表情、壁纸等记录引用原文件路径。

主窗口、文档列表和工作台对话框由 Godot 原生控件构建。后台是独立的 Windows C++ 进程，沿用旧版索引、HTTP/磁力下载、二维码识别、屏幕采集、视频编码和桌面挂载等实现。截图遮罩、录屏控制条、独立图片和视频窗口沿用现有原生组件。动态壁纸的旧图像播放引擎在 C++ 进程中运行。文档编辑器在独立窗口中使用 Chromium 的 HTML 富文本编辑，原便签分类与 HTML 内容可直接打开。文档支持字体、字号、字色、标题、对齐、编号、项目符号、表格和内嵌图片。Ctrl+S 保存，保存后可通过「导出文档」生成 `.DaDocH` 文件，接收方在文档页点击「导入文档」即可打开。

编辑器采用三栏布局：左侧按有序列表及手动编号段落生成导航，点击定位正文；中间是宽度自适应的连续编辑区域，不分页；右侧提供正文、标题、重点及引用等字体预设，可选择应用到整段或选中文字，并通过「配置样式预设」修改字体、字号、加粗、斜体与字色。预设随每篇文档保存。

`.DaDocH` 是标准 ZIP 容器，使用 STORE 模式（仅打包，不进行额外压缩）。包内包含 `index.html`、`resources/` 图片资源和 `document.json` 元数据及样式预设。同一图片按内容哈希去重。未设置密码时不加密；保存后点击「密码锁」可设置或修改密码，留空解除密码，设置密码时包内所有文件使用 WinZip AES-256 加密。导出保留密码锁，关闭编辑窗口或切换文档后重新锁定，密码不写入索引或配置。加密包需要支持 WinZip AES 的解压工具。

现有 HTML 文档可继续读取，首次保存后转为 `note/<文档标识>/document.DaDocH`，成功提交新包与索引后移除旧的 `index.html`。旧目录中其他文件不会递归删除。尚未打开保存的旧文档不会批量改写。

打包使用随源码附带的 minizip-ng 4.0.10，许可证见 `native/third_party/minizip-ng-4.0.10/LICENSE`。构建通过本工程的 `native/services/NoteService.cpp` 适配文档存储。所需 C++ 后台与资源均已迁入 `native/`，不再依赖旧 Qt 工程目录。

简单验证：部署后台后运行 `python native/tests/document_package_test.py` 检查普通包、加密、图片与旧格式迁移，运行 `python native/tests/document_ui_test.py` 检查真实编辑窗口的布局和保存重开。测试使用临时数据，依赖 Pillow、pyzipper 和 websocket-client。


## 源码构建

仓库根目录是 Godot 工作台。`ui/` 为界面，`native/src/` 为当前功能使用的 C++ 服务、系统交互和媒体工具；旧 Qt 主程序、旧网页主界面、书签浏览器、画板、关卡及地形编辑器不再包含在工程中。

Windows x64 构建需要 Godot 4.6+、Visual Studio C++ 工具链、CMake 3.21+、Ninja、Qt 6.8+（含 WebEngine、WebChannel、Multimedia）、libtorrent 2.1、Boost 和 OpenSSL。`native/vcpkg.json` 提供 libtorrent 依赖清单，第三方库应使用与 Qt 匹配的 MSVC 架构和运行库。也可使用预先安装的依赖前缀。

```powershell
.\启动.ps1 -Rebuild -GodotExe 'C:\Tools\Godot\Godot.exe' `
  -QtRoot 'C:\Qt\6.10.3\msvc2022_64' `
  -CMake 'C:\Tools\CMake\bin\cmake.exe' -Ninja 'C:\Tools\Ninja\ninja.exe' `
  -VsShell 'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Launch-VsDevShell.ps1' `
  -NativeDeps 'C:\Dependencies\native' -BoostConfig 'C:\Dependencies\boost-config' `
  -OpenSsl 'C:\Dependencies\openssl'
```

`NativeDeps` 是包含 libtorrent CMake 包的安装前缀，`BoostConfig` 是含 `BoostConfig.cmake` 的目录，`OpenSsl` 是 OpenSSL 安装前缀。脚本默认工具路径仅适用于开发机，其他机器请显式传入上述路径。构建输出、运行库及用户数据不上传 Git；无需旧 Qt 软件的源码。

使用 `-BuildOnly` 可在构建并部署后台后退出，不启动 Godot 窗口。
