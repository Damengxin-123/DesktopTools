#pragma once

#include <QObject>
#include <QSharedMemory>
class SingleInstanceGuard : public QObject
{
    Q_OBJECT
public:
    explicit SingleInstanceGuard(const QString& key, QObject* parent = nullptr)
        : QObject(parent), m_key(key)
    {
        m_sharedMemory.setKey(key);

        if (m_sharedMemory.attach()) {
            m_isRunning = true;
        }
        else {
            m_sharedMemory.create(1);
            m_isRunning = false;
        }
    }

    bool isRunning() const { return m_isRunning; }

private:
    QString m_key;
    QSharedMemory m_sharedMemory;
    bool m_isRunning = false;
};
