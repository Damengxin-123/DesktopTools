#include "QrService.h"
#include "QrDecoder.h"
#include "ServiceResult.h"

#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace {
// 历史容量限制：数量、总体积与单张图片的 PNG 大小。
constexpr int MaximumItems = 100;
constexpr qint64 MaximumHistoryBytes = 64 * 1024 * 1024;
constexpr qsizetype MaximumTextBytes = 16 * 1024;
constexpr qint64 MaximumImageBytes = 8 * 1024 * 1024;
constexpr int MaximumSourceChars = 200;
// 缩略图按正方形等比缩小，用于历史列表显示。
constexpr int ThumbnailSide = 240;

const QString imagePrefix = QStringLiteral("data:image/png;base64,");

// 把图片编码为 PNG 数据地址；超过单张限制时返回空。
QString imageDataUrl(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG") || bytes.size() > MaximumImageBytes)
        return {};
    return imagePrefix + QString::fromLatin1(bytes.toBase64());
}

// 校验数据地址属于本服务保存的 PNG 格式且体积可控。
bool validDataUrl(const QString& value)
{
    return value.startsWith(imagePrefix) && value.size() <= MaximumImageBytes * 4 / 3 + 100;
}
}

QString QrService::encodeBoundedImage(QImage image)
{
    if (image.isNull())
        return {};
    for (const int side : {0, 1600, 1024}) {
        QImage candidate = image;
        if (side > 0 && std::max(candidate.width(), candidate.height()) > side)
            candidate = candidate.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QString encoded = imageDataUrl(candidate);
        if (!encoded.isEmpty())
            return encoded;
    }
    return {};
}

QrService::QrService(const QString& dataRoot, QObject* parent)
    : QObject(parent), m_path(QDir(dataRoot).absoluteFilePath(QStringLiteral("qr-history.v1.json")))
{
    load();
}

void QrService::load()
{
    if (!QFileInfo::exists(m_path))
        return;
    QFile file(m_path);
    m_loadError = QStringLiteral("二维码历史无法读取或格式损坏，已停止记录以保护原文件。请检查数据目录中的 qr-history.v1.json。");
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumHistoryBytes)
        return;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto root = document.object();
    if (error.error != QJsonParseError::NoError || root.value("version").toInt() != 1
        || !root.value("items").isArray() || root.value("items").toArray().size() > MaximumItems)
        return;
    QSet<QString> ids;
    for (const auto& value : root.value("items").toArray()) {
        const auto item = value.toObject();
        const QString id = item.value("id").toString();
        if (QUuid(id).isNull() || ids.contains(id)
            || !QDateTime::fromString(item.value("decodedAt").toString(), Qt::ISODateWithMs).isValid()
            || !item.value("text").isString()
            || item.value("text").toString().toUtf8().size() > MaximumTextBytes
            || !validDataUrl(item.value("image").toString())
            || !validDataUrl(item.value("thumbnail").toString())
            || !item.value("source").isString()
            || item.value("source").toString().size() > MaximumSourceChars
            || item.value("version").toInt() < 1 || item.value("version").toInt() > 40)
            return;
        ids.insert(id);
    }
    m_items = root.value("items").toArray().toVariantList();
    m_loadError.clear();
}

QVariantMap QrService::snapshot() const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVariantList summaries;
    for (const auto& value : m_items) {
        auto item = value.toMap();
        const QString text = item.value("text").toString();
        item.remove("image");
        item.insert("preview", text.left(600));
        item.remove("text");
        summaries.append(item);
    }
    return ServiceResult::success(QVariantMap{{"items", summaries}, {"total", m_items.size()}});
}

QVariantMap QrService::decode(const QImage& image, const QString& source)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const auto decoded = QrDecoder::decode(image);
    if (decoded.text.isEmpty())
        return ServiceResult::failure(decoded.error.isEmpty()
            ? QStringLiteral("未在图片中识别到二维码。") : decoded.error);
    if (decoded.text.toUtf8().size() > MaximumTextBytes)
        return ServiceResult::failure(QStringLiteral("二维码内容过长，无法保存到历史。"));
    const QString encoded = QrService::encodeBoundedImage(image);
    if (encoded.isEmpty())
        return ServiceResult::failure(QStringLiteral("图片过大或无法编码，无法保存到历史。"));
    const QString thumbnail = imageDataUrl(image.scaled(ThumbnailSide, ThumbnailSide,
        Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (thumbnail.isEmpty())
        return ServiceResult::failure(QStringLiteral("缩略图编码失败，无法保存到历史。"));
    QVariantMap record{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"decodedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {"text", decoded.text}, {"image", encoded}, {"thumbnail", thumbnail},
        {"source", source.left(MaximumSourceChars)}, {"version", decoded.version}};
    // 相同内容的旧记录上移为最新一条，避免重复解码刷屏。
    QVariantList items{record};
    for (const auto& value : m_items) {
        if (value.toMap().value("text").toString() != decoded.text)
            items.append(value);
    }
    while (items.size() > MaximumItems)
        items.removeLast();
    const auto committed = commit(items);
    return committed.value("ok").toBool() ? ServiceResult::success(record) : committed;
}

qsizetype QrService::position(const QString& id) const
{
    for (qsizetype index = 0; index < m_items.size(); ++index) {
        if (m_items.at(index).toMap().value("id").toString() == id)
            return index;
    }
    return -1;
}

QVariantMap QrService::read(const QString& id) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const auto index = position(id);
    return index < 0 ? ServiceResult::failure(QStringLiteral("该识别记录已不存在。"))
                     : ServiceResult::success(m_items.at(index));
}

QVariantMap QrService::commit(QVariantList items)
{
    const auto bytes = QJsonDocument(QJsonObject{{"version", 1},
        {"items", QJsonArray::fromVariantList(items)}}).toJson(QJsonDocument::Compact);
    if (items.size() > MaximumItems || bytes.size() > MaximumHistoryBytes)
        return ServiceResult::failure(QStringLiteral("二维码历史容量已满，请删除部分记录后重试。"));
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return ServiceResult::failure(QStringLiteral("无法创建二维码数据目录。"));
    QSaveFile file(m_path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return ServiceResult::failure(QStringLiteral("无法保存二维码历史：") + file.errorString());
    m_items = items;
    emit changed();
    return ServiceResult::success();
}

QVariantMap QrService::remove(const QStringList& ids)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QSet<QString> selected(ids.begin(), ids.end());
    QVariantList items;
    for (const auto& item : m_items) {
        if (!selected.contains(item.toMap().value("id").toString()))
            items.append(item);
    }
    return commit(items);
}

QVariantMap QrService::clear()
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    return commit({});
}
