#include "applogging.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QStandardPaths>
#include <QThread>

#include <cstdlib>
#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbghelp.h>
#endif

namespace {

QMutex g_logMutex;
QFile *g_logFile = nullptr;
QString g_logDirectory;
QString g_logFilePath;

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return "D";
    case QtInfoMsg: return "I";
    case QtWarningMsg: return "W";
    case QtCriticalMsg: return "C";
    case QtFatalMsg: return "F";
    }
    return "?";
}

QString contextFile(const QMessageLogContext &context)
{
    if (!context.file || *context.file == '\0')
        return QStringLiteral("qt");
    return QFileInfo(QString::fromUtf8(context.file)).fileName();
}

void messageHandler(QtMsgType type, const QMessageLogContext &context,
                    const QString &message)
{
    const QString line = QStringLiteral("[%1][%2][tid=%3][%4:%5] %6\n")
                             .arg(QDateTime::currentDateTime().toString(
                                      QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                                  QString::fromLatin1(levelName(type)),
                                  QString::number(reinterpret_cast<quintptr>(
                                      QThread::currentThreadId())),
                                  contextFile(context),
                                  context.line > 0 ? QString::number(context.line)
                                                   : QStringLiteral("-"),
                                  message);

    QMutexLocker locker(&g_logMutex);
    if (g_logFile && g_logFile->isOpen()) {
        g_logFile->write(line.toUtf8());
        g_logFile->flush();
    }
}

void terminateHandler()
{
    if (const std::exception_ptr exception = std::current_exception()) {
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception &error) {
            qCritical().noquote() << QStringLiteral("未捕获的 C++ 异常: %1")
                                         .arg(QString::fromUtf8(error.what()));
        } catch (...) {
            qCritical().noquote() << QStringLiteral("未捕获的未知 C++ 异常");
        }
    } else {
        qCritical().noquote() << QStringLiteral("std::terminate 被调用");
    }
    std::_Exit(EXIT_FAILURE);
}

#ifdef Q_OS_WIN
LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS *exceptionInfo)
{
    static QMutex crashMutex;
    if (!crashMutex.tryLock())
        return EXCEPTION_EXECUTE_HANDLER;

    const DWORD code = exceptionInfo && exceptionInfo->ExceptionRecord
                           ? exceptionInfo->ExceptionRecord->ExceptionCode
                           : 0;
    const void *address = exceptionInfo && exceptionInfo->ExceptionRecord
                              ? exceptionInfo->ExceptionRecord->ExceptionAddress
                              : nullptr;
    const QString dumpPath = g_logDirectory
        + QStringLiteral("/Scompetition-crash-%1.dmp")
              .arg(QDateTime::currentDateTime().toString(
                  QStringLiteral("yyyyMMdd-HHmmss-zzz")));

    QDir().mkpath(g_logDirectory);
    const HANDLE process = GetCurrentProcess();
    const HANDLE file = CreateFileW(reinterpret_cast<LPCWSTR>(dumpPath.utf16()),
                                    GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exceptionInformation{};
        exceptionInformation.ThreadId = GetCurrentThreadId();
        exceptionInformation.ExceptionPointers = exceptionInfo;
        exceptionInformation.ClientPointers = FALSE;
        const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithDataSegs | MiniDumpWithHandleData
            | MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory);
        const BOOL written = MiniDumpWriteDump(process, GetCurrentProcessId(), file,
                                               dumpType, &exceptionInformation,
                                               nullptr, nullptr);
        CloseHandle(file);
        if (!written)
            qCritical().noquote() << QStringLiteral("崩溃转储写入失败，GetLastError=%1")
                                         .arg(GetLastError());
    } else {
        qCritical().noquote() << QStringLiteral("无法创建崩溃转储文件，GetLastError=%1")
                                     .arg(GetLastError());
    }

    qCritical().noquote()
        << QStringLiteral("未处理异常: code=0x%1 address=%2 dump=%3")
               .arg(code, 8, 16, QLatin1Char('0'))
               .arg(reinterpret_cast<quintptr>(address), 0, 16)
               .arg(dumpPath);

    crashMutex.unlock();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

} // namespace

namespace applog {

QString logDirectory()
{
    return g_logDirectory;
}

QString currentLogFile()
{
    return g_logFilePath;
}

void initialize(QCoreApplication *application)
{
    Q_UNUSED(application);
    QDir().mkpath(QCoreApplication::applicationDirPath() + QStringLiteral("/logs"));
    g_logDirectory = QCoreApplication::applicationDirPath() + QStringLiteral("/logs");
    QString testPath = g_logDirectory + QStringLiteral("/.write_test");
    QFile testFile(testPath);
    if (!testFile.open(QIODevice::WriteOnly)) {
        g_logDirectory = QStandardPaths::writableLocation(
                             QStandardPaths::AppLocalDataLocation)
                         + QStringLiteral("/logs");
        QDir().mkpath(g_logDirectory);
    } else {
        testFile.close();
        testFile.remove();
    }

    const QString executableStem = QFileInfo(
        QCoreApplication::applicationFilePath()).completeBaseName();
    g_logFilePath = g_logDirectory
        + QStringLiteral("/%1-%2.log")
              .arg(executableStem.isEmpty() ? QStringLiteral("Scompetition")
                                            : executableStem,
                   QDate::currentDate().toString(QStringLiteral("yyyyMMdd")));
    g_logFile = new QFile(g_logFilePath);
    g_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);

    qInstallMessageHandler(messageHandler);
    std::set_terminate(terminateHandler);
#ifdef Q_OS_WIN
    SetUnhandledExceptionFilter(unhandledExceptionFilter);
#endif

    qInfo().noquote() << QStringLiteral("日志初始化完成: %1").arg(g_logFilePath);
    qInfo().noquote() << QStringLiteral("程序启动: %1 | Qt %2")
                             .arg(QCoreApplication::applicationFilePath(),
                                  QString::fromLatin1(qVersion()));
}

} // namespace applog
