#pragma once

#include "model/Project.h"

#include <QWidget>

#include <functional>

class Document;
class QVBoxLayout;

// Edits the effect chain of one layer, or of the composition: add, reorder, enable, remove and
// adjust parameters. Effects apply top to bottom.
class EffectChainEditor : public QWidget
{
    Q_OBJECT
public:
    explicit EffectChainEditor(Document *doc, QWidget *parent = nullptr);

    void setLayer(int layer); // -1 = composition
    void setClip(int layer, int column);
    void refresh();

signals:
    // Right-click "Learn MIDI" on a parameter: an EffectParam mapping for it.
    void midiLearnRequested(int action, int layer, int effect, int param);

private:
    QVector<EffectInstance> *chain();
    void edit(const std::function<void(QVector<EffectInstance> &)> &fn, bool structural);
    void rebuild();
    QString oscAddress(int effect, int param) const;

    Document *m_doc;
    int m_layer = -1;
    int m_column = -1; // >= 0: the chain of that clip
    QVBoxLayout *m_list;
    QStringList m_builtFor; // effect types the widgets were built for
    int m_builtLayer = -2;
    int m_builtColumn = -2;
    bool m_refreshing = false;
    // Per effect, per parameter: pushes a value into the widgets without signals.
    QVector<QVector<std::function<void(double)>>> m_setters;
    QVector<std::function<void(bool)>> m_enabledSetters;
};
