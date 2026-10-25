#include "model/Document.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"
#include "ui/ReportExporter.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QEventLoop>
#include <functional>
#include <QIcon>
#include <QJsonObject>
#include <QMutex>
#include <QTimer>
#include <QStandardPaths>
#include <QTime>
#include <QPalette>
#include <QStyleFactory>

// GUI builds have no console, so warnings (GL/shader/media errors) also go to a log file.
static void logToFile(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    static QMutex mutex; // the render thread logs too
    QMutexLocker lock(&mutex);
    static QFile file(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                      + QStringLiteral("/displayhawk.log"));
    if (!file.isOpen()) {
        QDir().mkpath(QFileInfo(file).absolutePath());
        file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    }
    static const char *levels[] = {"debug", "warning", "critical", "fatal", "info"};
    file.write(QStringLiteral("%1 [%2] %3\n")
                   .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")), QLatin1String(levels[type]), msg)
                   .toUtf8());
    file.flush();
    fprintf(stderr, "%s\n", qPrintable(msg));
}

static void applyDarkTheme(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    p.setColor(QPalette::Window, QColor(37, 37, 40));
    p.setColor(QPalette::WindowText, QColor(220, 220, 220));
    p.setColor(QPalette::Base, QColor(26, 26, 28));
    p.setColor(QPalette::AlternateBase, QColor(44, 44, 48));
    p.setColor(QPalette::ToolTipBase, QColor(50, 50, 55));
    p.setColor(QPalette::ToolTipText, QColor(230, 230, 230));
    p.setColor(QPalette::Text, QColor(220, 220, 220));
    p.setColor(QPalette::Button, QColor(48, 48, 52));
    p.setColor(QPalette::ButtonText, QColor(220, 220, 220));
    p.setColor(QPalette::Highlight, QColor(0, 150, 220));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(0, 170, 255));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(110, 110, 110));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(110, 110, 110));
    p.setColor(QPalette::Disabled, QPalette::WindowText, QColor(110, 110, 110));
    app.setPalette(p);
}

// --selftest <dir>: checks undo/redo, goes live with a windowed output, saves both editor previews
// as PNGs and fails if either is missing or blank. Results are in the log; exit code 0 = pass.
static int runSelfTest(QApplication &app, Document &doc, RenderEngine &engine, MainWindow &window,
                       const QString &outDir)
{
    QDir().mkpath(outDir);
    bool ok = true;
    auto check = [&ok](bool cond, const char *what) {
        qInfo("selftest: %s %s", cond ? "PASS" : "FAIL", what);
        ok = ok && cond;
    };

    const QJsonObject before = doc.project().toJson();
    doc.project().compWidth = 1234;
    doc.notifyChanged();
    doc.commitUndo();
    doc.undo();
    check(doc.project().toJson() == before, "undo restores the previous state");
    doc.redo();
    check(doc.project().compWidth == 1234, "redo re-applies the edit");
    doc.undo();

    doc.project().screens[0].display = QStringLiteral("window");
    doc.notifyChanged();
    window.setLive(true);

    QImage previews[2];
    int fps = 0;
    QObject::connect(&engine, &RenderEngine::previewReady, &app,
                     [&previews](int view, const QImage &img) { previews[view] = img; });
    QObject::connect(&engine, &RenderEngine::statsUpdated, &app, [&fps](int f) { fps = f; });
    // A local loop: QCoreApplication::quit() would try to close the windows (and prompt to save).
    auto waitMs = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    // Slow GPUs (CI virtual machines) take several frames to reflect a change in the preview, so each
    // check waits until its condition holds, up to a timeout, instead of sleeping a fixed time.
    auto waitFor = [&waitMs](const std::function<bool()> &condition, int timeoutMs = 5000) {
        for (int waited = 0; !condition() && waited < timeoutMs; waited += 50)
            waitMs(50);
        return condition();
    };
    auto litPercent = [](const QImage &img) {
        int lit = 0, samples = 0;
        for (int y = 0; y < img.height(); y += 4)
            for (int x = 0; x < img.width(); x += 4, ++samples)
                if (qGray(img.pixel(x, y)) > 16)
                    ++lit;
        return samples ? lit * 100 / samples : 0;
    };
    auto save = [&outDir](const QImage &img, const char *name) {
        const QString path = QStringLiteral("%1/%2.png").arg(outDir, QLatin1String(name));
        if (!img.isNull())
            img.save(path);
        return path;
    };
    // The first previews need the window shown and a few frames rendered; software GPUs on CI
    // machines can take several seconds.
    waitMs(1000);
    waitFor([&] { return litPercent(previews[0]) > 5 && litPercent(previews[1]) > 5; }, 20000);

    const char *names[] = {"preview-input", "preview-output"};
    for (int v = 0; v < 2; ++v) {
        const QImage &img = previews[v];
        const int lit = litPercent(img);
        const QString path = save(img, names[v]);
        qInfo("selftest: %s %dx%d, %d%% lit -> %s", names[v], img.width(), img.height(), lit, qPrintable(path));
        check(!img.isNull() && lit > 5, names[v]);
    }
    qInfo("selftest: %d fps while live on %s", fps, qPrintable(engine.backendName()));
    check(fps > 0, "render loop is running");

    // Clip deck: playback is not part of undo, blackout is black, triggering changes the picture.
    const QImage panelMap = previews[0];
    doc.triggerColumn(2); // colour bars
    check(doc.project().layers[0].activeColumn == 2, "column trigger starts the clip");
    doc.project().compHeight += 8;
    doc.notifyChanged();
    doc.commitUndo();
    doc.undo();
    check(doc.project().layers[0].activeColumn == 2, "undo leaves playback alone");
    const bool barsShown =
        waitFor([&] { return !previews[0].isNull() && previews[0] != panelMap && litPercent(previews[0]) > 50; });
    save(previews[0], "deck-colour-bars");
    check(barsShown, "triggered clip reaches the composition");

    // Blend modes: a red solid on the top layer over the colour bars. Bars are white, yellow, cyan,
    // green, magenta, red, blue, black across the top three quarters.
    Clip red = Clip::fromPattern(TestPattern::Solid);
    red.solidColor = Qt::red;
    doc.setClip(1, 2, red);
    doc.project().layers[1].blend = BlendMode::Multiply;
    doc.notifyChanged();
    doc.triggerColumn(2);
    auto barColor = [&previews](int bar) {
        const QImage &img = previews[0];
        return img.isNull() ? QColor() : QColor(img.pixel(img.width() * (2 * bar + 1) / 16, img.height() / 3));
    };
    auto near = [](const QColor &c, int r, int g, int b) {
        return qAbs(c.red() - r) < 40 && qAbs(c.green() - g) < 40 && qAbs(c.blue() - b) < 40;
    };
    const bool multiplyOk = waitFor([&] { return near(barColor(0), 255, 0, 0) && near(barColor(3), 0, 0, 0); });
    save(previews[0], "deck-multiply");
    check(multiplyOk, "multiply blend (white->red, green->black)");
    doc.project().layers[1].blend = BlendMode::Add;
    doc.notifyChanged();
    const bool addOk = waitFor([&] { return near(barColor(3), 255, 255, 0) && near(barColor(6), 255, 0, 255); });
    save(previews[0], "deck-add");
    check(addOk, "add blend (green->yellow, blue->magenta)");
    doc.clearLayer(1);

    // Optional: DISPLAYHAWK_TEST_VIDEO=<file> checks that a video clip decodes and keeps moving.
    const QString video = qEnvironmentVariable("DISPLAYHAWK_TEST_VIDEO");
    if (!video.isEmpty()) {
        doc.setClip(0, 3, Clip::fromMedia(video));
        doc.triggerClip(0, 3);
        // Decoder start-up varies (FFmpeg loads on first use); allow up to 3 s for frames to arrive.
        waitFor([&] { return litPercent(previews[0]) > 20; });
        const QImage first = previews[0];
        waitMs(500);
        const QImage second = previews[0];
        save(first, "deck-video-a");
        save(second, "deck-video-b");
        check(litPercent(first) > 20, "video clip shows frames");
        check(!first.isNull() && first != second, "video clip is playing");
    }

    doc.setBlackout(true);
    const bool black = waitFor([&] { return litPercent(previews[0]) < 2; });
    save(previews[0], "deck-blackout");
    check(black, "blackout sends black");
    doc.setBlackout(false);

    // A crossfade must render without errors and end on the new clip.
    doc.project().layers[0].transitionMs = 300;
    doc.notifyChanged();
    doc.triggerColumn(0);
    waitMs(300);
    const bool faded = waitFor([&] { return litPercent(previews[0]) > 50; });
    save(previews[0], "deck-after-crossfade");
    check(faded, "crossfade finishes on the new clip");

    // LED tools. Colour correction: with red and blue gain at 0 only green may reach the output.
    {
        Slice &sl = doc.project().screens[0].slices[0];
        sl.gainR = 0.0;
        sl.gainB = 0.0;
        doc.notifyChanged();
        auto onlyGreen = [&] {
            const QImage &img = previews[1];
            if (img.isNull())
                return false;
            int coloured = 0, greenish = 0;
            for (int y = 0; y < img.height(); y += 3)
                for (int x = 0; x < img.width(); x += 3) {
                    const QColor c(img.pixel(x, y));
                    if (c.green() > 40) {
                        ++greenish;
                        coloured += (c.red() > 12 || c.blue() > 12) ? 1 : 0;
                    }
                }
            return greenish > 100 && coloured == 0;
        };
        const bool graded = waitFor(onlyGreen);
        save(previews[1], "led-colour-correction");
        check(graded, "slice colour correction (red/blue gain 0)");
        sl.gainR = sl.gainB = 1.0;
        doc.notifyChanged();
    }
    // Moving bars must move.
    {
        doc.setClip(0, 4, Clip::fromPattern(TestPattern::MovingBars));
        doc.triggerClip(0, 4);
        waitFor([&] { return litPercent(previews[0]) < 30 && litPercent(previews[0]) > 0; });
        const QImage a = previews[0];
        waitMs(500);
        save(previews[0], "led-moving-bars");
        check(!a.isNull() && a != previews[0], "moving bars pattern moves");
    }
    // Irregular walls: with masking on, a cabinet marked not fitted goes black.
    {
        LedWall &w = doc.project().walls[0];
        const QRect panel = w.panelRect(2, 1);
        const QRect neighbour = w.panelRect(3, 1);
        auto panelPixel = [&](const QRect &r) {
            const QImage &img = previews[0];
            if (img.isNull())
                return QColor();
            const QPointF c = QRectF(r).center();
            return QColor(img.pixel(int(c.x() * img.width() / doc.project().compWidth),
                                    int(c.y() * img.height() / doc.project().compHeight)));
        };
        doc.triggerClip(0, 2); // colour bars: both cabinets are lit
        const bool litBefore = waitFor([&] {
            return qGray(panelPixel(panel).rgb()) > 100 && qGray(panelPixel(neighbour).rgb()) > 100;
        });
        w.disabledPanels = {QPoint(2, 1)};
        doc.project().maskToPanels = true;
        doc.notifyChanged();
        const bool ok = litBefore && waitFor([&] {
            return qGray(panelPixel(panel).rgb()) < 8 && qGray(panelPixel(neighbour).rgb()) > 100;
        });
        save(previews[0], "led-panel-mask");
        check(ok, "panel mask blacks out a cabinet that is not fitted");
        w.disabledPanels.clear();
        doc.project().maskToPanels = false;
        doc.notifyChanged();
    }
    // Setup report.
    {
        QString error;
        const QStringList files =
            ReportExporter::exportReport(doc.project(), outDir + QStringLiteral("/report"), QStringLiteral("selftest"), &error);
        const QFileInfo pdf(outDir + QStringLiteral("/report/selftest-report.pdf"));
        qInfo("selftest: report wrote %d files, PDF %lld bytes %s", int(files.size()), pdf.size(), qPrintable(error));
        check(files.size() >= 4 && pdf.size() > 20000, "setup report export (PDF, PNGs, CSV)");
    }

    // The app's own window (not the desktop), for checking the UI layout: the wall page, with a
    // cabinet marked as not fitted so the panel grid editor shows both states.
    doc.project().walls[0].disabledPanels = {QPoint(9, 0)};
    doc.notifyChanged();
    doc.select(Selection::ofWall(0));
    waitMs(300);
    window.grab().save(QStringLiteral("%1/main-window.png").arg(outDir));
    window.setLive(false);
    return ok ? 0 : 1;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Display Hawk"));
    QApplication::setOrganizationName(QStringLiteral("Display Hawk"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/icon/displayhawk.png")));
    applyDarkTheme(app);
    qInstallMessageHandler(logToFile);

    Document doc;
    RenderEngine engine(&doc);
    QString error;
    if (!engine.start(&error)) {
        qCritical("Graphics initialisation failed (%s): %s", qPrintable(engine.backendName()), qPrintable(error));
        QMessageBox::critical(nullptr, QStringLiteral("Display Hawk"),
                              QStringLiteral("Could not start %1 rendering: %2\n\nUpdate your graphics driver, or "
                                             "set DISPLAYHAWK_RHI=opengl to try another backend.")
                                  .arg(engine.backendName(), error));
        return 1;
    }
    qInfo("Display Hawk started (%s)", qPrintable(engine.backendName()));

    int result = 0;
    {
        MainWindow window(&doc, &engine);
        window.show();
        const QStringList args = app.arguments();
        const int selfTest = args.indexOf(QStringLiteral("--selftest"));
        if (selfTest >= 0) {
            const QString dir = selfTest + 1 < args.size() ? args[selfTest + 1] : QDir::tempPath();
            result = runSelfTest(app, doc, engine, window, dir);
        } else {
            window.startup(args);
            result = app.exec();
        }
    } // outputs are released before the render thread stops
    engine.stop();
    return result;
}
