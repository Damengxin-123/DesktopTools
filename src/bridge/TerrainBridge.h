#pragma once

#include <QObject>
#include <QVariantMap>

class TerrainService;
class QWidget;

// 地形编辑窗口网页侧的唯一原生接口：项目数据流式读写、原生导出对话框与关闭协调。
// 网页是编辑期间唯一权威数据源，Qt 只负责持久化，不维护第二份地形演算。
class TerrainBridge final : public QObject
{
    Q_OBJECT
public:
    explicit TerrainBridge(TerrainService* service, const QString& projectId,
                           QWidget* dialogParent, QObject* parent = nullptr);

    // 是否存在网页已报告的未保存修改。
    bool dirty() const { return m_dirty; }
    // 保存并关闭流程是否正在进行（网页尚未完成回传）。
    bool savePending() const { return m_savePending; }
    // 保存等待超时后允许重新提示。
    void clearSavePending() { m_savePending = false; }

    // 返回项目元数据（名称、网格、间距、撤销步数）。
    Q_INVOKABLE QVariantMap getInitialData() const;
    // 打开高度读取流，返回令牌、分块大小与完整元数据。
    Q_INVOKABLE QVariantMap openHeightStream();
    // 按序读取一个高度分块（Base64）。
    Q_INVOKABLE QVariantMap readHeightChunk(const QString& token, int index);
    // 开始保存流：传入重采样后（或原样）的网格与设置，返回令牌。
    Q_INVOKABLE QVariantMap beginSave(int gridWidth, int gridDepth, double spacing,
                                      double undoSteps);
    // 按序写入一个高度分块（Base64 的小端 Float32）。
    Q_INVOKABLE QVariantMap writeHeightChunk(const QString& token, int index, const QString& base64);
    // 提交保存：服务端完成校验、安全写入与缩略图重建。
    Q_INVOKABLE QVariantMap commitSave(const QString& token);
    // 取消未完成的传输会话。
    Q_INVOKABLE QVariantMap cancelStream(const QString& token);

    // 网页报告未保存状态变化；用于窗口关闭保护。
    Q_INVOKABLE QVariantMap setDirty(bool dirty);
    // 显示原生保存对话框选择导出路径；返回路径或取消标志。
    Q_INVOKABLE QVariantMap chooseExportTarget(const QString& suggestedName);
    // 开始导出流：记录路径、分辨率、间距与高度映射范围。
    Q_INVOKABLE QVariantMap beginExport(const QString& filePath, int pngWidth, int pngHeight,
                                        double spacing, double minHeight, double maxHeight);
    // 按序写入一个导出分块（Base64 的小端 Uint16）。
    Q_INVOKABLE QVariantMap writeExportChunk(const QString& token, int index, const QString& base64);
    // 提交导出：编码 16 位灰度 PNG、写入 JSON 说明并独立回读校验。
    Q_INVOKABLE QVariantMap commitExport(const QString& token);

    // 网页保存成功后请求关闭窗口，触发原生关闭保护流程。
    Q_INVOKABLE QVariantMap requestClose();

signals:
    // 保存成功后广播，便于测试与窗口状态联动。
    void saved();
    // 网页请求关闭窗口。
    void closeRequested();

private:
    TerrainService* m_service;
    QString m_projectId;
    QWidget* m_dialogParent;
    bool m_dirty = false;
    bool m_savePending = false;
};
