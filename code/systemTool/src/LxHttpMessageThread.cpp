#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QByteArray>
#include "LxHttpMessageThread.h"

LxHttpMessageThread::LxHttpMessageThread(QObject *parent)
    : QThread(parent)
{
    // 创建网络访问管理器
    m_pNetworkManager = new QNetworkAccessManager(this);
    connect(m_pNetworkManager, &QNetworkAccessManager::finished,
        this, &LxHttpMessageThread::onReplyFinished);

}

LxHttpMessageThread::~LxHttpMessageThread()
{
    quit();
    wait();
}

void LxHttpMessageThread::setMessageData(const MessageType messType, const QString& url, const QString& message)
{
    m_nMessageType = messType;
    m_strUrl = QUrl(url);
    m_strMessage = message;
}

void LxHttpMessageThread::postMessage()
{
    QNetworkRequest request(m_strUrl);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json"); // 根据需要设置Content-Type

    QByteArray postData = m_strMessage.toUtf8();
    m_pNetworkManager->post(request, postData);
}

void LxHttpMessageThread::getMessage()
{
    // 发送get请求
    QUrl url = m_strUrl.toString() + "?" + m_strMessage; // 将参数添加到url中
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json"); // 根据需要设置Content-Type
    m_pNetworkManager->get(request);
}

void LxHttpMessageThread::run()
{
    switch (m_nMessageType) {
    case MessageType::Post:
        postMessage();
        break;
    case MessageType::Get:
        getMessage();
        break;
    default:
        m_strResult = "Invalid message type";
        emit result(m_strResult);
        return;
    
    }
    // 进入事件循环，等待请求完成
    exec();
}

void LxHttpMessageThread::onReplyFinished(QNetworkReply* reply)
{
    // 获取请求结果
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray response = reply->readAll();
        m_strResult = QString::fromUtf8(response);
    }
    else {
        m_strResult = reply->errorString();
    }
    // 发送请求结果信号
    emit result(m_strResult);
    reply->deleteLater();
    quit();
}
