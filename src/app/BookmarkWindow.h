#pragma once

#include <QList>
#include <QMainWindow>
#include <QPointer>

class QCloseEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QSplitter;
class QWebEngineDownloadRequest;
class QWebEngineProfile;
class QWebEngineView;
class BookmarkBridge;
class BookmarkService;

/** 网页书签窗口：左侧书签面板承载管理操作，右侧为可上传下载的真实浏览区。
 * 主窗口内作为子控件承载，测试可使用无父对象模式。 */
class BookmarkWindow final : public QMainWindow
{
    Q_OBJECT
public:
    /** 创建书签子控件；下载目录为空时使用系统下载目录。 */
    static BookmarkWindow* open(BookmarkService* service, const QString& downloadDirectory,
                                QWidget* parent = nullptr);
    /** 更新下载保存目录，使之后下载落到设置中的新位置。 */
    void setDownloadDirectory(const QString& directory);

protected:
    /** 仍有下载在途时先让用户确认，避免静默丢失已下载内容。 */
    void closeEvent(QCloseEvent* event) override;

private:
    /** 创建书签管理面板与隔离浏览区，父对象决定是否嵌入。 */
    explicit BookmarkWindow(BookmarkService* service, const QString& downloadDirectory, QWidget* parent);
    /** 在右侧浏览区加载地址。 */
    void load(const QString& url);
    /** 刷新后退、前进按钮的可用状态。 */
    void updateNavigationButtons();
    /** 显示空状态提示页。 */
    void showEmptyPage();
    /** 把右侧当前网址推送给左侧面板，便于一键收藏当前网页。 */
    void pushUrlToPanel();
    /** 返回下载保存目录；未指定时使用系统下载目录。 */
    QString downloadDirectory() const;
    /** 处理 profile 发起的下载请求：保存到下载目录并跟踪进度。 */
    void handleDownload(QWebEngineDownloadRequest* download);
    /** 更新底部状态条中的下载进度或结果。 */
    void updateDownloadStatus();

    QSplitter* m_splitter;         ///< 左右分区容器。
    QWebEngineView* m_panel;       ///< 左侧书签列表面板。
    QWebEngineView* m_web;         ///< 右侧网页浏览区。
    QWidget* m_toolbarContainer;   ///< 右侧导航工具栏容器。
    QPushButton* m_back;           ///< 后退按钮。
    QPushButton* m_forward;        ///< 前进按钮。
    QPushButton* m_reload;         ///< 重新加载按钮。
    QPushButton* m_stop;           ///< 停止加载按钮。
    QLineEdit* m_address;          ///< 右侧地址栏。
    QLabel* m_status;              ///< 底部状态条。
    QWebEngineProfile* m_webProfile; ///< 浏览区使用的持久化配置。
    BookmarkBridge* m_bridge;      ///< 面板页的原生接口。
    QString m_downloadDirectory;   ///< 下载保存目录。
    bool m_panelReady = false;     ///< 左侧面板已完成加载，可以执行脚本。
    QString m_lastDownloadPath;    ///< 最近一次下载完成的文件路径。
    QList<QPointer<QWebEngineDownloadRequest>> m_downloads; ///< 正在进行的下载。
    QString m_lastDownloadMessage; ///< 最近一次下载的结果文案。
};
