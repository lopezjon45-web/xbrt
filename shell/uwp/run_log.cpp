#include "shell/uwp/run_log.h"

#include <windows.h>

#include <cstdio>

namespace xbrt::run_log {

namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
FILE *g_run = nullptr;
FILE *g_shell = nullptr;

void write(FILE *const &file, std::string_view line)
{
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    AcquireSRWLockExclusive(&g_lock);
    if (file) {
        fwrite(line.data(), 1, line.size(), file);
        fputc('\n', file);
        fflush(file);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace

void open(const std::wstring &local_state)
{
    AcquireSRWLockExclusive(&g_lock);
    g_run = _wfopen((local_state + L"\\run.txt").c_str(), L"wb");
    g_shell = _wfopen((local_state + L"\\shell.txt").c_str(), L"wb");
    ReleaseSRWLockExclusive(&g_lock);
}

void run(std::string_view line) { write(g_run, line); }
void shell(std::string_view line) { write(g_shell, line); }

}  // namespace xbrt::run_log
