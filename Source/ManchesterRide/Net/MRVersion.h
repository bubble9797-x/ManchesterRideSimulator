#pragma once

// The game's version. Bump MR_GAME_BUILD for every release you put on the server: players with a lower
// build are sent the new installer when they join. Tools/installer/make_installer.ps1 reads both lines from
// this file and stamps them into ManchesterRide-Setup.exe, which is how the server knows its version.
#define MR_GAME_BUILD 2
#define MR_GAME_VERSION "0.2.0"
