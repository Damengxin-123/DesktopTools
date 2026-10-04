#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>

// 管理 Windows 全局热键，在新组合注册成功后才释放旧组合。
class GlobalHotkey final : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    // 安装原生事件过滤器；测试可关闭系统注册。idBase 区分多个热键实例的系统注册 ID。
    explicit GlobalHotkey(bool enabled = true, int idBase = 0x4D01, QObject* parent = nullptr);
    // 释放系统注册并移除事件过滤器。
    ~GlobalHotkey() override;
    // 尝试原子切换热键；失败时保留原来的热键。
    bool setShortcut(int modifier, int key, QString* error);
    // 预留新组合，同时继续持有旧组合，供设置保存事务使用。
    bool prepareShortcut(int modifier, int key, QString* error);
    // 设置保存成功后启用预留组合并释放旧组合。
    void commitShortcut();
    // 设置保存失败时只释放预留组合，保持旧组合不变。
    void cancelShortcut();
    // 读取当前成功注册的修饰键。
    int modifier() const { return m_modifier; }
    // 读取当前成功注册的虚拟键。
    int key() const { return m_key; }
    // 读取本实例的系统注册 ID 段起点，用于区分多个热键实例。
    int idBase() const { return m_idBase; }
signals:
    // 用户按下已注册的热键。
    void activated();
protected:
    // 把系统热键消息转为 Qt 信号。
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;
private:
    // 是否真正调用系统注册接口。
    bool m_enabled;
    // 本实例使用的系统注册 ID 段起点（预留相邻的 +1 作为切换备用 ID）。
    int m_idBase;
    // 当前注册 ID，零表示尚未注册。
    int m_id = 0;
    // 当前成功注册的修饰键位掩码。
    int m_modifier = 0;
    // 当前成功注册的虚拟键。
    int m_key = 0;
    // 暂时预留的新组合 ID，零表示没有待提交组合。
    int m_pendingId = 0;
    // 暂时预留的新组合修饰键。
    int m_pendingModifier = 0;
    // 暂时预留的新组合虚拟键。
    int m_pendingKey = 0;
};
