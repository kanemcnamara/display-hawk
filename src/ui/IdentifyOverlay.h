#pragma once

#include <QWidget>

class QScreen;
struct Project;

// Full-screen card on one physical display showing its number, name, resolution and which
// Display Hawk screens are assigned to it. Closes on click, key press or after a few seconds.
class IdentifyOverlay : public QWidget
{
    Q_OBJECT
public:
    // Shows one overlay per connected display; calling it again restarts them.
    static void showAll(const Project &project);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    IdentifyOverlay(QScreen *screen, int number, const QStringList &assigned);
    static void closeAll();

    QScreen *m_screen;
    int m_number;
    QStringList m_assigned;
};
