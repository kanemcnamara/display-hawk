#include "control/CueList.h"

#include "control/Timecode.h"
#include "model/Document.h"

#include <QSettings>

CueList::CueList(Document *doc, TimecodeInput *timecode, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_timecode(timecode)
{
    connect(m_timecode, &TimecodeInput::timecode, this, [this](double s) { onTimecode(s); });
    connect(m_doc, &Document::changed, this, &CueList::syncSource);
    syncSource();
}

void CueList::syncSource()
{
    const int source = int(m_doc->project().timecodeSource);
    if (source == m_appliedSource)
        return;
    m_appliedSource = source;
    m_lastTimecode = -1;
    m_timecode->setSource(TimecodeInput::Source(source), QSettings().value(QStringLiteral("timecode/ltcDevice")).toByteArray());
    if (m_current >= m_doc->project().cues.size())
        setCurrent(-1);
}

void CueList::run(const Cue &cue)
{
    Document *d = m_doc;
    switch (cue.action) {
    case CueAction::TriggerColumn: d->triggerColumn(cue.column); break;
    case CueAction::TriggerClip: d->triggerClip(cue.layer, cue.column); break;
    case CueAction::ClearLayer: d->clearLayer(cue.layer); break;
    case CueAction::ClearAll:
        for (int l = 0; l < d->project().layers.size(); ++l)
            d->clearLayer(l);
        break;
    case CueAction::Master: d->setMaster(cue.value); break;
    case CueAction::Blackout: d->setBlackout(cue.value >= 0.5); break;
    case CueAction::Crossfader: d->setCrossfader(cue.value); break;
    case CueAction::Bpm: d->setBpm(cue.value); break;
    }
}

void CueList::fire(int index)
{
    const QVector<Cue> &cues = m_doc->project().cues;
    if (index < 0 || index >= cues.size())
        return;
    const Cue cue = cues[index]; // running it may change the project
    m_current = index;
    run(cue);
    emit cueFired(index);
    emit currentChanged(index);
}

void CueList::go()
{
    if (m_current + 1 < m_doc->project().cues.size())
        fire(m_current + 1);
}

void CueList::back()
{
    if (m_current > 0)
        fire(m_current - 1);
    else
        setCurrent(-1);
}

void CueList::setCurrent(int index)
{
    m_current = qBound(-1, index, int(m_doc->project().cues.size()) - 1);
    emit currentChanged(m_current);
}

void CueList::onTimecode(double seconds)
{
    const QVector<Cue> &cues = m_doc->project().cues;
    const double last = m_lastTimecode;
    m_lastTimecode = seconds;
    if (last < 0 || seconds < last || seconds - last > 1.0) {
        // Started, rewound or located: find our place without firing anything.
        int position = -1;
        for (int i = 0; i < cues.size(); ++i)
            if (cues[i].time >= 0 && cues[i].time <= seconds)
                position = i;
        if (position != m_current)
            setCurrent(position);
        return;
    }
    // Running: fire every cue whose time was crossed, in time order.
    QVector<int> due;
    for (int i = 0; i < cues.size(); ++i)
        if (cues[i].time >= 0 && cues[i].time > last && cues[i].time <= seconds)
            due << i;
    std::sort(due.begin(), due.end(), [&cues](int a, int b) { return cues[a].time < cues[b].time; });
    for (int i : std::as_const(due))
        fire(i);
}
