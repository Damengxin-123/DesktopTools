#include "services/FileSearchIndex.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSet>
#include <QStringMatcher>
#include <QtEndian>

namespace
{
// 索引文件的魔数与版本；结构变化时递增版本使旧缓存自动失效。
const char IndexMagic[] = "DTFSIDX";
constexpr quint32 IndexVersion = 2;
// 记录键低 56 位为载荷；扫描键再叠加 ScanKeyFlag 特征位。
constexpr quint64 KeyPayloadMask = 0x00FFFFFFFFFFFFFFull;
// 扫描键的特征位：保证与卷内 0-15 号元数据记录键永不冲突。
constexpr quint64 ScanKeyFlag = 0x0080000000000000ull;
// NTFS 文件引用号低 48 位为记录号，高 16 位为序号；仅取记录号保证稳定。
constexpr quint64 FrnMask = 0x0000FFFFFFFFFFFFull;

// ASCII 词元使用直接 UTF-16 比较，非 ASCII 字符仍按 Unicode 折叠以保持匹配语义。
bool containsAscii(const QString& text, const QByteArray& needle)
{
    const ushort* source = text.utf16();
    const auto* pattern = reinterpret_cast<const uchar*>(needle.constData());
    const qsizetype length = needle.size();
    const auto fold = [](ushort character) -> ushort {
        if (character >= 'A' && character <= 'Z') return ushort(character + ('a' - 'A'));
        return character < 128 ? character : QChar::toCaseFolded(character);
    };
    for (qsizetype start = 0; start + length <= text.size(); ++start) {
        if (fold(source[start]) != pattern[0]) continue;
        qsizetype matched = 1;
        while (matched < length && fold(source[start + matched]) == pattern[matched]) ++matched;
        if (matched == length) return true;
    }
    return false;
}

// 逐字符折叠大小写比较，避免为匹配构造整串小写副本。
bool charsEqualIgnoreCase(ushort left, ushort right)
{
    if (left == right)
        return true;
    return QChar::toLower(left) == QChar::toLower(right);
}
}

quint64 FileSearchIndex::key(int driveIndex, quint64 frn)
{
    // MFT 引用号在调用侧已截取低 48 位；这里保留完整 56 位载荷供扫描键使用。
    return (quint64(driveIndex) << 56) | (frn & KeyPayloadMask);
}

quint64 FileSearchIndex::scanKey(int driveIndex, const QString& nativePath)
{
    // FNV-1a：路径大小写不敏感，重命名替换盘符外的路径才会改变键。
    const QString lowered = nativePath.toLower();
    quint64 hash = 1469598103934665603ull;
    const ushort* data = lowered.utf16();
    const int length = lowered.size();
    for (int i = 0; i < length; ++i) {
        hash ^= quint64(data[i]);
        hash *= 1099511628211ull;
    }
    return key(driveIndex, (hash & KeyPayloadMask) | ScanKeyFlag);
}

bool FileSearchIndex::applyJournalRecords(int drive, const QByteArray& bytes, bool enumeration, QString* error)
{
    // V2 固定头为 60 字节，后续是 UTF-16 文件名；先验证整个批次再改变索引。
    if (bytes.size() < 8) {
        if (error) *error = QStringLiteral("USN 数据缺少断点。");
        return false;
    }
    for (qsizetype offset = 8; offset < bytes.size();) {
        const auto* data = reinterpret_cast<const uchar*>(bytes.constData() + offset);
        if (bytes.size() - offset < 60) {
            if (error) *error = QStringLiteral("USN 记录头不完整。");
            return false;
        }
        const quint32 length = qFromLittleEndian<quint32>(data);
        const quint16 version = qFromLittleEndian<quint16>(data + 4);
        const quint16 nameLength = qFromLittleEndian<quint16>(data + 56);
        const quint16 nameOffset = qFromLittleEndian<quint16>(data + 58);
        if (version != 2 || length < 60 || length % 8 || length > bytes.size() - offset
            || nameOffset < 60 || nameOffset % 2 || nameLength % 2 || !nameLength
            || quint32(nameOffset) + nameLength > length) {
            if (error) *error = QStringLiteral("USN 记录版本或边界无效。");
            return false;
        }
        offset += length;
    }
    QVector<quint64> deleted;
    for (qsizetype offset = 8; offset < bytes.size();) {
        const auto* data = reinterpret_cast<const uchar*>(bytes.constData() + offset);
        const quint32 length = qFromLittleEndian<quint32>(data);
        const quint64 reference = qFromLittleEndian<quint64>(data + 8);
        quint64 parent = qFromLittleEndian<quint64>(data + 16) & FrnMask;
        const quint64 frn = reference & FrnMask;
        const quint16 sequence = quint16(reference >> 48);
        const quint32 reason = qFromLittleEndian<quint32>(data + 40);
        const quint32 attributes = qFromLittleEndian<quint32>(data + 52);
        const quint16 nameLength = qFromLittleEndian<quint16>(data + 56);
        const quint16 nameOffset = qFromLittleEndian<quint16>(data + 58);
        offset += length;
        if (frn < 16) continue;
        const quint64 recordKey = key(drive, frn);
        const auto* previous = record(recordKey);
        if (!enumeration && (reason & 0x00000200u)) {
            // FILE_DELETE 与 CREATE 可以同时出现；删除必须优先且校验序号。
            if (previous && previous->sequence == sequence) deleted.append(recordKey);
            continue;
        }
        if (!enumeration && !(reason & (0x00000100u | 0x00002000u))) continue;
        // 提交此前的删除后再新增，保证同一批次中的槽位复用按日志顺序生效。
        if (!deleted.isEmpty()) { remove(deleted); deleted.clear(); previous = record(recordKey); }
        if (previous && previous->sequence != sequence) remove({recordKey});
        if (parent == 5) parent = 0;
        const QString name = QString::fromUtf16(reinterpret_cast<const char16_t*>(data + nameOffset), nameLength / 2);
        upsert(recordKey, key(drive, parent), name, (attributes & 0x10u) != 0);
        m_records[recordKey].sequence = sequence;
    }
    if (!deleted.isEmpty()) remove(deleted);
    return true;
}

void FileSearchIndex::clear()
{
    ++m_revision;
    m_cachedHits.clear();
    m_records.clear();
    m_fileCount = 0;
    m_directoryCount = 0;
}

void FileSearchIndex::upsert(quint64 key, quint64 parentKey, const QString& name, bool isDirectory)
{
    const auto iterator = m_records.find(key);
    if (iterator != m_records.end()) {
        if (iterator->parent == parentKey && iterator->name == name && iterator->isDirectory == isDirectory)
            return;
        ++m_revision;
        iterator->nameMask = nameMask(name);
        if (iterator->isDirectory != isDirectory) {
            if (isDirectory) { --m_fileCount; ++m_directoryCount; }
            else { --m_directoryCount; ++m_fileCount; }
            iterator->isDirectory = isDirectory;
        }
        iterator->parent = parentKey;
        iterator->name = name;
        return;
    }
    ++m_revision;
    FileSearchRecord record;
    record.nameMask = nameMask(name);
    record.parent = parentKey;
    record.name = name;
    record.isDirectory = isDirectory;
    m_records.insert(key, record);
    if (isDirectory)
        ++m_directoryCount;
    else
        ++m_fileCount;
}

QVector<quint64> FileSearchIndex::remove(const QVector<quint64>& keys)
{
    QSet<quint64> doomed(keys.cbegin(), keys.cend());
    bool hasDirectory = false;
    for (const quint64 key : keys) {
        const auto iterator = m_records.constFind(key);
        // 缺失父节点仍可能留下孤儿，需保留清理孤儿的语义。
        hasDirectory = hasDirectory || iterator == m_records.constEnd() || iterator->isDirectory;
    }
    if (hasDirectory) {
        // 仅构造目录关系，广度遍历闭包后一次清理文件，避免逐层扫描全表。
        QMultiHash<quint64, quint64> directories;
        for (auto iterator = m_records.constBegin(); iterator != m_records.constEnd(); ++iterator) {
            if (iterator->isDirectory)
                directories.insert(iterator->parent, iterator.key());
        }
        QVector<quint64> pending(keys);
        while (!pending.isEmpty()) {
            const quint64 parent = pending.takeLast();
            const auto range = directories.equal_range(parent);
            for (auto iterator = range.first; iterator != range.second; ++iterator) {
                if (!doomed.contains(iterator.value())) {
                    doomed.insert(iterator.value());
                    pending.append(iterator.value());
                }
            }
        }
        for (auto iterator = m_records.constBegin(); iterator != m_records.constEnd(); ++iterator) {
            if (doomed.contains(iterator->parent))
                doomed.insert(iterator.key());
        }
    }
    QVector<quint64> removed;
    removed.reserve(doomed.size());
    for (const quint64 key : std::as_const(doomed)) {
        const auto iterator = m_records.find(key);
        if (iterator == m_records.end())
            continue;
        if (iterator->isDirectory)
            --m_directoryCount;
        else
            --m_fileCount;
        ++m_revision;
        m_records.erase(iterator);
        removed.append(key);
    }
    return removed;
}

bool FileSearchIndex::contains(quint64 key) const
{
    return m_records.contains(key);
}

const FileSearchRecord* FileSearchIndex::record(quint64 key) const
{
    const auto iterator = m_records.constFind(key);
    return iterator != m_records.constEnd() ? &iterator.value() : nullptr;
}

QVector<quint64> FileSearchIndex::childKeys(quint64 parentKey) const
{
    QVector<quint64> children;
    for (auto iterator = m_records.constBegin(); iterator != m_records.constEnd(); ++iterator) {
        if (iterator->parent == parentKey)
            children.append(iterator.key());
    }
    return children;
}

QVector<quint64> FileSearchIndex::volumeKeys(int driveIndex) const
{
    const quint64 prefix = quint64(driveIndex) << 56;
    QVector<quint64> keys;
    for (auto iterator = m_records.constBegin(); iterator != m_records.constEnd(); ++iterator) {
        if ((iterator.key() & 0xFF00000000000000ull) == prefix)
            keys.append(iterator.key());
    }
    return keys;
}

void FileSearchIndex::dropOrphans()
{
    QVector<quint64> orphans;
    for (auto iterator = m_records.constBegin(); iterator != m_records.constEnd(); ++iterator) {
        if (iterator->parent != 0 && !m_records.contains(iterator->parent))
            orphans.append(iterator.key());
    }
    if (!orphans.isEmpty())
        remove(orphans);
}

QString FileSearchIndex::pathOf(quint64 key) const
{
    // 根记录的名称即卷前缀（"C:" 或注入目录），向上回溯后逆序拼接。
    QVector<const FileSearchRecord*> chain;
    quint64 current = key;
    for (int depth = 0; depth < 96; ++depth) {
        const auto iterator = m_records.constFind(current);
        if (iterator == m_records.constEnd())
            return QString();
        chain.append(&iterator.value());
        if (iterator->parent == 0)
            break;
        current = iterator->parent;
        if (depth == 95)
            return QString();
    }
    QString path = chain.last()->name;
    for (int i = chain.size() - 2; i >= 0; --i)
        path += QLatin1Char('\\') + chain[i]->name;
    return path;
}

bool FileSearchIndex::Query::isEmpty() const
{
    return substrings.isEmpty() && wildcards.isEmpty() && extensions.isEmpty()
        && !foldersOnly && !filesOnly;
}

FileSearchIndex::Query FileSearchIndex::Query::parse(const QString& raw, bool searchPath)
{
    Query query;
    query.text = raw.trimmed();
    query.searchPath = searchPath;
    // 按空白拆分（QChar::isSpace 覆盖制表符等）。
    QVector<QString> tokens;
    QString buffer;
    for (const QChar character : query.text) {
        if (character.isSpace()) {
            if (!buffer.isEmpty()) { tokens.append(buffer); buffer.clear(); }
        } else {
            buffer.append(character);
        }
    }
    if (!buffer.isEmpty())
        tokens.append(buffer);
    for (const QString& token : std::as_const(tokens)) {
        if (token.compare(QLatin1String("path:"), Qt::CaseInsensitive) == 0) {
            query.searchPath = true;
            continue;
        }
        if (token.compare(QLatin1String("folder:"), Qt::CaseInsensitive) == 0) {
            query.foldersOnly = true;
            continue;
        }
        if (token.compare(QLatin1String("file:"), Qt::CaseInsensitive) == 0) {
            query.filesOnly = true;
            continue;
        }
        if (token.startsWith(QLatin1String("ext:"), Qt::CaseInsensitive)) {
            const QString value = token.mid(4).toLower();
            for (const QString& part : value.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                QString extension = part.trimmed();
                while (extension.startsWith(QLatin1Char('.')))
                    extension.remove(0, 1);
                if (!extension.isEmpty())
                    query.extensions.append(extension);
            }
            continue;
        }
        if (token.contains(QLatin1Char('*')) || token.contains(QLatin1Char('?')))
            query.wildcards.append(token.toLower());
        else if (!token.isEmpty())
            query.substrings.append(token);
    }
    return query;
}

bool FileSearchIndex::matchTokens(const QString& text, const Query& query)
{
    for (const QString& token : query.substrings) {
        if (!text.contains(token, Qt::CaseInsensitive))
            return false;
    }
    for (const QString& pattern : query.wildcards) {
        if (!wildcardMatch(text, pattern))
            return false;
    }
    return true;
}

bool FileSearchIndex::wildcardMatch(const QString& text, const QString& pattern)
{
    // 经典回退算法：记录最近一个 * 的位置，失配时回溯重试。
    const ushort* t = text.utf16();
    const int tLength = text.size();
    const ushort* p = pattern.utf16();
    const int pLength = pattern.size();
    int tIndex = 0;
    int pIndex = 0;
    int starIndex = -1;
    int starMatch = 0;
    while (tIndex < tLength) {
        if (pIndex < pLength && (p[pIndex] == '?' || charsEqualIgnoreCase(p[pIndex], t[tIndex]))) {
            ++tIndex;
            ++pIndex;
        } else if (pIndex < pLength && p[pIndex] == '*') {
            starIndex = pIndex;
            starMatch = tIndex;
            ++pIndex;
        } else if (starIndex >= 0) {
            pIndex = starIndex + 1;
            tIndex = ++starMatch;
        } else {
            return false;
        }
    }
    while (pIndex < pLength && p[pIndex] == '*')
        ++pIndex;
    return pIndex == pLength;
}

quint32 FileSearchIndex::nameMask(const QString& text)
{
    quint32 result = 0;
    uint previous = 0, beforePrevious = 0;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i).isSurrogate())
            return ~quint32(0);
        const uint folded = text.at(i).toCaseFolded().unicode();
        if (i >= 2)
            result |= quint32(1) << ((beforePrevious * 31u + previous * 17u + folded) & 31u);
        beforePrevious = previous;
        previous = folded;
    }
    return result;
}

QVector<quint64> FileSearchIndex::search(const Query& query, int offset, int limit,
                                         int* total, int* collected,
                                         const std::function<bool()>& cancelled) const
{
    if (total) *total = 0;
    if (collected) *collected = 0;
    if (query.isEmpty() || limit <= 0 || (cancelled && cancelled()))
        return {};
    offset = qMax(0, offset);
    const bool cached = m_cachedRevision == m_revision
        && query.searchPath == m_cachedQuery.searchPath
        && query.substrings == m_cachedQuery.substrings && query.wildcards == m_cachedQuery.wildcards
        && query.extensions == m_cachedQuery.extensions && query.filesOnly == m_cachedQuery.filesOnly
        && query.foldersOnly == m_cachedQuery.foldersOnly;
    if (!cached) {
        // 精确匹配器只编译一次；指纹预筛避免每条文件名都执行字符串搜索。
        // 每个词元只编译一次；ASCII 快速路径保留 Unicode 特殊字符的折叠规则。
        struct Matcher {
            QStringMatcher unicode; // 通用 Unicode 匹配器。
            QByteArray ascii; // 非空时使用直接比较。
        };
        QVector<Matcher> matchers;
        quint32 requiredMask = 0;
        for (const auto& token : query.substrings) {
            const bool ascii = std::all_of(token.cbegin(), token.cend(), [](QChar c) { return c.unicode() < 128; });
            matchers.append({QStringMatcher(token, Qt::CaseInsensitive), ascii ? token.toLatin1().toLower() : QByteArray()});
            const quint32 mask = nameMask(token);
            if (mask != ~quint32(0)) requiredMask |= mask;
        }
        QStringList extensions;
        for (const auto& extension : query.extensions)
            extensions.append(QLatin1Char('.') + extension);
        // 路径只在本次查询按父目录复用，并限制缓存容量。
        QHash<quint64, QString> parentPaths;
        const auto makePath = [this, &parentPaths](const FileSearchRecord& record) {
            if (record.parent == 0) return record.name;
            auto parent = parentPaths.constFind(record.parent);
            QString prefix = parent != parentPaths.constEnd() ? parent.value() : pathOf(record.parent);
            if (parent == parentPaths.constEnd() && parentPaths.size() < 32768)
                parentPaths.insert(record.parent, prefix);
            return prefix.isEmpty() ? record.name : prefix + QLatin1Char('\\') + record.name;
        };
        // 仅保留展示上限内的路径，完成排序后只缓存记录键。
        struct Hit {
            quint64 key; // 命中的索引键。
            QString path; // 本次排序需要的完整路径。
        };
        QVector<Hit> hits;
        hits.reserve(CollectionCap);
        int matched = 0, checked = 0;
        for (auto iterator = m_records.constBegin(); iterator != m_records.constEnd(); ++iterator) {
            if ((++checked & 511) == 0 && cancelled && cancelled()) return {};
            const auto& record = iterator.value();
            if ((query.foldersOnly && !record.isDirectory) || (query.filesOnly && record.isDirectory))
                continue;
            if (!query.searchPath && (record.nameMask & requiredMask) != requiredMask)
                continue;
            bool allowed = extensions.isEmpty();
            for (const auto& extension : extensions) {
                if (record.name.endsWith(extension, Qt::CaseInsensitive)) { allowed = true; break; }
            }
            if (!allowed) continue;
            QString path;
            if (query.searchPath) path = makePath(record);
            const QString& text = query.searchPath ? path : record.name;
            for (const auto& matcher : matchers) {
                if (matcher.ascii.isEmpty() ? matcher.unicode.indexIn(text) < 0 : !containsAscii(text, matcher.ascii)) { allowed = false; break; }
            }
            if (!allowed) continue;
            for (const auto& pattern : query.wildcards) {
                if (!wildcardMatch(text, pattern)) { allowed = false; break; }
            }
            if (!allowed) continue;
            ++matched;
            if (hits.size() < CollectionCap)
                hits.append({iterator.key(), query.searchPath ? path : makePath(record)});
        }
        if (cancelled && cancelled()) return {};
        std::sort(hits.begin(), hits.end(), [](const Hit& left, const Hit& right) {
            const int compared = left.path.compare(right.path, Qt::CaseInsensitive);
            return compared != 0 ? compared < 0 : left.key < right.key;
        });
        m_cachedHits.clear();
        m_cachedHits.reserve(hits.size());
        for (const auto& hit : hits) m_cachedHits.append(hit.key);
        m_cachedQuery = query;
        m_cachedTotal = matched;
        m_cachedRevision = m_revision;
    }
    if (total) *total = m_cachedTotal;
    if (collected) *collected = m_cachedHits.size();
    return m_cachedHits.mid(offset, limit);
}

namespace
{
// 写入 UTF-16 字符串：长度（字符数）加原始字节，避免 Qt 流版本差异。
void writeString(QDataStream& stream, const QString& value)
{
    const ushort* data = value.utf16();
    const int length = value.size();
    stream << quint32(length);
    if (length > 0)
        stream.writeRawData(reinterpret_cast<const char*>(data), length * int(sizeof(ushort)));
}

QString readString(QDataStream& stream)
{
    quint32 length = 0;
    stream >> length;
    if (length == 0 || length > 65535)
        return QString();
    QByteArray bytes(int(length) * int(sizeof(ushort)), Qt::Uninitialized);
    if (stream.readRawData(bytes.data(), bytes.size()) != bytes.size())
        return QString();
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(bytes.constData()), int(length));
}
}

bool FileSearchIndex::save(const QString& filePath, const QVector<FileSearchVolume>& volumes,
                           const FileSearchIndex& index, QString* error)
{
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData(IndexMagic, 7);
    stream << IndexVersion;
    stream << quint32(volumes.size());
    for (const FileSearchVolume& volume : volumes) {
        stream << quint8(volume.driveIndex) << quint8(volume.mftMode ? 1 : 0);
        writeString(stream, volume.displayRoot);
        writeString(stream, volume.fileSystem);
        stream << volume.journalId << volume.nextUsn << volume.serialNumber;
    }
    stream << quint32(index.m_records.size());
    for (auto iterator = index.m_records.constBegin(); iterator != index.m_records.constEnd(); ++iterator) {
        stream << iterator.key() << iterator->parent;
        stream << quint8(iterator->isDirectory ? 1 : 0) << iterator->sequence;
        writeString(stream, iterator->name);
    }
    if (stream.status() != QDataStream::Ok || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

bool FileSearchIndex::load(const QString& filePath, QVector<FileSearchVolume>* volumes,
                           FileSearchIndex* index, QString* error)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    char magic[7] = {};
    if (stream.readRawData(magic, 7) != 7 || memcmp(magic, IndexMagic, 7) != 0) {
        if (error)
            *error = QStringLiteral("索引缓存格式无法识别。");
        return false;
    }
    quint32 version = 0;
    stream >> version;
    if (version != IndexVersion) {
        if (error)
            *error = QStringLiteral("索引缓存版本过旧，需要重建。");
        return false;
    }
    quint32 volumeCount = 0;
    stream >> volumeCount;
    QVector<FileSearchVolume> loadedVolumes;
    if (volumeCount > 26 || stream.status() != QDataStream::Ok) {
        if (error) *error = QStringLiteral("索引卷数量无效。");
        return false;
    }
    loadedVolumes.reserve(int(volumeCount));
    for (quint32 i = 0; i < volumeCount; ++i) {
        quint8 driveIndex = 0;
        quint8 mftMode = 0;
        stream >> driveIndex >> mftMode;
        FileSearchVolume volume;
        volume.driveIndex = driveIndex;
        volume.mftMode = mftMode != 0;
        volume.displayRoot = readString(stream);
        volume.fileSystem = readString(stream);
        stream >> volume.journalId >> volume.nextUsn >> volume.serialNumber;
        loadedVolumes.append(volume);
    }
    quint32 recordCount = 0;
    stream >> recordCount;
    if (recordCount > quint64(file.size()) / 21 || stream.status() != QDataStream::Ok) {
        if (error) *error = QStringLiteral("索引记录数量无效。");
        return false;
    }
    FileSearchIndex loaded;
    loaded.m_records.reserve(int(recordCount));
    for (quint32 i = 0; i < recordCount; ++i) {
        quint64 key = 0;
        quint64 parent = 0;
        quint8 flags = 0;
        stream >> key >> parent >> flags;
        FileSearchRecord record;
        stream >> record.sequence;
        record.parent = parent;
        record.isDirectory = flags != 0;
        record.name = readString(stream);
        record.nameMask = nameMask(record.name);
        if (stream.status() != QDataStream::Ok)
            break;
        if (record.isDirectory)
            ++loaded.m_directoryCount;
        else
            ++loaded.m_fileCount;
        loaded.m_records.insert(key, record);
    }
    if (stream.status() != QDataStream::Ok) {
        if (error)
            *error = QStringLiteral("索引缓存损坏，需要重建。");
        return false;
    }
    if (volumes)
        *volumes = loadedVolumes;
    if (index)
        *index = std::move(loaded);
    return true;
}
