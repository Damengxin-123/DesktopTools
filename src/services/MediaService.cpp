#include "MediaService.h"
#include "ServiceResult.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QImageReader>
#include <QProcess>
#include <QWidget>
#include <QDesktopServices>
#include <QUrl>
#include <algorithm>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#endif

namespace
{
// 两类浏览器的 ProgId 与描述；命令模板在 writeProgId 中生成。
const QString kImageProgId = QStringLiteral("DesktopTool.ImageBrowser");
const QString kVideoProgId = QStringLiteral("DesktopTool.VideoPlayer");

// Windows 的“打开方式”也可能以 Applications\可执行文件名记录用户选择。
bool isToolHandler(const QString& progId, bool video)
{
    return progId.compare(video ? kVideoProgId : kImageProgId, Qt::CaseInsensitive) == 0
        || progId.compare(QStringLiteral("Applications\\")
            + QFileInfo(MediaService::toolExecutablePath(video)).fileName(), Qt::CaseInsensitive) == 0;
}

// 支持的扩展名保持与网页说明一致；.ts 等易与程序源码冲突的类型不参与关联。
QStringList imageSuffixes()
{
    return {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("gif"), QStringLiteral("webp"),
        QStringLiteral("ico"), QStringLiteral("tif"), QStringLiteral("tiff"),
        QStringLiteral("svg")};
}

QStringList videoSuffixes()
{
    return {QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("webm"),
        QStringLiteral("avi"), QStringLiteral("mov"), QStringLiteral("m4v"),
        QStringLiteral("wmv"), QStringLiteral("mpg"), QStringLiteral("mpeg"),
        QStringLiteral("flv"), QStringLiteral("3gp")};
}

bool matchesSuffix(const QString& path, const QStringList& suffixes)
{
    return suffixes.contains(QFileInfo(path).suffix().toLower(), Qt::CaseInsensitive);
}

// 目录清单最多返回的条目数，避免超大连环图库卡住界面。
constexpr int MaximumListedFiles = 500;
}

MediaService::MediaService(bool nativeIntegration, QObject* parent)
    : QObject(parent)
{
#ifdef Q_OS_WIN
    if (nativeIntegration) {
        m_classes = new QSettings(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes"),
            QSettings::NativeFormat, this);
        m_fileExts = new QSettings(QStringLiteral(
            "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts"),
            QSettings::NativeFormat, this);
        m_nativeRegistry = true;
    }
#else
    Q_UNUSED(nativeIntegration);
#endif
}

MediaService::MediaService(QObject* parent, QSettings* classesStorage, QSettings* fileExtsStorage)
    : QObject(parent)
    , m_classes(classesStorage)
    , m_fileExts(fileExtsStorage)
{
}

bool MediaService::isImageFile(const QString& path) { return matchesSuffix(path, imageSuffixes()); }
bool MediaService::isVideoFile(const QString& path) { return matchesSuffix(path, videoSuffixes()); }
QStringList MediaService::imageExtensions() { return imageSuffixes(); }
QStringList MediaService::videoExtensions() { return videoSuffixes(); }

QStringList MediaService::filesWithExtensions(const QString& directory, const QStringList& suffixes)
{
    QStringList filters;
    for (const QString& suffix : suffixes)
        filters.append(QStringLiteral("*.") + suffix);
    QStringList paths;
    const QFileInfoList entries = QDir(directory)
        .entryInfoList(filters, QDir::Files, QDir::Name | QDir::LocaleAware);
    paths.reserve(entries.size());
    for (const QFileInfo& entry : entries)
        paths.append(entry.absoluteFilePath());
    return paths;
}

QString MediaService::applicationRootPath()
{
    QDir directory(QCoreApplication::applicationDirPath());
    const QString name = QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName();
    if (name.compare(QStringLiteral("DesktopToolImageBrowser"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("DesktopToolVideoPlayer"), Qt::CaseInsensitive) == 0)
        directory.cdUp();
    return directory.absolutePath();
}

QString MediaService::toolExecutablePath(bool video)
{
    QString name = video ? QStringLiteral("DesktopToolVideoPlayer") : QStringLiteral("DesktopToolImageBrowser");
#ifdef Q_OS_WIN
    name += QStringLiteral(".exe");
#endif
    return QDir(applicationRootPath()).filePath(QStringLiteral("tools/") + name);
}

QVariantMap MediaService::launchFile(const QString& path, const QString& dataRoot) const
{
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable())
        return ServiceResult::failure(QStringLiteral("文件不存在或无法读取，可能已被移动或删除。"));
    const bool video = isVideoFile(path);
    if (!video && !isImageFile(path))
        return ServiceResult::failure(QStringLiteral("暂不支持该媒体格式。"));
    if (!video) {
        QImageReader reader(file.absoluteFilePath());
        reader.setDecideFormatFromContent(true);
        reader.setAutoTransform(true);
        if (!reader.canRead())
            return ServiceResult::failure(QStringLiteral("图片无法读取或格式不支持。"));
    }
    const QString executable = toolExecutablePath(video);
    if (!QFileInfo(executable).isFile())
        return ServiceResult::failure(QStringLiteral("独立工具不存在，请重新编译或完整安装程序：%1").arg(executable));
    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--data-dir"), QDir(dataRoot).absolutePath(),
        QStringLiteral("--"), file.absoluteFilePath()});
    process.setWorkingDirectory(QFileInfo(executable).absolutePath());
    qint64 pid = 0;
    if (!process.startDetached(&pid))
        return ServiceResult::failure(QStringLiteral("无法启动独立工具：%1\n%2").arg(executable, process.errorString()));
    return ServiceResult::success(QVariantMap{{QStringLiteral("executable"), executable},
        {QStringLiteral("pid"), pid}});
}

QVariantMap MediaService::listDirectory(const QString& directory) const
{
    const QDir dir(directory);
    if (!dir.exists() || !dir.isReadable())
        return ServiceResult::failure(QStringLiteral("目录不存在或无法读取，请重新选择。"));
    QStringList filters;
    for (const QString& suffix : imageSuffixes() + videoSuffixes())
        filters.append(QStringLiteral("*.") + suffix);
    QVariantList items;
    int imageCount = 0;
    int videoCount = 0;
    bool truncated = false;
    const QFileInfoList entries = dir.entryInfoList(filters, QDir::Files, QDir::Name | QDir::LocaleAware);
    for (const QFileInfo& entry : entries) {
        if (items.size() >= MaximumListedFiles) {
            truncated = true;
            break;
        }
        const bool image = matchesSuffix(entry.absoluteFilePath(), imageSuffixes());
        QVariantMap item;
        item.insert(QStringLiteral("name"), entry.fileName());
        item.insert(QStringLiteral("path"), entry.absoluteFilePath());
        item.insert(QStringLiteral("kind"), image ? QStringLiteral("image") : QStringLiteral("video"));
        item.insert(QStringLiteral("bytes"), entry.size());
        items.append(item);
        if (image)
            ++imageCount;
        else
            ++videoCount;
    }
    return ServiceResult::success(QVariantMap{
        {QStringLiteral("directory"), dir.absolutePath()},
        {QStringLiteral("items"), items},
        {QStringLiteral("imageCount"), imageCount},
        {QStringLiteral("videoCount"), videoCount},
        {QStringLiteral("truncated"), truncated}});
}

QString MediaService::effectiveProgId(const QString& extension) const
{
#ifdef Q_OS_WIN
    // 使用系统解析器读取真正生效的关联，涵盖机器级注册与受保护的用户选择。
    if (m_nativeRegistry) {
        wchar_t buffer[1024] = {};
        DWORD length = 1024;
        if (SUCCEEDED(AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_PROGID,
                reinterpret_cast<PCWSTR>(extension.utf16()), nullptr, buffer, &length)))
            return QString::fromWCharArray(buffer);
        return {};
    }
#endif
    const QString choice = m_fileExts
        ? m_fileExts->value(extension + QStringLiteral("/UserChoice/ProgId")).toString() : QString();
    if (!choice.isEmpty())
        return choice;
    return m_classes ? m_classes->value(extension).toString() : QString();
}

// 尽力把 ProgId 解析为可读名称；解析失败时回退到原始 ProgId。
QString MediaService::handlerDisplayName(const QString& progId) const
{
    if (isToolHandler(progId, false))
        return QStringLiteral("DesktopTool 图像浏览器");
    if (isToolHandler(progId, true))
        return QStringLiteral("DesktopTool 视频浏览器");
    if (progId.isEmpty())
        return QStringLiteral("其他程序");
    QString raw;
    if (m_classes) {
        raw = m_classes->value(progId + QStringLiteral("/Default")).toString();
        if (raw.isEmpty()) raw = m_classes->value(progId).toString();
        if (raw.isEmpty())
            raw = m_classes->value(progId + QStringLiteral("/FriendlyTypeName")).toString();
    }
#ifdef Q_OS_WIN
    if (raw.isEmpty()) {
        QSettings systemClasses(QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Classes"),
            QSettings::NativeFormat);
        raw = systemClasses.value(progId + QStringLiteral("/Default")).toString();
        if (raw.isEmpty())
            raw = systemClasses.value(progId + QStringLiteral("/FriendlyTypeName")).toString();
    }
    // “@文件,-资源”形式的间接字符串用系统 API 解析为真实名称。
    if (raw.startsWith(QLatin1Char('@'))) {
        wchar_t buffer[256] = {};
        if (SUCCEEDED(SHLoadIndirectString(reinterpret_cast<const wchar_t*>(raw.utf16()),
                buffer, 256, nullptr)) && buffer[0] != L'\0') {
            raw = QString::fromWCharArray(buffer);
        } else {
            raw.clear();
        }
    }
#endif
    if (raw.isEmpty()) {
        // AppX/UWP 处理程序没有可读的类名，用统一说明代替长串标识符。
        if (progId.startsWith(QStringLiteral("AppX")))
            return QStringLiteral("Windows 内置或商店应用");
        return progId;
    }
    return raw;
}

QVariantMap MediaService::extensionHandler(const QString& extension) const
{
    if (m_classes)
        m_classes->sync();
    if (m_fileExts)
        m_fileExts->sync();
    const QString progId = effectiveProgId(extension);
    const bool ours = isToolHandler(progId, false) || isToolHandler(progId, true);
    return QVariantMap{
        {QStringLiteral("progId"), progId},
        {QStringLiteral("name"), handlerDisplayName(progId)},
        {QStringLiteral("ours"), ours}};
}

bool MediaService::kindIsDefault(const QStringList& extensions, const QString& progId) const
{
    if (!m_classes || !m_fileExts || extensions.isEmpty())
        return false;
    m_classes->sync();
    m_fileExts->sync();
    for (const QString& suffix : extensions) {
        if (!isToolHandler(effectiveProgId(QChar::fromLatin1('.') + suffix), progId == kVideoProgId))
            return false;
    }
    return true;
}

QVariantMap MediaService::associationStatus() const
{
    const bool supported = m_classes && m_fileExts;
    QVariantMap handlers;
    for (const QString& suffix : imageSuffixes() + videoSuffixes())
        handlers.insert(suffix, extensionHandler(QStringLiteral(".") + suffix));
    return ServiceResult::success(QVariantMap{
        {QStringLiteral("supported"), supported},
        {QStringLiteral("handlers"), handlers},
        {QStringLiteral("imageDefault"), kindIsDefault(imageSuffixes(), kImageProgId)},
        {QStringLiteral("videoDefault"), kindIsDefault(videoSuffixes(), kVideoProgId)},
        {QStringLiteral("imageExtensions"), imageSuffixes()},
        {QStringLiteral("videoExtensions"), videoSuffixes()}});
}

void MediaService::writeProgId(QSettings& storage, const QString& progId, const QString& description) const
{
    const QString executable = QDir::toNativeSeparators(toolExecutablePath(progId == kVideoProgId));
    // QSettings 必须使用 /Default 才能写入注册表子键的默认值。
    storage.setValue(progId + QStringLiteral("/Default"), description);
    storage.setValue(progId + QStringLiteral("/FriendlyTypeName"), description);
    storage.setValue(progId + QStringLiteral("/DefaultIcon/Default"), QStringLiteral("\"%1\",0").arg(executable));
    storage.setValue(progId + QStringLiteral("/shell/open/command/Default"),
        QStringLiteral("\"%1\" \"%2\"").arg(executable, QStringLiteral("%1")));
}

// OpenWithProgids 的真实布局：每个 ProgId 是值名、数据为空的 REG_NONE 值，
// QSettings 无法表达该类型，Windows 上直接使用注册表 API。
#ifdef Q_OS_WIN
namespace
{
QString openWithKeyPath(const QString& extension)
{
    return QStringLiteral("Software\\Classes\\") + extension + QStringLiteral("\\OpenWithProgids");
}
}

bool MediaService::openWithProgIdContains(const QString& extension, const QString& progId) const
{
    if (!m_nativeRegistry)
        return false;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(
            openWithKeyPath(extension).utf16()), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    const LSTATUS result = RegQueryValueExW(key,
        reinterpret_cast<const wchar_t*>(progId.utf16()), nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

// 绕过 QSettings 直接探测 UserChoice 键，用于区分“删除被拒绝”与“删除后被恢复”。
bool MediaService::userChoiceExists(const QString& extension) const
{
    if (!m_nativeRegistry)
        return false;
    const QString path = QStringLiteral(
        "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\")
        + extension + QStringLiteral("\\UserChoice");
    HKEY key = nullptr;
    const LSTATUS result = RegOpenKeyExW(HKEY_CURRENT_USER,
        reinterpret_cast<const wchar_t*>(path.utf16()), 0, KEY_QUERY_VALUE, &key);
    if (result == ERROR_SUCCESS)
        RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void MediaService::addOpenWithProgId(const QString& extension, const QString& progId)
{
    if (!m_nativeRegistry || openWithProgIdContains(extension, progId))
        return;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(
            openWithKeyPath(extension).utf16()), 0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, reinterpret_cast<const wchar_t*>(progId.utf16()),
        0, REG_NONE, nullptr, 0);
    RegCloseKey(key);
}

void MediaService::removeOpenWithProgId(const QString& extension, const QString& progId)
{
    if (!m_nativeRegistry)
        return;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, reinterpret_cast<const wchar_t*>(
            openWithKeyPath(extension).utf16()), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(key, reinterpret_cast<const wchar_t*>(progId.utf16()));
    RegCloseKey(key);
}
#else
bool MediaService::openWithProgIdContains(const QString&, const QString&) const { return false; }
void MediaService::addOpenWithProgId(const QString&, const QString&) {}
void MediaService::removeOpenWithProgId(const QString&, const QString&) {}
bool MediaService::userChoiceExists(const QString&) const { return false; }
#endif

void MediaService::writeApplication(bool video)
{
    const QString executable = toolExecutablePath(video);
    const QString base = QStringLiteral("Applications/%1/").arg(QFileInfo(executable).fileName());
    m_classes->setValue(base + QStringLiteral("shell/open/command/Default"),
        QStringLiteral("\"%1\" \"%2\"").arg(QDir::toNativeSeparators(executable), QStringLiteral("%1")));
    m_classes->setValue(base + QStringLiteral("FriendlyAppName"), video
        ? QStringLiteral("DesktopTool 视频浏览器") : QStringLiteral("DesktopTool 图像浏览器"));
    m_classes->setValue(base + QStringLiteral("DefaultIcon/Default"),
        QStringLiteral("\"%1\",0").arg(QDir::toNativeSeparators(executable)));
    const QStringList extensions = video ? videoSuffixes() : imageSuffixes();
    for (const QString& suffix : extensions)
        m_classes->setValue(base + QStringLiteral("SupportedTypes/.") + suffix, QString());
}

// 保留 ProgId，使旧版已生效的默认关联自动转向独立工具。
void MediaService::registerBrowserProgIds()
{
    if (!m_classes)
        return;
    writeProgId(*m_classes, kImageProgId, QStringLiteral("DesktopTool 图像浏览器"));
    writeProgId(*m_classes, kVideoProgId, QStringLiteral("DesktopTool 视频浏览器"));
    writeApplication(false);
    writeApplication(true);
    // 隐藏旧主程序的“打开方式”候选入口，媒体使用上面登记的两个独立工具。
    m_classes->setValue(QStringLiteral("Applications/DesktopTool.exe/NoOpenWith"), QString());
    for (const QString& suffix : imageSuffixes())
        addOpenWithProgId(QChar::fromLatin1('.') + suffix, kImageProgId);
    for (const QString& suffix : videoSuffixes())
        addOpenWithProgId(QChar::fromLatin1('.') + suffix, kVideoProgId);
    m_classes->sync();
#ifdef Q_OS_WIN
    if (m_nativeRegistry)
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
}

QVariantMap MediaService::setKindDefault(const QStringList& extensions, const QString& progId,
    const QString& description, bool enabled)
{
    Q_UNUSED(extensions)
    Q_UNUSED(progId)
    Q_UNUSED(description)
    Q_UNUSED(enabled)
    // 保留旧桥接接口但禁止直接篡改 UserChoice；默认应用必须由用户在系统设置确认。
    return ServiceResult::failure(QStringLiteral("请勾选文件类型并打开系统默认应用设置完成确认。"));
}

// 仅登记用户勾选的类型；不写默认值、不删除其他程序的用户选择。
QVariantMap MediaService::prepareDefaultApplications(const QStringList& extensions)
{
    if (!m_classes || !m_fileExts)
        return ServiceResult::failure(QStringLiteral("当前环境不支持修改系统默认打开方式。"));
    QStringList selected;
    for (QString suffix : extensions) {
        suffix = suffix.trimmed().toLower();
        if (suffix.startsWith(QLatin1Char('.'))) suffix.remove(0, 1);
        if (!imageSuffixes().contains(suffix) && !videoSuffixes().contains(suffix))
            return ServiceResult::failure(QStringLiteral("不支持的文件类型：") + suffix);
        if (!selected.contains(suffix)) selected.append(suffix);
    }
    if (selected.isEmpty())
        return ServiceResult::failure(QStringLiteral("请至少勾选一种文件类型。"));
    // 原生登记前检查实际文件；测试替身存储不要求部署工具。
    for (const QString& suffix : selected) {
        if (m_nativeRegistry && !QFileInfo(toolExecutablePath(videoSuffixes().contains(suffix))).isFile())
            return ServiceResult::failure(QStringLiteral("独立工具不存在，请先完整安装程序。"));
    }
    registerBrowserProgIds();
    m_classes->remove(QStringLiteral("DesktopTool/Capabilities"));
    for (const bool video : {false, true}) {
        const QString progId = video ? kVideoProgId : kImageProgId;
        const QString base = progId + QStringLiteral("/Capabilities/");
        m_classes->setValue(base + QStringLiteral("ApplicationName"), video
            ? QStringLiteral("DesktopTool 视频浏览器") : QStringLiteral("DesktopTool 图像浏览器"));
        m_classes->setValue(base + QStringLiteral("ApplicationDescription"), video
            ? QStringLiteral("独立视频播放器") : QStringLiteral("独立图像浏览器"));
        m_classes->setValue(base + QStringLiteral("ApplicationIcon"),
            QStringLiteral("\"%1\",0").arg(QDir::toNativeSeparators(toolExecutablePath(video))));
        m_classes->remove(base + QStringLiteral("FileAssociations"));
        for (const QString& suffix : selected) {
            if (videoSuffixes().contains(suffix) == video)
                m_classes->setValue(base + QStringLiteral("FileAssociations/.") + suffix, progId);
        }
    }
    m_classes->sync();
    if (m_classes->status() != QSettings::NoError)
        return ServiceResult::failure(QStringLiteral("无法登记文件类型，请检查当前用户注册表权限。"));
#ifdef Q_OS_WIN
    if (m_nativeRegistry) {
        QSettings applications(QStringLiteral("HKEY_CURRENT_USER\\Software\\RegisteredApplications"), QSettings::NativeFormat);
        applications.remove(QStringLiteral("DesktopTool"));
        applications.setValue(QStringLiteral("DesktopToolImageBrowser"),
            QStringLiteral("Software\\Classes\\DesktopTool.ImageBrowser\\Capabilities"));
        applications.setValue(QStringLiteral("DesktopToolVideoPlayer"),
            QStringLiteral("Software\\Classes\\DesktopTool.VideoPlayer\\Capabilities"));
        applications.sync();
        if (applications.status() != QSettings::NoError)
            return ServiceResult::failure(QStringLiteral("无法登记系统默认应用入口。"));
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
#endif
    return ServiceResult::success(QVariantMap{{QStringLiteral("selected"), selected}});
}

// 系统设置异步打开；成功只代表设置页已启动，不代表用户已经确认关联。
QVariantMap MediaService::openDefaultApplications(const QStringList& extensions)
{
    const auto result = prepareDefaultApplications(extensions);
    if (!result.value(QStringLiteral("ok")).toBool()) return result;
    if (!m_nativeRegistry)
        return ServiceResult::failure(QStringLiteral("当前环境不支持打开系统默认应用设置。"));
    const QStringList selected = result.value(QStringLiteral("data")).toMap()
        .value(QStringLiteral("selected")).toStringList();
    const bool images = std::any_of(selected.begin(), selected.end(), [](const QString& suffix) {
        return imageSuffixes().contains(suffix);
    });
    const bool videos = std::any_of(selected.begin(), selected.end(), [](const QString& suffix) {
        return videoSuffixes().contains(suffix);
    });
    QString url = QStringLiteral("ms-settings:defaultapps");
    if (images != videos)
        url += QStringLiteral("?registeredAppUser=") + (videos
            ? QStringLiteral("DesktopToolVideoPlayer") : QStringLiteral("DesktopToolImageBrowser"));
    if (!QDesktopServices::openUrl(QUrl(url)))
        return ServiceResult::failure(QStringLiteral("无法打开 Windows 默认应用设置，请从系统设置中打开“应用 → 默认应用”。"));
    return ServiceResult::success(QVariantMap{{QStringLiteral("pending"), true}});
}

QVariantMap MediaService::setImageBrowserDefault(bool enabled)
{
    return setKindDefault(imageSuffixes(), kImageProgId,
        QStringLiteral("DesktopTool 图像浏览器"), enabled);
}

QVariantMap MediaService::setVideoPlayerDefault(bool enabled)
{
    return setKindDefault(videoSuffixes(), kVideoProgId,
        QStringLiteral("DesktopTool 视频浏览器"), enabled);
}

// 兼容旧入口，统一使用系统默认应用设置。
bool MediaService::openSystemOpenWithDialog(const QString& extension, QWidget* parent, QString* error)
{
    Q_UNUSED(parent)
    const auto result = openDefaultApplications({extension});
    if (!result.value(QStringLiteral("ok")).toBool() && error)
        *error = result.value(QStringLiteral("error")).toString();
    return result.value(QStringLiteral("ok")).toBool();
}
