#pragma once

#include "Core.hpp"

namespace rm {

// True when the first command-line argument names a command-line operation
// ("status", "submit", "help", ...) instead of starting the desktop window.
bool IsCommandLineCommand(const std::wstring& argument);
// Runs the operation and returns the process exit code (see `help`).
int RunCommandLine(int count, LPWSTR* values);

}  // namespace rm
