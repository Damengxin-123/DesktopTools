#pragma once

#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QMap>
#include <functional>

class QTimer;
class WallpaperWindow;

// 管理动态壁纸历史记录与桌面挂载；仅保存原文件引用，不复制文件实体。
class WallpaperService final : public QObject
{
    Q_OBJECT
public:
    // 从数据目录加载历史；测试可关闭原生集成，避免创建壁纸窗口。
    explicit WallpaperService(const QString& dataRoot, bool nativeIntegration = true, QObject* parent = nullptr,
        std::function<QVariantList()> screenProvider = {});
    ~WallpaperService() override;
    // 返回开关状态、当前使用标识和历史列表，附带文件是否存在的标志。
    QVariantMap snapshot() const;
    // 添加本地媒体文件并设为当前壁纸；同一文件不重复记录。
    QVariantMap add(const QVariantMap& item);
    // 切换到历史中的指定记录，原文件丢失时返回明确错误。
    QVariantMap use(const QString& id, const QString& screenId = {});
    // 清除指定显示器的壁纸，恢复该屏系统桌面，其他屏幕不受影响。
    QVariantMap clearScreen(const QString& screenId);
    // 保存指定屏幕的图片显示方式，关闭壁纸时也可预先设置。
    QVariantMap setDisplayMode(const QString& screenId, const QString& mode);
    // 批量删除历史记录，不删除原文件；删除当前项时停止显示。
    QVariantMap remove(const QStringList& ids);
    // 开启或关闭动态壁纸；开启时恢复上次的资源。
    QVariantMap setEnabled(bool enabled);
    // 打开原文件所在目录并选中该文件，原文件丢失时返回中文错误。
    QVariantMap openDirectory(const QString& id);
signals:
    // 开关或历史成功写入后通知界面刷新。
    void changed();
private:
    // 一条壁纸记录的持久化业务字段。
    struct Item
    {
        QString id; // 不随修改变化的稳定标识。
        QString name; // 显示名称，取原文件名。
        QString path; // 原文件的绝对路径，仅作引用。
        QString type; // 媒体类型：image 或 video。
        bool animated = false; // 图片是否为多帧动图。
        QString addedAt; // 添加时间。
        QString lastUsedAt; // 最近一次设为当前的时间。
        QString thumbnail; // 静图缩略图数据地址，视频留空。
        int width = 0; // 图片经过方向校正后的原始像素宽度。
        int height = 0; // 图片经过方向校正后的原始像素高度。
    };
    // 校验并加载历史，损坏时停止写入以保护原数据。
    void load();
    // 校验记录结构与开关一致性。
    QString validate(const QVector<Item>& items, bool enabled, const QString& activeId) const;
    // 通过临时文件原子保存，成功后才替换内存数据。
    QVariantMap commit(const QVector<Item>& items, bool enabled, const QMap<QString, QString>& assignments,
        const QMap<QString, QString>& modes);
    // 将当前业务数据转换为网页可接收的快照。
    QVariantMap data() const;
    // 将单条记录转换为可序列化字段，附带文件存在标志。
    QVariantMap itemMap(const Item& item) const;
    // 返回记录位置，不存在时返回负数。
    qsizetype position(const QString& id) const;
    // 判断文件是视频还是图片，并读取图片的动图标志与尺寸。
    static QString classify(const QString& path, bool* animated);
    // 生成受限尺寸的静图缩略图数据地址。
    static QString buildThumbnail(const QString& path);
    // 创建并挂载壁纸窗口，随后应用当前资源。
    bool startEngine();
    // 立即销毁全部壁纸窗口；保留屏幕拓扑监测。
    void stopEngine();
    // 桌面重建导致窗口失活时重建挂载。
    void ensureEngineAlive();
    // 更新显示器拓扑；拔屏即回收窗口，重连保留原屏设置。
    void refreshScreens();
    // 返回主屏标识，兼容旧版未传目标屏幕的调用。
    QString primaryScreenId() const;
    // 检查屏幕是否仍连接，拒绝把断开屏幕的操作误应用到主屏。
    bool hasScreen(const QString& id) const;

    QString m_path; // 历史索引的绝对路径。
    QString m_loadError; // 加载错误，存在时拒绝覆盖。
    QVector<Item> m_items; // 最近添加排在前面的记录。
    bool m_enabled = false; // 动态壁纸功能开关。
    QMap<QString, QString> m_assignments; // 每个稳定屏幕标识对应的壁纸记录，含暂时断开的屏幕。
    QMap<QString, QString> m_displayModes; // 每屏图片显示方式，未设置时使用填充。
    QVariantList m_screens; // 当前显示器拓扑的完整物理像素信息。
    std::function<QVariantList()> m_screenProvider; // 默认原生枚举，测试可注入多屏和热插拔场景。
    bool m_nativeIntegration; // 测试模式下不创建原生窗口。
    QMap<QString, WallpaperWindow*> m_engines; // 每个已连接且配置了资源的屏幕独立持有窗口。
    QString m_engineError; // 最近一次挂载失败信息，避免界面误报正在显示。
    QMap<QString, QString> m_failedItems; // 每屏播放失败的记录，守护检查不反复创建黑屏窗口。
    QMap<QString, QString> m_playbackErrors; // 每屏的解码失败原因，切换资源或主动重试时清除。
    QTimer* m_watchdog = nullptr; // 定期检查挂载状态的守护定时器。
};
