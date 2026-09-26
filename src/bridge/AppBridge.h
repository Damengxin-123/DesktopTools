#pragma once

#include <QObject>
#include <QVariantMap>
#include <QStringList>

class GlobalHotkey;
class NoteService;
class SettingsService;
class ShortcutService;
class QWidget;

// HTML 页面的唯一原生接口，转发业务操作并处理系统交互。
class AppBridge final : public QObject
{
    Q_OBJECT
public:
    // 创建服务并加载兼容数据；测试可关闭系统热键注册。
    explicit AppBridge(const QString& dataRoot, QWidget* window, bool nativeIntegration = true);
    // 返回快捷方式和分类快照。
    Q_INVOKABLE QVariantMap getShortcuts() const;
    // 新增或修改快捷方式。
    Q_INVOKABLE QVariantMap saveShortcut(const QVariantMap& item);
    // 批量删除指定快捷方式。
    Q_INVOKABLE QVariantMap deleteShortcuts(const QStringList& ids);
    // 移动条目到指定分类与顺序。
    Q_INVOKABLE QVariantMap moveShortcut(const QString& id, const QString& categoryId, const QString& beforeId);
    // 新增或重命名快捷方式分类。
    Q_INVOKABLE QVariantMap saveShortcutCategory(const QString& id, const QString& name);
    // 删除分类并保留其中的条目。
    Q_INVOKABLE QVariantMap deleteShortcutCategory(const QString& id);
    // 使用系统关联程序打开已保存的快捷方式。
    Q_INVOKABLE QVariantMap openShortcut(const QString& id);
    // 显示原生文件或目录选择器。
    Q_INVOKABLE QVariantMap chooseTarget(int type);
    // 将文本复制到系统剪贴板。
    Q_INVOKABLE QVariantMap copyText(const QString& text);
    // 返回便签列表与分类。
    Q_INVOKABLE QVariantMap getNotes() const;
    // 读取指定便签的安全 HTML 内容。
    Q_INVOKABLE QVariantMap getNote(const QString& id) const;
    // 保存便签标题、分类与内容。
    Q_INVOKABLE QVariantMap saveNote(const QVariantMap& note);
    // 批量移除便签索引并保留磁盘原件。
    Q_INVOKABLE QVariantMap deleteNotes(const QStringList& ids);
    // 修改便签所属分类或排序。
    Q_INVOKABLE QVariantMap moveNote(const QString& id, const QString& categoryId, const QString& beforeId);
    // 新增或重命名便签分类。
    Q_INVOKABLE QVariantMap saveNoteCategory(const QString& id, const QString& name);
    // 删除便签分类并保留其中的便签。
    Q_INVOKABLE QVariantMap deleteNoteCategory(const QString& id);
    // 读取统一设置。
    Q_INVOKABLE QVariantMap getSettings() const;
    // 校验、切换系统热键并持久化设置，失败时恢复旧组合。
    Q_INVOKABLE QVariantMap saveSettings(const QVariantMap& settings);
    // 恢复默认字号和 F8 热键。
    Q_INVOKABLE QVariantMap resetSettings();
    // 使用资源管理器打开数据目录。
    Q_INVOKABLE QVariantMap openDataDirectory();
    // 返回版本、数据目录和热键启动状态。
    Q_INVOKABLE QVariantMap getAppInfo() const;
signals:
    // 快捷方式数据已持久化。
    void shortcutsChanged();
    // 便签数据已持久化。
    void notesChanged();
    // 设置已持久化。
    void settingsChanged();
    // 热键请求唤醒主窗口。
    void activateWindowRequested();
private:
    // 当前数据根目录。
    QString m_dataRoot;
    // 原生对话框的所属窗口。
    QWidget* m_window;
    // 快捷方式服务，由本对象管理生命周期。
    ShortcutService* m_shortcuts;
    // 便签服务，由本对象管理生命周期。
    NoteService* m_notes;
    // 设置服务，由本对象管理生命周期。
    SettingsService* m_settings;
    // 全局热键注册器。
    GlobalHotkey* m_hotkey;
    // 启动时热键注册失败的说明。
    QString m_hotkeyWarning;
};
