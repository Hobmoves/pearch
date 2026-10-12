#pragma once

#include "app/cli.h"

namespace app {

// Runs a parsed command line; returns the process exit code.
int run(const Options& options);

}  // namespace app
