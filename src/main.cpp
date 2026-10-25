#include "control/ArtNet.h"
#include "control/ControlRouter.h"
#include "control/Sacn.h"
#include "media/Ndi.h"
#include "model/Document.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"
#include "ui/ReportExporter.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QCameraDevice>
#include <QDialog>
#include <QMediaDevices>
#include <QTabWidget>
#include <QEventLoop>
#include <functional>
#include <QIcon>
#include <QJsonObject>
#include <QMutex>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
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
    waitFor([&] { return fps > 0; }, 5000); // statistics arrive once per second
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

    // OSC: real UDP packets to the app on localhost.
    {
        ControlRouter *router = window.controlRouter();
        QString error;
        const bool listening = router->setOsc(true, 0, &error, false, true); // free local port, not remembered
        check(listening, "OSC listener starts");
        QUdpSocket sender;
        auto send = [&](const QString &address, const QVariantList &args = {}) {
            sender.writeDatagram(Osc::encode({address, args}), QHostAddress::LocalHost, quint16(router->oscPort()));
        };
        const Project &p = doc.project();
        send(QStringLiteral("/dh/master"), {0.25});
        send(QStringLiteral("/dh/column/2/trigger"), {1});
        send(QStringLiteral("/dh/layer/1/opacity"), {0.5});
        check(waitFor([&] { return qAbs(p.master - 0.25) < 0.01 && p.layers[0].activeColumn == 1
                                   && qAbs(p.layers[0].opacity - 0.5) < 0.01; }, 3000),
              "OSC master, column trigger and layer opacity");
        send(QStringLiteral("/dh/blackout")); // no argument: toggle on
        const bool toggledOn = waitFor([&] { return p.blackout; }, 3000);
        send(QStringLiteral("/dh/blackout"), {false}); // explicit off (T/F argument)
        check(toggledOn && waitFor([&] { return !p.blackout; }, 3000), "OSC blackout toggle and explicit off");
        // A bundle with two messages.
        QByteArray bundle("#bundle\0", 8);
        bundle += QByteArray(8, '\0');
        for (const OscMessage &m : {OscMessage{QStringLiteral("/dh/master"), {1.0}},
                                    OscMessage{QStringLiteral("/dh/layer/1/opacity"), {1.0}}}) {
            const QByteArray enc = Osc::encode(m);
            char size[4];
            qToBigEndian(qint32(enc.size()), size);
            bundle += QByteArray(size, 4) + enc;
        }
        sender.writeDatagram(bundle, QHostAddress::LocalHost, quint16(router->oscPort()));
        check(waitFor([&] { return p.master > 0.99 && p.layers[0].opacity > 0.99; }, 3000), "OSC bundle");
        window.showControls(); // the OSC & MIDI window with the mappings learned below
        router->setOsc(false, 0, nullptr, false);
    }
    // MIDI: learn, then notes toggle and CCs drive faders/triggers (synthetic messages).
    {
        ControlRouter *router = window.controlRouter();
        QVector<ControlMapping> &controls = doc.project().controls;
        auto learn = [&](ControlAction action, int layer, int column, const MidiMessage &m) {
            ControlMapping c;
            c.action = action;
            c.layer = layer;
            c.column = column;
            controls.append(c);
            router->startLearn(int(controls.size()) - 1);
            router->handleMidi(m);
            return controls.last().midiSet;
        };
        const MidiMessage note60{0x90, 60, 100, QStringLiteral("test")}; // note on, channel 1
        const MidiMessage cc7{0xB2, 7, 64, QStringLiteral("test")};      // CC 7 = 64 on channel 3
        const bool learnedAll = learn(ControlAction::LayerBypass, 0, 0, note60)
                                && learn(ControlAction::Master, 0, 0, cc7)
                                && learn(ControlAction::TriggerClip, 0, 2, MidiMessage{0xB0, 20, 0, QStringLiteral("test")});
        check(learnedAll && controls[controls.size() - 2].midiIsCC && controls[controls.size() - 2].midiChannel == 2,
              "MIDI learn assigns note and CC controls");
        router->handleMidi(note60);
        const bool bypassOn = doc.project().layers[0].bypass;
        router->handleMidi(note60);
        check(bypassOn && !doc.project().layers[0].bypass, "MIDI note toggles layer bypass");
        router->handleMidi(cc7);
        check(qAbs(doc.project().master - 64 / 127.0) < 0.001, "MIDI CC drives the master fader");
        doc.triggerColumn(0);
        router->handleMidi(MidiMessage{0xB0, 20, 127, QStringLiteral("test")});
        check(doc.project().layers[0].activeColumn == 2, "MIDI CC triggers a clip");
        doc.setMaster(1.0);
    }

    // Art-Net: real ArtDmx packets on localhost, universe 3, patch at channel 10, two layers.
    {
        ControlRouter *router = window.controlRouter();
        router->setArtNetLocalOnly(true);
        doc.project().dmx = DmxPatch{true, DmxProtocol::ArtNet, 3, 10, 2};
        doc.notifyChanged();
        check(router->artNetListening(), "Art-Net listener starts");
        QUdpSocket desk;
        int sequence = 1;
        auto sendDmx = [&](const QVector<int> &personality, int universe = 3) {
            QByteArray data(512, '\0');
            for (int i = 0; i < personality.size(); ++i)
                data[9 + i] = char(personality[i]); // channel 10 onwards
            desk.writeDatagram(ArtNet::buildDmx(universe, data, sequence++), QHostAddress::LocalHost, ArtNet::kPort);
        };
        const Project &p = doc.project();
        doc.triggerColumn(0);
        //        master blk col  L1clip L1op L1byp  L2clip L2op L2byp
        sendDmx({255, 0, 0, 3, 255, 0, 0, 255, 0});
        const bool first = waitFor([&] { return router->lastDmx().size() == 512; }, 3000);
        check(first && p.master > 0.99 && p.layers[0].activeColumn == 0,
              "Art-Net first packet sets faders but triggers nothing");
        sendDmx({64, 0, 0, 4, 128, 0, 0, 255, 0}); // clip channel 3 -> 4: play column 4
        check(waitFor([&] { return qAbs(p.master - 64 / 255.0) < 0.01 && p.layers[0].activeColumn == 3
                                   && qAbs(p.layers[0].opacity - 128 / 255.0) < 0.01; }, 3000),
              "Art-Net master, clip select and layer opacity");
        sendDmx({64, 255, 2, 4, 128, 255, 0, 255, 0}); // clip channel unchanged: no retrigger
        check(waitFor([&] { return p.blackout && p.layers[0].activeColumn == 1 && p.layers[0].bypass; }, 3000),
              "Art-Net blackout, column trigger and bypass");
        sendDmx({0, 0, 2, 4, 128, 255, 0, 255, 0}, 4); // another universe: ignored
        waitMs(300);
        check(p.master > 0.2, "Art-Net ignores other universes");
        const QByteArray reply = ArtNet::buildPollReply(QHostAddress(QStringLiteral("10.1.2.3")), 3,
                                                        QStringLiteral("Display Hawk"), QStringLiteral("long"), QString());
        check(reply.size() == 239 && ArtNet::opcode(reply) == ArtNet::kOpPollReply && quint8(reply[10]) == 10
                  && quint8(reply[13]) == 3 && reply.mid(26, 12) == "Display Hawk" && quint8(reply[190]) == 3,
              "Art-Net poll reply layout");
        doc.setBlackout(false);
        doc.setLayerPlayback(0, 1.0, false, false);
        doc.setMaster(1.0);
        doc.project().dmx.enabled = false;
        doc.notifyChanged();
        check(!router->artNetListening(), "Art-Net listener stops with the patch");

        // sACN: same personality, universe 7, priorities and stream termination.
        doc.project().dmx = DmxPatch{true, DmxProtocol::Sacn, 7, 1, 1};
        doc.notifyChanged();
        check(router->sacnListening() && !router->artNetListening(), "sACN listener replaces Art-Net");
        QUdpSocket console;
        int seq = 1;
        auto sendSacn = [&](const QVector<int> &personality, int priority, const QString &name, bool terminated = false) {
            QByteArray data(512, '\0');
            for (int i = 0; i < personality.size(); ++i)
                data[i] = char(personality[i]);
            console.writeDatagram(Sacn::build(7, data, priority, seq++, name, terminated), QHostAddress::LocalHost,
                                  Sacn::kPort);
        };
        doc.triggerColumn(0);
        sendSacn({200, 0, 0, 1, 255, 0}, 100, QStringLiteral("main desk"));     // first packet: faders only
        check(waitFor([&] { return qAbs(p.master - 200 / 255.0) < 0.01; }, 3000) && p.layers[0].activeColumn == 0,
              "sACN first packet sets the master");
        sendSacn({200, 0, 0, 3, 255, 0}, 100, QStringLiteral("main desk"));     // clip 1 -> 3
        check(waitFor([&] { return p.layers[0].activeColumn == 2; }, 3000), "sACN clip select");
        sendSacn({10, 0, 0, 3, 255, 0}, 50, QStringLiteral("backup desk"));     // lower priority: ignored
        waitMs(300);
        const bool backupIgnored = qAbs(p.master - 200 / 255.0) < 0.01;
        sendSacn({200, 0, 0, 3, 255, 0}, 100, QStringLiteral("main desk"), true); // main stops
        sendSacn({10, 0, 0, 3, 255, 0}, 50, QStringLiteral("backup desk"));     // backup takes over
        check(backupIgnored && waitFor([&] { return qAbs(p.master - 10 / 255.0) < 0.01; }, 3000),
              "sACN priority: higher wins, backup takes over when the main source terminates");
        QByteArray preview = Sacn::build(7, QByteArray(512, char(255)), 200, seq++, QStringLiteral("visualiser"));
        preview[112] = char(0x80);
        console.writeDatagram(preview, QHostAddress::LocalHost, Sacn::kPort);
        waitMs(300);
        check(qAbs(p.master - 10 / 255.0) < 0.01, "sACN ignores preview data");
        doc.setMaster(1.0);
        doc.project().dmx.enabled = false;
        doc.notifyChanged();
    }

    // Live inputs and NDI. Hardware- or runtime-dependent checks are skipped (and say so) when absent.
    {
        Project roundTrip = doc.project();
        roundTrip.layers[1].clips[6] = Clip::fromCapture(QStringLiteral("\\\\?\\usb#cam"), QStringLiteral("Desk camera"));
        roundTrip.layers[1].clips[7] = Clip::fromNdi(QStringLiteral("STUDIO (Program)"));
        const Project back = Project::fromJson(roundTrip.toJson());
        check(back.layers[1].clips[6].mediaKey() == QStringLiteral("capture:\\\\?\\usb#cam")
                  && back.layers[1].clips[6].name == QStringLiteral("Desk camera")
                  && back.layers[1].clips[7].mediaKey() == QStringLiteral("ndi:STUDIO (Program)"),
              "live input clips save and load");

        const bool ndiRuntime = Ndi::available();
        qInfo("selftest: %s", qPrintable(Ndi::runtimeStatus()));
        // NDI out: with or without the runtime the render loop must carry on.
        doc.project().screens[0].display = QStringLiteral("ndi");
        doc.notifyChanged();
        fps = 0;
        check(waitFor([&] { return fps > 0; }, 5000), "NDI output degrades gracefully / keeps rendering");
        if (ndiRuntime) {
            // Loopback: receive our own NDI output as a clip.
            const QString name = QStringLiteral("Display Hawk - %1").arg(doc.project().screens[0].name);
            QString found;
            waitFor([&] {
                for (const QString &s : Ndi::sources())
                    if (s.contains(name))
                        found = s;
                return !found.isEmpty();
            }, 10000);
            check(!found.isEmpty(), "NDI output is discoverable on the network");
            doc.setClip(1, 7, Clip::fromNdi(found));
            doc.triggerClip(1, 7);
            check(waitFor([&] { return litPercent(previews[0]) > 20; }, 10000), "NDI loopback: our output comes back as a clip");
            save(previews[0], "ndi-loopback");
            doc.clearLayer(1);
        } else {
            qInfo("selftest: SKIP NDI loopback (no NDI runtime on this machine)");
        }
        doc.project().screens[0].display = QStringLiteral("window");
        doc.notifyChanged();

        const QList<QCameraDevice> cameras = QMediaDevices::videoInputs();
        if (!cameras.isEmpty()) {
            doc.setClip(1, 6, Clip::fromCapture(QString::fromUtf8(cameras.first().id()), cameras.first().description()));
            doc.triggerClip(1, 6);
            check(waitFor([&] { return litPercent(previews[0]) > 5; }, 10000), "capture device delivers frames");
            save(previews[0], "capture");
            doc.clearLayer(1);
        } else {
            qInfo("selftest: SKIP capture device (none connected)");
        }
    }

    // The app's own window (not the desktop), for checking the UI layout: the wall page, with a
    // cabinet marked as not fitted so the panel grid editor shows both states.
    doc.project().walls[0].disabledPanels = {QPoint(9, 0)};
    doc.notifyChanged();
    doc.select(Selection::ofWall(0));
    waitMs(300);
    if (auto *dialog = window.findChild<QDialog *>()) {
        dialog->grab().save(QStringLiteral("%1/osc-midi-window.png").arg(outDir));
        if (auto *tabs = dialog->findChild<QTabWidget *>()) {
            doc.project().dmx.enabled = false;
            tabs->setCurrentIndex(2);
            waitMs(300);
            dialog->grab().save(QStringLiteral("%1/artnet-window.png").arg(outDir));
        }
    }
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
