#pragma once

#include <QString>
#include <QList>
#include <QUuid>

enum class ETreeNodeType
{
    Category,
    Item
};

class LxTreeItem
{
public:
    explicit LxTreeItem(ETreeNodeType type,
        const QString& name,
        LxTreeItem* parent = nullptr);
    ~LxTreeItem();

    // ===== 结构 =====
    LxTreeItem* parent() const;
    LxTreeItem* child(int row) const;
    int childCount() const;
    int row() const;

    void appendChild(LxTreeItem* child);
    void removeChild(int row);

    // ===== 数据 =====
    ETreeNodeType type() const;
    QString name() const;
    void setName(const QString& name);
    QUuid id() const;

private:
    QList<LxTreeItem*> m_children;
    LxTreeItem* m_parent = nullptr;

    ETreeNodeType m_type;
    QString m_name;
    QUuid m_id;
};
