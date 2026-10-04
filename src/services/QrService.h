#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QStringList>

class QImage;

// 二维码识别历史：保存识别内容、原图与缩略图，供列表回看与再次复制。
// 与剪贴板历史相同，采用内联 JSON 存储、原子写入与损坏保护。
class QrService final : public QObject
{
    Q_OBJECT
public:
    // 从指定数据根目录加载历史索引。
    explicit QrService(const QString& dataRoot, QObject* parent = nullptr);
    // 返回历史摘要；不携带原图数据，列表使用缩略图。
    QVariantMap snapshot() const;
    // 识别一张图片；成功时新增历史（同内容旧记录上移）并返回完整数据。
    QVariantMap decode(const QImage& image, const QString& source);
    // 读取完整记录，包含原图数据地址。
    QVariantMap read(const QString& id) const;
    // 批量删除历史记录；不存在的标识直接忽略。
    QVariantMap remove(const QStringList& ids);
    // 清空全部历史。
    QVariantMap clear();
    // 把图片编码为 PNG 数据地址；超出单张限制时逐级缩小，仍超限返回空。
    static QString encodeBoundedImage(QImage image);
signals:
    // 历史发生变化（识别成功、删除或清空）。
    void changed();
private:
    // 校验并加载历史索引，损坏时停止写入以保护原数据。
    void load();
    // 原子写入历史并在成功后更新内存、发出变更信号。
    QVariantMap commit(QVariantList items);
    // 返回条目位置；不存在时返回负数。
    qsizetype position(const QString& id) const;

    // 历史索引的绝对路径。
    QString m_path;
    // 最近识别排在前面的完整记录。
    QVariantList m_items;
    // 索引损坏等不可覆盖错误。
    QString m_loadError;
};
