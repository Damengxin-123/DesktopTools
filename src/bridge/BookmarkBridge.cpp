#include "BookmarkBridge.h"

#include "services/BookmarkService.h"
#include "services/ServiceResult.h"

BookmarkBridge::BookmarkBridge(BookmarkService* service, QObject* parent)
    : QObject(parent), m_service(service)
{
    connect(m_service, &BookmarkService::probeFinished, this, &BookmarkBridge::urlChecked);
    connect(m_service, &BookmarkService::changed, this, &BookmarkBridge::bookmarksChanged);
}

QVariantMap BookmarkBridge::getBookmarks() const
{
    return m_service->snapshot();
}

QVariantMap BookmarkBridge::saveBookmark(const QVariantMap& item)
{
    return m_service->save(item);
}

QVariantMap BookmarkBridge::deleteBookmarks(const QStringList& ids)
{
    return m_service->remove(ids);
}

QVariantMap BookmarkBridge::moveBookmark(const QString& id, const QString& beforeId)
{
    return m_service->move(id, beforeId);
}

QVariantMap BookmarkBridge::checkUrl(const QString& address)
{
    return m_service->probe(address);
}

QVariantMap BookmarkBridge::openUrl(const QString& address)
{
    const QVariantMap candidate = BookmarkService::normalize(address);
    if (!candidate.value(QStringLiteral("ok")).toBool())
        return candidate;
    const QString url = candidate.value(QStringLiteral("data")).toMap().value(QStringLiteral("url")).toString();
    emit openRequested(url);
    return ServiceResult::success(QVariantMap{{QStringLiteral("url"), url}});
}
