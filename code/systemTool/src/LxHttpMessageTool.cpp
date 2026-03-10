#include "LxHttpMessageTool.h"
#include "LxHttpMessageThread.h"

LxHttpMessageTool::LxHttpMessageTool(QObject *parent)
    : QObject(parent)
{}

LxHttpMessageTool::~LxHttpMessageTool()
{}

void LxHttpMessageTool::appLoginMessage(const QString& url, const QString& username, const QString password)
{
    // 创建新的http消息线程
    LxHttpMessageThread* pThread = new LxHttpMessageThread(this);
    connect(pThread, &LxHttpMessageThread::result,
        this, [=](QString result) {
            emit onReplyFinished(result);
            
            // 线程结束后，删除线程对象
            pThread->deleteLater();
            m_httpThreadList.removeOne(pThread);
        });

    // 放入集合中
    m_httpThreadList.append(pThread);

    // 设置请求参数
    QString message = QString("{\"username\":\"%1\", \"password\":\"%2\"}").arg(username).arg(password);
    pThread->setMessageData(MessageType::Post, url, message);
    // 启动线程
    pThread->start();
}
