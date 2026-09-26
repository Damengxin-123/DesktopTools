#pragma once

#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVariant>
#include <memory>

// 仅绑定回环地址的网页测试 HTTP 服务，分块发送合成数据并支持续传。
class DownloadPageServer final : public QTcpServer
{
public:
    // 为每个连接安装独立的请求缓冲区，不访问外网或用户文件。
    explicit DownloadPageServer(QObject* parent = nullptr) : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (auto* socket = nextPendingConnection()) {
                auto request = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request]() {
                    if (socket->property("responding").toBool())
                        return;
                    request->append(socket->readAll());
                    if (!request->contains("\r\n\r\n"))
                        return;
                    socket->setProperty("responding", true);
                    const QByteArray route = request->split(' ').value(1);
                    const QByteArray payload = body(route);
                    const auto match = QRegularExpression(QStringLiteral("\\r\\nRange: bytes=(\\d+)-"),
                        QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(*request));
                    const qint64 offset = match.hasMatch() ? match.captured(1).toLongLong() : 0;
                    if (offset < 0 || offset >= payload.size()) {
                        socket->write("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();
                        return;
                    }
                    QByteArray headers = match.hasMatch() ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
                    headers += "Content-Type: application/octet-stream\r\nETag: \"web-test-v1\"\r\nConnection: close\r\n";
                    if (!route.contains("unknown"))
                        headers += "Content-Length: " + QByteArray::number(payload.size() - offset) + "\r\n";
                    if (match.hasMatch())
                        headers += "Content-Range: bytes " + QByteArray::number(offset) + "-"
                            + QByteArray::number(payload.size() - 1) + "/" + QByteArray::number(payload.size()) + "\r\n";
                    socket->write(headers + "\r\n");
                    auto position = std::make_shared<qint64>(offset);
                    auto* timer = new QTimer(socket);
                    timer->setInterval(route.startsWith("/slow") ? 20 : 5);
                    connect(timer, &QTimer::timeout, socket, [socket, timer, position, payload]() {
                        if (socket->state() != QAbstractSocket::ConnectedState) {
                            timer->stop();
                            return;
                        }
                        const QByteArray chunk = payload.mid(*position, 8192);
                        socket->write(chunk);
                        *position += chunk.size();
                        if (*position >= payload.size()) {
                            timer->stop();
                            socket->disconnectFromHost();
                        }
                    });
                    timer->start();
                });
            }
        });
    }

    // 获取可复现的响应内容，以逐字节核对下载完成的文件。
    static QByteArray body(const QByteArray& route)
    {
        return QByteArray(route.startsWith("/slow") ? 2 * 1024 * 1024 : 64 * 1024,
            route.startsWith("/slow") ? 's' : 'q');
    }
};
