#pragma once

#include <QObject>

class Document;
class TimecodeInput;
struct Cue;

// Runs the project's cue list: GO fires the next cue, back steps to the previous one and fires it,
// and with a timecode source the cues fire as the timecode passes their times. A jump in
// timecode (a locate) moves the position without firing everything in between.
class CueList : public QObject
{
    Q_OBJECT
public:
    CueList(Document *doc, TimecodeInput *timecode, QObject *parent = nullptr);

    int current() const { return m_current; } // the last cue fired; -1 = before the first
    TimecodeInput *timecode() const { return m_timecode; }

public slots:
    void go();
    void back();
    void fire(int index);
    void setCurrent(int index); // position only, nothing fires
    void onTimecode(double seconds);

signals:
    void currentChanged(int index);
    void cueFired(int index);

private:
    void syncSource();
    void run(const Cue &cue);

    Document *m_doc;
    TimecodeInput *m_timecode;
    int m_current = -1;
    double m_lastTimecode = -1;
    int m_appliedSource = -1;
};
