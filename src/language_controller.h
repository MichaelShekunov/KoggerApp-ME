#pragma once

#include <QObject>
#include <QSettings>
#include <QTranslator>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <QString>
#include <QLocale>
#include <QGuiApplication>

#include <algorithm>
#include <iterator>

/**
 * @brief The app's UI language: the languages there are, the one in use, the switch at run time.
 *
 * A language is its code, the suffix of its translation: translations/translation_<code>.ts, compiled into
 * :/translations/translation_<code>.qm. Adding one is a line in languages() and its .ts in CMakeLists.txt's
 * qt_add_translations; the pickers (Settings, the first-launch page) list what languages() lists.
 *
 * The choice is kept by its code, main/appLanguageCode. main/appLanguage, the index into the list as it was before
 * the codes (legacyCode()), is read when there is no code yet and still written for the languages it has, so a
 * build that reads only the index opens in the same language.
 */
class LanguageController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentCode READ currentCode NOTIFY currentIndexChanged)
    Q_PROPERTY(QVariantList languages READ languageList CONSTANT)
public:
    struct Language { QString code; QString name; };

    /// The languages in the order the pickers offer them: the code, and the language's name in itself.
    static const QVector<Language>& languages() {
        static const QVector<Language> list{
            { QStringLiteral("en"), QStringLiteral("English") },
            { QStringLiteral("ru"), QStringLiteral("Русский") },   // Russian
            { QStringLiteral("pl"), QStringLiteral("Polski") },
            { QStringLiteral("zh"), QStringLiteral("中文") },                                 // Chinese
            { QStringLiteral("es"), QStringLiteral("Español") },                                 // Spanish
            { QStringLiteral("hi"), QStringLiteral("हिन्दी") },         // Hindi
        };
        return list;
    }

    /// The code main/appLanguage meant: its order before the codes, which never changes.
    static QString legacyCode(int index) {
        static const char* const order[] = { "en", "ru", "pl", "zh" };
        return (index >= 0 && index < int(std::size(order))) ? QString::fromLatin1(order[index]) : QString();
    }

    static int indexOf(const QString& code) {
        const auto& l = languages();
        for (int i = 0; i < l.size(); ++i) {
            if (l.at(i).code == code)
                return i;
        }
        return -1;
    }

    /// The language to start in: the one chosen (its code, else the old index), else the system's when the app has
    /// it, else English. A choice found only as an index is kept as its code from then on.
    static QString startupCode() {
        QSettings s;
        QString code = s.value(QStringLiteral("main/appLanguageCode")).toString();
        if (indexOf(code) < 0) {
            code = legacyCode(s.value(QStringLiteral("main/appLanguage"), -1).toInt());
            if (indexOf(code) >= 0)
                s.setValue(QStringLiteral("main/appLanguageCode"), code);
        }
        if (indexOf(code) < 0) {
            code = QLocale::system().name().split(QLatin1Char('_')).first();
            if (indexOf(code) < 0)
                code = QStringLiteral("en");
        }
        return code;
    }

    static QString translationFile(const QString& code) {
        return QStringLiteral(":/translations/translation_") + code + QStringLiteral(".qm");
    }

    explicit LanguageController(QObject* parent = nullptr) : QObject(parent), index_(std::max(0, indexOf(startupCode()))) {}

    int currentIndex() const { return index_; }
    QString currentCode() const { return languages().at(index_).code; }

    QVariantList languageList() const {
        QVariantList out;
        for (const Language& l : languages())
            out.append(QVariantMap{ { QStringLiteral("code"), l.code }, { QStringLiteral("name"), l.name } });
        return out;
    }

    // Call once after loadLanguage() so we can remove the startup translator on first switch
    void setStartupTranslator(QTranslator* t) { startupTranslator_ = t; }

    Q_INVOKABLE void apply(int i) {
        if (i < 0 || i >= languages().size() || i == index_) return;
        index_ = i;
        const QString code = languages().at(i).code;
        QSettings s;
        s.setValue(QStringLiteral("main/appLanguageCode"), code);
        for (int k = 0; !legacyCode(k).isEmpty(); ++k) {
            if (legacyCode(k) == code) {
                s.setValue(QStringLiteral("main/appLanguage"), k);
                break;
            }
        }
        if (startupTranslator_) {
            qGuiApp->removeTranslator(startupTranslator_);
            startupTranslator_ = nullptr;
        }
        qGuiApp->removeTranslator(&translator_);
        if (translator_.load(translationFile(code)))
            qGuiApp->installTranslator(&translator_);
        emit currentIndexChanged();
    }

    Q_INVOKABLE void applyCode(const QString& code) { apply(indexOf(code)); }

    /// After the settings changed under the app (an import): the language they name, else the one in use kept.
    Q_INVOKABLE void reloadFromSettings() {
        QSettings s;
        QString code = s.value(QStringLiteral("main/appLanguageCode")).toString();
        if (indexOf(code) < 0)
            code = legacyCode(s.value(QStringLiteral("main/appLanguage"), -1).toInt());
        applyCode(code);
    }

signals:
    void currentIndexChanged();
    // Emitted by main.cpp around engine.retranslate() so QML can preserve state
    // that gets reset on model rebuild (e.g. ComboBox.currentIndex when its
    // model is `[qsTr("A"), qsTr("B"), ...]`).
    void aboutToRetranslate();
    void retranslated();

private:
    QTranslator  translator_;
    QTranslator* startupTranslator_{nullptr};
    int index_{0};
};
