#pragma once

// Echogram editing (docs/echogram-editing.md): the bottom line drawn, erased and limited, pings
// marked bad, the recording trimmed. Every stroke of a tool is one step to undo and redo. The edits
// of an opened recording are kept in a file next to it (<recording>.kedit) and come back when it is
// opened again; the recording itself is never written. A ping marked bad or trimmed stays out of the
// mosaic, the bottom surface and the exports (Dataset::isEpochExcluded).

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

#include "echo_edit_model.h"

class Dataset;
class DataProcessor;
class Epoch;
struct ChannelId;

class EchogramEditor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool canUndo    READ canUndo   NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo    READ canRedo   NOTIFY historyChanged)
    Q_PROPERTY(int  undoKind   READ undoKind  NOTIFY historyChanged)   // echoedit::Command::Kind; -1: none
    Q_PROPERTY(int  redoKind   READ redoKind  NOTIFY historyChanged)
    Q_PROPERTY(bool magnet     READ magnet    WRITE setMagnet NOTIFY magnetChanged)
    Q_PROPERTY(bool trimmed    READ trimmed   NOTIFY stateChanged)
    Q_PROPERTY(bool hasEdits   READ hasEdits  NOTIFY stateChanged)
    Q_PROPERTY(bool saving     READ saving    NOTIFY stateChanged)     // kept in a file next to the recording
    Q_PROPERTY(QString editsFile READ editsFile NOTIFY stateChanged)

public:
    explicit EchogramEditor(QObject* parent = nullptr);
    ~EchogramEditor() override;

    void setDataset(Dataset* dataset);
    void setDataProcessor(DataProcessor* dataProcessor);

    // A stroke of a tool from the press to the release (Plot2D, the 3D bottom track): every epoch it
    // reaches is touched before it changes, and the release makes the stroke one step. A begin inside
    // a stroke joins it.
    void beginStroke(echoedit::Command::Kind kind);
    void touch(int epoch);
    void endStroke();
    bool inStroke() const { return strokeDepth_ > 0; }
    echoedit::Command::Kind strokeKind() const { return strokeKind_; }

    // Within a stroke: the "bad" mark over [from, to), shown at once (unmarking also puts the sides
    // back into the mosaic); the trim's boundary moved to an epoch (a TrimStart or TrimEnd stroke),
    // shown as the stroke goes.
    void markRange(int from, int to, bool bad);
    void moveTrim(int epoch);
    // Within a stroke of the brushes: one side (a channel's sub-channel) out of the mosaic (or back);
    // the bottom smoothed with the median of its neighbours; the gaps of the bottom closed on a straight
    // line. The new depths are the hand's, on every channel, as the pencil's.
    void setMosaicOut(int from, int to, uint8_t address, uint8_t sub, bool out);
    void smoothRange(int from, int to);
    void fillGapsRange(int from, int to);

    // Over the whole recording, one step each, the user told what was done: spikes of the bottom
    // replaced with the median around them; the turns' sides left out of the mosaic. The count of
    // pings changed.
    Q_INVOKABLE int removeSpikes();
    Q_INVOKABLE int turnsOutOfMosaic();

    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }
    int undoCount() const { return history_.undoCount(); }
    int redoCount() const { return history_.redoCount(); }
    int undoKind() const;
    int redoKind() const;
    bool magnet() const { return magnet_; }
    void setMagnet(bool on);
    bool trimmed() const;
    bool hasEdits() const { return hasEdits_; }
    bool saving() const;
    QString editsFile() const;

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void clearTrim();
    // Every edit of the recording taken back, one step (undone like any other). A file of edits that did
    // not fit the recording is then written over (the old one kept as .kedit.bak).
    Q_INVOKABLE void resetAll();

    // Core, the file's life: before it is read (what the last one had is saved first), read (before the
    // processing starts: its edits come back), saved now, closed.
    void fileOpening(const QString& path, bool append);
    void fileRead(bool processingStarted);
    void flush();
    void fileClosed();

    static QString editsPathFor(const QString& recordPath);
    // The edits of the dataset as a file holds them (also what tools/perf/echoedit_e2e.ps1 compares).
    echoedit::EditFile collect() const;
    // The bottom track over [from, to) again (after search limits were set there).
    void runBottomTrack(int from, int to) { rerunBottomTrack(from, to); }

    // The pencil's magnet at an epoch: the bottom the bottom track would find there (its filter over the
    // pings of its window, its settings) looked for within radiusM of dist; dist when none is found.
    // Plot2D gives a radius of a few pixels.
    static float magnetDistance(Dataset* dataset, int epoch, const ChannelId& ch1, uint8_t sub1,
                                const ChannelId& ch2, uint8_t sub2, float dist, float radiusM);

signals:
    void historyChanged();
    void magnetChanged();
    void stateChanged();
    // For the user (Core shows it as a notification).
    void message(const QString& text, bool warning);
    // The edits of a recording were read from its file (fileRead): how many pings they touch.
    void editsLoaded(int pings);

private:
    void onPoolCleared();
    void apply(const echoedit::Command& c, bool undo);
    void afterChange(const echoedit::Command& c, bool undo, bool toolRanTrack);
    void notifyRange(int from, int to);
    void rerunBottomTrack(int from, int to);
    void requestRebuild(bool withSurface);
    void scheduleSave();
    void save();
    int applyFile(const echoedit::EditFile& f, int* skipped);
    void clearStroke();
    // The first channel's depth of the epochs [from, to), NaN where there is none.
    QVector<float> bottomsOf(int from, int to) const;
    // A hand point at this depth on every channel of the epoch (touched first).
    void setHandBottom(int epoch, float distance);

    Dataset* dataset_ = nullptr;
    QPointer<DataProcessor> dataProcessor_;
    echoedit::History history_;

    int strokeDepth_ = 0;
    echoedit::Command::Kind strokeKind_ = echoedit::Command::Draw;
    QHash<int, echoedit::PingState> strokeBefore_;
    echoedit::Trim strokeTrimBefore_;

    QString recordPath_;
    int files_ = 0;                  // files read into the dataset since the last open that was not an append
    bool saveBlocked_ = false;       // the file next to it did not fit: it is not written over
    bool dirty_ = false;
    QTimer saveTimer_;
    bool magnet_ = false;
    bool hasEdits_ = false;
    QString lastSaveError_;
};
