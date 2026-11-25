// Where the mod's files live. libfpv's INTEGRATION.md "Label the mod libfpv
// inside the game": the loader is GTAIV/libfpv.asi and everything else is in
// GTAIV/libfpv/ (the core, libfpv.dll, the log, test triggers, screenshots).
#pragma once

#include <string>

namespace paths {

// The game folder (GTAIV), without a trailing slash.
std::string game_dir();

// GTAIV\libfpv, created on first use.
std::string mod_dir();

// mod_dir() + "\\" + name.
std::string mod_file(const char *name);

}  // namespace paths
