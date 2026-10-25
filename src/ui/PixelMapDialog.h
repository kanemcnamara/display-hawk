#pragma once

#include <QDialog>

#include <functional>

class Document;
class PixelMapper;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QStackedWidget;
struct PixelFixture;

// Output > Pixel mapping: DMX fixtures (LED strips, matrices, single fixtures) placed on the
// composition, and where their Art-Net / sACN goes.
class PixelMapDialog : public QDialog
{
    Q_OBJECT
public:
    PixelMapDialog(Document *doc, PixelMapper *mapper, QWidget *parent = nullptr);

private:
    void refresh();
    void refreshFixture();
    void edit(const std::function<void(PixelFixture &)> &fn, bool structural = false);
    void addFixture(int kind);

    Document *m_doc;
    PixelMapper *m_mapper;
    bool m_refreshing = false;
    QCheckBox *m_enabled;
    QComboBox *m_protocol;
    QLineEdit *m_target;
    QSpinBox *m_rate;
    QLabel *m_status;
    QListWidget *m_list;
    QWidget *m_form;
    QLineEdit *m_name;
    QCheckBox *m_fixtureOn;
    QComboBox *m_kind, *m_order;
    QDoubleSpinBox *m_x1, *m_y1, *m_x2, *m_y2, *m_w, *m_h;
    QSpinBox *m_pixels, *m_columns, *m_rows, *m_universe, *m_address;
    QCheckBox *m_serpentine;
    QLabel *m_channels;
};
