#pragma once

#include <QThread>

/// <summary>
/// 当前类型的操作类型枚举
/// </summary>
enum LxOperationType {
    OperationNone = 0, // 无操作
    Upload = 1,  // 上传文件
    Download = 2 // 下载文件
};

class QNetworkReply;
class QNetworkAccessManager;
class QFile;
class QNetworkReply;


/// <summary>
/// 使用http进行的文件操作，包括上传、下载
/// </summary>
class LxHttpFileToolThread : public QThread
{
    Q_OBJECT

public:

    /// <summary>
    /// 设置文件请求的url和保存路径
    /// </summary>
    /// <param name="url">服务端地址</param>
    /// <param name="localFilesPath">本地文件地址</param>
    /// <param name="type">操作类型</param>
    void setOperationInfo(const QString& url, const QString& localFilesPath, LxOperationType type);

    LxHttpFileToolThread(QObject *parent);
    ~LxHttpFileToolThread();

private slots:
    /// <summary>
    /// http请求完成的回调函数
    /// </summary>
    void onReplyFinished();
signals:
    /// <summary>
    /// 线程文件操作结果信号
    /// </summary>
    void result(QString result);

private:

    /// <summary>
    /// 运行线程
    /// </summary>
    void run() override;

    /// <summary>
    /// 下载文件
    /// </summary>
    void downloadFile();



    // 请求的url地址
    QString m_url;           
    // 本地保存路径 上传时为本地文件来源，下载时为本地保存路径
    QString m_localFilesPath; 
    // 操作类型 上传或下载
    LxOperationType m_operationType = LxOperationType::OperationNone;
    // 网络访问管理器
    QNetworkAccessManager* m_networkManager = nullptr;
    // 网络操作句柄
    QNetworkReply* m_networkReply = nullptr;

    // 文件句柄
    QFile* m_pFile = nullptr;
};
