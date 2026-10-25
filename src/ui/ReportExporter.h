#pragma once

#include <QImage>
#include <QStringList>

struct Project;
struct Screen;

// Writes a setup report for the LED technician into a folder: a PDF (summary, wall specs, panel
// map, one page per screen with its slice table), the panel map and each screen's processor view
// as PNGs, and the slice mappings as CSV.
namespace ReportExporter {

// baseName prefixes every file. Returns the files written; empty with *error set on failure.
QStringList exportReport(const Project &project, const QString &dir, const QString &baseName, QString *error);

// What a screen's processor receives: the panel map cut into slices and placed (rotated/flipped)
// exactly as the outputs do, with slice outlines and labels.
QImage screenLayout(const Project &project, const Screen &screen, const QImage &panelMap);

} // namespace ReportExporter
