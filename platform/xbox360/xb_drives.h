// Harissa64 V2 - the console's drives (Xbox 360).
//
// A title sees only game:\ (its own folder) until it maps the other drives
// itself: DrivesMount links Hdd1:, Usb0:..Usb2: and Dvd: to their devices,
// as homebrew file managers do. The ROM list's file browser (xb_menu.cpp)
// lists the ones present.
#ifndef XB_DRIVES_H
#define XB_DRIVES_H

// Maps the drives (once, at start-up; a link that already exists is kept).
void DrivesMount(void);
// The drive roots present now ("game:\", "Hdd1:\", "Usb0:\"...): up to `max`
// into roots[], each of `size` bytes. Returns how many. USB drives plugged in
// after start-up appear here too.
int DrivesList(char roots[][16], int max);

#endif
