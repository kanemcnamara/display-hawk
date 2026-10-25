#pragma once

#include <QString>

// When Display Hawk crashes, leaves something to find out why: a crash file in the app's data
// folder (crashes/) with the faulting thread's stack (module + offset per frame; addr2line or a
// debugger turns those into functions), and on Windows a minidump beside it. The setup itself is
// covered by the autosave, which is offered back on the next launch.
namespace CrashReport {

void install(const QString &folder);

} // namespace CrashReport
