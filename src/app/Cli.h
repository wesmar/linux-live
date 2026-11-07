// Command-line mode: any argument on the command line selects it.
#pragma once
#include <string>

namespace DL::Cli {

// Exit codes (documented in --help).
enum Exit : int { kOk = 0, kFailed = 1, kUsage = 2, kUnavailable = 3, kCanceled = 4 };

int Run(int argc, wchar_t** argv);

// The --help text (also shown by Help > Command line in the GUI).
std::wstring HelpText();

}  // namespace DL::Cli
