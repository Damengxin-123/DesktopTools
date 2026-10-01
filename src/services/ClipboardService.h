#pragma once

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <QVariantMap>
#include <QStringList>

class QMimeData;

// 监听系统剪贴板，按用户选择保存历史；普通文件仅记录引用，不复制文件实体。
class ClipboardService final : public QObject
{
    Q_OBJECT
public:
    // 加载本地历史，可关闭系统监听以便使用独立测试数据。
    explicit ClipboardService(const QString& dataRoot, bool listen = true, QObject* parent = nullptr);
    // 返回轻量卡片摘要、类型设置和最近的保存提示。
    QVariantMap snapshot(const QString& query = {}) const;
    // 持久化要记录的类型；空列表表示暂停记录。
    QVariantMap setTypes(const QStringList& types);
    // 解析复制事件并合并短时间连续重复内容；混合文件按图像实体与普通文件引用分别记录。
    QVariantMap capture(const QMimeData* mime);
    // 获取完整文本或图片，用于详情和再次复制。
    QVariantMap read(const QString& id) const;
    // 恢复历史到系统剪贴板，不生成本程序操作导致的重复历史。
    QVariantMap copy(const QString& id);
    // 将外部文件写入系统剪贴板供粘贴，同时屏蔽历史记录。
    QVariantMap copyExternalFile(const QString& path);
    // 设置置顶状态，自动清理时保留置顶记录。
    QVariantMap pin(const QString& id, bool pinned);
    // 删除选中记录，仅清除历史，不删除引用的原文件。
    QVariantMap remove(const QStringList& ids);
    // 清空全部历史，包括置顶记录，保留监听设置。
    QVariantMap clear();
    // 打开所记录文件的父目录，原文件丢失时返回中文错误。
    QVariantMap openDirectory(const QString& id, int fileIndex);
signals:
    // 历史、设置或监听提示发生变化。
    void changed();
private:
    // 校验并加载完整索引，损坏时停止写入以保护原数据。
    void load();
    // 将候选设置和内容一起原子写入，成功后再更新内存。
    QVariantMap commit(const QVariantList& items, const QStringList& types);
    // 返回条目的位置，不存在时返回负数。
    qsizetype position(const QString& id) const;
    // 报告自动保存失败，使页面可见且不影响其他功能。
    QVariantMap captureFailure(const QString& message);
    // 历史索引的绝对路径。
    QString m_path;
    // 用户勾选的类型，首次使用默认不记录。
    QStringList m_types;
    // 最近复制排在前面的完整记录。
    QVariantList m_items;
    // 索引加载错误，存在时拒绝覆盖。
    QString m_loadError;
    // 最近自动记录错误或容量清理说明。
    QString m_notice;
    // 本程序正在恢复剪贴板内容，阻止递归记录。
    bool m_restoring = false;
    // 标记本服务恢复的内容，屏蔽 Windows 延迟送达的同一次剪贴板通知。
    QByteArray m_restoreToken;
    // 正在读取或保存剪贴板，阻止延迟渲染引发的递归采集。
    bool m_capturing = false;
    // 最近成功采集内容的摘要，不包含时间、标识和额外剪贴板格式。
    QByteArray m_lastCaptureFingerprint;
    // 从首次成功采集开始计时，重复通知不会延长去重窗口。
    QElapsedTimer m_lastCaptureTime;
};
