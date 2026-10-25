#include "audio/AudioAnalyzer.h"
#include "audio/AudioReactive.h"
#include "ui/AudioReactiveDialog.h"
#include "control/ArtNet.h"
#include "control/ControlRouter.h"
#include "control/TempoClock.h"
#include "control/Sacn.h"
#include "media/MediaSource.h"
#include "media/Ndi.h"
#include "media/TextureShare.h"
#include "model/CabinetMapping.h"
#include "model/Document.h"
#include "render/Effects.h"
#include "render/Generators.h"
#include "render/Isf.h"
#include "render/RenderEngine.h"
#include "ui/MainWindow.h"
#include <QMediaPlayer>
#include "ui/MediaBrowser.h"
#include "ui/ClipPreviewPanel.h"
#include "media/OutputRecorder.h"
#include "ui/ReportExporter.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QAudioDevice>
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
#include <QtMath>
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
    qputenv("DISPLAYHAWK_NO_AUDIO_INPUT", "1");
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

    // Optional: DISPLAYHAWK_TEST_SOUND=<video with audio>, DISPLAYHAWK_TEST_HAP_SOUND=<HAP .mov with audio>.
    const QString soundFile = qEnvironmentVariable("DISPLAYHAWK_TEST_SOUND");
    if (!soundFile.isEmpty()) {
        qInfo("selftest: default audio output: %s", qPrintable(QMediaDevices::defaultAudioOutput().description()));
        const Clip sound = Clip::fromMedia(soundFile);
        doc.setClip(0, 5, sound);
        doc.project().layers[0].transitionMs = 0;
        doc.setLayerVolume(0, 0.5);
        doc.triggerClip(0, 5);
        const bool audible = waitFor([&] {
            MediaSource *m = engine.mediaFor(sound.id);
            return m && m->hasAudio() && qAbs(engine.audioLevel(sound.id) - 0.5) < 0.02;
        }, 8000);
        check(audible, "clip sound plays at clip x layer x master level");
        doc.setAudioMuted(true);
        check(waitFor([&] { return engine.audioLevel(sound.id) < 0.001; }, 2000), "audio mute silences clips");
        doc.setAudioMuted(false);
        window.controlRouter()->handleOsc({QStringLiteral("/dh/layer/1/volume"), {1.0}});
        check(waitFor([&] { return engine.audioLevel(sound.id) > 0.99; }, 2000), "OSC layer volume");
        // A 1 s crossfade to a test pattern fades the sound out over the same second.
        doc.project().layers[0].transitionMs = 1000;
        doc.notifyChanged();
        doc.triggerClip(0, 0);
        waitMs(500);
        const double mid = engine.audioLevel(sound.id);
        const bool gone = waitFor([&] { return engine.audioLevel(sound.id) < 0.001; }, 3000);
        qInfo("selftest: level half-way through the crossfade: %.2f", mid);
        check(mid > 0.2 && mid < 0.8 && gone, "audio fades out with the layer crossfade");
        doc.project().layers[0].transitionMs = 0;
        doc.notifyChanged();
    } else {
        qInfo("selftest: SKIP clip audio (set DISPLAYHAWK_TEST_SOUND)");
    }
    const QString hapSound = qEnvironmentVariable("DISPLAYHAWK_TEST_HAP_SOUND");
    if (!hapSound.isEmpty()) {
        const Clip hap = Clip::fromMedia(hapSound);
        doc.setClip(0, 6, hap);
        // Start from black so only the HAP pictures themselves can light the composition.
        doc.project().layers[0].transitionMs = 0;
        doc.clearLayer(0);
        waitFor([&] { return litPercent(previews[0]) < 2; }, 3000);
        doc.triggerClip(0, 6);
        const bool both = waitFor([&] {
            MediaSource *m = engine.mediaFor(hap.id);
            return m && m->isHap() && m->hasAudio() && engine.audioLevel(hap.id) > 0.99 && litPercent(previews[0]) > 20;
        }, 8000);
        save(previews[0], "hap-with-sound");
        check(both, "HAP movie plays its pictures and its soundtrack");
        doc.triggerClip(0, 0);
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

        // Spout / Syphon loopback: share screen 1, then receive our own sender as a clip.
        const QString tech = TextureShare::technology();
        const bool shareBackend = (tech == QLatin1String("Spout") && engine.backendName() == QLatin1String("Direct3D 11"))
                                  || (tech == QLatin1String("Syphon") && engine.backendName() == QLatin1String("Metal"));
        if (!tech.isEmpty()) {
            doc.project().screens[0].display = QStringLiteral("share");
            doc.notifyChanged();
            if (shareBackend) {
                const QString name = QStringLiteral("Display Hawk - %1").arg(doc.project().screens[0].name);
                QString found;
                waitFor([&] {
                    for (const QString &s : TextureShare::sources())
                        if (s.contains(name))
                            found = s;
                    return !found.isEmpty();
                }, 10000);
                check(!found.isEmpty(), qPrintable(tech + QStringLiteral(" output is listed for other apps")));
                // Show what comes back on a second wall outside the area the output samples (no
                // feedback loop). The shared screen is the main wall's colour bars at its top left and
                // black elsewhere, so in the returned picture point A is lit and point B is black,
                // where the colour bars underneath are bright: only a real received frame passes.
                LedWall testWall;
                testWall.name = QStringLiteral("Share test");
                testWall.cols = 5;
                testWall.rows = 3;
                testWall.x = 1280;
                testWall.y = 640; // 640 x 384; the 1920x1080 frame fits as 640x360 at y 652
                doc.project().walls.append(testWall);
                doc.project().layers[1].targetWall = int(doc.project().walls.size()) - 1;
                doc.project().layers[1].blend = BlendMode::Normal;
                doc.notifyChanged(true);
                doc.triggerClip(0, 2); // colour bars underneath
                doc.setClip(1, 5, Clip::fromShare(found));
                doc.triggerClip(1, 5);
                auto compPixel = [&](int x, int y) {
                    const QImage &img = previews[0];
                    return img.isNull() ? QColor()
                                        : QColor(img.pixel(x * img.width() / doc.project().compWidth,
                                                           y * img.height() / doc.project().compHeight));
                };
                const bool ok = waitFor([&] {
                    return qGray(compPixel(1280 + 200, 652 + 100).rgb()) > 60  // A: inside the shared wall area
                           && qGray(compPixel(1813, 952).rgb()) < 12;         // B: black part of the screen
                }, 10000);
                save(previews[0], "share-loopback");
                check(ok, qPrintable(tech + QStringLiteral(" loopback: our shared output comes back as a clip")));
                doc.clearLayer(1);
                doc.project().layers[1].targetWall = -1;
                doc.removeWall(int(doc.project().walls.size()) - 1);
            } else {
                fps = 0;
                check(waitFor([&] { return fps > 0; }, 5000),
                      qPrintable(tech + QStringLiteral(" on an unsupported backend keeps rendering")));
                qInfo("selftest: SKIP %s loopback (needs %s, running %s)", qPrintable(tech),
                      tech == QLatin1String("Spout") ? "Direct3D 11" : "Metal", qPrintable(engine.backendName()));
            }
            doc.project().screens[0].display = QStringLiteral("window");
            doc.notifyChanged();
        }

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

    // Audio reactive. First the analyser on synthetic audio, then the show integration with
    // injected levels (no live input: DISPLAYHAWK_NO_AUDIO_INPUT is set for the self-test).
    {
        constexpr int rate = 48000;
        AudioAnalyzer analyzer;
        auto tone = [&](double hz, double seconds, double amp) {
            QVector<float> s(int(rate * seconds));
            for (int i = 0; i < s.size(); ++i)
                s[i] = float(amp * std::sin(2.0 * M_PI * hz * i / rate));
            analyzer.process(s.constData(), int(s.size()), rate);
            return analyzer.levels();
        };
        const AudioLevels bassTone = tone(60, 2.0, 0.5);
        const AudioLevels trebleTone = tone(5000, 2.0, 0.5);
        qInfo("selftest: 60 Hz -> bass %.2f treble %.2f; 5 kHz -> bass %.2f treble %.2f", bassTone.bass,
              bassTone.treble, trebleTone.bass, trebleTone.treble);
        check(bassTone.bass > 0.6 && bassTone.treble < 0.25 && trebleTone.treble > 0.6 && trebleTone.bass < 0.3,
              "audio analysis separates bass and treble");
        // Kick drums at 120 BPM for 4 s: 60 Hz bursts decaying over ~120 ms, over faint noise.
        int beats = 0;
        QMetaObject::Connection counter =
            QObject::connect(&analyzer, &AudioAnalyzer::levelsChanged, [&beats](const AudioLevels &l) { beats += l.beat; });
        QVector<float> kicks(rate * 4);
        quint32 noise = 12345;
        for (int i = 0; i < kicks.size(); ++i) {
            const double t = double(i % (rate / 2)) / rate; // time since the last kick
            noise = noise * 1664525u + 1013904223u;
            kicks[i] = float(0.8 * std::exp(-t / 0.04) * std::sin(2.0 * M_PI * 60.0 * t) + 0.01 * (int(noise >> 16) % 200 - 100) / 100.0);
        }
        analyzer.process(kicks.constData(), int(kicks.size()), rate);
        QObject::disconnect(counter);
        qInfo("selftest: %d beats detected in 8 kicks", beats);
        check(beats >= 7 && beats <= 9, "beat detection on a 120 BPM kick pattern");
        tone(60, 0.0, 0.0);
        QVector<float> silence(rate * 2, 0.0f);
        analyzer.process(silence.constData(), int(silence.size()), rate);
        check(analyzer.levels().level < 0.05 && analyzer.levels().bass < 0.05, "silence reads as zero");

        // Show integration: bass drives layer 1's opacity; the fader stays at 100%.
        AudioReactive *reactive = window.audioReactive();
        const Project &p = doc.project();
        doc.project().layers[0].transitionMs = 0;
        doc.setLayerPlayback(0, 1.0, false, false);
        doc.clearLayer(1);
        doc.triggerClip(0, 2); // colour bars
        doc.project().audioReactive.enabled = true;
        doc.project().audioReactive.links = {AudioLink{AudioSignal::Bass, AudioTarget::LayerOpacity, 0, 0, 0, 1.0},
                                             AudioLink{AudioSignal::Beat, AudioTarget::NextColumn, 0, 0, 0, 1.0}};
        doc.notifyChanged();
        reactive->apply(AudioLevels{0.0, 0.0, 0.0, 0.0, false});
        const bool dark = waitFor([&] { return litPercent(previews[0]) < 3; }, 3000);
        reactive->apply(AudioLevels{1.0, 1.0, 0.0, 0.0, false});
        const bool bright = waitFor([&] { return litPercent(previews[0]) > 50; }, 3000);
        check(dark && bright && p.layers[0].opacity == 1.0, "bass modulates layer opacity without moving the fader");
        const int before = p.layers[0].activeColumn;
        reactive->apply(AudioLevels{1.0, 1.0, 0.0, 0.0, true});
        check(p.layers[0].activeColumn != before && p.layers[0].activeColumn >= 0, "a beat steps to the next column");
        reactive->apply(AudioLevels{0.0, 0.0, 0.0, 0.0, false}); // modulated to black...
        waitFor([&] { return litPercent(previews[0]) < 3; }, 3000);
        doc.project().audioReactive.enabled = false; // ...and switching off restores the faders exactly
        doc.notifyChanged();
        check(waitFor([&] { return litPercent(previews[0]) > 20; }, 3000), "turning audio reactive off restores the faders");
        // The dialog with two links and the meters fed by a bass-heavy tone, for checking its layout.
        doc.project().audioReactive.links = {AudioLink{AudioSignal::Bass, AudioTarget::LayerOpacity, 0, 0, 0, 0.8},
                                             AudioLink{AudioSignal::Beat, AudioTarget::NextClip, 1, 0, 0, 1.0}};
        doc.notifyChanged(true);
        window.showAudioReactive();
        tone(80, 0.5, 0.6);
        if (auto *d = window.findChild<AudioReactiveDialog *>()) {
            if (auto *meters = d->findChild<AudioMeters *>())
                meters->setLevels(AudioLevels{0.7, 0.9, 0.4, 0.15, true});
            waitMs(200);
            d->grab().save(QStringLiteral("%1/audio-reactive-window.png").arg(outDir));
            d->hide();
        }
        doc.project().audioReactive.links.clear();
        doc.notifyChanged(true);
    }

    // Effects. Layer 1 plays a red solid, layer 2 the colour bars, in a new column.
    {
        Project &p = doc.project();
        const int col = p.columns;
        doc.addColumn();
        Clip red = Clip::fromPattern(TestPattern::Solid);
        red.solidColor = Qt::red;
        doc.setClip(0, col, red);
        doc.setClip(1, col, Clip::fromPattern(TestPattern::ColorBars));
        p.layers[0].transitionMs = p.layers[1].transitionMs = 0;
        p.layers[0].blend = p.layers[1].blend = BlendMode::Normal;
        p.compEffects.clear();
        doc.setLayerPlayback(0, 1.0, false, false);
        doc.setLayerPlayback(1, 1.0, false, false);
        doc.triggerColumn(col);
        auto setFx = [&](int layer, const QVector<EffectInstance> &chain) {
            (layer < 0 ? p.compEffects : p.layers[layer].effects) = chain;
            doc.notifyChanged(true);
        };

        setFx(1, {makeEffect(QStringLiteral("invert"))});
        check(waitFor([&] { return near(barColor(0), 0, 0, 0) && near(barColor(6), 255, 255, 0); }),
              "invert effect (white->black, blue->yellow)");
        save(previews[0], "fx-invert");

        // Colour key: the white bar becomes transparent and the red layer below shows through.
        setFx(1, {makeEffect(QStringLiteral("colorKey"))});
        check(waitFor([&] { return near(barColor(0), 255, 0, 0) && near(barColor(3), 0, 255, 0); }),
              "colour key removes white (red layer shows through, green stays)");
        save(previews[0], "fx-colour-key");

        // Two-pass blur: the hard edge between the white and yellow bars turns into a ramp.
        EffectInstance blur = makeEffect(QStringLiteral("blur"));
        blur.params[0] = 40;
        setFx(1, {blur});
        auto edgeBlue = [&] {
            const QImage &img = previews[0];
            return img.isNull() ? -1 : QColor(img.pixel(img.width() * 2 / 16, img.height() / 3)).blue();
        };
        check(waitFor([&] { return edgeBlue() > 50 && edgeBlue() < 205; }), "blur softens edges");
        save(previews[0], "fx-blur");
        setFx(1, {});
        doc.clearLayer(1);

        // LED panel effects on the 10 x 5 wall of 128 px cabinets at 0,0. (A copy: editing the
        // project detaches its wall list from the undo snapshot, so references would dangle.)
        const LedWall w = p.walls[0];
        auto at = [&](double cx, double cy) {
            const QImage &img = previews[0];
            if (img.isNull())
                return QColor();
            return QColor(img.pixel(qBound(0, int(cx * img.width() / p.compWidth), img.width() - 1),
                                    qBound(0, int(cy * img.height() / p.compHeight), img.height() - 1)));
        };
        auto centre = [&](int c, int r) {
            const QPointF m = QRectF(w.panelRect(c, r)).center();
            return at(m.x(), m.y());
        };

        EffectInstance outline = makeEffect(QStringLiteral("panelOutline"));
        outline.params[0] = 12;
        setFx(-1, {outline});
        const QRect cab = w.panelRect(3, 2);
        check(waitFor([&] {
                  return near(at(cab.left() + 5, cab.center().y()), 255, 255, 255) && near(centre(3, 2), 255, 0, 0)
                         && near(at(w.rect().right() + 100, 300), 255, 0, 0);
              }),
              "panel outline draws cabinet edges only on the wall");
        save(previews[0], "fx-panel-outline");

        // A still chase (speed 0) lights the panels whose order is within the band behind 0:
        // left to right, column c has order c / 10, so columns 0 and 7..9 are lit, 1..6 dark.
        EffectInstance chase = makeEffect(QStringLiteral("panelChase"));
        chase.params = {0, 0, 0.35, 0, 0};
        setFx(-1, {chase});
        check(waitFor([&] {
                  return near(centre(8, 1), 255, 0, 0) && near(centre(0, 3), 255, 0, 0) && near(centre(3, 1), 0, 0, 0)
                         && near(centre(6, 4), 0, 0, 0) && near(at(w.rect().right() + 100, 300), 0, 0, 0);
              }),
              "panel chase lights cabinets in left-to-right order");
        save(previews[0], "fx-panel-chase");
        // Not-fitted cabinets are skipped: they stay dark even inside the band.
        p.walls[0].disabledPanels = {QPoint(8, 1)};
        doc.notifyChanged();
        check(waitFor([&] { return near(centre(8, 1), 0, 0, 0) && near(centre(8, 2), 255, 0, 0); }),
              "panel chase skips cabinets that are not fitted");
        p.walls[0].disabledPanels.clear();
        doc.notifyChanged();

        // Panel colours: hue by left-to-right order, column 0 red, column 3 (hue 0.3) green.
        EffectInstance colours = makeEffect(QStringLiteral("panelColours"));
        colours.params = {0, 1, 0, 1};
        setFx(-1, {colours});
        check(waitFor([&] {
                  const QColor a = centre(0, 0), b = centre(3, 0);
                  return near(a, 255, 0, 0) && b.green() > 200 && b.red() < 100 && b.blue() < 60;
              }),
              "panel colours give each cabinet its own hue");
        save(previews[0], "fx-panel-colours");
        setFx(-1, {});

        // Live control: OSC sets an effect parameter (0..1 spans its range) without an undo step,
        // and audio-reactive pushes it from its set value towards the maximum.
        doc.setClip(1, col, Clip::fromPattern(TestPattern::ColorBars));
        doc.triggerColumn(col);
        setFx(1, {makeEffect(QStringLiteral("invert"))});
        doc.commitUndo();
        const bool couldUndo = doc.canUndo();
        ControlRouter *router = window.controlRouter();
        router->setOsc(true, 0, nullptr, false, true);
        QUdpSocket sender;
        sender.writeDatagram(Osc::encode({QStringLiteral("/dh/layer/2/effect/1/1"), {0.0}}), QHostAddress::LocalHost,
                             quint16(router->oscPort()));
        check(waitFor([&] { return p.layers[1].effects[0].params[0] == 0.0 && near(barColor(0), 255, 255, 255); }, 3000),
              "OSC sets an effect parameter");
        doc.undo(); // undoes the effect being added; the live value is not a step of its own
        check(couldUndo && p.layers[1].effects.isEmpty(), "live effect control is not an undo step");
        router->setOsc(false, 0, nullptr, false);
        doc.setClip(1, col, Clip::fromPattern(TestPattern::ColorBars)); // the undo may have taken it too
        doc.triggerColumn(col);

        setFx(1, {makeEffect(QStringLiteral("invert"))});
        p.layers[1].effects[0].params[0] = 0.0;
        doc.notifyChanged();
        AudioReactive *reactive = window.audioReactive();
        p.audioReactive.enabled = true;
        AudioLink link;
        link.signal = AudioSignal::Bass;
        link.target = AudioTarget::EffectParam;
        link.layer = 1;
        link.effect = 0;
        link.param = 0;
        p.audioReactive.links = {link};
        doc.notifyChanged();
        reactive->apply(AudioLevels{1.0, 1.0, 0.0, 0.0, false});
        const bool inverted = waitFor([&] { return near(barColor(0), 0, 0, 0); }, 3000);
        reactive->apply(AudioLevels{0.0, 0.0, 0.0, 0.0, false});
        const bool normal = waitFor([&] { return near(barColor(0), 255, 255, 255); }, 3000);
        check(inverted && normal && p.layers[1].effects[0].params[0] == 0.0,
              "audio-reactive drives an effect parameter");
        p.audioReactive.enabled = false;
        p.audioReactive.links.clear();
        doc.notifyChanged(true);

        // The layer page with an effect chain, for checking the editor's layout.
        setFx(1, {makeEffect(QStringLiteral("colorKey")), makeEffect(QStringLiteral("panelChase"))});
        doc.select(Selection::ofLayer(1));
        waitMs(300);
        window.grab().save(QStringLiteral("%1/effects-window.png").arg(outDir));
        setFx(1, {});
        doc.clearLayer(1);
    }

    // Cabinet layouts: the 10 x 5 wall of 128 px cabinets, one slice per cabinet on screen 1.
    {
        Project &p = doc.project();
        const QVector<Slice> savedSlices = p.screens[0].slices;
        p.screens[0].width = 1920;
        p.screens[0].height = 1080;
        p.screens[0].slices.clear();
        doc.notifyChanged(true);
        CabinetLayout layout;
        layout.wall = 0;
        layout.arrange = CabinetArrange::PackRows;
        layout.order = CabinetOrder::RowsSerpentine;
        layout.portCapacity = 655360; // 40 cabinets of 16,384 px per port
        doc.addCabinetLayout(0, layout);
        const QVector<Slice> &cabs = p.screens[0].slices;
        check(cabs.size() == 50 && cabs[9].cabinet == QPoint(9, 0) && cabs[10].cabinet == QPoint(9, 1)
                  && cabs[19].cabinet == QPoint(0, 1),
              "cabinet chain runs serpentine through the rows");
        check(cabs[14].output == QRect(14 * 128, 0, 128, 128) && cabs[15].output == QRect(0, 128, 128, 128),
              "packed rows wrap at the screen width");
        check(cabs[39].port == 1 && cabs[40].port == 2 && CabinetMapping::summarize(p.screens[0], 0).ports == 2,
              "cabinets are split onto ports by capacity");

        // Per-cabinet settings survive regeneration; a cabinet marked not fitted drops out of the chain.
        for (Slice &sl : p.screens[0].slices)
            if (sl.cabinet == QPoint(5, 2))
                sl.gainR = 0.0;
        LedWall w = p.walls[0];
        w.disabledPanels = {QPoint(3, 0)};
        doc.applyWall(0, w);
        bool kept = false;
        for (const Slice &sl : p.screens[0].slices)
            kept = kept || (sl.cabinet == QPoint(5, 2) && sl.gainR == 0.0);
        check(p.screens[0].slices.size() == 49 && p.screens[0].slices[3].cabinet == QPoint(4, 0) && kept,
              "layouts follow wall edits and keep per-cabinet colour correction");
        w.disabledPanels.clear();
        doc.applyWall(0, w);
        for (Slice &sl : p.screens[0].slices)
            sl.gainR = 1.0;

        // Wall shape turned 90 degrees clockwise: the wall's top-left cabinet lands top-right.
        layout.arrange = CabinetArrange::WallShape;
        layout.rotation = 1;
        layout.portCapacity = 0;
        doc.setCabinetLayout(0, 0, layout);
        check(p.screens[0].slices[0].cabinet == QPoint(0, 0) && p.screens[0].slices[0].output == QRect(4 * 128, 0, 128, 128)
                  && p.screens[0].slices[0].rotation == 1 && p.screens[0].slices[0].port == 0,
              "wall-shape layout rotates the whole wall");

        // On the GPU: every cabinet shows its own cabinet. Panel Colours gives each one a hue by panel
        // number; each packed output cabinet must match its cabinet's colour in the composition.
        layout.arrange = CabinetArrange::PackRows;
        layout.rotation = 0;
        layout.portCapacity = 655360;
        doc.setCabinetLayout(0, 0, layout);
        p.compEffects = {makeEffect(QStringLiteral("panelColours"))};
        p.compEffects[0].params = {2, 1, 0, 1}; // by panel number, full spread, no drift
        doc.notifyChanged(true);
        auto pixelAt = [&](int view, const QPointF &pt, const QSize &space) {
            const QImage &img = previews[view];
            if (img.isNull())
                return QColor();
            return QColor(img.pixel(qBound(0, int(pt.x() * img.width() / space.width()), img.width() - 1),
                                    qBound(0, int(pt.y() * img.height() / space.height()), img.height() - 1)));
        };
        auto cabinetsMatch = [&] {
            const QSize screen(p.screens[0].width, p.screens[0].height);
            for (int i : {0, 7, 13, 22, 38, 49}) {
                const Slice &sl = p.screens[0].slices[i];
                const QColor in = pixelAt(0, QRectF(sl.input).center(), p.compSize());
                const QColor out = pixelAt(1, QRectF(sl.output).center(), screen);
                if (!in.isValid() || !out.isValid() || qGray(in.rgb()) < 40 || !near(in, out.red(), out.green(), out.blue()))
                    return false;
            }
            return true;
        };
        check(waitFor(cabinetsMatch, 8000), "each output cabinet shows its own cabinet's content");
        save(previews[1], "cabinet-layout-output");
        // Identify slices: port 2's route starts with a marker in its colour on the real output.
        {
            const QSize screen(p.screens[0].width, p.screens[0].height);
            const QPointF start = QRectF(p.screens[0].slices[40].output).center();
            const QColor port2 = CabinetMapping::portColor(2);
            engine.setIdentifySlices(true);
            const bool shown = waitFor([&] { return near(pixelAt(1, start, screen), port2.red(), port2.green(), port2.blue()); });
            save(previews[1], "identify-slices");
            engine.setIdentifySlices(false);
            const bool hidden = waitFor([&] { return !near(pixelAt(1, start, screen), port2.red(), port2.green(), port2.blue()); });
            check(shown && hidden, "identify slices draws port routes on the outputs");
        }
        doc.select(Selection::ofScreen(0));
        waitMs(300);
        window.grab().save(QStringLiteral("%1/cabinet-layout-window.png").arg(outDir));
        p.compEffects.clear();
        doc.removeCabinetLayout(0, 0);
        check(p.screens[0].slices.isEmpty() && p.screens[0].cabinetLayouts.isEmpty(),
              "removing a layout removes its cabinet slices");

        // Angled slice: a 480 px square over the white and yellow bars, turned 90 degrees, shows the
        // bars lying down: yellow along the top of the output, white along the bottom.
        doc.setClip(0, 2, Clip::fromPattern(TestPattern::ColorBars));
        doc.clearLayer(1);
        doc.triggerClip(0, 2);
        Slice sq;
        sq.input = QRect(0, 0, 480, 480);
        sq.output = QRect(0, 0, 480, 480);
        p.screens[0].slices = {sq};
        doc.notifyChanged(true);
        const QSize screen(p.screens[0].width, p.screens[0].height);
        auto isWhite = [](const QColor &c) { return c.red() > 200 && c.green() > 200 && c.blue() > 200; };
        auto isYellow = [](const QColor &c) { return c.red() > 200 && c.green() > 200 && c.blue() < 80; };
        const bool upright = waitFor([&] {
            return isWhite(pixelAt(1, {60, 240}, screen)) && isYellow(pixelAt(1, {420, 240}, screen));
        });
        p.screens[0].slices[0].angle = 90.0;
        doc.notifyChanged();
        const bool turned = waitFor([&] {
            return isYellow(pixelAt(1, {240, 60}, screen)) && isWhite(pixelAt(1, {240, 420}, screen));
        });
        save(previews[1], "slice-angle");
        check(upright && turned, "angled slice samples a rotated area");

        // Corner pin: the whole composition squeezed into a trapezoid (narrower at the top).
        Slice pin;
        pin.input = QRect(QPoint(), p.compSize());
        pin.output = QRect(0, 0, 960, 540);
        pin.warp = QPolygonF({QPointF(100, 0), QPointF(860, 0), QPointF(960, 540), QPointF(0, 540)});
        p.screens[0].slices = {pin};
        doc.notifyChanged(true);
        auto isBlue = [](const QColor &c) { return c.blue() > 200 && c.red() < 60 && c.green() < 60; };
        const bool pinned = waitFor([&] {
            return qGray(pixelAt(1, {40, 300}, screen).rgb()) < 10 && isWhite(pixelAt(1, {75, 300}, screen))
                   && isBlue(pixelAt(1, {750, 300}, screen)) && qGray(pixelAt(1, {935, 300}, screen).rgb()) < 10;
        });
        save(previews[1], "slice-corner-pin");
        check(pinned, "corner-pinned slice fills its quad");

        // Mesh warp: a flat 3 x 3 mesh shows the bars as before; pulling the centre point right
        // stretches the left half, so green (bar 3 of 8) reaches x = 540 on the middle row.
        Slice meshed;
        meshed.input = QRect(QPoint(), p.compSize());
        meshed.output = QRect(0, 0, 960, 540);
        meshed.resetMesh(3, 3);
        p.screens[0].slices = {meshed};
        doc.notifyChanged(true);
        auto isGreen = [](const QColor &c) { return c.green() > 200 && c.red() < 60 && c.blue() < 60; };
        auto isMagenta = [](const QColor &c) { return c.red() > 200 && c.blue() > 200 && c.green() < 60; };
        const bool flat = waitFor([&] {
            return isWhite(pixelAt(1, {60, 200}, screen)) && isBlue(pixelAt(1, {780, 200}, screen))
                   && isMagenta(pixelAt(1, {540, 270}, screen));
        });
        p.screens[0].slices[0].mesh[4] = QPointF(680, 270);
        doc.notifyChanged();
        const bool bent = waitFor([&] {
            return isGreen(pixelAt(1, {540, 270}, screen)) && isWhite(pixelAt(1, {60, 270}, screen));
        });
        save(previews[1], "slice-mesh-warp");
        check(flat && bent, "mesh warp bends the slice smoothly");
        doc.select(Selection::ofSlice(0, 0));
        waitMs(300);
        window.grab().save(QStringLiteral("%1/projection-window.png").arg(outDir));

        // Edge blend over white: a linear 240 px ramp on the right goes from full to black.
        Clip whiteSolid = Clip::fromPattern(TestPattern::Solid);
        whiteSolid.solidColor = Qt::white;
        doc.setClip(0, 2, whiteSolid);
        doc.triggerClip(0, 2);
        Slice blended;
        blended.input = QRect(QPoint(), p.compSize());
        blended.output = QRect(0, 0, 960, 540);
        blended.blendRight = 240;
        blended.blendCurve = 1.0;
        blended.blendGamma = 1.0;
        p.screens[0].slices = {blended};
        doc.notifyChanged(true);
        const bool ramp = waitFor([&] {
            const int full = qGray(pixelAt(1, {400, 270}, screen).rgb());
            const int mid = qGray(pixelAt(1, {840, 270}, screen).rgb());
            const int edge = qGray(pixelAt(1, {956, 270}, screen).rgb());
            return full > 240 && mid > 100 && mid < 160 && edge < 40;
        });
        save(previews[1], "slice-edge-blend");
        check(ramp, "edge blend ramps the overlap down");

        // Projection masks: the middle blacked out; inverted, only the middle stays.
        Slice plain;
        plain.input = QRect(QPoint(), p.compSize());
        plain.output = QRect(0, 0, p.screens[0].width, p.screens[0].height);
        p.screens[0].slices = {plain};
        ProjectionMask mask;
        mask.feather = 0;
        mask.points = QPolygonF(QRectF(660, 340, 600, 400)).mid(0, 4);
        p.screens[0].masks = {mask};
        doc.notifyChanged(true);
        const bool masked = waitFor([&] {
            return qGray(pixelAt(1, {960, 540}, screen).rgb()) < 10 && qGray(pixelAt(1, {200, 200}, screen).rgb()) > 240;
        });
        p.screens[0].masks[0].invert = true;
        doc.notifyChanged();
        const bool inverted = waitFor([&] {
            return qGray(pixelAt(1, {960, 540}, screen).rgb()) > 240 && qGray(pixelAt(1, {200, 200}, screen).rgb()) < 10;
        });
        save(previews[1], "projection-mask");
        p.screens[0].masks[0].invert = false;
        p.screens[0].masks[0].feather = 120;
        doc.notifyChanged();
        const bool soft = waitFor([&] { // on the edge of a 120 px feather: half way
            const int g = qGray(pixelAt(1, {660, 540}, screen).rgb());
            return g > 70 && g < 190;
        });
        check(masked && inverted && soft, "projection masks black out with soft edges");
        p.screens[0].masks.clear();
        doc.setClip(0, 2, Clip::fromPattern(TestPattern::ColorBars));

        p.screens[0].slices = savedSlices;
        doc.notifyChanged(true);
    }

    // Rotated walls: a 4 x 2 wall of 128 px cabinets at 700,400 turned 90 degrees clockwise about its
    // centre (956,528). Cabinet C1 R1 (unrotated centre 764,464) hangs at 1020,336; C4 R1 at 1020,720.
    {
        Project &p = doc.project();
        const LedWall savedWall = p.walls[0];
        const QVector<Screen> savedScreens = p.screens;
        LedWall w = savedWall;
        w.cols = 4;
        w.rows = 2;
        w.x = 700;
        w.y = 400;
        w.angle = 90.0;
        w.disabledPanels.clear();
        doc.applyWall(0, w);
        const QPointF c11 = p.walls[0].panelCentre(0, 0), c41 = p.walls[0].panelCentre(3, 0);
        check(qAbs(c11.x() - 1020) < 0.01 && qAbs(c11.y() - 336) < 0.01 && qAbs(c41.y() - 720) < 0.01
                  && QRect(827, 271, 258, 514).contains(p.walls[0].bounds())
                  && p.walls[0].bounds().contains(QRect(829, 273, 254, 510)),
              "rotated wall geometry");

        auto pixel = [&](const QPointF &pt) {
            const QImage &img = previews[0];
            if (img.isNull())
                return QColor();
            return QColor(img.pixel(qBound(0, int(pt.x() * img.width() / p.compWidth), img.width() - 1),
                                    qBound(0, int(pt.y() * img.height() / p.compHeight), img.height() - 1)));
        };
        const QPointF unrotatedOnly(720, 420); // inside the upright rect, outside the turned wall

        // Panel mask over a white solid: only the turned cabinets stay lit.
        Clip white = Clip::fromPattern(TestPattern::Solid);
        white.solidColor = Qt::white;
        const int col = p.columns;
        doc.addColumn();
        doc.setClip(0, col, white);
        doc.clearLayer(1);
        p.layers[0].targetWall = -1;
        p.layers[0].effects.clear();
        p.compEffects.clear();
        doc.triggerClip(0, col);
        p.maskToPanels = true;
        doc.notifyChanged();
        check(waitFor([&] { return qGray(pixel(c11).rgb()) > 200 && qGray(pixel(c41).rgb()) > 200
                                   && qGray(pixel(unrotatedOnly).rgb()) < 10; }),
              "panel mask follows a rotated wall");
        save(previews[0], "rotated-wall-mask");
        p.maskToPanels = false;

        // Panel outline in each cabinet's own frame: 5 px inside C1 R1's left edge (unrotated 705,464).
        EffectInstance outline = makeEffect(QStringLiteral("panelOutline"));
        outline.params = {12, 1, 0, 0, 1}; // red, 12 px
        p.compEffects = {outline};
        doc.notifyChanged(true);
        const QPointF nearEdge = p.walls[0].transform().map(QPointF(705, 464));
        check(waitFor([&] { return near(pixel(nearEdge), 255, 0, 0) && near(pixel(c11), 255, 255, 255); }),
              "panel outline follows rotated cabinets");
        save(previews[0], "rotated-wall-outline");

        // Panel colours by panel number: C1 R1 is red where it hangs.
        EffectInstance colours = makeEffect(QStringLiteral("panelColours"));
        colours.params = {2, 1, 0, 1};
        p.compEffects = {colours};
        doc.notifyChanged(true);
        check(waitFor([&] { return near(pixel(c11), 255, 0, 0); }), "panel effects find rotated cabinets");
        p.compEffects.clear();
        doc.notifyChanged(true);

        // A layer fitted into the wall turns with it: colour bars stretched over the upright wall
        // put white (left bar) in C1 R1 and black (right bar) in C4 R1, wherever they hang.
        Clip bars = Clip::fromPattern(TestPattern::ColorBars);
        bars.scale = ScaleMode::Stretch;
        doc.setClip(0, col, bars);
        p.layers[0].targetWall = 0;
        doc.triggerClip(0, col);
        doc.notifyChanged();
        const QPointF inWhiteBar = p.walls[0].transform().map(QPointF(730, 464));
        const QPointF inBlackBar = p.walls[0].transform().map(QPointF(1180, 464));
        check(waitFor([&] { return near(pixel(inWhiteBar), 255, 255, 255) && qGray(pixel(inBlackBar).rgb()) < 40
                                   && qGray(pixel(unrotatedOnly).rgb()) < 10; }),
              "layers fitted into a rotated wall turn with it");
        save(previews[0], "rotated-wall-layer");

        // Cabinet slices sample each cabinet where it hangs, at the wall's angle.
        p.screens[0].slices.clear();
        CabinetLayout layout;
        layout.wall = 0;
        layout.arrange = CabinetArrange::PackRows;
        doc.addCabinetLayout(0, layout);
        const Slice first = p.screens[0].slices.value(0);
        check(p.screens[0].slices.size() == 8 && first.angle == 90.0
                  && QRectF(first.input).center() == c11,
              "cabinet slices of a rotated wall sample each cabinet where it hangs");
        const QSize screen(p.screens[0].width, p.screens[0].height);
        auto outPixel = [&](const QPointF &pt) {
            const QImage &img = previews[1];
            return img.isNull() ? QColor()
                                : QColor(img.pixel(qBound(0, int(pt.x() * img.width() / screen.width()), img.width() - 1),
                                                   qBound(0, int(pt.y() * img.height() / screen.height()), img.height() - 1)));
        };
        // The output shows the cabinet upright: 30 px from its left is the wall's white bar.
        check(waitFor([&] { return near(outPixel(QPointF(first.output.x() + 30, QRectF(first.output).center().y())), 255, 255, 255); }),
              "rotated cabinet content reaches the output");
        save(previews[1], "rotated-wall-output");

        doc.removeCabinetLayout(0, 0);
        p.layers[0].targetWall = -1;
        doc.applyWall(0, savedWall);
        p.screens = savedScreens;
        doc.notifyChanged(true);
    }

    // Clip playback: trigger modes, transitions, transforms and clip effects (any machine), then
    // movie playback controls when DISPLAYHAWK_TEST_HAP_PLAYBACK names a HAP movie of a few seconds.
    {
        Project &p = doc.project();
        p.layers[0].effects.clear();
        p.layers[1].effects.clear();
        p.compEffects.clear();
        p.layers[0].blend = p.layers[1].blend = BlendMode::Normal;
        doc.clearLayer(1);
        const int col = p.columns;
        doc.addColumn();
        doc.addColumn();
        Clip red = Clip::fromPattern(TestPattern::Solid);
        red.solidColor = Qt::red;
        Clip blue = Clip::fromPattern(TestPattern::Solid);
        blue.solidColor = Qt::blue;
        doc.setClip(0, col, red);
        doc.setClip(0, col + 1, blue);
        p.layers[0].targetWall = -1;
        p.layers[0].transitionMs = 0;
        doc.notifyChanged();
        auto at = [&](double fx, double fy) {
            const QImage &img = previews[0];
            return img.isNull() ? QColor() : QColor(img.pixel(int(fx * (img.width() - 1)), int(fy * (img.height() - 1))));
        };

        // Toggle: triggering the playing clip again stops the layer. Piano: releasing stops it.
        p.layers[0].clips[col].trigger = TriggerMode::Toggle;
        doc.triggerClip(0, col);
        const bool started = p.layers[0].activeColumn == col;
        doc.triggerClip(0, col);
        check(started && p.layers[0].activeColumn == -1, "toggle clips stop when triggered again");
        p.layers[0].clips[col].trigger = TriggerMode::Piano;
        doc.triggerClip(0, col);
        doc.releaseClip(0, col);
        check(p.layers[0].activeColumn == -1, "piano clips stop when released");
        p.layers[0].clips[col].trigger = TriggerMode::Retrigger;
        doc.triggerClip(0, col);
        doc.releaseClip(0, col);
        check(p.layers[0].activeColumn == col, "retrigger clips keep playing after release");
        check(waitFor([&] { return near(at(0.5, 0.5), 255, 0, 0); }), "red clip shows");

        // Wipe right over 4 s: part way, blue has come in from the left, red is still on the right.
        p.layers[0].transitionMs = 4000;
        p.layers[0].transition = TransitionStyle::WipeRight;
        doc.notifyChanged();
        doc.triggerClip(0, col + 1);
        const bool wipe = waitFor([&] { return near(at(0.1, 0.5), 0, 0, 255) && near(at(0.9, 0.5), 255, 0, 0); }, 3000);
        save(previews[0], "transition-wipe");
        check(wipe, "wipe transition reveals the new clip from one side");
        // Push left: the incoming clip slides in from the right.
        p.layers[0].transition = TransitionStyle::PushLeft;
        doc.notifyChanged();
        doc.triggerClip(0, col);
        const bool push = waitFor([&] { return near(at(0.9, 0.5), 255, 0, 0) && near(at(0.1, 0.5), 0, 0, 255); }, 3000);
        check(push, "push transition slides the new clip in");
        p.layers[0].transitionMs = 0;
        p.layers[0].transition = TransitionStyle::Crossfade;
        doc.notifyChanged();
        doc.clearLayer(0);
        doc.triggerClip(0, col + 1);

        // Clip transform: half size leaves the corners empty; moved right, the left edge empties.
        p.layers[0].clips[col + 1].scaleBy = 0.5;
        doc.notifyChanged();
        const bool scaled = waitFor([&] { return qGray(at(0.05, 0.05).rgb()) < 10 && near(at(0.5, 0.5), 0, 0, 255); });
        p.layers[0].clips[col + 1].scaleBy = 1.0;
        p.layers[0].clips[col + 1].posX = p.compWidth / 2.0;
        doc.notifyChanged();
        const bool moved = waitFor([&] { return qGray(at(0.25, 0.5).rgb()) < 10 && near(at(0.75, 0.5), 0, 0, 255); });
        save(previews[0], "clip-transform");
        check(scaled && moved, "clip transform scales and moves the clip");
        p.layers[0].clips[col + 1].posX = 0.0;

        // Clip effects: invert on the clip turns blue to yellow.
        p.layers[0].clips[col + 1].effects = {makeEffect(QStringLiteral("invert"))};
        doc.notifyChanged(true);
        check(waitFor([&] { return near(at(0.5, 0.5), 255, 255, 0); }), "clip effects apply to the clip");
        p.layers[0].clips[col + 1].effects.clear();
        doc.notifyChanged(true);

        // Autopilot on clips without an end: after the layer's time it moves to the next clip.
        p.layers[0].autopilot = Autopilot::Next;
        p.layers[0].autopilotSeconds = 0.5;
        doc.notifyChanged();
        doc.triggerClip(0, col);
        const bool moved1 = waitFor([&] { return p.layers[0].activeColumn == col + 1; }, 3000);
        check(moved1, "autopilot moves on after its time");
        p.layers[0].autopilot = Autopilot::Off;
        doc.notifyChanged();

        const QString hap = qEnvironmentVariable("DISPLAYHAWK_TEST_HAP_PLAYBACK");
        if (!hap.isEmpty()) {
            doc.setClip(0, col, Clip::fromMedia(hap));
            Clip &movie = p.layers[0].clips[col];
            movie.playMode = PlayMode::OnceHold;
            doc.notifyChanged();
            doc.triggerClip(0, col);
            const QString id = movie.id;
            const bool opened = waitFor([&] {
                const MediaSource *m = engine.mediaFor(id);
                return m && m->isHap() && m->position() >= 0;
            }, 5000);
            MediaSource *m = engine.mediaFor(id);
            auto pos = [&] { return m ? m->position() : -1.0; };
            auto setMovie = [&](const std::function<void(Clip &)> &fn) {
                fn(p.layers[0].clips[col]);
                doc.notifyChanged();
            };
            if (opened) {
                const double duration = m->durationSeconds();
                // Speed: at 2x, 0.5 s of play covers 1 s of the movie.
                setMovie([](Clip &c) { c.speed = 2.0; });
                m->restart();
                waitMs(500);
                const double fast = pos() * duration;
                check(fast > 0.75 && fast < 1.3, qPrintable(QStringLiteral("speed 2x (%1 s of movie in 0.5 s)").arg(fast, 0, 'f', 2)));
                // Reverse: from the end, the position goes down.
                setMovie([](Clip &c) {
                    c.speed = 1.0;
                    c.reverse = true;
                });
                m->restart();
                waitMs(100);
                const double r0 = pos();
                waitMs(400);
                const double r1 = pos();
                check(r0 > 0.8 && r1 < r0 - 0.1, "reverse plays backwards");
                // In/out points, looping: the position stays between them.
                setMovie([](Clip &c) {
                    c.reverse = false;
                    c.playMode = PlayMode::Loop;
                    c.inPoint = 0.25;
                    c.outPoint = 0.5;
                });
                m->restart();
                bool inside = true;
                double lo = 1, hi = 0;
                for (int i = 0; i < 20; ++i) {
                    waitMs(75);
                    lo = qMin(lo, pos());
                    hi = qMax(hi, pos());
                    inside = inside && pos() >= 0.24 && pos() <= 0.51;
                }
                check(inside && hi - lo > 0.15, "in and out points bound the loop");
                // Bounce: it runs up to the out point, then back down.
                setMovie([](Clip &c) { c.playMode = PlayMode::Bounce; });
                m->restart();
                bool up = false, down = false;
                double last = pos();
                for (int i = 0; i < 30; ++i) {
                    waitMs(60);
                    const double now = pos();
                    up = up || now > last + 0.005;
                    down = down || (up && now < last - 0.005);
                    last = now;
                }
                check(up && down, "bounce plays forwards then backwards");
                // Play once and clear: the layer empties at the out point.
                bool finished = false;
                QObject::connect(&engine, &RenderEngine::clipFinished, &app,
                                 [&](const QString &clip, bool ended) { finished = finished || (ended && clip == id); });
                setMovie([](Clip &c) {
                    c.playMode = PlayMode::OnceClear;
                    c.inPoint = 0.0;
                    c.outPoint = 1.0;
                    c.speed = 4.0;
                });
                m->restart();
                check(waitFor([&] { return finished && p.layers[0].activeColumn == -1; }, int(duration * 1000) + 2000),
                      "play once and clear empties the layer at the end");
                // Autopilot on a movie: when it reaches its end the layer moves to the next clip.
                setMovie([](Clip &c) { c.playMode = PlayMode::OnceHold; });
                p.layers[0].autopilot = Autopilot::Next;
                doc.notifyChanged();
                doc.triggerClip(0, col);
                check(waitFor([&] { return p.layers[0].activeColumn == col + 1; }, int(duration * 1000) + 2000),
                      "autopilot follows a movie's end");
                p.layers[0].autopilot = Autopilot::Off;
                doc.notifyChanged();
            } else {
                check(false, "HAP playback movie opens");
            }
        } else {
            qInfo("selftest: SKIP movie playback controls (set DISPLAYHAWK_TEST_HAP_PLAYBACK to a HAP movie)");
        }
        doc.clearLayer(0);
    }

    // Tempo: tap, MIDI clock and audio beats (synthetic timestamps), quantized triggers, LFOs.
    {
        Project &p = doc.project();
        TempoClock *clock = window.tempoClock();
        const qint64 t0 = TempoClock::nowNs();
        const qint64 ms = 1'000'000;
        p.tempoSource = TempoSource::Internal;
        doc.notifyChanged();
        for (int i = 0; i < 4; ++i)
            clock->tap(t0 + i * 500 * ms);
        check(qAbs(p.bpm - 120.0) < 0.5, qPrintable(QStringLiteral("tap tempo (%1 BPM from 500 ms taps)").arg(p.bpm, 0, 'f', 1)));

        p.tempoSource = TempoSource::MidiClock;
        doc.notifyChanged();
        const qint64 tick = qint64(60e9 / (130.0 * 24));
        clock->midiRealtime(0xFA, t0);
        for (int i = 0; i <= 48; ++i)
            clock->midiRealtime(0xF8, t0 + i * tick);
        check(qAbs(p.bpm - 130.0) < 0.5, qPrintable(QStringLiteral("MIDI clock tempo (%1 BPM)").arg(p.bpm, 0, 'f', 1)));

        p.tempoSource = TempoSource::AudioBeats;
        doc.notifyChanged();
        for (int i = 0; i < 8; ++i)
            clock->audioBeat(t0 + 10'000 * ms + i * 500 * ms);
        check(qAbs(p.bpm - 120.0) < 2.0, qPrintable(QStringLiteral("tempo follows audio beats (%1 BPM)").arg(p.bpm, 0, 'f', 1)));

        // Quantized to the bar: a trigger waits for the next bar start.
        p.tempoSource = TempoSource::Internal;
        doc.setBpm(120.0);
        p.quantize = TriggerQuantize::Bar;
        doc.notifyChanged();
        const int col = p.columns;
        doc.addColumn();
        doc.setClip(0, col, Clip::fromPattern(TestPattern::ColorBars));
        p.layers[0].transitionMs = 0;
        doc.clearLayer(0);
        clock->resync();
        waitMs(700); // part way through the bar
        double startedAt = -1.0;
        const QMetaObject::Connection c = QObject::connect(&doc, &Document::clipTriggered, &app, [&] { startedAt = clock->beats(); });
        doc.triggerClip(0, col);
        const bool waited = p.layers[0].activeColumn == -1;
        const bool started = waitFor([&] { return p.layers[0].activeColumn == col; }, 2500);
        QObject::disconnect(c);
        const double intoBar = startedAt - 4.0 * qFloor(startedAt / 4.0 + 0.5);
        check(waited && started && qAbs(intoBar) < 0.1,
              qPrintable(QStringLiteral("quantized trigger starts on the bar (%1 beats off)").arg(intoBar, 0, 'f', 3)));
        p.quantize = TriggerQuantize::Off;
        doc.notifyChanged();

        // LFO: a square wave on the master over 2 beats (1 s at 120 BPM) blinks the output.
        Lfo lfo;
        lfo.target = AudioTarget::Master;
        lfo.shape = LfoShape::Square;
        lfo.beats = 2.0;
        p.lfos = {lfo};
        doc.notifyChanged(true);
        bool dark = false, bright = false;
        for (int i = 0; i < 50 && !(dark && bright); ++i) {
            waitMs(60);
            const int lit = litPercent(previews[0]);
            dark = dark || lit < 3;
            bright = bright || lit > 40;
        }
        check(dark && bright, "LFO on the master follows the tempo");
        p.lfos.clear();
        doc.notifyChanged(true);
        check(waitFor([&] { return litPercent(previews[0]) > 40; }, 2000), "removing the LFO restores the master");

        // Beat sync: a 2 s movie stretched to 8 beats at 120 BPM plays at half speed.
        const QString hap = qEnvironmentVariable("DISPLAYHAWK_TEST_HAP_PLAYBACK");
        if (!hap.isEmpty()) {
            Clip movie = Clip::fromMedia(hap);
            movie.beatSync = 8;
            movie.playMode = PlayMode::OnceHold;
            doc.setClip(0, col, movie);
            doc.triggerClip(0, col);
            const QString id = p.layers[0].clips[col].id;
            waitFor([&] { const MediaSource *m = engine.mediaFor(id); return m && m->durationSeconds() > 0; }, 5000);
            MediaSource *m = engine.mediaFor(id);
            if (m) {
                waitMs(200); // the speed is set once the length is known
                m->restart();
                waitMs(1000);
                const double movieSeconds = m->position() * m->durationSeconds();
                check(movieSeconds > 0.35 && movieSeconds < 0.65,
                      qPrintable(QStringLiteral("beat sync stretches the movie (%1 s of movie in 1 s)").arg(movieSeconds, 0, 'f', 2)));
            }
        }
        doc.clearLayer(0);
    }

    // Mixing: shader blend modes and masks, layer groups, the A/B crossfader. Layer 1 plays red,
    // layer 2 the colour bars (white, yellow, cyan, green, magenta, red, blue, black).
    {
        Project &p = doc.project();
        const int col = p.columns;
        doc.addColumn();
        Clip red = Clip::fromPattern(TestPattern::Solid);
        red.solidColor = Qt::red;
        doc.setClip(0, col, red);
        doc.setClip(1, col, Clip::fromPattern(TestPattern::ColorBars));
        for (Layer &l : p.layers) {
            l.transitionMs = 0;
            l.effects.clear();
            l.group.clear();
            l.crossfade = CrossfadeSide::None;
            l.targetWall = -1;
        }
        p.compEffects.clear();
        p.layers[0].blend = BlendMode::Normal;
        doc.setLayerPlayback(0, 1.0, false, false);
        doc.setLayerPlayback(1, 1.0, false, false);
        doc.triggerColumn(col);
        auto mode = [&](BlendMode m) {
            p.layers[1].blend = m;
            doc.notifyChanged();
        };
        mode(BlendMode::Difference);
        check(waitFor([&] { return near(barColor(0), 0, 255, 255) && near(barColor(3), 255, 255, 0); }),
              "difference blend (red vs white -> cyan, vs green -> yellow)");
        save(previews[0], "blend-difference");
        mode(BlendMode::Subtract);
        check(waitFor([&] { return near(barColor(0), 0, 0, 0) && near(barColor(6), 255, 0, 0); }),
              "subtract blend (red - white -> black, red - blue -> red)");
        mode(BlendMode::LumaMask);
        check(waitFor([&] { return near(barColor(0), 255, 0, 0) && near(barColor(7), 0, 0, 0)
                                   && qAbs(barColor(3).red() - 150) < 40; }),
              "luma mask shows the layers below through bright areas only");
        save(previews[0], "blend-luma-mask");
        mode(BlendMode::Normal);
        check(waitFor([&] { return near(barColor(0), 255, 255, 255) && near(barColor(6), 0, 0, 255); }),
              "back to normal blending");

        // A group holding layer 2: its opacity and bypass act on the layer as one.
        LayerGroup g;
        g.id = newId();
        g.name = QStringLiteral("Bars");
        g.opacity = 0.0;
        p.groups = {g};
        p.layers[1].group = g.id;
        doc.notifyChanged(true);
        check(waitFor([&] { return near(barColor(0), 255, 0, 0) && near(barColor(6), 255, 0, 0); }),
              "group at zero opacity hides its layers");
        p.groups[0].opacity = 1.0;
        doc.notifyChanged();
        check(waitFor([&] { return near(barColor(0), 255, 255, 255) && near(barColor(6), 0, 0, 255); }),
              "group at full opacity shows its layers");
        p.groups[0].blend = BlendMode::Difference; // shader blend for the whole group
        doc.notifyChanged();
        const bool groupBlend = waitFor([&] { return near(barColor(0), 0, 255, 255); });
        save(previews[0], "group-difference");
        check(groupBlend, "group blend mode applies to the group");
        p.groups[0].bypass = true;
        doc.notifyChanged();
        check(waitFor([&] { return near(barColor(0), 255, 0, 0); }), "bypassed group is hidden");
        p.groups.clear();
        p.layers[1].group.clear();
        doc.notifyChanged(true);

        // Crossfader: layer 2 on B, layer 1 on A.
        p.layers[0].crossfade = CrossfadeSide::A;
        p.layers[1].crossfade = CrossfadeSide::B;
        doc.notifyChanged();
        doc.setCrossfader(0.0);
        const bool onlyA = waitFor([&] { return near(barColor(0), 255, 0, 0) && near(barColor(6), 255, 0, 0); });
        doc.setCrossfader(1.0);
        const bool onlyB = waitFor([&] { return near(barColor(0), 255, 255, 255) && near(barColor(7), 0, 0, 0); });
        doc.setCrossfader(0.5);
        const bool both = waitFor([&] { return near(barColor(7), 0, 0, 0) && near(barColor(0), 255, 255, 255); });
        check(onlyA && onlyB && both, "A/B crossfader");

        // Groups, decks and crossfader sides are saved.
        p.groups = {g};
        p.layers[1].group = g.id;
        p.decks = {Deck{QStringLiteral("Intro"), 0, 4}, Deck{QStringLiteral("Drop"), 4, 4}};
        const Project back = Project::fromJson(p.toJson());
        check(back.groups.size() == 1 && back.groups[0].name == QStringLiteral("Bars") && back.layers[1].group == g.id
                  && back.decks.size() == 2 && back.decks[1].name == QStringLiteral("Drop") && back.decks[1].firstColumn == 4
                  && back.layers[1].crossfade == CrossfadeSide::B && back.layers[0].crossfade == CrossfadeSide::A,
              "groups, decks and crossfader sides save and load");
        p.groups.clear();
        p.decks.clear();
        for (Layer &l : p.layers) {
            l.group.clear();
            l.crossfade = CrossfadeSide::None;
        }
        doc.notifyChanged(true);
        doc.clearLayer(1);
    }

    // Generators and text.
    {
        Project &p = doc.project();
        doc.clearLayer(1);
        const int col = p.columns;
        doc.addColumn();
        p.layers[0].transitionMs = 0;
        p.layers[0].blend = BlendMode::Normal;
        p.layers[0].effects.clear();
        p.compEffects.clear();
        doc.setLayerPlayback(0, 1.0, false, false);
        auto centre = [&] {
            const QImage &img = previews[0];
            return img.isNull() ? QColor() : QColor(img.pixel(img.width() / 2, img.height() / 2));
        };

        Clip text = Clip::fromText(QStringLiteral("HAWK"));
        text.solidColor = Qt::white;
        doc.setClip(0, col, text);
        doc.triggerClip(0, col);
        check(waitFor([&] { const int lit = litPercent(previews[0]); return lit > 3 && lit < 80; }), "text clip draws its text");
        save(previews[0], "text-still");
        p.layers[0].clips[col].textMotion = TextMotion::ScrollLeft;
        p.layers[0].clips[col].textSpeed = 600;
        doc.notifyChanged();
        waitFor([&] { return litPercent(previews[0]) > 3; });
        const QImage before = previews[0];
        waitMs(400);
        save(previews[0], "text-ticker");
        check(litPercent(previews[0]) > 3 && previews[0] != before, "text ticker scrolls");

        for (const GeneratorDef &d : generatorDefs()) {
            Clip g = Clip::fromGenerator(d.id, generatorDefaults(d.id));
            g.solidColor = Qt::white;
            g.color2 = Qt::black;
            doc.setClip(0, col, g);
            doc.triggerClip(0, col);
            const bool drawn = waitFor([&] { return litPercent(previews[0]) > 1; }, 3000);
            const QImage a = previews[0];
            waitMs(400);
            const bool still = d.id == QLatin1String("shapes"); // the default shape does not move
            save(previews[0], qPrintable(QStringLiteral("generator-%1").arg(d.id)));
            check(drawn && (still || previews[0] != a),
                  qPrintable(QStringLiteral("generator %1 draws and moves").arg(d.name)));
        }
        // Shapes: a red circle on black, half the height: red in the middle, black in the corner.
        Clip circle = Clip::fromGenerator(QStringLiteral("shapes"), generatorDefaults(QStringLiteral("shapes")));
        circle.solidColor = Qt::red;
        circle.color2 = Qt::black;
        doc.setClip(0, col, circle);
        doc.triggerClip(0, col);
        check(waitFor([&] { return near(centre(), 255, 0, 0) && qGray(QColor(previews[0].pixel(2, 2)).rgb()) < 10; }),
              "shapes generator draws a circle in the clip's colours");
        doc.clearLayer(0);
    }

    // The rest of the effect catalogue on the colour bars: each renders; a few are checked closely.
    {
        Project &p = doc.project();
        const int col = p.columns;
        doc.addColumn();
        doc.clearLayer(1);
        doc.setClip(0, col, Clip::fromPattern(TestPattern::ColorBars));
        p.layers[0].transitionMs = 0;
        p.layers[0].blend = BlendMode::Normal;
        p.compEffects.clear();
        doc.triggerClip(0, col);
        const QStringList newer = {QStringLiteral("wave"), QStringLiteral("twirl"), QStringLiteral("bulge"),
                                   QStringLiteral("hexMosaic"), QStringLiteral("scroll"), QStringLiteral("vignette"),
                                   QStringLiteral("threshold"), QStringLiteral("duotone"), QStringLiteral("hueCycle"),
                                   QStringLiteral("glow"), QStringLiteral("grain"), QStringLiteral("scanlines"),
                                   QStringLiteral("sharpen"), QStringLiteral("zoomBlur")}; // trails: on moving content below
        p.layers[0].effects.clear();
        doc.notifyChanged(true);
        waitFor([&] { return near(barColor(0), 255, 255, 255); });
        const QImage plain = previews[0];
        QImage last = plain;
        for (const QString &id : newer) {
            p.layers[0].effects = {makeEffect(id)};
            doc.notifyChanged(true);
            // A new picture: neither the plain bars nor the previous effect's.
            const bool changed = waitFor([&] {
                return litPercent(previews[0]) > 0 && previews[0] != plain && previews[0] != last;
            }, 3000);
            waitMs(150); // one more preview, so the picture saved is settled
            last = previews[0];
            save(previews[0], qPrintable(QStringLiteral("fx-%1").arg(id)));
            check(changed, qPrintable(QStringLiteral("effect %1 renders").arg(effectDef(id)->name)));
        }
        // Threshold at 0.5: white and yellow stay white, blue (dark) goes black.
        p.layers[0].effects = {makeEffect(QStringLiteral("threshold"))};
        doc.notifyChanged(true);
        check(waitFor([&] { return near(barColor(0), 255, 255, 255) && near(barColor(1), 255, 255, 255) && near(barColor(6), 0, 0, 0); }),
              "threshold makes light parts white and dark parts black");
        // Trails on the moving bars leave more lit pixels than the bars alone.
        doc.setClip(0, col, Clip::fromPattern(TestPattern::MovingBars));
        p.layers[0].effects.clear();
        doc.notifyChanged(true);
        doc.triggerClip(0, col);
        waitMs(500);
        const int bare = litPercent(previews[0]);
        EffectInstance trails = makeEffect(QStringLiteral("trails"));
        trails.params[0] = 0.97;
        p.layers[0].effects = {trails};
        doc.notifyChanged(true);
        const bool smeared = waitFor([&] { return litPercent(previews[0]) > bare + 5; }, 3000);
        save(previews[0], "fx-trails-moving");
        check(smeared, qPrintable(QStringLiteral("trails leave a fading copy behind (%1%% lit -> %2%%)").arg(bare).arg(litPercent(previews[0]))));
        // Vignette on white: the corners go dark, the middle stays white.
        Clip white = Clip::fromPattern(TestPattern::Solid);
        white.solidColor = Qt::white;
        doc.setClip(0, col, white);
        p.layers[0].effects = {makeEffect(QStringLiteral("vignette"))};
        doc.notifyChanged(true);
        doc.triggerClip(0, col);
        check(waitFor([&] {
                  const QImage &img = previews[0];
                  return !img.isNull() && qGray(img.pixel(1, 1)) < 100 && qGray(img.pixel(img.width() / 2, img.height() / 2)) > 240;
              }),
              "vignette darkens the corners");
        p.layers[0].effects.clear();
        doc.notifyChanged(true);
        doc.clearLayer(0);
    }

    // ISF shaders: the bundled examples are found, compile at runtime and render; a broken one is
    // reported and skipped.
    {
        Project &p = doc.project();
        const QStringList filters = Isf::filterIds(), generators = Isf::generatorIds();
        auto has = [](const QStringList &ids, const char *name) {
            for (const QString &id : ids)
                if (id.endsWith(QLatin1String(name)))
                    return true;
            return false;
        };
        check(has(filters, "Ripple.fs") && has(filters, "Colour Pulse.fs") && has(generators, "Spiral Rays.fs")
                  && has(generators, "Metaballs.fs"),
              "ISF examples are found (filters as effects, the rest as generators)");
        const Isf::Info bad = Isf::parse(QStringLiteral("bad.fs"), QByteArray("void main() {}"));
        check(!bad.error.isEmpty(), "ISF without a JSON header is rejected");

        const int col = p.columns;
        doc.addColumn();
        doc.clearLayer(1);
        p.layers[0].transitionMs = 0;
        p.layers[0].effects.clear();
        p.compEffects.clear();
        QImage last = previews[0];
        for (const QString &id : generators) {
            Clip g = Clip::fromGenerator(id, {});
            doc.setClip(0, col, g);
            doc.triggerClip(0, col);
            // A new picture, not the previous test's.
            const bool lit = waitFor([&] { return litPercent(previews[0]) > 5 && previews[0] != last; }, 5000);
            waitMs(150);
            last = previews[0];
            save(previews[0], qPrintable(QStringLiteral("isf-%1").arg(QFileInfo(id).completeBaseName())));
            check(lit, qPrintable(QStringLiteral("ISF generator %1 compiles and renders").arg(QFileInfo(id).completeBaseName())));
        }
        doc.setClip(0, col, Clip::fromPattern(TestPattern::ColorBars));
        doc.triggerClip(0, col);
        waitFor([&] { return near(barColor(0), 255, 255, 255); });
        const QImage plain = previews[0];
        last = plain;
        for (const QString &id : filters) {
            p.layers[0].effects = {makeEffect(id)};
            doc.notifyChanged(true);
            const bool changed = waitFor([&] { return litPercent(previews[0]) > 5 && previews[0] != plain && previews[0] != last; }, 5000);
            waitMs(150);
            last = previews[0];
            save(previews[0], qPrintable(QStringLiteral("isf-%1").arg(QFileInfo(id).completeBaseName())));
            check(changed, qPrintable(QStringLiteral("ISF effect %1 compiles and renders").arg(QFileInfo(id).completeBaseName())));
        }
        // Ripple bends the vertical bar edges: one row's colours no longer match the next's.
        p.layers[0].effects.clear();
        for (const QString &id : filters)
            if (id.endsWith(QLatin1String("Ripple.fs"))) {
                EffectInstance ripple = makeEffect(id);
                ripple.params[2] = 0.05; // amplitude
                p.layers[0].effects = {ripple};
            }
        doc.notifyChanged(true);
        check(waitFor([&] {
                  const QImage &img = previews[0];
                  if (img.isNull())
                      return false;
                  int differ = 0;
                  for (int x = 0; x < img.width(); x += 2)
                      differ += img.pixel(x, img.height() / 4) != img.pixel(x, img.height() / 2) ? 1 : 0;
                  return differ > img.width() / 40;
              }, 5000),
              "ISF ripple distorts the picture");

        // A user shader that does not compile: listed, but the effect is skipped and nothing breaks.
        const QString dir = QDir::temp().filePath(QStringLiteral("displayhawk-isf-test"));
        QDir().mkpath(dir);
        QFile broken(dir + QStringLiteral("/Broken.fs"));
        if (broken.open(QIODevice::WriteOnly)) {
            broken.write("/*{ \"INPUTS\": [ { \"NAME\": \"inputImage\", \"TYPE\": \"image\" } ] }*/\n"
                         "void main() { gl_FragColor = this is not glsl; }\n");
            broken.close();
        }
        Isf::scan({dir});
        const QString brokenId = QStringLiteral("isf:") + broken.fileName();
        p.layers[0].effects = {makeEffect(brokenId)};
        doc.notifyChanged(true);
        waitMs(500);
        check(Isf::info(brokenId) != nullptr && waitFor([&] { return near(barColor(0), 255, 255, 255); }, 3000),
              "an ISF shader that does not compile is skipped");
        p.layers[0].effects.clear();
        doc.notifyChanged(true);
        doc.clearLayer(0);
    }

    // Workflow: the preview monitor, thumbnails, recording.
    {
        Project &p = doc.project();
        p.layers[0].effects.clear();
        p.compEffects.clear();
        doc.clearLayer(1);
        const int col = p.columns;
        doc.addColumn();
        doc.addColumn();
        Clip red = Clip::fromPattern(TestPattern::Solid);
        red.solidColor = Qt::red;
        doc.setClip(0, col, Clip::fromPattern(TestPattern::ColorBars));
        doc.setClip(0, col + 1, red);
        p.layers[0].transitionMs = 0;
        doc.triggerClip(0, col);
        // Selecting the (not playing) red clip previews it; the outputs keep showing the bars.
        ClipPreviewPanel *monitor = window.clipPreview();
        doc.select(Selection::ofClip(0, col + 1));
        const bool previewed = waitFor([&] {
            const QImage img = monitor->image();
            return !img.isNull() && near(QColor(img.pixel(img.width() / 2, img.height() / 2)), 255, 0, 0);
        }, 5000);
        check(previewed && near(barColor(0), 255, 255, 255) && p.layers[0].activeColumn == col,
              "preview monitor shows the selected clip without sending it out");
        monitor->image().save(QStringLiteral("%1/preview-monitor.png").arg(outDir));

        // Thumbnails: an image and (if available) a video.
        Thumbnailer thumbs;
        QHash<QString, QImage> got;
        QObject::connect(&thumbs, &Thumbnailer::ready, &app, [&](const QString &path, const QImage &img) { got.insert(path, img); });
        const QString png = outDir + QStringLiteral("/thumb-source.png");
        QImage source(400, 200, QImage::Format_RGB32);
        source.fill(Qt::green);
        source.save(png);
        thumbs.request(png);
        const QString video = qEnvironmentVariable("DISPLAYHAWK_TEST_VIDEO");
        if (!video.isEmpty())
            thumbs.request(video);
        const bool imageThumb = waitFor([&] {
            return got.contains(png) && near(QColor(got[png].pixel(Thumbnailer::size().width() / 2, Thumbnailer::size().height() / 2)), 0, 255, 0);
        }, 5000);
        const bool videoThumb = video.isEmpty() || waitFor([&] { return got.contains(video) && !got[video].isNull(); }, 8000);
        check(imageThumb && videoThumb, "media browser thumbnails (image and video)");

        // Record two seconds of the composition and play the file back.
        const QString file = QDir::temp().filePath(QStringLiteral("displayhawk-recording-test.mp4"));
        QFile::remove(file);
        OutputRecorder *recorder = window.recorder();
        QString error;
        const bool started = recorder->start(file, 30, &error);
        waitMs(2000);
        const int frames = recorder->framesWritten();
        recorder->stop();
        waitFor([&] { return QFileInfo(file).size() > 10000; }, 5000);
        QMediaPlayer player;
        player.setSource(QUrl::fromLocalFile(file));
        const bool playable = waitFor([&] { return player.duration() > 1000; }, 5000);
        check(started && frames > 30 && playable,
              qPrintable(QStringLiteral("recording writes a playable video (%1 frames, %2 ms, %3 bytes)%4")
                             .arg(frames).arg(player.duration()).arg(QFileInfo(file).size())
                             .arg(error.isEmpty() ? QString() : QStringLiteral(": ") + error)));
        doc.select(Selection::composition());
        doc.clearLayer(0);
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
    Isf::scan({Isf::defaultFolder()}); // bundled and user ISF shaders, before any project uses them
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
