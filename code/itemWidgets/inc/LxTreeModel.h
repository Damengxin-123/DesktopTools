#pragma once

#include <QAbstractItemModel>
#include "LxTreeItem.h"

class LxTreeModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    explicit LxTreeModel(QObject* parent = nullptr);
    ~LxTreeModel() override;

    // ===== Qt Model 接口 =====
    QModelIndex index(int row, int column,
        const QModelIndex& parent) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent) const override;
    int columnCount(const QModelIndex&) const override;
    QVariant data(const QModelIndex& index,
        int role) const override;
    bool setData(const QModelIndex& index,
        const QVariant& value,
        int role) override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    // ===== 业务接口 =====
    QModelIndex addCategory(const QString& name);
    QModelIndex addItem(const QModelIndex& category,
        const QString& name);

    bool removeNode(const QModelIndex& index);
    bool moveItem(const QModelIndex& item,
        const QModelIndex& targetCategory);

    LxTreeItem* itemFromIndex(const QModelIndex& index) const;

signals:
    void itemAdded(const QUuid& id);
    void itemRemoved(const QUuid& id);
    void itemModified(const QUuid& id);

private:
    LxTreeItem* m_root;
};
