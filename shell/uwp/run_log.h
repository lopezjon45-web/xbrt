// LocalState\run.txt, the stage record that console/run-u0.sh reads (it waits for a line "END"), and
// LocalState\shell.txt, everything else. Any thread. Each call writes one line and flushes it, so a process that dies
// leaves its last line on disk.
#pragma once

#include <string>
#include <string_view>

namespace xbrt::run_log {

// Truncates both files. Lines written before open are dropped.
void open(const std::wstring &local_state);

void run(std::string_view line);
void shell(std::string_view line);

}  // namespace xbrt::run_log
