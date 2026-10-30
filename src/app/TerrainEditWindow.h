#pragma once

#include <QHash>
#include <QMainWindow>
#include <QPointer>

class QCloseEvent;
class QWebEngineView;
class TerrainBridge;
class TerrainService;

// 独立地形编辑窗口：承载 terrain.html 的三维编辑页面。
// 同一项目重复打开时激活既有窗口；关闭时按网页报告的未保存状态提供保存、放弃、取消。
class TerrainEditWindow final : public QMainWindow
{
    Q_OBJECT
public:
    // 创建并加载编辑页面；项目不存在时返回 nullptr 并弹窗说明。
    static TerrainEditWindow* open(TerrainService* service, const QString& projectId,
                                   QWidget* parent);
    // 该项目是否已有打开的编辑窗口。
    static bool isProjectOpen(const QString& projectId);
    // 激活该项目已打开的编辑窗口。
    static void activateExisting(const QString& projectId);

protected:
    // 未保存修改时的保存/放弃/取消保护。
    void closeEvent(QCloseEvent* event) override;

private:
    explicit TerrainEditWindow(TerrainService* service, const QString& projectId,
                               const QString& title, QWidget* parent);

    QWebEngineView* m_view;
    TerrainBridge* m_bridge;
    // 等待网页完成“保存并关闭”的回传。
    bool m_saveWaiting = false;
    // 项目标识 → 打开的编辑窗口。
    static QHash<QString, QPointer<TerrainEditWindow>> s_open;
};
