#pragma once

#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QString>

class InputDeviceTracker;

/// Controller behind the on-screen keyboard (qml/kqml_types/KScreenKeyboard.qml).
///
/// It watches the application focus object and reports whether an editable text input
/// (TextInput/TextField/TextEdit/TextArea) currently has focus, what kind of input it wants
/// (password, numeric) and where it is on screen, so the QML panel can open next to it.
/// Key presses coming from the panel are delivered straight to that focus object: text as an
/// input-method commit event, control keys as key events. Delivery goes through the object's
/// own event() so application-wide event filters (input-device tracking, hotkeys) never see
/// the synthetic events.
///
/// Visibility rule: visible = editable input focused AND not dismissed for this focus AND
/// (mode == On OR (mode == Auto AND the last physical input device was a touchscreen)).
/// The mode is persisted in QSettings under main/ui/screenKeyboardMode.
class ScreenKeyboard : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(bool inputActive READ inputActive NOTIFY stateChanged)
    Q_PROPERTY(bool visible READ visible NOTIFY stateChanged)
    Q_PROPERTY(bool passwordMode READ passwordMode NOTIFY stateChanged)
    Q_PROPERTY(bool numericMode READ numericMode NOTIFY stateChanged)
    Q_PROPERTY(QRectF focusRect READ focusRect NOTIFY stateChanged)

public:
    enum Mode { Auto = 0, On = 1, Off = 2 };
    Q_ENUM(Mode)

    explicit ScreenKeyboard(InputDeviceTracker* tracker, QObject* parent = nullptr);

    int mode() const;
    void setMode(int mode);

    bool inputActive() const;
    bool visible() const;
    bool passwordMode() const;
    bool numericMode() const;
    /// Bounds of the focused input in window coordinates (empty when nothing is focused).
    QRectF focusRect() const;

    /// Insert text at the cursor of the focused input.
    Q_INVOKABLE void sendText(const QString& text);
    /// Press and release a control key (Qt::Key_Backspace, Qt::Key_Return, Qt::Key_Left, ...).
    Q_INVOKABLE void sendKey(int qtKey, int modifiers = 0);
    /// Hide the panel until focus moves to another input.
    Q_INVOKABLE void hide();
    /// Show the panel again for the current input (after hide()).
    Q_INVOKABLE void show();
    /// Re-read the focused input's geometry (call after layout changes).
    Q_INVOKABLE void updateFocusRect();

signals:
    void modeChanged();
    void stateChanged();

private:
    void onFocusObjectChanged(QObject* object);
    void refreshFocusInfo();
    bool computeVisible() const;
    void emitIfChanged();

    InputDeviceTracker* m_tracker = nullptr;
    QPointer<QObject> m_focus;
    int m_mode = Auto;
    bool m_dismissed = false;
    bool m_inputActive = false;
    bool m_password = false;
    bool m_numeric = false;
    bool m_lastVisible = false;
    QRectF m_focusRect;
};
