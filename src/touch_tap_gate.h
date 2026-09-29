#pragma once

#include <QPointF>
#include <QRectF>

/**
 * @brief When the on-screen keyboard may come up for a focus that arrived during a touch (ScreenKeyboard).
 *
 * Qt focuses a text input on press, so a finger that starts scrolling a page on a field focuses it (field test
 * 2026-09-29: the keyboard came up while scrolling the settings). Focus that arrives while a touch is down, with the
 * keyboard not up already, is held back: if that touch ends as a tap the keyboard comes up; after a drag (beyond the
 * drag distance), a second finger or a cancel it stays down until the field itself is tapped. Window coordinates.
 */
class TouchTapGate
{
public:
    void touchBegan(const QPointF& at, int points)
    {
        down_ = true;
        moved_ = points > 1;
        start_ = at;
        last_ = at;
    }

    void touchMoved(const QPointF& at, int points, qreal dragDistance)
    {
        if (points > 1) {
            moved_ = true;
            return;
        }
        last_ = at;
        if ((last_ - start_).manhattanLength() > dragDistance)
            moved_ = true;
    }

    /** The touch ended (canceled: taken over by the system); true when it was a tap. */
    bool touchEnded(const QPointF& at, bool canceled)
    {
        last_ = at;
        down_ = false;
        return !canceled && !moved_;
    }

    bool touchDown() const { return down_; }
    QPointF lastPoint() const { return last_; }

    /** Focus moved to another object; keyboardVisible: the keyboard is up for the previous one. */
    void focusChanged(bool editable, bool keyboardVisible)
    {
        heldBack_ = down_ && editable && !keyboardVisible;
    }

    /** After touchEnded: a tap on the focused field (`field`, where it is now; `slack` for a tap that ends a few
     *  pixels off) lets the keyboard come up. true when it did. */
    bool releaseOnTap(bool tap, const QRectF& field, qreal slack)
    {
        if (!heldBack_ || !tap || !field.adjusted(-slack, -slack, slack, slack).contains(last_))
            return false;
        heldBack_ = false;
        return true;
    }

    bool heldBack() const { return heldBack_; }

private:
    bool down_ = false;
    bool moved_ = false;
    bool heldBack_ = false;
    QPointF start_;
    QPointF last_;
};
