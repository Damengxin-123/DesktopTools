#pragma once

#include <QObject>
#include <QList>

class LxHttpMessageThread;
/// <summary>
/// 普通http消息工具类
/// 主要用于http消息的发送和接收,独立线程异步处理
/// 1、先创建对象，然后绑定请求响应信号
/// 2、在使用时，调用具体的消息请求函数，然后异步接收请求结果即可
/// </summary>
class LxHttpMessageTool : public QObject
{
    Q_OBJECT

public:
    LxHttpMessageTool(QObject *parent);
    ~LxHttpMessageTool();

    /// <summary>
    /// 登录消息请求
    /// </summary>
    /// <param name="url">请求访问目标地址</param>
    /// <param name="message">用户名</param>
    /// <param name="messType">密码</param>
    void appLoginMessage(const QString& url, const QString& username, const QString password);

signals:
    /// <summary>
    /// 请求完成的信号
    /// </summary>
    void onReplyFinished(QString result);
private:
    // http请求线程列表
    QList<LxHttpMessageThread*> m_httpThreadList;
};
