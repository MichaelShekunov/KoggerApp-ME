#include "screen_keyboard.h"

#include "input_device_tracker.h"

#include <QGuiApplication>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QQuickItem>
#include <QSettings>
#include <QStyleHints>
#include <QTouchEvent>
#include <QVariant>

namespace {

constexpr auto kSettingsKey = "main/ui/screenKeyboardMode";

bool isEditableTextInput(QObject* object)
{
    if (!object)
        return false;
    if (!object->inherits("QQuickTextInput") && !object->inherits("QQuickTextEdit"))
        return false;
    if (object->property("readOnly").toBool())
        return false;
    const QVariant enabled = object->property("enabled");
    return !enabled.isValid() || enabled.toBool();
}

} // namespace

ScreenKeyboard::ScreenKeyboard(InputDeviceTracker* tracker, QObject* parent)
    : QObject(parent)
    , m_tracker(tracker)
{
    const int stored = QSettings().value(QLatin1String(kSettingsKey), int(Auto)).toInt();
    m_mode = (stored >= Auto && stored <= Off) ? stored : int(Auto);

    if (auto* app = qGuiApp) {
        connect(app, &QGuiApplication::focusObjectChanged, this, &ScreenKeyboard::onFocusObjectChanged);
        onFocusObjectChanged(app->focusObject());
        app->installEventFilter(this);
    }
    if (m_tracker)
        connect(m_tracker, &InputDeviceTracker::currentModeChanged, this, &ScreenKeyboard::emitIfChanged);
}

int ScreenKeyboard::mode() const
{
    return m_mode;
}

void ScreenKeyboard::setMode(int mode)
{
    if (mode < Auto || mode > Off || mode == m_mode)
        return;
    m_mode = mode;
    QSettings().setValue(QLatin1String(kSettingsKey), m_mode);
    emit modeChanged();
    emitIfChanged();
}

bool ScreenKeyboard::inputActive() const
{
    return m_inputActive;
}

bool ScreenKeyboard::visible() const
{
    return computeVisible();
}

bool ScreenKeyboard::passwordMode() const
{
    return m_password;
}

bool ScreenKeyboard::numericMode() const
{
    return m_numeric;
}

QRectF ScreenKeyboard::focusRect() const
{
    return m_focusRect;
}

void ScreenKeyboard::sendText(const QString& text)
{
    if (!m_focus || text.isEmpty())
        return;
    QInputMethodEvent event;
    event.setCommitString(text);
    m_focus->event(&event);
}

void ScreenKeyboard::sendKey(int qtKey, int modifiers)
{
    if (!m_focus)
        return;
    QString text;
    if (qtKey == Qt::Key_Return || qtKey == Qt::Key_Enter)
        text = QStringLiteral("\r");
    else if (qtKey == Qt::Key_Tab)
        text = QStringLiteral("\t");
    else if (qtKey == Qt::Key_Space)
        text = QStringLiteral(" ");
    const auto mods = Qt::KeyboardModifiers(modifiers);
    QKeyEvent press(QEvent::KeyPress, qtKey, mods, text);
    m_focus->event(&press);
    QKeyEvent release(QEvent::KeyRelease, qtKey, mods, text);
    m_focus->event(&release);
}

void ScreenKeyboard::hide()
{
    if (m_dismissed)
        return;
    m_dismissed = true;
    emitIfChanged();
}

void ScreenKeyboard::show()
{
    if (!m_dismissed)
        return;
    m_dismissed = false;
    emitIfChanged();
}

void ScreenKeyboard::updateFocusRect()
{
    refreshFocusInfo();
    emit stateChanged();
}

void ScreenKeyboard::onFocusObjectChanged(QObject* object)
{
    if (object == m_focus.data())
        return;
    m_focus = object;
    m_dismissed = false;
    // A press focuses the field before anyone knows whether it starts a scroll: wait for the touch to end.
    m_gate.focusChanged(isEditableTextInput(object), m_lastVisible);
    refreshFocusInfo();
    emit stateChanged();
    m_lastVisible = computeVisible();
}

bool ScreenKeyboard::eventFilter(QObject* watched, QEvent* event)
{
    // The window's own touch events only: an item gets the same touch (or a mouse event made of it) after it.
    if (!watched || !watched->isWindowType())
        return QObject::eventFilter(watched, event);

    const auto type = event->type();
    if (type != QEvent::TouchBegin && type != QEvent::TouchUpdate && type != QEvent::TouchEnd && type != QEvent::TouchCancel)
        return QObject::eventFilter(watched, event);
    const auto* touch = static_cast<QTouchEvent*>(event);
    switch (type) {
    case QEvent::TouchBegin:
        m_gate.touchBegan(touch->points().isEmpty() ? QPointF() : touch->points().first().position(),
                          int(touch->points().size()));
        break;
    case QEvent::TouchUpdate:
        // the drag distance is what a Flickable takes for a drag
        m_gate.touchMoved(touch->points().isEmpty() ? m_gate.lastPoint() : touch->points().first().position(),
                          int(touch->points().size()), QGuiApplication::styleHints()->startDragDistance());
        break;
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        onTouchFinished(m_gate.touchEnded(touch->points().isEmpty() ? m_gate.lastPoint() : touch->points().first().position(),
                                          event->type() == QEvent::TouchCancel));
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void ScreenKeyboard::onTouchFinished(bool tap)
{
    if (!m_gate.heldBack() || !tap)
        return;
    // The tap that focused the field, or a later tap on the field itself (where it is now, after the scroll);
    // a tap may end a few pixels off the field it pressed.
    auto* item = qobject_cast<QQuickItem*>(m_focus.data());
    if (!item)
        return;
    const QRectF field = item->mapRectToScene(QRectF(0.0, 0.0, item->width(), item->height()));
    if (!m_gate.releaseOnTap(tap, field, QGuiApplication::styleHints()->startDragDistance()))
        return;
    refreshFocusInfo();
    emit stateChanged();
    m_lastVisible = computeVisible();
}

void ScreenKeyboard::refreshFocusInfo()
{
    m_inputActive = isEditableTextInput(m_focus);
    m_password = false;
    m_numeric = false;
    m_focusRect = QRectF();
    if (!m_inputActive)
        return;

    m_password = m_focus->property("echoMode").toInt() != 0;

    bool ok = false;
    const int hints = m_focus->property("inputMethodHints").toInt(&ok);
    if (ok) {
        const int numericHints = int(Qt::ImhDigitsOnly | Qt::ImhFormattedNumbersOnly
                                     | Qt::ImhDialableCharactersOnly | Qt::ImhPreferNumbers);
        m_numeric = (hints & numericHints) != 0;
    }

    if (auto* item = qobject_cast<QQuickItem*>(m_focus.data()))
        m_focusRect = item->mapRectToScene(QRectF(0.0, 0.0, item->width(), item->height()));
}

bool ScreenKeyboard::computeVisible() const
{
#ifdef Q_OS_ANDROID
    // Android raises its own input method when a text field takes focus, and every
    // input there is touch, so Auto would open this panel on top of it: two
    // keyboards for one field. The panel exists for the head unit, which has none.
    return false;
#else
    if (!m_inputActive || m_dismissed || m_gate.heldBack() || m_mode == Off)
        return false;
    if (m_mode == On)
        return true;
    return m_tracker && m_tracker->touchMode();
#endif
}

void ScreenKeyboard::emitIfChanged()
{
    const bool now = computeVisible();
    if (now == m_lastVisible)
        return;
    m_lastVisible = now;
    emit stateChanged();
}
