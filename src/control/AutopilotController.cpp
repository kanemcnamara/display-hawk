#include "control/AutopilotController.h"

#include "media/MediaSource.h"
#include "model/Document.h"
#include "render/RenderEngine.h"

#include <QRandomGenerator>
#include <QTimer>

AutopilotController::AutopilotController(Document *doc, RenderEngine *engine, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_engine(engine)
    , m_timer(new QTimer(this))
{
    connect(m_engine, &RenderEngine::clipFinished, this, &AutopilotController::onClipFinished);
    connect(m_doc, &Document::clipTriggered, this, [this](const QString &clipId) {
        for (const Layer &l : m_doc->project().layers) {
            const Clip *c = l.activeClip();
            if (c && c->id == clipId) {
                m_playing[l.id].clipId = clipId;
                m_playing[l.id].since.start();
            }
        }
    });
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &AutopilotController::tick);
    m_timer->start();
}

bool AutopilotController::hasEnd(const QString &clipId) const
{
    const MediaSource *m = m_engine->mediaFor(clipId);
    return m && m->isVideo() && !m->isLive();
}

void AutopilotController::onClipFinished(const QString &clipId, bool ended)
{
    const Project &p = m_doc->project();
    for (int li = 0; li < p.layers.size(); ++li) {
        const Layer &l = p.layers[li];
        const Clip *c = l.activeClip();
        if (!c || c->id != clipId)
            continue;
        if (l.autopilot != Autopilot::Off) {
            advance(li);
        } else if (ended && c->playMode == PlayMode::OnceClear) {
            m_doc->clearLayer(li);
        }
        return;
    }
}

void AutopilotController::tick()
{
    const Project &p = m_doc->project();
    for (int li = 0; li < p.layers.size(); ++li) {
        const Layer &l = p.layers[li];
        const Clip *c = l.activeClip();
        if (!c) {
            m_playing.remove(l.id);
            continue;
        }
        Playing &state = m_playing[l.id];
        if (state.clipId != c->id) { // started some other way (a column, undo of a file...)
            state.clipId = c->id;
            state.since.start();
        }
        if (l.autopilot == Autopilot::Off || hasEnd(c->id))
            continue;
        if (state.since.isValid() && state.since.elapsed() >= qint64(l.autopilotSeconds * 1000.0)) {
            state.since.start();
            advance(li);
            return; // the project changed; carry on next tick
        }
    }
}

void AutopilotController::advance(int li)
{
    const Layer &l = m_doc->project().layers[li];
    QVector<int> filled;
    for (int c = 0; c < l.clips.size(); ++c)
        if (!l.clips[c].isEmpty())
            filled << c;
    if (filled.isEmpty())
        return;
    int next = -1;
    if (l.autopilot == Autopilot::Random && filled.size() > 1) {
        do
            next = filled[int(QRandomGenerator::global()->bounded(int(filled.size())))];
        while (next == l.activeColumn);
    } else {
        for (int step = 1; step <= l.clips.size(); ++step) {
            const int c = (qMax(-1, l.activeColumn) + step) % int(l.clips.size());
            if (!l.clips[c].isEmpty()) {
                next = c;
                break;
            }
        }
    }
    if (next < 0)
        return;
    // A toggle clip that is already playing would stop: re-start it instead.
    if (next == l.activeColumn && l.clips[next].trigger == TriggerMode::Toggle)
        m_doc->clearLayer(li);
    m_doc->triggerClip(li, next);
}
