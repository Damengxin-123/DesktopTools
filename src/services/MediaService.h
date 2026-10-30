#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class QSettings;
class QWidget;

// 资源浏览器的媒体支撑服务：图片/视频分类、目录清单，以及 Windows 下
// “设置为默认打开方式”的注册表读写。注册表存储可注入，便于离线测试。
class MediaService final : public QObject
{
    Q_OBJECT
public:
    // 原生模式：直接读写当前用户的注册表；不支持的平台保持空存储。
    explicit MediaService(bool nativeIntegration, QObject* parent = nullptr);
    // 测试注入存储；两个存储由调用方持有并保证生命周期覆盖本服务。
    MediaService(QObject* parent, QSettings* classesStorage, QSettings* fileExtsStorage);

    // 按扩展名判断文件类型（大小写不敏感）。
    static bool isImageFile(const QString& path);
    static bool isVideoFile(const QString& path);
    // 两类浏览器支持的扩展名（小写、不含点）。
    static QStringList imageExtensions();
    static QStringList videoExtensions();
    // 返回目录中匹配扩展名的文件绝对路径列表（自然排序）。
    static QStringList filesWithExtensions(const QString& directory, const QStringList& suffixes);

    // 返回主程序所在根目录；从独立工具调用时返回 tools 的上级目录。
    static QString applicationRootPath();
    // 返回对应独立工具的绝对路径，不依赖当前工作目录或系统 PATH。
    static QString toolExecutablePath(bool video);
    // 检查媒体文件并启动独立进程；返回工具路径和进程号，供调用方反馈启动状态。
    QVariantMap launchFile(const QString& path, const QString& dataRoot) const;

    // 列出一个目录中的图片与视频文件（非递归，最多 500 条）。
    QVariantMap listDirectory(const QString& directory) const;
    // 返回两类浏览器是否已是系统默认打开方式，以及支持的扩展名。
    QVariantMap associationStatus() const;
    // 校验并登记所选文件类型，保持系统现有默认关联不变。
    QVariantMap prepareDefaultApplications(const QStringList& extensions);
    // 打开系统默认应用设置，由用户完成选择后再核验状态。
    QVariantMap openDefaultApplications(const QStringList& extensions);
    // 兼容旧接口，返回引导提示，不直接改写默认关联。
    QVariantMap setImageBrowserDefault(bool enabled);
    // 兼容旧接口，返回引导提示，不直接改写默认关联。
    QVariantMap setVideoPlayerDefault(bool enabled);
    // 返回扩展名当前生效的打开方式；extension 需带点（如 ".png"）。
    // 字段：progId、name（尽力解析的显示名）、ours（是否本程序）。
    QVariantMap extensionHandler(const QString& extension) const;
    // 幂等登记两类浏览器的 ProgId、应用入口与各扩展名的 OpenWithProgids，
    // 使本程序出现在系统的“打开方式”列表中。
    void registerBrowserProgIds();
    // 兼容单格式入口，打开 Windows 默认应用设置。
    bool openSystemOpenWithDialog(const QString& extension, QWidget* parent, QString* error);

private:
    // 拦截旧版直接修改默认关联的调用，提示使用系统设置。
    QVariantMap setKindDefault(const QStringList& extensions, const QString& progId,
        const QString& description, bool enabled);
    // 判断扩展名集合当前的有效打开方式是否全部为本程序的 ProgId。
    bool kindIsDefault(const QStringList& extensions, const QString& progId) const;
    // 登记独立 exe 的打开方式入口及支持格式。
    void writeApplication(bool video);
    // 登记一个 ProgId（名称、图标与独立工具打开命令）。
    void writeProgId(QSettings& storage, const QString& progId, const QString& description) const;
    // 读取扩展名当前有效的 ProgId：优先 Explorer 的 UserChoice，其次用户类默认值。
    QString effectiveProgId(const QString& extension) const;
    // 尽力把 ProgId 解析为可读名称（类默认值或 FriendlyTypeName）。
    QString handlerDisplayName(const QString& progId) const;
    // OpenWithProgids 采用“每个 ProgId 一个空值名”的布局（REG_NONE），
    // QSettings 无法表达该类型，Windows 上直接使用注册表 API。
    bool openWithProgIdContains(const QString& extension, const QString& progId) const;
    void addOpenWithProgId(const QString& extension, const QString& progId);
    void removeOpenWithProgId(const QString& extension, const QString& progId);
    // 绕过 QSettings 直接探测 Explorer 的 UserChoice 键是否存在（实时性）。
    bool userChoiceExists(const QString& extension) const;

    // HKCU\Software\Classes 与 Explorer FileExts；测试时为注入的替身存储。
    // 原生集成关闭或平台不支持时保持空指针，关联操作返回明确错误。
    QSettings* m_classes = nullptr;
    QSettings* m_fileExts = nullptr;
    // 关联变更后是否广播系统刷新、是否可写 OpenWithProgids（仅真实注册表）。
    bool m_nativeRegistry = false;
};
