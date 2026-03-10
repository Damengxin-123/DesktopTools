#include "LxTreeWidget.h"
#include "LxTreeModel.h"
#include <QMenu>

LxTreeWidget::LxTreeWidget(QWidget *parent)
    : QWidget(parent)
{
    ui.setupUi(this);

    m_pTreeViewModel = new LxTreeModel(this);
    ui.uTreeView->setModel(m_pTreeViewModel);
    ui.uTreeView->setHeaderHidden(true);
    ui.uTreeView->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(ui.uTreeView, &QTreeView::customContextMenuRequested,
        this, &LxTreeWidget::onTreeContextMenu);



    auto cat = m_pTreeViewModel->addCategory("分类1");
    m_pTreeViewModel->addItem(cat, "条目1");
    m_pTreeViewModel->addItem(cat, "条目2");
}

LxTreeWidget::~LxTreeWidget()
{
}
void LxTreeWidget::onTreeContextMenu(const QPoint& pos)
{
    QModelIndex index = ui.uTreeView->indexAt(pos);
    QMenu menu;

    if (!index.isValid())
    {
        // ========= 空白区域 =========
        menu.addAction("新增分类", this, [=]() {
            m_pTreeViewModel->addCategory("新分类");
            });
    }
    else
    {
        auto* item = m_pTreeViewModel->itemFromIndex(index);

        if (item->type() == ETreeNodeType::Category)
        {
            // ========= 分类 =========
            QAction* addItem = menu.addAction("添加条目");
            QAction* rename = menu.addAction("重命名分类");
            QAction* remove = menu.addAction("删除分类");

            connect(addItem, &QAction::triggered, this, [=]() {
                m_pTreeViewModel->addItem(index, "新条目");
                });
            connect(rename, &QAction::triggered, this, [=] (){
                ui.uTreeView->edit(index);
                });
            connect(remove, &QAction::triggered, this, [=]() {
                m_pTreeViewModel->removeNode(index);
                });
        }
        else
        {
            // ========= 条目 =========
            QAction* edit = menu.addAction("编辑条目");
            QAction* remove = menu.addAction("删除条目");

            connect(edit, &QAction::triggered, this, [=]() {
                ui.uTreeView->edit(index);
                });
            connect(remove, &QAction::triggered, this, [=]() {
                m_pTreeViewModel->removeNode(index);
                });
        }
    }

    menu.exec(ui.uTreeView->viewport()->mapToGlobal(pos));

}
