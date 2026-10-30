#include "BookmarkService.h"
#include "ServiceResult.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStringConverter>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace {
/** 索引文件允许的最大字节数，避免异常文件占用过多内存。 */
constexpr qint64 MaximumIndexBytes = 8 * 1024 * 1024;
/** 书签数量上限，防止异常索引导致界面卡顿。 */
constexpr int MaximumItems = 500;
/** 名称与地址允许的长度。 */
constexpr int MaximumNameLength = 200;
constexpr int MaximumUrlLength = 2048;
/** 检测标题时最多接收的响应字节，超出后立即中断剩余下载。 */
constexpr qint64 MaximumProbeBytes = 512 * 1024;
/** 单次网址检测的等待上限。 */
constexpr int ProbeTimeoutMs = 15000;

/** 根据当前平台以正确的大小写规则判断路径从属关系。 */
bool isWithin(const QString& candidate, const QString& root)
{
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return candidate.compare(root, sensitivity) == 0
        || candidate.startsWith(root + QLatin1Char('/'), sensitivity);
}

/** 生成只含十六进制字符的稳定记录标识。 */
QString newId()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

/** 当前 UTC 时间，采用与便签一致的时间格式。 */
QString nowText()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
}

BookmarkService::BookmarkService(const QString& dataRoot, QObject* parent)
    : QObject(parent)
    , m_bookmarkRoot(QDir::cleanPath(QFileInfo(dataRoot).absoluteFilePath()) + QStringLiteral("/bookmark"))
    , m_indexPath(m_bookmarkRoot + QStringLiteral("/bookmarks.v1.json"))
    , m_network(new QNetworkAccessManager(this))
{
    load();
}

void BookmarkService::load()
{
    if (QFileInfo::exists(m_indexPath) && !isWithin(m_indexPath, m_bookmarkRoot)) {
        m_loadError = QStringLiteral("书签数据路径包含不安全的目录链接，已停止读取和保存。");
        return;
    }
    if (!QFileInfo::exists(m_indexPath))
        return;
    QFile file(m_indexPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaximumIndexBytes) {
        m_loadError = QStringLiteral("无法读取书签索引，已保护原文件，禁止覆盖。");
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || root.value(QStringLiteral("version")).toInt() != 1
        || !root.value(QStringLiteral("items")).isArray()) {
        m_loadError = QStringLiteral("书签索引损坏或版本不支持，已保护原文件，禁止覆盖。");
        return;
    }
    static const QRegularExpression stableId(QStringLiteral("^[0-9a-f]{32}$"));
    QSet<QString> ids;
    for (const QJsonValue& value : root.value(QStringLiteral("items")).toArray()) {
        if (!value.isObject()) {
            m_loadError = QStringLiteral("书签索引包含无效记录，已禁止覆盖。");
            return;
        }
        const QJsonObject object = value.toObject();
        const Item item{object.value(QStringLiteral("id")).toString(),
                        object.value(QStringLiteral("name")).toString(),
                        object.value(QStringLiteral("url")).toString(),
                        object.value(QStringLiteral("createdAt")).toString(),
                        object.value(QStringLiteral("updatedAt")).toString()};
        const QUrl url(item.url);
        const QString scheme = url.scheme();
        if (!stableId.match(item.id).hasMatch() || ids.contains(item.id)
            || item.name.trimmed().isEmpty() || item.name.size() > MaximumNameLength
            || item.url.isEmpty() || item.url.size() > MaximumUrlLength
            || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))
            || url.host().isEmpty() || url.isRelative()) {
            m_loadError = QStringLiteral("书签索引包含无效记录，已禁止覆盖。");
            return;
        }
        ids.insert(item.id);
        m_items.append(item);
    }
    if (m_items.size() > MaximumItems) {
        m_items.clear();
        m_loadError = QStringLiteral("书签数量超过上限，已停止读取以避免数据丢失。");
    }
}

QVariantMap BookmarkService::snapshot() const
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    QVariantList items;
    for (const Item& item : m_items)
        items.append(QVariantMap{{QStringLiteral("id"), item.id}, {QStringLiteral("name"), item.name},
            {QStringLiteral("url"), item.url}, {QStringLiteral("createdAt"), item.createdAt},
            {QStringLiteral("updatedAt"), item.updatedAt}});
    return ServiceResult::success(QVariantMap{{QStringLiteral("items"), items},
        {QStringLiteral("warnings"), m_warnings}});
}

bool BookmarkService::prepareIndex(QSaveFile& file, const QVector<Item>& items, QString& error) const
{
    if (!m_loadError.isEmpty()) {
        error = m_loadError;
        return false;
    }
    if (!isWithin(m_indexPath, m_bookmarkRoot) || !QDir().mkpath(m_bookmarkRoot)) {
        error = QStringLiteral("书签索引路径不安全或目录无法创建。");
        return false;
    }
    QJsonArray array;
    for (const Item& item : items) {
        array.append(QJsonObject{{QStringLiteral("id"), item.id}, {QStringLiteral("name"), item.name},
            {QStringLiteral("url"), item.url}, {QStringLiteral("createdAt"), item.createdAt},
            {QStringLiteral("updatedAt"), item.updatedAt}});
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("items"), array}}).toJson();
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        error = QStringLiteral("无法保存书签索引：") + file.errorString();
        return false;
    }
    return true;
}

QVariantMap BookmarkService::commit(const QVector<Item>& items)
{
    QString error;
    QSaveFile file(m_indexPath);
    if (!prepareIndex(file, items, error))
        return ServiceResult::failure(error);
    if (!file.commit())
        return ServiceResult::failure(QStringLiteral("无法提交书签索引：") + file.errorString());
    m_items = items;
    emit changed();
    return ServiceResult::success();
}

int BookmarkService::position(const QString& id) const
{
    for (qsizetype index = 0; index < m_items.size(); ++index) {
        if (m_items.at(index).id == id)
            return static_cast<int>(index);
    }
    return -1;
}

QVariantMap BookmarkService::normalize(const QString& address)
{
    const QString text = address.trimmed();
    if (text.isEmpty())
        return ServiceResult::failure(QStringLiteral("请输入网页地址。"));
    if (text.size() > MaximumUrlLength)
        return ServiceResult::failure(QStringLiteral("网页地址不能超过 2048 个字符。"));
    if (text.contains(QRegularExpression(QStringLiteral("[\\x{0000}-\\x{001F}\\x{007F}\\s]"))))
        return ServiceResult::failure(QStringLiteral("网页地址不能包含空格或控制字符。"));

    // 未写明协议时按 https 补全；写明其他协议（含本地 file）一律拒绝。
    const QString lowered = text.toLower();
    const bool hasScheme = lowered.startsWith(QStringLiteral("https://"))
        || lowered.startsWith(QStringLiteral("http://"));
    if (!hasScheme) {
        if (text.contains(QStringLiteral("://")))
            return ServiceResult::failure(QStringLiteral("只支持 http 与 https 网页地址。"));
        if (text.contains(QStringLiteral("//")))
            return ServiceResult::failure(QStringLiteral("网页地址无效，请检查后重试。"));
    }
    const QUrl url(hasScheme ? text : QStringLiteral("https://") + text);
    const QString scheme = url.scheme();
    if (!url.isValid() || url.isRelative() || url.host().isEmpty()
        || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")))
        return ServiceResult::failure(QStringLiteral("无法识别这个网页地址，请输入类似 example.com 的地址。"));
    return ServiceResult::success(QVariantMap{{QStringLiteral("url"), url.toString()}});
}

QVariantMap BookmarkService::save(const QVariantMap& item)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QString requestedId = item.value(QStringLiteral("id")).toString();
    const int index = position(requestedId);
    if (!requestedId.isEmpty() && index < 0)
        return ServiceResult::failure(QStringLiteral("书签不存在，无法保存。"));
    const QString name = item.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty() || name.size() > MaximumNameLength)
        return ServiceResult::failure(QStringLiteral("书签名称不能为空，且不能超过 200 个字符。"));
    const QVariantMap address = normalize(item.value(QStringLiteral("url")).toString());
    if (!address.value(QStringLiteral("ok")).toBool())
        return address;
    const QString url = address.value(QStringLiteral("data")).toMap().value(QStringLiteral("url")).toString();
    if (index < 0 && m_items.size() >= MaximumItems)
        return ServiceResult::failure(QStringLiteral("书签数量已达上限（500 个），请先删除不再需要的书签。"));

    Item record{index < 0 ? newId() : requestedId, name, url,
                index < 0 ? nowText() : m_items.at(index).createdAt, nowText()};
    QVector<Item> items = m_items;
    if (index < 0)
        items.append(record);
    else
        items[index] = record;
    const QVariantMap result = commit(items);
    return result.value(QStringLiteral("ok")).toBool()
        ? ServiceResult::success(QVariantMap{{QStringLiteral("id"), record.id},
            {QStringLiteral("name"), record.name}, {QStringLiteral("url"), record.url},
            {QStringLiteral("createdAt"), record.createdAt}, {QStringLiteral("updatedAt"), record.updatedAt}})
        : result;
}

QVariantMap BookmarkService::remove(const QStringList& ids)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const QSet<QString> selected(ids.begin(), ids.end());
    for (const QString& id : selected) {
        if (position(id) < 0)
            return ServiceResult::failure(QStringLiteral("部分待删除书签不存在，已取消本次删除。"));
    }
    QVector<Item> items;
    for (const Item& item : m_items) {
        if (!selected.contains(item.id))
            items.append(item);
    }
    return commit(items);
}

QVariantMap BookmarkService::move(const QString& id, const QString& beforeId)
{
    if (!m_loadError.isEmpty())
        return ServiceResult::failure(m_loadError);
    const int index = position(id);
    if (index < 0 || beforeId == id)
        return ServiceResult::failure(QStringLiteral("书签不存在，无法调整顺序。"));
    qsizetype insertion = m_items.size();
    if (!beforeId.isEmpty()) {
        const int beforePosition = position(beforeId);
        if (beforePosition < 0)
            return ServiceResult::failure(QStringLiteral("排序目标不存在。"));
        insertion = beforePosition;
    }
    QVector<Item> items = m_items;
    const Item item = items.takeAt(index);
    // 取出后目标位置可能因前移而减小一位。
    if (insertion > index && !beforeId.isEmpty())
        --insertion;
    items.insert(insertion, item);
    return commit(items);
}

QString BookmarkService::charsetName(const QByteArray& bytes, const QString& contentType)
{
    static const QRegularExpression fromHeader(QStringLiteral("charset\\s*=\\s*\"?([A-Za-z0-9_.:+-]+)"),
        QRegularExpression::CaseInsensitiveOption);
    auto match = fromHeader.match(contentType);
    if (match.hasMatch())
        return match.captured(1);

    // 标签本身使用 ASCII，直接按字节在前若干 KB 中查找 meta 声明。
    const QString head = QString::fromLatin1(bytes.constData(), qMin(bytes.size(), qint64(4096)));
    static const QRegularExpression metaCharset(QStringLiteral("<meta[^>]*charset\\s*=\\s*\"?([A-Za-z0-9_.:+-]+)"),
        QRegularExpression::CaseInsensitiveOption);
    match = metaCharset.match(head);
    if (match.hasMatch())
        return match.captured(1);
    static const QRegularExpression metaType(QStringLiteral("<meta[^>]*http-equiv\\s*=\\s*[\"']?content-type[\"']?[^>]*content\\s*=\\s*[\"']?[^\"'>]"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch anchor = metaType.match(head);
    if (anchor.hasMatch()) {
        match = fromHeader.match(head.mid(anchor.capturedStart(), anchor.capturedLength() + 120));
        if (match.hasMatch())
            return match.captured(1);
    }
    return {};
}

QString BookmarkService::decodeBytes(const QByteArray& bytes, const QString& charset)
{
    if (!charset.isEmpty()) {
        auto decoder = QStringDecoder(charset.toLatin1().constData());
        if (decoder.isValid())
            return decoder(bytes);
    }
    return QString::fromUtf8(bytes);
}

QString BookmarkService::decodeEntities(const QString& text)
{
    QString result;
    result.reserve(text.size());
    for (qsizetype index = 0; index < text.size();) {
        const QChar current = text.at(index);
        if (current != QLatin1Char('&')) {
            result.append(current);
            ++index;
            continue;
        }
        const qsizetype end = text.indexOf(QLatin1Char(';'), index + 1);
        if (end < 0 || end - index > 16) {
            result.append(QLatin1Char('&'));
            ++index;
            continue;
        }
        const QString entity = text.mid(index + 1, end - index - 1);
        const qsizetype cleaned = entity.startsWith(QLatin1Char('#')) ? 1 : 0;
        bool numeric = cleaned == 1;
        const int base = entity.startsWith(QLatin1Char('x'), Qt::CaseInsensitive)
            || entity.startsWith(QLatin1Char('X')) ? 16 : 10;
        const int code = numeric ? entity.mid(cleaned + (base == 16 ? 1 : 0)).toInt(&numeric, base) : 0;
        // 非法数值回落为已命名实体，仍无法识别时保留原字符。
        if (!numeric || code <= 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            static const QHash<QString, QChar> named{
                {QStringLiteral("amp"), QLatin1Char('&')}, {QStringLiteral("lt"), QLatin1Char('<')},
                {QStringLiteral("gt"), QLatin1Char('>')}, {QStringLiteral("quot"), QLatin1Char('"')},
                {QStringLiteral("apos"), QLatin1Char('\'')}, {QStringLiteral("nbsp"), QChar(0x00A0)}};
            const auto found = named.constFind(entity.toLower());
            if (found == named.constEnd()) {
                result.append(QLatin1Char('&'));
                ++index;
                continue;
            }
            result.append(*found);
            index = end + 1;
            continue;
        }
        // fromUcs4 负责处理 BMP 之外的字符，避免出现孤立代理项。
        const char32_t codePoint = static_cast<char32_t>(code);
        result.append(QString::fromUcs4(&codePoint, 1));
        index = end + 1;
    }
    return result;
}

QString BookmarkService::htmlTitle(const QByteArray& bytes, const QString& contentType)
{
    if (bytes.isEmpty())
        return {};
    QString html = decodeBytes(bytes, charsetName(bytes, contentType));
    // 未声明编码的页面多为 UTF-8；出现替换字符时再尝试常见的中文编码。
    if (charsetName(bytes, contentType).isEmpty() && html.contains(QChar(0xFFFD)))
        html = decodeBytes(bytes, QStringLiteral("GB18030"));
    const qsizetype start = html.indexOf(QStringLiteral("<title"), 0, Qt::CaseInsensitive);
    if (start < 0)
        return {};
    const qsizetype open = html.indexOf(QLatin1Char('>'), start);
    const qsizetype close = html.indexOf(QStringLiteral("</title"), start, Qt::CaseInsensitive);
    if (open < 0 || close < 0 || close <= open)
        return {};
    QString title = html.mid(open + 1, close - open - 1);
    title = decodeEntities(title);
    title.remove(QRegularExpression(QStringLiteral("[\\x{0000}-\\x{001F}\\x{007F}]")));
    title = title.simplified();
    return title.left(MaximumNameLength);
}

QVariantMap BookmarkService::probe(const QString& address)
{
    const QString token = QString::number(++m_probeToken);
    const QVariantMap candidate = normalize(address);
    if (!candidate.value(QStringLiteral("ok")).toBool()) {
        const QString error = candidate.value(QStringLiteral("error")).toString();
        QTimer::singleShot(0, this, [this, token, error]() {
            // 失败也携带请求令牌，面板才能结束对应检测并允许重试。
            QVariantMap result = ServiceResult::failure(error);
            result.insert(QStringLiteral("data"), QVariantMap{{QStringLiteral("token"), token}});
            emit probeFinished(result);
        });
        return ServiceResult::success(QVariantMap{{QStringLiteral("token"), token}});
    }
    const QUrl url(candidate.value(QStringLiteral("data")).toMap().value(QStringLiteral("url")).toString());

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("Accept", "text/html,application/xhtml+xml,*/*;q=0.8");
    request.setRawHeader("User-Agent", "Mozilla/5.0 DesktopTool/1.0 (BookmarkProbe)");
    QNetworkReply* reply = m_network->get(request);

    // 多个 Qt 连接共享同一份状态：接收一段就尝试解析标题，命中后立即中断剩余下载。
    const auto state = std::make_shared<ProbeState>();
    auto* deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    deadline->start(ProbeTimeoutMs);
    connect(deadline, &QTimer::timeout, this, [state, reply]() {
        state->timedOut = true;
        reply->abort();
    });
    connect(reply, &QNetworkReply::readyRead, this, [state, reply]() {
        state->buffer.append(reply->readAll());
        // 取得 </title> 或超过体积上限时不再等待剩余正文。
        if (state->buffer.size() >= MaximumProbeBytes
            || state->buffer.toLower().contains("</title>")) {
            state->exhausted = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, token, reply, state]() {
        reply->deleteLater();
        const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool cancelled = reply->error() == QNetworkReply::OperationCanceledError;
        state->buffer.append(reply->readAll());
        QVariantMap data{{QStringLiteral("token"), token}, {QStringLiteral("url"), reply->url().toString()}};
        if (reply->error() != QNetworkReply::NoError && !(cancelled && state->exhausted)) {
            const QString reason = state->timedOut ? QStringLiteral("连接超时，未能打开网页。")
                : QStringLiteral("无法打开网页：") + reply->errorString();
            data.insert(QStringLiteral("available"), false);
            data.insert(QStringLiteral("error"), reason);
            emit probeFinished(ServiceResult::success(data));
            return;
        }
        if (status >= 400) {
            data.insert(QStringLiteral("available"), false);
            data.insert(QStringLiteral("error"),
                QStringLiteral("网页返回了错误状态（%1），暂时无法读取标题。").arg(status));
            emit probeFinished(ServiceResult::success(data));
            return;
        }
        const QString title = htmlTitle(state->buffer.left(MaximumProbeBytes), contentType);
        data.insert(QStringLiteral("available"), true);
        data.insert(QStringLiteral("title"), title);
        emit probeFinished(ServiceResult::success(data));
    });
    return ServiceResult::success(QVariantMap{{QStringLiteral("token"), token}});
}
