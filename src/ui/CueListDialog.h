#pragma once

#include <QDialog>

class CueList;
class Document;
class QComboBox;
class QLabel;
class QTableWidget;

// Control > Cue list: the show's cues (what, and optionally at what timecode), GO / back, and the
// timecode input (MIDI timecode or LTC from an audio input).
class CueListDialog : public QDialog
{
    Q_OBJECT
public:
    CueListDialog(Document *doc, CueList *cues, QWidget *parent = nullptr);

private:
    void rebuild();
    void refreshTimecode();

    Document *m_doc;
    CueList *m_cues;
    QTableWidget *m_table;
    QComboBox *m_source, *m_ltcDevice, *m_fps;
    QLabel *m_clock, *m_status;
    bool m_refreshing = false;
};
