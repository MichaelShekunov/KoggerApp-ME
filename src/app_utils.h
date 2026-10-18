#pragma once

#include <QFile>
#include <QObject>
#include <QUuid>

#include "build_version.h"

class AppUtils : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int instanceIndex READ instanceIndex CONSTANT)
    Q_PROPERTY(QString instanceSuffix READ instanceSuffix CONSTANT)
    Q_PROPERTY(QString gitRevision READ gitRevision CONSTANT)
    Q_PROPERTY(QString versionFull READ versionFull CONSTANT)

public:
    explicit AppUtils(QObject *parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE QString generateUuid() const {
        return QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    QString gitRevision() const { return koggerGitRevision(); }
    // What the window's title ends with, and Qt's application display name.
    QString versionFull() const { return koggerVersionFull(); }

    Q_INVOKABLE QString licenseText(const QString& name) const {
        if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
            || name.contains(QLatin1String(".."))) {
            return QString();
        }
        QFile file(QStringLiteral(":/licenses/") + name);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString();
        }
        return QString::fromUtf8(file.readAll());
    }

    int instanceIndex() const { return instanceIndex_; }

    QString instanceSuffix() const {
        return instanceIndex_ > 1 ? QStringLiteral(" (%1)").arg(instanceIndex_) : QString();
    }

    void setInstanceIndex(int index) { instanceIndex_ = index; }

private:
    int instanceIndex_ = 1;
};
