#pragma once

#include <QObject>
#include <QHash>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

class QSaveFile;

// 管理地形项目索引与每个项目的浮点高度数据。
// 每个项目使用独立目录：project.json（元数据）、heightmap.bin（Float32 高度）、
// thumbnail.png（只作展示缓存的灰度晕染图，不作为恢复地形的依据）。
class TerrainService final : public QObject
{
    Q_OBJECT

public:
    // 从指定数据根目录加载项目索引。
    explicit TerrainService(const QString& dataRoot, QObject* parent = nullptr);

    // 返回项目摘要列表。
    QVariantMap snapshot() const;
    // 新建平坦地形项目；尺寸不能整除采样间距时按规则调整为实际尺寸并回传。
    QVariantMap create(const QString& title, double widthMeters, double lengthMeters,
                       double spacing, double initialHeight);
    // 重命名项目。
    QVariantMap rename(const QString& id, const QString& title);
    // 批量删除项目及其数据目录。
    QVariantMap remove(const QStringList& ids);
    // 读取项目元数据（编辑窗口初始信息）。
    QVariantMap readMeta(const QString& id) const;

    // 返回降采样后的预览高度数据（字节为小端 Float32、行主序），供主页面三维缩略预览。
    QVariantMap readPreview(const QString& id, int maxSamples) const;
    // 返回晕染缩略图 PNG 字节；缺失时返回空数据（网页侧回退为占位样式）。
    QVariantMap readThumbnail(const QString& id) const;

    // ---- 高度数据分块流式传输（避免单条消息携带数十 MB 的 Base64） ----
    // 打开读取流：返回令牌、分块大小与项目元数据。
    QVariantMap openHeightStream(const QString& id);
    // 读取一个分块，返回 Base64 文本与实际字节数。
    QVariantMap readHeightChunk(const QString& token, int index);
    // 开始写入流：校验新网格参数后返回令牌与分块大小。
    QVariantMap beginHeightWrite(const QString& id, int gridWidth, int gridDepth,
                                 double spacing, double undoSteps);
    // 写入一个分块（Base64 的 Float32 小端字节）。
    QVariantMap writeHeightChunk(const QString& token, int index, const QString& base64);
    // 提交写入：校验完整性与数值合法后安全替换 heightmap.bin 并更新元数据、重建缩略图。
    QVariantMap commitHeightWrite(const QString& token);
    // 取消未完成的流并释放缓冲。
    QVariantMap cancelStream(const QString& token);

    // ---- 16 位灰度 PNG 高度图导出 ----
    // 开始导出流：记录项目、目标路径、网格与高度映射参数。
    QVariantMap beginExport(const QString& id, const QString& filePath, int pngWidth, int pngHeight,
                            double spacing, double minHeight, double maxHeight);
    // 写入一个导出分块（Base64 的小端 Uint16 行数据）。
    QVariantMap writeExportChunk(const QString& token, int index, const QString& base64);
    // 提交导出：编码 PNG、写入同名 JSON 说明文件，并独立回读校验位深与样本值。
    QVariantMap commitExport(const QString& token);

signals:
    // 项目索引或某个项目的数据已持久化，通知界面刷新列表与预览。
    void changed();

private:
    // 项目索引中的摘要。
    struct Item {
        QString id;        // 项目的稳定标识。
        QString title;     // 显示名称。
        QString updatedAt; // 最近更新时间，ISO 8601。
        int gridWidth = 0;   // 高度场列数（X 方向顶点数）。
        int gridDepth = 0;   // 高度场行数（Z 方向顶点数）。
        double spacing = 0;  // 编辑采样间距（米）。
        double sizeWidth = 0;   // 实际地形宽度（米）= (gridWidth-1)*spacing。
        double sizeDepth = 0;   // 实际地形长度（米）。
        double undoSteps = 30;  // 撤销步数上限，随项目保存。
    };

    // 高度读取/写入流的状态。
    struct HeightStream {
        QString projectId;   // 所属项目。
        QByteArray buffer;   // 已组装（读取）或已接收（写入）的字节。
        qint64 expected = 0; // 期望的总字节数。
        int chunkSize = 0;   // 单块最大字节数。
        int receivedChunks = 0; // 已按序接收的块数（写入流）。
        int gridWidth = 0;   // 写入流声明的网格列数（写入模式）。
        int gridDepth = 0;   // 写入流声明的网格行数（写入模式）。
        double spacing = 0;  // 写入流的采样间距（保存或重采样后的值）。
        double undoSteps = 30; // 写入流的撤销步数设置。
        bool write = false;  // true 为写入流，false 为读取流。
    };

    // 导出流的状态。
    struct ExportStream {
        QString projectId;   // 所属项目。
        QString filePath;    // 目标 PNG 路径。
        int pngWidth = 0;    // 导出图像宽（列数）。
        int pngHeight = 0;   // 导出图像高（行数）。
        double spacing = 0;  // 实际导出采样间距。
        double minHeight = 0; // 高度映射下限。
        double maxHeight = 0; // 高度映射上限。
        QByteArray buffer;   // 已接收的 Uint16 字节。
        qint64 expected = 0; // 期望的总字节数。
        int receivedChunks = 0;
        int chunkSize = 0;
    };

    // 校验并加载项目索引，损坏时停止写入以保护原数据。
    void load();
    // 校验路径位于数据根目录中，且不经过链接或联接点。
    bool safePath(const QString& path) const;
    // 校验用于磁盘路径的单个片段。
    static bool safeSegment(const QString& name);
    // 生成只含十六进制字符的稳定项目标识。
    static QString newId();
    // 将项目记录转换为公开摘要。
    static QVariantMap summary(const Item& item);
    // 返回项目位置；不存在时返回负数。
    int position(const QString& id) const;
    // 项目数据目录与文件路径。
    QString projectDir(const QString& id) const;
    QString metaPath(const QString& id) const;
    QString heightsPath(const QString& id) const;
    QString thumbnailPath(const QString& id) const;
    // 读取并校验项目元数据文件。
    bool readProjectMeta(const QString& id, Item* item, QString* error) const;
    // 从原始高度字节解析网格，校验大小、NaN 与数值范围。
    bool validateHeights(const QByteArray& bytes, int gridWidth, int gridDepth,
                         QVector<float>* out, QString* error) const;
    // 读取 heightmap.bin 的完整字节（含文件头）。
    QByteArray readHeightFile(const QString& id, QString* error) const;
    // 由浮点高度生成晕染缩略图并安全写入 thumbnail.png。
    bool writeThumbnail(const QString& id, const QVector<float>& heights,
                        int gridWidth, int gridDepth, double spacing, QString* error) const;
    // 用临时文件安全替换目标文件；directWriteFallback 关闭以保证原子性。
    static bool atomicWrite(const QString& path, const QByteArray& bytes, QString* error);
    // 追加或更新索引项并提交；成功后同步内存状态并发出 changed。
    QVariantMap commitItems(const QVector<Item>& items);
    // 写入项目元数据 JSON。
    bool writeProjectMeta(const Item& item, QString* error) const;
    // 解析 Base64 为字节；失败或超过上限时返回空数组。
    static QByteArray decodeChunk(const QString& base64, int maxSize);
    // 解析 PNG 文件头，输出尺寸、位深与颜色类型（独立于 QImage 的回读验证）。
    static bool parsePngHeader(const QByteArray& bytes, int* width, int* height,
                               int* bitDepth, int* colorType);

    QString m_dataRoot;           // 调用方指定的数据根目录。
    QString m_root;               // 地形数据目录。
    QString m_indexPath;          // 项目索引路径。
    QVector<Item> m_items;        // 按创建顺序保存的项目。
    QStringList m_warnings;       // 可恢复问题的说明。
    QString m_loadError;          // 索引损坏等不可覆盖错误。
    QHash<QString, HeightStream> m_streams;   // 活动的高度流，令牌 → 状态。
    QHash<QString, ExportStream> m_exports;   // 活动的导出流，令牌 → 状态。
};
