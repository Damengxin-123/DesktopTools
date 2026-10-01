#include "ClipboardService.h"
#include "ServiceResult.h"

#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPixmap>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSet>
#include <QTextDocument>
#include <QUrl>
#include <QUuid>

namespace {
// 限制历史总量与单次内容大小，防止持续监听无限占用磁盘和内存。
constexpr qsizetype MaximumItems = 500;
constexpr qint64 MaximumHistoryBytes = 64 * 1024 * 1024;
constexpr qsizetype MaximumTextBytes = 2 * 1024 * 1024;
constexpr qsizetype MaximumImageBytes = 8 * 1024 * 1024;
// 合并同一次复制的短时重复通知，稍后主动复制相同内容仍保留新记录。
constexpr qint64 DuplicateWindowMs = 1000;
// 在解码图像文件之前检查尺寸，避免压缩文件解码后占用过多内存。
constexpr qint64 MaximumImagePixels = 20000000;
// 支持的监听类型；其他类型只保存格式说明。
const QStringList SupportedTypes{"text", "image", "files", "links", "other"};
// 私有格式仅用于识别本服务写回剪贴板的通知，不写入历史索引。
const QString RestoreMime = QStringLiteral("application/x-desktoptool-clipboard-restore");

// 按实际保存的内容计算批次摘要，避免补充 HTML、格式顺序或图片缓存路径造成重复。
QByteArray captureFingerprint(const QVariantList& items)
{
    QJsonArray content;
    for (const auto& value : items) {
        const auto item = value.toMap();
        const QString type = item.value("type").toString();
        QJsonObject payload{{"type", type}};
        if (type == "text") {
            QString text = item.value("text").toString();
            // 只统一换行形式参与比较，原文和有意义的空白仍原样保存。
            text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
            text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
            payload.insert("text", text);
        } else if (type == "image") {
            payload.insert("image", item.value("image").toString());
        } else if (type == "files" || type == "links") {
            payload.insert("files", QJsonArray::fromVariantList(item.value("files").toList()));
        } else {
            // 未保存专有格式的内容，不能仅凭格式名相同判定重复。
            return {};
        }
        content.append(payload);
    }
    return QCryptographicHash::hash(QJsonDocument(content).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
}

// 序列化设置与历史，保证写入、容量统计使用同一格式。
QByteArray historyBytes(const QVariantList& items, const QStringList& types)
{
    return QJsonDocument(QJsonObject{{"version", 1}, {"types", QJsonArray::fromStringList(types)},
        {"items", QJsonArray::fromVariantList(items)}}).toJson(QJsonDocument::Compact);
}

// 把图片编码为 PNG 数据地址，仅由 Qt 编码的图片进入网页。
QString imageDataUrl(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG") || bytes.size() > MaximumImageBytes)
        return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
}

// 判断文件是否包含图像；兼容聊天缓存使用临时扩展名的情况。
bool isImageFile(const QString& path)
{
    const QFileInfo file(path);
    if (file.isDir())
        return false;
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    if (reader.canRead())
        return true;
    // 已知图像扩展名即使损坏或临时路径失效，也不能误归为普通文件。
    const auto suffix = file.suffix().toLower().toLatin1();
    return !suffix.isEmpty() && QImageReader::supportedImageFormats().contains(suffix);
}

// 在读取像素前检查图像头部尺寸；外部文件只使用本地路径，不下载网络资源。
QImage readBoundedImage(QImageReader& reader)
{
    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > MaximumImagePixels)
        return {};
    reader.setAutoTransform(true);
    return reader.read();
}
}

ClipboardService::ClipboardService(const QString& dataRoot, bool listen, QObject* parent)
    : QObject(parent), m_path(QDir(dataRoot).absoluteFilePath(QStringLiteral("clipboard-history.v1.json"))),
      m_restoreToken(QUuid::createUuid().toByteArray())
{
    load();
    if (listen) {
        connect(QGuiApplication::clipboard(), &QClipboard::changed, this, [this](QClipboard::Mode mode) {
            if (mode != QClipboard::Clipboard || m_restoring || m_capturing || m_types.isEmpty())
                return;
            auto* clipboard = QGuiApplication::clipboard();
            const auto* mime = clipboard->mimeData();
            // 仅在本进程仍拥有剪贴板时屏蔽自己的恢复；其他应用再次复制仍会记录。
            if (mime && clipboard->ownsClipboard() && mime->data(RestoreMime) == m_restoreToken)
                return;
            capture(mime);
        });
    }
}

void ClipboardService::load()
{
    if (!QFileInfo::exists(m_path))
        return;
    QFile file(m_path);
    m_loadError = QStringLiteral("剪贴板历史无法读取或格式损坏，已停止记录以保护原文件。请检查数据目录中的 clipboard-history.v1.json。");
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumHistoryBytes)
        return;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    const auto root = document.object();
    if (error.error != QJsonParseError::NoError || root.value("version").toInt() != 1
        || !root.value("items").isArray() || !root.value("types").isArray()
        || root.value("items").toArray().size() > MaximumItems)
        return;
    QStringList types;
    for (const auto& value : root.value("types").toArray()) {
        if (!value.isString() || !SupportedTypes.contains(value.toString()) || types.contains(value.toString()))
            return;
        types.append(value.toString());
    }
    QSet<QString> ids;
    for (const auto& value : root.value("items").toArray()) {
        const auto item = value.toObject();
        const QString type = item.value("type").toString();
        const QString id = item.value("id").toString();
        if (QUuid(id).isNull() || ids.contains(id) || !SupportedTypes.contains(type)
            || !QDateTime::fromString(item.value("copiedAt").toString(), Qt::ISODateWithMs).isValid()
            || !item.value("pinned").isBool() || !item.value("text").isString()
            || item.value("text").toString().toUtf8().size() > MaximumTextBytes)
            return;
        if (type == "image") {
            const QString prefix = QStringLiteral("data:image/png;base64,");
            if (!item.value("image").toString().startsWith(prefix)
                || !item.value("thumbnail").toString().startsWith(prefix)
                || item.value("image").toString().size() > MaximumImageBytes * 4 / 3 + 100)
                return;
        }
        if (type == "files" || type == "links") {
            const auto files = item.value("files").toArray();
            if (files.isEmpty() || files.size() > 1000)
                return;
            for (const auto& entry : files) {
                const auto reference = entry.toObject();
                if (!reference.value("path").isString() || reference.value("path").toString().isEmpty()
                    || !reference.value("name").isString() || !reference.value("local").isBool())
                    return;
            }
        }
        ids.insert(id);
    }
    m_items = root.value("items").toArray().toVariantList();
    m_types = types;
    m_loadError.clear();
}

QVariantMap ClipboardService::snapshot(const QString& query) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVariantList summaries;
    for (const auto& value : m_items) {
        auto item = value.toMap();
        item.remove("image");
        if (!query.trimmed().isEmpty() && !item.value("text").toString().contains(query.trimmed(), Qt::CaseInsensitive))
            continue;
        item.insert("preview", item.value("text").toString().left(600));
        item.remove("text");
        summaries.append(item);
    }
    return ServiceResult::success(QVariantMap{{"items", summaries}, {"types", m_types}, {"notice", m_notice}, {"total", m_items.size()}});
}

QVariantMap ClipboardService::commit(const QVariantList& items, const QStringList& types)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const auto bytes = historyBytes(items, types);
    if (items.size() > MaximumItems || bytes.size() > MaximumHistoryBytes)
        return ServiceResult::failure(QStringLiteral("剪贴板历史容量已满，请删除部分记录后重试。"));
    if (!QDir().mkpath(QFileInfo(m_path).absolutePath()))
        return ServiceResult::failure(QStringLiteral("无法创建剪贴板数据目录。"));
    QSaveFile file(m_path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return ServiceResult::failure(QStringLiteral("无法保存剪贴板历史：") + file.errorString());
    m_items = items;
    m_types = types;
    m_notice.clear();
    emit changed();
    return ServiceResult::success();
}

QVariantMap ClipboardService::setTypes(const QStringList& types)
{
    for (const auto& type : types) {
        if (!SupportedTypes.contains(type))
            return ServiceResult::failure(QStringLiteral("不支持的剪贴板内容类型。"));
    }
    QStringList unique = types;
    unique.removeDuplicates();
    const bool typesChanged = unique != m_types;
    const auto result = commit(m_items, unique);
    if (typesChanged && result.value("ok").toBool())
        m_lastCaptureTime.invalidate();
    return result;
}

QVariantMap ClipboardService::captureFailure(const QString& message)
{
    m_lastCaptureTime.invalidate();
    m_notice = message;
    emit changed();
    return ServiceResult::failure(message);
}

QVariantMap ClipboardService::capture(const QMimeData* mime)
{
    if (!m_loadError.isEmpty())
        return captureFailure(m_loadError);
    if (m_restoring || m_capturing)
        return ServiceResult::success();
    QScopedValueRollback<bool> capturing(m_capturing, true);
    if (!mime || m_types.isEmpty() || mime->formats().isEmpty())
        return ServiceResult::success();
    const QString copiedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    // 同一次复制拆分出的图片和文件共享时间，各自使用独立标识。
    const auto newRecord = [&copiedAt]() {
        return QVariantMap{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {"copiedAt", copiedAt}, {"pinned", false}, {"text", QString()}};
    };
    QVariantList captured;
    QStringList errors;
    // 限制单次多图复制的累计编码大小，避免提交前在内存中无限堆积图片。
    qint64 capturedImageBytes = 0;
    // 保存图片实体与缩略图，保持图片筛选和再次复制的语义一致。
    const auto appendImage = [&](const QImage& image, const QString& sourcePath = QString()) {
        if (!m_types.contains("image"))
            return;
        if (image.isNull() || qint64(image.width()) * image.height() > MaximumImagePixels) {
            errors.append(QStringLiteral("图像无法读取或超过 2000 万像素") + (sourcePath.isEmpty() ? QString() : QStringLiteral("：") + sourcePath));
            return;
        }
        const QString encoded = imageDataUrl(image);
        if (encoded.isEmpty()) {
            errors.append(QStringLiteral("图像编码失败或 PNG 超过 8 MB") + (sourcePath.isEmpty() ? QString() : QStringLiteral("：") + sourcePath));
            return;
        }
        const QString thumbnail = imageDataUrl(image.scaled(360, 220, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        const qint64 recordBytes = encoded.size() + thumbnail.size() + sourcePath.toUtf8().size() + 512;
        if (captured.size() >= MaximumItems || capturedImageBytes + recordBytes > MaximumHistoryBytes) {
            errors.append(QStringLiteral("本次复制的图像数量或总大小超过历史容量，已跳过：") + sourcePath);
            return;
        }
        capturedImageBytes += recordBytes;
        auto record = newRecord();
        record.insert("type", "image");
        record.insert("image", encoded);
        record.insert("thumbnail", thumbnail);
        record.insert("text", QStringLiteral("%1 × %2 像素").arg(image.width()).arg(image.height())
            + (sourcePath.isEmpty() ? QString() : QLatin1Char('\n') + QDir::toNativeSeparators(sourcePath)));
        captured.append(record);
    };
    auto item = newRecord();
    const auto urls = mime->urls();
    if (urls.size() > 1000)
        return captureFailure(QStringLiteral("本次复制包含超过 1000 个文件或链接，未保存。"));
    QImage clipboardImage;
    bool hasImageContent = mime->hasImage();
    if (m_types.contains("image") && hasImageContent) {
        const QVariant imageData = mime->imageData();
        clipboardImage = qvariant_cast<QImage>(imageData);
        if (clipboardImage.isNull() && imageData.canConvert<QPixmap>())
            clipboardImage = qvariant_cast<QPixmap>(imageData).toImage();
    }
    // 部分聊天工具提供 PNG 数据格式，但不提供 Qt 的标准图像格式。
    for (const QString& format : {QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
             QStringLiteral("application/x-qt-windows-mime;value=\"PNG\"")}) {
        if (!mime->hasFormat(format))
            continue;
        hasImageContent = true;
        if (!m_types.contains("image") || !clipboardImage.isNull())
            continue;
        QByteArray bytes = mime->data(format);
        if (bytes.size() > MaximumImageBytes)
            continue;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        reader.setDecideFormatFromContent(true);
        clipboardImage = readBoundedImage(reader);
    }
    QString type;
    if (hasImageContent && urls.size() <= 1 && (!clipboardImage.isNull() || !m_types.contains("image") || urls.isEmpty())) {
        // 单张聊天图片同时附带临时文件路径时，以图像为准，不重复保存为文件。
        appendImage(clipboardImage, urls.size() == 1 && urls.first().isLocalFile() ? urls.first().toLocalFile() : QString());
    } else if (!urls.isEmpty()) {
        QVariantList files;
        QStringList paths;
        for (const auto& url : urls) {
            if (url.isLocalFile() && isImageFile(url.toLocalFile())) {
                if (m_types.contains("image")) {
                    QImageReader reader(url.toLocalFile());
                    reader.setDecideFormatFromContent(true);
                    appendImage(readBoundedImage(reader), url.toLocalFile());
                }
                continue;
            }
            const QString path = url.isLocalFile() ? QDir::toNativeSeparators(url.toLocalFile()) : url.toString();
            files.append(QVariantMap{{"name", url.fileName()}, {"path", path}, {"local", url.isLocalFile()}});
            paths.append(path);
            if (type.isEmpty() || url.isLocalFile())
                type = url.isLocalFile() ? QStringLiteral("files") : QStringLiteral("links");
        }
        item.insert("files", files);
        item.insert("text", paths.join('\n'));
    } else if (mime->hasText() || mime->hasHtml()) {
        type = QStringLiteral("text");
        if (!m_types.contains(type)) {
            m_lastCaptureTime.invalidate();
            return ServiceResult::success();
        }
        QString text = mime->text();
        if (text.isEmpty() && mime->hasHtml()) {
            if (mime->html().toUtf8().size() > MaximumTextBytes)
                return captureFailure(QStringLiteral("复制的富文本超过 2 MB，未保存。"));
            QTextDocument document;
            document.setHtml(mime->html());
            text = document.toPlainText();
        }
        if (text.isEmpty())
            return ServiceResult::success();
        item.insert("text", text);
    } else {
        type = QStringLiteral("other");
        item.insert("text", QStringLiteral("仅记录格式信息，不保存原始内容：\n") + mime->formats().join('\n'));
    }
    if (m_types.contains(type)) {
        if (item.value("text").toString().toUtf8().size() > MaximumTextBytes)
            errors.append(QStringLiteral("复制的文本或路径信息超过 2 MB"));
        else {
            item.insert("type", type);
            captured.append(item);
        }
    }
    if (captured.isEmpty()) {
        m_lastCaptureTime.invalidate();
        return errors.isEmpty() ? ServiceResult::success() : captureFailure(errors.join('\n') + QStringLiteral("；本次复制未保存。"));
    }
    const QByteArray fingerprint = errors.isEmpty() ? captureFingerprint(captured) : QByteArray();
    if (!fingerprint.isEmpty() && fingerprint == m_lastCaptureFingerprint
        && m_lastCaptureTime.isValid() && m_lastCaptureTime.elapsed() < DuplicateWindowMs)
        return ServiceResult::success();
    // 不同内容或失败尝试会打断连续性，不能把 A、B、A 误合并成 A、B。
    m_lastCaptureTime.invalidate();
    QVariantList items = m_items;
    // 保持同一次复制内的顺序，并在一次原子写入中提交所有拆分记录。
    for (qsizetype index = captured.size(); index > 0; --index)
        items.prepend(captured.at(index - 1));
    bool pruned = false;
    while (items.size() > MaximumItems || historyBytes(items, m_types).size() > MaximumHistoryBytes) {
        qsizetype index = items.size() - 1;
        while (index >= captured.size() && items.at(index).toMap().value("pinned").toBool())
            --index;
        if (index < captured.size())
            return captureFailure(QStringLiteral("本次复制或置顶记录超过历史容量，本次复制未保存。请减少一次复制的内容，或删除部分记录、取消置顶。"));
        items.removeAt(index);
        pruned = true;
    }
    const auto result = commit(items, m_types);
    if (!result.value("ok").toBool())
        return captureFailure(result.value("error").toString());
    m_lastCaptureFingerprint = fingerprint;
    if (!fingerprint.isEmpty())
        m_lastCaptureTime.start();
    if (pruned || !errors.isEmpty()) {
        m_notice = errors.isEmpty() ? QString() : QStringLiteral("部分复制内容未保存：\n") + errors.join('\n');
        if (pruned)
            m_notice += QStringLiteral("\n已达到 500 条或 64 MB 历史上限，自动清理了最早的未置顶记录。");
        emit changed();
    }
    return result;
}

qsizetype ClipboardService::position(const QString& id) const
{
    for (qsizetype index = 0; index < m_items.size(); ++index) {
        if (m_items.at(index).toMap().value("id").toString() == id)
            return index;
    }
    return -1;
}

QVariantMap ClipboardService::read(const QString& id) const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const auto index = position(id);
    return index < 0 ? ServiceResult::failure(QStringLiteral("该剪贴板记录已不存在。")) : ServiceResult::success(m_items.at(index));
}

QVariantMap ClipboardService::copy(const QString& id)
{
    const auto result = read(id);
    if (!result.value("ok").toBool())
        return result;
    const auto item = result.value("data").toMap();
    const QString type = item.value("type").toString();
    if (type == "other")
        return ServiceResult::failure(QStringLiteral("此记录只保留格式说明，无法恢复原始内容。"));
    QScopedValueRollback<bool> restoring(m_restoring, true);
    // 先在栈上校验全部内容，成功后才把所有权交给系统剪贴板。
    QMimeData prepared;
    if (type == "image") {
        const auto bytes = QByteArray::fromBase64(item.value("image").toString().section(',', 1).toLatin1());
        const QImage image = QImage::fromData(bytes, "PNG");
        if (image.isNull())
            return ServiceResult::failure(QStringLiteral("保存的图片无法读取。"));
        prepared.setImageData(image);
    } else if (type == "files" || type == "links") {
        QList<QUrl> urls;
        for (const auto& value : item.value("files").toList()) {
            const auto file = value.toMap();
            const QString path = file.value("path").toString();
            if (file.value("local").toBool() && !QFileInfo::exists(path))
                return ServiceResult::failure(QStringLiteral("原文件或目录已不存在：") + path);
            urls.append(file.value("local").toBool() ? QUrl::fromLocalFile(path) : QUrl(path));
        }
        prepared.setUrls(urls);
        prepared.setText(item.value("text").toString());
    } else {
        prepared.setText(item.value("text").toString());
    }
    auto* mime = new QMimeData;
    if (prepared.hasImage()) mime->setImageData(prepared.imageData());
    if (prepared.hasUrls()) mime->setUrls(prepared.urls());
    if (prepared.hasText()) mime->setText(prepared.text());
    mime->setData(RestoreMime, m_restoreToken);
    // 恢复历史也是一次明确的剪贴板操作，之后的外部复制应重新计数。
    m_lastCaptureTime.invalidate();
    QGuiApplication::clipboard()->setMimeData(mime);
    return ServiceResult::success();
}

QVariantMap ClipboardService::copyExternalFile(const QString& path)
{
    const QFileInfo file(path);
    if (!file.isFile() || !file.exists())
        return ServiceResult::failure(QStringLiteral("原文件已不存在，无法复制：") + QDir::toNativeSeparators(path));
    // 可解码时同时提供图像内容，聊天工具可以直接粘贴图片；无法解码仍按文件引用复制。
    QImageReader reader(file.absoluteFilePath());
    reader.setDecideFormatFromContent(true);
    const QImage image = readBoundedImage(reader);
    QScopedValueRollback<bool> restoring(m_restoring, true);
    auto* mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(file.absoluteFilePath())});
    if (!image.isNull())
        mime->setImageData(image);
    mime->setText(QDir::toNativeSeparators(file.absoluteFilePath()));
    mime->setData(RestoreMime, m_restoreToken);
    // 外部文件复制也是一次明确的剪贴板操作，之后的其他应用复制应重新计数。
    m_lastCaptureTime.invalidate();
    QGuiApplication::clipboard()->setMimeData(mime);
    return ServiceResult::success();
}

QVariantMap ClipboardService::pin(const QString& id, bool pinned)
{
    const auto index = position(id);
    if (index < 0)
        return ServiceResult::failure(QStringLiteral("该剪贴板记录已不存在。"));
    QVariantList items = m_items;
    auto item = items.at(index).toMap();
    item.insert("pinned", pinned);
    items[index] = item;
    return commit(items, m_types);
}

QVariantMap ClipboardService::remove(const QStringList& ids)
{
    const QSet<QString> selected(ids.begin(), ids.end());
    QVariantList items;
    for (const auto& item : m_items) {
        if (!selected.contains(item.toMap().value("id").toString()))
            items.append(item);
    }
    const auto result = commit(items, m_types);
    if (result.value("ok").toBool())
        m_lastCaptureTime.invalidate();
    return result;
}

QVariantMap ClipboardService::clear()
{
    const auto result = commit({}, m_types);
    if (result.value("ok").toBool())
        m_lastCaptureTime.invalidate();
    return result;
}

QVariantMap ClipboardService::openDirectory(const QString& id, int fileIndex)
{
    const auto result = read(id);
    if (!result.value("ok").toBool())
        return result;
    const auto files = result.value("data").toMap().value("files").toList();
    if (fileIndex < 0 || fileIndex >= files.size() || !files.at(fileIndex).toMap().value("local").toBool())
        return ServiceResult::failure(QStringLiteral("该记录没有可打开的本地目录。"));
    const QFileInfo file(files.at(fileIndex).toMap().value("path").toString());
    if (!file.exists())
        return ServiceResult::failure(QStringLiteral("原文件或目录已不存在，仍可在详情中查看原路径。"));
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file.isDir() ? file.absoluteFilePath() : file.absolutePath())))
        return ServiceResult::failure(QStringLiteral("无法打开所在目录。"));
    return ServiceResult::success();
}
