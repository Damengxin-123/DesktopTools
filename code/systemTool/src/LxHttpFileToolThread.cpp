#include "LxHttpFileToolThread.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QFile>
/// <summary>
/// 设置请求的url地址、本地文件路径和操作类型
/// </summary>
void LxHttpFileToolThread::setOperationInfo(const QString& url, const QString& localFilesPath, LxOperationType type)
{
    // 设置请求的url地址、本地文件路径和操作类型
    m_url = url;
    m_localFilesPath = localFilesPath;
    m_operationType = type;
}

LxHttpFileToolThread::LxHttpFileToolThread(QObject *parent)
    : QThread(parent)
{
    // 1. 创建网络访问管理器
    m_networkManager = new QNetworkAccessManager(this);
    
}

LxHttpFileToolThread::~LxHttpFileToolThread()
{
}
/// <summary>
/// 根据操作类型，执行不同的文件操作
/// 1. 如果是下载操作，调用downloadFile函数
/// 2. 如果是上传操作，暂未实现
/// </summary>
void LxHttpFileToolThread::run()
{
    switch (m_operationType)
    {   
    case LxOperationType::OperationNone:
        break;
    case LxOperationType::Upload:
        break;
    case LxOperationType::Download:
        downloadFile();
        break;
    default:
        break;
    }
    // 进入事件循环，等待请求完成
    exec();
}
/// <summary>
/// TODO: 实现文件下载逻辑
/// 1. 创建网络访问管理器
/// 2. 创建网络请求
/// 3. 发送请求
/// 4. 处理响应
/// 5. 保存文件到本地路径
/// 6. 发出下载完成信号
/// </summary>
void LxHttpFileToolThread::downloadFile()
{
    QNetworkRequest request(m_url);
    m_networkReply = m_networkManager->get(request);

    // 创建本地文件
    m_pFile = new QFile(m_localFilesPath);
    if (!m_pFile->open(QIODevice::WriteOnly)) {
        emit result(QStringLiteral("false 文件保存失败: %1").arg(m_localFilesPath));
        m_pFile->deleteLater();
        m_pFile = nullptr;
        m_networkReply->abort();
        m_networkReply->deleteLater();
        quit();
        return;
    }

    // 连接信号，分块写入
    connect(m_networkReply, &QNetworkReply::readyRead, this, &LxHttpFileToolThread::onReplyFinished);

    // 下载完成处理
    connect(m_networkReply, &QNetworkReply::finished, this, [this]() {
        if (m_pFile) {
            m_pFile->flush();
            m_pFile->close();
            m_pFile->deleteLater();
            m_pFile = nullptr;
        }
        if (m_networkReply->error() == QNetworkReply::NoError) {
            emit result(QStringLiteral("true %1").arg(m_localFilesPath));
        }
        else {
            emit result(QStringLiteral("false: %1").arg(m_networkReply->errorString()));
        }
        m_networkReply->deleteLater();
        quit();
        });
}
/// <summary>
/// 文件下载完成后的处理函数
/// 1. 检查是否有错误
/// 2. 如果没有错误，保存文件到本地路径
/// 3. 如果保存成功，发出下载成功信号
/// 4. 如果有错误，发出下载失败信号
/// 5. 删除回复对象
/// </summary>
void LxHttpFileToolThread::onReplyFinished()
{
     if (m_pFile && m_networkReply)
     {
         m_pFile->write(m_networkReply->readAll());
     }
}
