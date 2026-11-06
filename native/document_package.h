#pragma once
#include <QByteArray>
#include <QString>
#include <QVariantMap>

// 标准 ZIP 容器：不压缩资源，仅在设置密码时使用 WinZip AES-256。
namespace DocumentPackage {
// 将内嵌图片拆成资源文件，与 HTML 和预设一起打包。
QByteArray encode(const QString& html, const QVariantMap& metadata, const QString& password, QString& error);
// 在内存中读取包并还原图片，不向磁盘解压不可信路径。
QVariantMap decode(const QByteArray& bytes, const QString& password);
// 读取受大小限制的文档包。
QVariantMap read(const QString& path, const QString& password);
// 只在当前编辑会话内保存解锁密码，不写入索引或设置。
void remember(const QString& path, const QString& password);
// 取得当前编辑会话已经验证的密码。
QString password(const QString& path);
// 关闭或切换文档后清除会话密码。
void forget(const QString& path);
}
