#pragma once

#include <cstddef>

// RECHARGED_TEST_DRIVER=<dir> lets a script drive the game, for automated
// tests (the rollback's, first): keyboard & mouse is player 1 and no
// controller plays, the script's keys are held as if typed into the focused
// window, and the script can take screenshots. Unset, nothing changes.
//
// The driver runs <dir>/script whenever the file changes, one command per
// line, then writes "done <n>" to <dir>/status (n counts the scripts run):
//   key <SDL scancode name> <ms>   hold a key (e.g. "key Return 100"), then
//                                  release it for 60 ms
//   wait <ms>                      do nothing
//   snap <name>                    screenshot to <dir>/<name>.png
//   point <x> <y>                  aim the pointer there, as fractions of the
//                                  picture (0 0 top left); "point off" hides it
//   mash <ms> <key> [<key> ...]    for that long, hold random keys of these
//                                  (or none) for 30 to 400 ms each; another
//                                  sequence in each run, logged with its seed
//                                  (RECHARGED_TEST_DRIVER_SEED=<n> repeats one)
// '#' starts a comment.
namespace mscharged::platform
{
bool TestDriverActive();
// From the desktop input service, every few milliseconds on the owner thread.
void ServiceTestDriver();
// Ors the keys the script holds into `keys` (indexed by SDL scancode).
void ApplyTestDriverKeys(bool* keys, std::size_t count);
// Where the script aims the keyboard player's pointer; false when it does not.
bool TestDriverPointer(float& x, float& y);
} // namespace mscharged::platform
