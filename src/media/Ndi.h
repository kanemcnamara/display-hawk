#pragma once

#include <QImage>
#include <QStringList>
#include <QThread>

#include <atomic>

// NDI® video over IP, through the NDI runtime installed on the machine (NDI Tools / NDI Runtime from
// ndi.video). Nothing is linked at build time: the runtime library is loaded when first needed and
// its C API resolved by name, so Display Hawk runs fine without it.
// NDI® is a registered trademark of Vizrt NDI AB.
namespace Ndi {

bool available();          // the runtime was found and initialised
QString runtimeStatus();   // human-readable: where it was loaded from, or how to get it
QStringList sources();     // NDI sources currently visible on the network (discovery runs in the background)

} // namespace Ndi

// Receives one NDI source on its own thread and emits each video frame as an RGBA image.
class NdiReceiver : public QThread
{
    Q_OBJECT
public:
    explicit NdiReceiver(const QString &sourceName, QObject *parent = nullptr);
    ~NdiReceiver() override;
    void stop();

signals:
    void frame(const QImage &image); // emitted on the receiver thread
    void status(const QString &message);

protected:
    void run() override;

private:
    QString m_source;
    std::atomic<bool> m_stop{false};
};

// Publishes frames as an NDI source. send() may be called from any one thread.
class NdiSender
{
public:
    explicit NdiSender(const QString &name);
    ~NdiSender();
    bool isValid() const { return m_instance != nullptr; }
    QString name() const { return m_name; }
    // RGBA8888 frame, top row first. Sent asynchronously: the image is kept until the next send.
    void send(const QImage &rgba, int frameRateN = 60000, int frameRateD = 1000);
    int connections() const;

private:
    void *m_instance = nullptr;
    QString m_name;
    QByteArray m_nameUtf8;
    QImage m_inFlight[2]; // NDI reads asynchronously from the previous buffer
    int m_next = 0;
};
