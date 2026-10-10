#ifndef APPLOGGING_H
#define APPLOGGING_H

#include <QString>

class QCoreApplication;

namespace applog {

// Installs the process-wide Qt message handler and, on Windows, a native
// unhandled-exception handler that writes a minidump beside the log file.
void initialize(QCoreApplication *application);

QString logDirectory();
QString currentLogFile();

} // namespace applog

#endif // APPLOGGING_H
