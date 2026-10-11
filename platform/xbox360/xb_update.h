// Harissa64 - is a newer release on GitHub? (checked once, in the background)

#ifndef XB_UPDATE_H
#define XB_UPDATE_H

#include <stddef.h>

// Starts the check (first call only): a thread asks GitHub for the latest
// release and compares it with `current` (H64_VERSION_STRING).
void UpdateCheckStart(const char *current);

// 1 (once) when the check found a newer release: its version ("2.0.2") goes
// to `version`.
int UpdateCheckTake(char *version, size_t size);

#endif
