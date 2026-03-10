#pragma once

#include <QThread>
#include <QUrl>


class QNetworkAccessManager;
class QNetworkReply;

/// <summary>
/// 请求类型
/// </summary>
enum MessageType {
    MessageNone = 0,  // 无
    Post = 1,   // post请求
    Get = 2,    // get请求
};

/// <summary>
/// 负责发送http消息的线程，在请求执行完成之后，返还结果到线程管理类
/// 1、使用时，先创建对象，绑定请求结果信号到处理函数
/// 2、然后调用setMessageData函数设置请求参数
/// 3、调用start函数启动线程，请求开始发送
/// 4、请求结束后，线程会自动结束，并发出信号
/// </summary>
class LxHttpMessageThread : public QThread
{
    Q_OBJECT

public:
    LxHttpMessageThread(QObject *parent);
    ~LxHttpMessageThread();

    /// <summary>
    /// 设置http请求的url
    /// </summary>
    /// <param name="messType">请求类型</param>
    /// <param name="url">请求地址，get请求不包含请求参数</param>
    /// <param name="message">请求内容，get请求则为请求参数，post请求为请求内容json</param>
    void setMessageData(const MessageType messType, const QString& url, const QString& message);




private slots:
    // http请求完成的回调函数
    void onReplyFinished(QNetworkReply* reply); 
signals:
    void result(QString result);

private:

    /// <summary>
    /// 运行线程 在调用setMessageData函数设置请求参数之后，执行start函数，调用此方法
    /// </summary>
    void run();

    // post请求
    void postMessage();



    // get请求
    void getMessage();

    // 请求类型
    MessageType m_nMessageType;

    // http请求的url
    QUrl m_strUrl;
    // http请求的消息
    QString m_strMessage;
    // http请求的结果
    QString m_strResult;
    // http请求的管理器
    QNetworkAccessManager* m_pNetworkManager = nullptr;
};
