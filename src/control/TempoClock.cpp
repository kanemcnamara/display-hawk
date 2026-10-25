#include "control/TempoClock.h"

#include "control/Midi.h"
#include "model/Document.h"

#include <QTimer>
#include <QtMath>
#include <algorithm>

namespace {
constexpr qint64 kTapResetNs = 2'000'000'000; // a pause this long starts a new tap sequence
constexpr int kTapsKept = 8;
constexpr int kMidiPpqn = 24; // MIDI clock ticks per beat
} // namespace

TempoClock::TempoClock(Document *doc, QObject *parent)
    : QObject(parent)
    , m_doc(doc)
    , m_poll(new QTimer(this))
{
    m_bpm = m_doc->project().bpm;
    m_originNs = nowNs();
    connect(m_doc, &Document::changed, this, &TempoClock::syncFromProject);
    m_doc->setQuantizer([this](int beats) { return msUntil(beats); });
    // Beat events for the indicator (and anyone else) without busy-waiting: 5 ms resolution.
    m_poll->setInterval(5);
    m_poll->setTimerType(Qt::PreciseTimer);
    connect(m_poll, &QTimer::timeout, this, [this] {
        const qint64 b = qFloor(beats());
        if (b != m_lastBeat) {
            m_lastBeat = b;
            emit beat(int(((b % 4) + 4) % 4));
        }
    });
    m_poll->start();
}

qint64 TempoClock::nowNs()
{
    return MidiInput::nowNs();
}

double TempoClock::beatsAt(qint64 ns) const
{
    return m_originBeats + double(ns - m_originNs) / 1e9 * m_bpm / 60.0;
}

int TempoClock::msUntil(int beats) const
{
    const double now = this->beats();
    const double next = (qFloor(now / beats) + 1) * double(beats);
    return qMax(0, qRound((next - now) * 60000.0 / m_bpm));
}

void TempoClock::rebase(qint64 ns, double beatsAtNs)
{
    m_originNs = ns;
    m_originBeats = beatsAtNs;
}

void TempoClock::setTempo(double bpm, qint64 ns)
{
    bpm = qBound(20.0, bpm, 300.0);
    rebase(ns, beatsAt(ns)); // keep the phase where it is
    m_bpm = bpm;
    m_doc->setBpm(bpm);
}

void TempoClock::syncFromProject()
{
    // The BPM typed in (or loaded): continue from the current beat at the new rate.
    const double bpm = m_doc->project().bpm;
    if (qAbs(bpm - m_bpm) > 1e-9) {
        const qint64 now = nowNs();
        rebase(now, beatsAt(now));
        m_bpm = bpm;
    }
}

void TempoClock::tap(qint64 ns)
{
    if (ns < 0)
        ns = nowNs();
    if (!m_taps.isEmpty() && ns - m_taps.last() > kTapResetNs)
        m_taps.clear();
    m_taps.append(ns);
    if (m_taps.size() > kTapsKept)
        m_taps.removeFirst();
    if (m_taps.size() >= 2 && m_doc->project().tempoSource == TempoSource::Internal) {
        const double interval = double(m_taps.last() - m_taps.first()) / (m_taps.size() - 1);
        setTempo(60e9 / interval, ns);
    }
    rebase(ns, qRound(beatsAt(ns))); // the tap lands on a beat
}

void TempoClock::resync(qint64 ns)
{
    if (ns < 0)
        ns = nowNs();
    rebase(ns, qRound(beatsAt(ns) / 4.0) * 4.0);
}

void TempoClock::nudge(double beats)
{
    const qint64 now = nowNs();
    rebase(now, beatsAt(now) - beats);
}

void TempoClock::midiRealtime(quint8 byte, qint64 ns)
{
    if (m_doc->project().tempoSource != TempoSource::MidiClock)
        return;
    if (byte == 0xFA) { // start: the next tick is the first beat of a bar
        m_midiTicks.clear();
        m_midiTickCount = 0;
        resync(ns);
        return;
    }
    if (byte != 0xF8)
        return;
    m_midiTicks.append(ns);
    if (m_midiTicks.size() > 2 * kMidiPpqn + 1)
        m_midiTicks.removeFirst();
    // Tempo from the average over up to two beats of ticks (clock jitter averages out).
    if (m_midiTicks.size() >= kMidiPpqn + 1) {
        const double interval = double(m_midiTicks.last() - m_midiTicks.first()) / (m_midiTicks.size() - 1);
        const double bpm = 60e9 / (interval * kMidiPpqn);
        if (qAbs(bpm - m_bpm) > 0.05)
            setTempo(bpm, ns);
    }
    // Each 24th tick is a beat: hold the phase to it.
    if (m_midiTickCount++ % kMidiPpqn == 0)
        rebase(ns, qRound(beatsAt(ns)));
}

void TempoClock::audioBeat(qint64 ns)
{
    if (m_doc->project().tempoSource != TempoSource::AudioBeats)
        return;
    if (ns < 0)
        ns = nowNs();
    if (!m_audioBeats.isEmpty() && ns - m_audioBeats.last() > kTapResetNs)
        m_audioBeats.clear();
    m_audioBeats.append(ns);
    if (m_audioBeats.size() > 9)
        m_audioBeats.removeFirst();
    if (m_audioBeats.size() >= 4) {
        // The median gap between detected beats (robust against a missed or extra beat), folded
        // into a danceable 75..180 BPM range, eased in so the tempo does not jump about.
        QVector<qint64> gaps;
        for (int i = 1; i < m_audioBeats.size(); ++i)
            gaps << m_audioBeats[i] - m_audioBeats[i - 1];
        std::sort(gaps.begin(), gaps.end());
        double bpm = 60e9 / double(gaps[gaps.size() / 2]);
        while (bpm < 75.0)
            bpm *= 2.0;
        while (bpm > 180.0)
            bpm /= 2.0;
        setTempo(qAbs(bpm - m_bpm) > 8.0 ? bpm : m_bpm * 0.7 + bpm * 0.3, ns);
    }
    // Pull the phase onto the detected beat if it is close; otherwise this beat starts a new phase.
    const double b = beatsAt(ns);
    const double offset = b - qRound(b);
    rebase(ns, qAbs(offset) < 0.25 ? b - offset * 0.5 : qRound(b));
}
