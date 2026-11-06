#include "document_package.h"
#include "services/ServiceResult.h"
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <ctime>
#include "mz.h"
#include "mz_strm.h"
#include "mz_strm_mem.h"
#include "mz_zip.h"
#include "mz_zip_rw.h"

namespace {
// 限制文档包与解包后的总大小，防止异常归档耗尽内存。
constexpr qint64 MaximumBytes = 64 * 1024 * 1024;
// 密码仅驻留进程内存，按受控文档路径隔离。
QHash<QString, QString> passwords;
}

void DocumentPackage::remember(const QString& path, const QString& value) { passwords.insert(path, value); }
QString DocumentPackage::password(const QString& path) { return passwords.value(path); }
void DocumentPackage::forget(const QString& path) { passwords.remove(path); }

QByteArray DocumentPackage::encode(const QString& source, const QVariantMap& metadata, const QString& password, QString& error)
{
    QMap<QString, QByteArray> files;
    QString html = source;
    // 服务已经校验图片；使用内容哈希避免重复保存相同资源。
    const QRegularExpression images(QStringLiteral("data:image/(png|jpeg|gif|webp|bmp);base64,([A-Za-z0-9+/=]+)"));
    auto matches = images.globalMatch(source);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const auto bytes = QByteArray::fromBase64(match.captured(2).toLatin1());
        const auto name = QStringLiteral("resources/%1.%2").arg(QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()), match.captured(1));
        files.insert(name, bytes);
        html.replace(match.captured(), name);
    }
    files.insert(QStringLiteral("index.html"), html.toUtf8());
    auto manifest = metadata;
    manifest.insert("format", "DaDocH");
    manifest.insert("version", 1);
    manifest.remove("password");
    files.insert(QStringLiteral("document.json"), QJsonDocument::fromVariant(manifest).toJson(QJsonDocument::Compact));
    qint64 total = 0;
    for (const auto& bytes : files) total += bytes.size();
    if (total > MaximumBytes || files.size() > 2048) {
        error = QStringLiteral("文档资源总量超过 64 MB 或文件数过多。");
        return {};
    }
    void* stream = mz_stream_mem_create();
    void* writer = mz_zip_writer_create();
    const auto secret = password.toUtf8();
    mz_stream_mem_open(stream, nullptr, MZ_OPEN_MODE_CREATE);
    mz_zip_writer_set_compress_method(writer, MZ_COMPRESS_METHOD_STORE);
    mz_zip_writer_set_aes(writer, !password.isEmpty());
    if (!password.isEmpty()) mz_zip_writer_set_password(writer, secret.constData());
    int result = mz_zip_writer_open(writer, stream, 0);
    for (auto it = files.begin(); result == MZ_OK && it != files.end(); ++it) {
        const auto name = it.key().toUtf8();
        mz_zip_file info{};
        info.filename = name.constData();
        info.flag = MZ_ZIP_FLAG_UTF8;
        info.compression_method = MZ_COMPRESS_METHOD_STORE;
        info.modified_date = std::time(nullptr);
        info.aes_strength = MZ_AES_STRENGTH_256;
        info.aes_version = password.isEmpty() ? 0 : MZ_AES_VERSION;
        result = mz_zip_writer_add_buffer(writer, it.value().data(), int(it.value().size()), &info);
    }
    const int closed = mz_zip_writer_close(writer);
    QByteArray archive;
    if (result == MZ_OK && closed == MZ_OK) {
        const void* buffer = nullptr;
        int32_t size = 0;
        mz_stream_mem_get_buffer(stream, &buffer);
        mz_stream_mem_get_buffer_length(stream, &size);
        archive = QByteArray(static_cast<const char*>(buffer), size);
    } else error = QStringLiteral("文档打包失败（%1）。").arg(result != MZ_OK ? result : closed);
    if (archive.size() > MaximumBytes) {
        archive.clear();
        error = QStringLiteral("打包后的文档超过 64 MB。 ");
    }
    mz_zip_writer_delete(&writer);
    mz_stream_mem_delete(&stream);
    return archive;
}

QVariantMap DocumentPackage::decode(const QByteArray& bytes, const QString& password)
{
    if (bytes.isEmpty() || bytes.size() > MaximumBytes)
        return ServiceResult::failure(QStringLiteral("文档包为空或超过 64 MB。"));
    void* reader = mz_zip_reader_create();
    const auto secret = password.toUtf8();
    if (!password.isEmpty()) mz_zip_reader_set_password(reader, secret.constData());
    int result = mz_zip_reader_open_buffer(reader, reinterpret_cast<uint8_t*>(const_cast<char*>(bytes.constData())), int(bytes.size()), 0);
    QMap<QString, QByteArray> files;
    qint64 total = 0;
    bool encrypted = false;
    bool needsPassword = false;
    if (result == MZ_OK) result = mz_zip_reader_goto_first_entry(reader);
    while (result == MZ_OK) {
        mz_zip_file* info = nullptr;
        result = mz_zip_reader_entry_get_info(reader, &info);
        if (result != MZ_OK) break;
        const auto name = QString::fromUtf8(info->filename);
        encrypted |= (info->flag & MZ_ZIP_FLAG_ENCRYPTED) != 0;
        if (encrypted && password.isEmpty()) { needsPassword = true; break; }
        total += info->uncompressed_size;
        // 只接受应用生成的平坦资源路径，不跟随链接也不落盘。
        static const QRegularExpression resource(QStringLiteral("^resources/[a-f0-9]{64}\\.(png|jpeg|gif|webp|bmp)$"));
        if ((name != "index.html" && name != "document.json" && !resource.match(name).hasMatch())
            || files.contains(name) || files.size() >= 2048 || info->uncompressed_size < 0
            || total > MaximumBytes || (info->linkname && *info->linkname)) { result = MZ_FORMAT_ERROR; break; }
        QByteArray content(int(info->uncompressed_size), Qt::Uninitialized);
        result = mz_zip_reader_entry_save_buffer(reader, content.data(), int(content.size()));
        if (result != MZ_OK) break;
        files.insert(name, content);
        result = mz_zip_reader_goto_next_entry(reader);
    }
    mz_zip_reader_close(reader);
    mz_zip_reader_delete(&reader);
    if (needsPassword) {
        auto failure = ServiceResult::failure(QStringLiteral("文档已加密，请输入密码。"));
        failure.insert("requiresPassword", true);
        return failure;
    }
    if (result != MZ_END_OF_LIST || !files.contains("index.html") || !files.contains("document.json"))
        return ServiceResult::failure(encrypted ? QStringLiteral("密码错误或文档包已损坏。") : QStringLiteral("文档包损坏或包含不支持的内容。"));
    const auto manifest = QJsonDocument::fromJson(files.value("document.json")).object();
    if (manifest.value("format").toString() != "DaDocH" || manifest.value("version").toInt() != 1)
        return ServiceResult::failure(QStringLiteral("不支持此文档格式版本。"));
    QString html = QString::fromUtf8(files.value("index.html"));
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (it.key().startsWith("resources/"))
            html.replace(it.key(), QStringLiteral("data:image/%1;base64,%2").arg(it.key().section('.', -1), QString::fromLatin1(it.value().toBase64())));
    }
    auto data = manifest.toVariantMap();
    data.insert("html", html);
    data.insert("locked", encrypted);
    return ServiceResult::success(data);
}

QVariantMap DocumentPackage::read(const QString& path, const QString& password)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumBytes)
        return ServiceResult::failure(QStringLiteral("无法读取文档包，文件可能缺失、过大或不可访问。"));
    return decode(file.readAll(), password);
}
