#include "LxTreeModel.h"

LxTreeModel::LxTreeModel(QObject* parent)
    : QAbstractItemModel(parent)
{
    m_root = new LxTreeItem(ETreeNodeType::Category,
        "ROOT",
        nullptr);
}

LxTreeModel::~LxTreeModel()
{
    delete m_root;
}

QModelIndex LxTreeModel::index(int row, int column,
    const QModelIndex& parent) const
{
    if (!hasIndex(row, column, parent))
        return QModelIndex();

    LxTreeItem* parentItem =
        parent.isValid()
        ? static_cast<LxTreeItem*>(parent.internalPointer())
        : m_root;

    LxTreeItem* childItem = parentItem->child(row);
    if (childItem)
        return createIndex(row, column, childItem);

    return QModelIndex();
}

QModelIndex LxTreeModel::parent(const QModelIndex& index) const
{
    if (!index.isValid())
        return QModelIndex();

    auto* childItem =
        static_cast<LxTreeItem*>(index.internalPointer());
    LxTreeItem* parentItem = childItem->parent();

    if (parentItem == m_root || !parentItem)
        return QModelIndex();

    return createIndex(parentItem->row(), 0, parentItem);
}

int LxTreeModel::rowCount(const QModelIndex& parent) const
{
    LxTreeItem* parentItem =
        parent.isValid()
        ? static_cast<LxTreeItem*>(parent.internalPointer())
        : m_root;

    return parentItem->childCount();
}

int LxTreeModel::columnCount(const QModelIndex&) const
{
    return 1;
}

QVariant LxTreeModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();

    auto* item =
        static_cast<LxTreeItem*>(index.internalPointer());

    if (role == Qt::DisplayRole || role == Qt::EditRole)
        return item->name();

    return QVariant();
}

bool LxTreeModel::setData(const QModelIndex& index,
    const QVariant& value,
    int role)
{
    if (!index.isValid() || role != Qt::EditRole)
        return false;

    auto* item =
        static_cast<LxTreeItem*>(index.internalPointer());
    item->setName(value.toString());

    emit itemModified(item->id());
    emit dataChanged(index, index);

    return true;
}

Qt::ItemFlags LxTreeModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    return Qt::ItemIsEnabled
        | Qt::ItemIsSelectable
        | Qt::ItemIsEditable
        | Qt::ItemIsDragEnabled
        | Qt::ItemIsDropEnabled;
}

QModelIndex LxTreeModel::addCategory(const QString& name)
{
    int row = m_root->childCount();
    beginInsertRows(QModelIndex(), row, row);

    auto* category =
        new LxTreeItem(ETreeNodeType::Category,
            name,
            m_root);
    m_root->appendChild(category);

    endInsertRows();
    return index(row, 0, QModelIndex());
}

QModelIndex LxTreeModel::addItem(const QModelIndex& category,
    const QString& name)
{
    if (!category.isValid())
        return QModelIndex();

    auto* parentItem =
        static_cast<LxTreeItem*>(category.internalPointer());
    if (parentItem->type() != ETreeNodeType::Category)
        return QModelIndex();

    int row = parentItem->childCount();
    beginInsertRows(category, row, row);

    auto* item =
        new LxTreeItem(ETreeNodeType::Item,
            name,
            parentItem);
    parentItem->appendChild(item);

    endInsertRows();
    emit itemAdded(item->id());

    return index(row, 0, category);
}

bool LxTreeModel::removeNode(const QModelIndex& index)
{
    if (!index.isValid())
        return false;

    auto* item =
        static_cast<LxTreeItem*>(index.internalPointer());
    auto* parentItem = item->parent();

    int row = item->row();
    QModelIndex parentIndex =
        parentItem == m_root
        ? QModelIndex()
        : createIndex(parentItem->row(), 0, parentItem);

    emit itemRemoved(item->id());

    beginRemoveRows(parentIndex, row, row);
    parentItem->removeChild(row);
    endRemoveRows();

    return true;
}

bool LxTreeModel::moveItem(const QModelIndex& item,
    const QModelIndex& targetCategory)
{
    if (!item.isValid() || !targetCategory.isValid())
        return false;

    auto* itemNode =
        static_cast<LxTreeItem*>(item.internalPointer());
    auto* oldParent = itemNode->parent();
    auto* newParent =
        static_cast<LxTreeItem*>(targetCategory.internalPointer());

    if (itemNode->type() != ETreeNodeType::Item ||
        newParent->type() != ETreeNodeType::Category)
        return false;

    int oldRow = itemNode->row();
    int newRow = newParent->childCount();

    QModelIndex oldParentIndex =
        oldParent == m_root
        ? QModelIndex()
        : createIndex(oldParent->row(), 0, oldParent);

    beginMoveRows(oldParentIndex, oldRow, oldRow,
        targetCategory, newRow);

    oldParent->removeChild(oldRow);
    itemNode->appendChild(nullptr); // ·ÀÖ¹Ðü¿Õ
    itemNode->~LxTreeItem();
    newParent->appendChild(itemNode);

    endMoveRows();
    return true;
}

LxTreeItem* LxTreeModel::itemFromIndex(const QModelIndex& index) const
{
    if (!index.isValid())
        return nullptr;
    return static_cast<LxTreeItem*>(index.internalPointer());
}
