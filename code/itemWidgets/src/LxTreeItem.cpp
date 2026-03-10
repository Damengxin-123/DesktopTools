#include "LxTreeItem.h"

LxTreeItem::LxTreeItem(ETreeNodeType type,
    const QString& name,
    LxTreeItem* parent)
    : m_parent(parent)
    , m_type(type)
    , m_name(name)
    , m_id(QUuid::createUuid())
{
}

LxTreeItem::~LxTreeItem()
{
    qDeleteAll(m_children);
}

LxTreeItem* LxTreeItem::parent() const
{
    return m_parent;
}

LxTreeItem* LxTreeItem::child(int row) const
{
    return m_children.value(row);
}

int LxTreeItem::childCount() const
{
    return m_children.size();
}

int LxTreeItem::row() const
{
    if (!m_parent)
        return 0;
    return m_parent->m_children.indexOf(
        const_cast<LxTreeItem*>(this));
}

void LxTreeItem::appendChild(LxTreeItem* child)
{
    m_children.append(child);
}

void LxTreeItem::removeChild(int row)
{
    delete m_children.takeAt(row);
}

ETreeNodeType LxTreeItem::type() const
{
    return m_type;
}

QString LxTreeItem::name() const
{
    return m_name;
}

void LxTreeItem::setName(const QString& name)
{
    m_name = name;
}

QUuid LxTreeItem::id() const
{
    return m_id;
}
