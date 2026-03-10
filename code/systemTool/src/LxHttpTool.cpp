#include "LxHttpTool.h"
#include "LxHttpMessageThread.h"
#include "LxHttpFileToolThread.h"

LxHttpTool::LxHttpTool(QObject *parent)
    : QObject(parent)
{}

LxHttpTool::~LxHttpTool()
{}

void LxHttpTool::appLoginMessage(const QString& url, const QString& username, const QString password)
{
    // 创建新的http消息线程
    LxHttpMessageThread* pThread = new LxHttpMessageThread(this);
    connect(pThread, &LxHttpMessageThread::result,
        this, [=](QString result) {
            emit onReplyFinished(result);
            
            // 线程结束后，删除线程对象
            pThread->deleteLater();
            m_httpMessageThreadList.removeOne(pThread);
        });

    // 放入集合中
    m_httpMessageThreadList.append(pThread);

    // 设置请求参数
    QString message = QString("{\"username\":\"%1\", \"password\":\"%2\"}").arg(username).arg(password);
    pThread->setMessageData(MessageType::Post, url, message);
    // 启动线程
    pThread->start();
}

void LxHttpTool::downloadFile(const QString& url, const QString& savePath)
{
    // 创建新的http消息线程
    LxHttpFileToolThread* pThread = new LxHttpFileToolThread(this);
    connect(pThread, &LxHttpFileToolThread::result,
        this, [=](QString result) {
            emit onReplyFinished(result);

            // 线程结束后，删除线程对象
            pThread->deleteLater();
            m_httpFileToolThreadList.removeOne(pThread);
        });

    // 放入集合中
    m_httpFileToolThreadList.append(pThread);

    // 设置请求参数
    pThread->setOperationInfo(url, savePath, LxOperationType::Download);
    // 启动线程
    pThread->start();
}
