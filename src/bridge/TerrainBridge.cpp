#include "app/InterfaceLanguage.h"
#include "TerrainBridge.h"
#include "services/ServiceResult.h"
#include "services/TerrainService.h"

#include <QFileDialog>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>

TerrainBridge::TerrainBridge(TerrainService* service, const QString& projectId,
                             QWidget* dialogParent, QObject* parent)
    : QObject(parent), m_service(service), m_projectId(projectId), m_dialogParent(dialogParent)
{
}

QVariantMap TerrainBridge::getInitialData() const
{
    return m_service->readMeta(m_projectId);
}

QVariantMap TerrainBridge::openHeightStream()
{
    return m_service->openHeightStream(m_projectId);
}

QVariantMap TerrainBridge::readHeightChunk(const QString& token, int index)
{
    return m_service->readHeightChunk(token, index);
}

QVariantMap TerrainBridge::beginSave(int gridWidth, int gridDepth, double spacing, double undoSteps)
{
    return m_service->beginHeightWrite(m_projectId, gridWidth, gridDepth, spacing, undoSteps);
}

QVariantMap TerrainBridge::writeHeightChunk(const QString& token, int index, const QString& base64)
{
    return m_service->writeHeightChunk(token, index, base64);
}

QVariantMap TerrainBridge::commitSave(const QString& token)
{
    const QVariantMap result = m_service->commitHeightWrite(token);
    if (result.value(QStringLiteral("ok")).toBool()) {
        m_dirty = false;
        m_savePending = false;
        emit saved();
    }
    return result;
}

QVariantMap TerrainBridge::cancelStream(const QString& token)
{
    return m_service->cancelStream(token);
}

QVariantMap TerrainBridge::setDirty(bool dirty)
{
    m_dirty = dirty;
    return ServiceResult::success();
}

QVariantMap TerrainBridge::chooseExportTarget(const QString& suggestedName)
{
    // 导出文件名仅保留文件系统允许的字符；支持中文路径。
    QString name = suggestedName.trimmed();
    for (const QChar character : QStringLiteral("\\/:*?\"<>|"))
        name.remove(character);
    if (name.isEmpty())
        name = QStringLiteral("地形高度图");
    const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString suggested = QDir(pictures.isEmpty() ? QDir::homePath() : pictures)
                                  .filePath(name + QStringLiteral(".png"));
    const QString path = QFileDialog::getSaveFileName(m_dialogParent,
        InterfaceLanguage::text(QStringLiteral("导出 16 位灰度高度图")), suggested,
        InterfaceLanguage::text(QStringLiteral("PNG 图片 (*.png);;所有文件 (*.*)")));
    if (path.isEmpty())
        return ServiceResult::success(QVariantMap{{QStringLiteral("cancelled"), true}});
    return ServiceResult::success(QVariantMap{{QStringLiteral("target"),
        QDir::cleanPath(QFileInfo(path).absoluteFilePath())}});
}

QVariantMap TerrainBridge::beginExport(const QString& filePath, int pngWidth, int pngHeight,
                                       double spacing, double minHeight, double maxHeight)
{
    return m_service->beginExport(m_projectId, filePath, pngWidth, pngHeight, spacing,
        minHeight, maxHeight);
}

QVariantMap TerrainBridge::writeExportChunk(const QString& token, int index, const QString& base64)
{
    return m_service->writeExportChunk(token, index, base64);
}

QVariantMap TerrainBridge::commitExport(const QString& token)
{
    return m_service->commitExport(token);
}

QVariantMap TerrainBridge::requestClose()
{
    emit closeRequested();
    return ServiceResult::success();
}
