#include "ui/IdentifyOverlay.h"

#include "model/Project.h"

#include <QGuiApplication>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QTimer>

namespace {
constexpr int kShowMs = 5000;
QList<QPointer<IdentifyOverlay>> g_overlays;
} // namespace

void IdentifyOverlay::showAll(const Project &project)
{
    closeAll();
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        QStringList assigned;
        for (const Screen &s : project.screens)
            if (s.display == screens[i]->name())
                assigned << QStringLiteral("%1  (%2 x %3)").arg(s.name).arg(s.width).arg(s.height);
        auto *w = new IdentifyOverlay(screens[i], i + 1, assigned);
        g_overlays.append(w);
        w->show();
    }
    QTimer::singleShot(kShowMs, qApp, &IdentifyOverlay::closeAll);
}

void IdentifyOverlay::closeAll()
{
    for (const QPointer<IdentifyOverlay> &w : std::as_const(g_overlays))
        if (w)
            w->close();
    g_overlays.clear();
}

IdentifyOverlay::IdentifyOverlay(QScreen *screen, int number, const QStringList &assigned)
    : QWidget(nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool)
    , m_screen(screen)
    , m_number(number)
    , m_assigned(assigned)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setGeometry(screen->geometry());
}

void IdentifyOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(10, 10, 12));
    const QColor accent = m_assigned.isEmpty() ? QColor(120, 120, 130) : QColor(0, 170, 255);
    p.setPen(QPen(accent, 16));
    p.drawRect(rect().adjusted(8, 8, -8, -8));

    const int h = height();
    QFont big = font();
    big.setBold(true);
    big.setPixelSize(qMax(40, h * 2 / 5));
    p.setFont(big);
    p.setPen(Qt::white);
    const QRect numberRect(0, h / 12, width(), h / 2);
    p.drawText(numberRect, Qt::AlignCenter, QString::number(m_number));

    const QSize px = m_screen->geometry().size() * m_screen->devicePixelRatio();
    QStringList lines;
    lines << (m_screen->model().isEmpty() ? m_screen->name() : m_screen->model() + QStringLiteral("  ·  ") + m_screen->name());
    lines << tr("%1 x %2 @ %3 Hz   at %4, %5%6")
                 .arg(px.width())
                 .arg(px.height())
                 .arg(qRound(m_screen->refreshRate()))
                 .arg(m_screen->geometry().x())
                 .arg(m_screen->geometry().y())
                 .arg(m_screen == QGuiApplication::primaryScreen() ? tr("   (primary)") : QString());
    if (m_assigned.isEmpty())
        lines << tr("No Display Hawk screen assigned");
    else
        lines << tr("Assigned: %1").arg(m_assigned.join(QStringLiteral(", ")));

    QFont small = font();
    small.setPixelSize(qMax(14, h / 28));
    p.setFont(small);
    const int lineH = small.pixelSize() * 3 / 2;
    int y = numberRect.bottom() + lineH;
    for (int i = 0; i < lines.size(); ++i, y += lineH) {
        p.setPen(i == 2 ? accent : QColor(220, 220, 220));
        p.drawText(QRect(0, y, width(), lineH), Qt::AlignCenter, lines[i]);
    }
}

void IdentifyOverlay::mousePressEvent(QMouseEvent *)
{
    closeAll();
}

void IdentifyOverlay::keyPressEvent(QKeyEvent *)
{
    closeAll();
}
