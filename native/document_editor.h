#pragma once

#include <QMainWindow>
#include <QVariantMap>

class AppBridge;
class QCloseEvent;

// 在独立窗口中编辑标准 HTML，复用原文档目录、分类及原子写入服务。
class DocumentEditor final : public QMainWindow
{
    Q_OBJECT
public:
    // 创建受限的本地编辑器页面。
    explicit DocumentEditor(AppBridge& bridge);

    // 打开指定文档，空 ID 创建一篇新文档。
    QVariantMap openDocument(const QString& id, const QString& category, const QString& theme);
    // 同步工作台的外观偏好。
    void setAppearance(const QString& theme);

    // 返回本次编辑会话的数据、标题、分类及主题。
    Q_INVOKABLE QVariantMap state() const;
    // 保存当前文档，拒绝已经切走的编辑会话。
    Q_INVOKABLE QVariantMap saveDocument(int revision, const QVariantMap& input);
    // 导出完整文档包，保留原有密码锁。
    Q_INVOKABLE QVariantMap exportPackage(int revision);
    // 设置、更改或解除密码锁，密码只进入原生编辑会话。
    Q_INVOKABLE QVariantMap configurePassword(int revision);
    // 导入其他人发送的文档包，并在当前分类中打开。
    QVariantMap importPackage(const QString& category, const QString& theme);
    // 记录页面是否有未保存的修改。
    Q_INVOKABLE void setDirty(int revision, bool dirty);

signals:
    // 切换文档时重新加载标题、分类与 HTML。
    void documentChanged(const QVariantMap& state);
    // 不重载正文地同步配色。
    void appearanceChanged(const QString& theme);
    // 编辑窗口关闭后通知后台检查生命周期。
    void editorClosed();

protected:
    // 在关闭前检查是否有未保存的修改。
    void closeEvent(QCloseEvent* event) override;

private:
    // 统一处理切换与关闭时的草稿确认。
    bool mayDiscard();
    // 重新聚焦窗口，保留用户设置的最大化状态。
    void focusEditor();

    AppBridge& m_bridge;             // 已有的文档存储与分类接口。
    QVariantMap m_document;          // 当前文档的持久化标识、分类及原始 HTML。
    QVariantList m_categories;      // 打开时取得的文档分类快照。
    QString m_appearance;            // 工作台传入的主题标识。
    int m_revision = 0;              // 每次打开文档时递增的会话标识。
    bool m_dirty = false;            // 当前编辑会话是否有未保存改动。
};
