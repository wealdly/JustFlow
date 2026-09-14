// nrfilter logging: one file + stderr, timestamped. Log() is printf-style and thread-safe.
#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>

inline FILE*& LogFile() { static FILE* f = nullptr; return f; }
inline std::mutex& LogMutex() { static std::mutex m; return m; }

inline void LogInit(const wchar_t* path)
{
    std::lock_guard<std::mutex> lk(LogMutex());
    if (LogFile()) { fclose(LogFile()); LogFile() = nullptr; }
    if (path && *path) _wfopen_s(&LogFile(), path, L"w");
}

inline void Log(const char* fmt, ...)
{
    char line[2048];
    SYSTEMTIME st; GetLocalTime(&st);
    int n = snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(LogMutex());
    fputs(line, stderr); fputc('\n', stderr);
    if (LogFile()) { fputs(line, LogFile()); fputc('\n', LogFile()); fflush(LogFile()); }
}
