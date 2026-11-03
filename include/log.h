#pragma once

// On-disk diagnostic log (tinta.log in the directory the app was started in —
// its "current directory" at launch, captured before the process parks its
// working directory into the system folder).
//
// Tinta's reported problems tend to be machine- or build-specific (the
// Windows 7 port is the prime example): the window opens but content or
// clicks never arrive. A message box or a debugger is not available on those
// machines, so every interesting step — startup, file opens, paints, mouse
// clicks and errors — is written here in plain UTF-8 text. A user can paste
// the file, or a maintainer can reproduce the launch and read the same trail.
//
// The file is created on the first call and truncated on each launch
// (CREATE_ALWAYS), so tinta.log always describes exactly one session. If the
// launch directory cannot be written the module falls back to the system temp
// folder; if that fails too, lines go to the debugger only. Writing is
// synchronous, cheap and safe from any thread.
#include <string>

// printf-style: appends one timestamped line to the log (and to
// OutputDebugStringA on Windows so a debugger sees it live).
void tintaLog(const char* fmt, ...);

// The file path the next line will be written to (resolved after the first
// call; empty when even the temp-folder fallback could not be opened).
std::string tintaLogPath();

// Test hooks: point the log at an explicit UTF-8 path, or close it and let
// the next line re-resolve the default. Used by tests/log_tests.cpp so the
// assertions never touch a real launch directory.
void tintaLogSetPath(const std::string& path);
void tintaLogReset();
