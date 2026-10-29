PCSX2 Reliquary - Disc Fix v3
Based on SlimDread/pcsx2-reliquary (master efa06a63f), AVX2 build.

WHAT IT DOES
Playing from a real disc in a PC DVD drive:
 - The game keeps running (audio, animation, input) while the drive seeks, like a real PS2.
 - It keeps reading ahead of the game and keeps the drive spinning, so streaming games
   don't stall while the drive spins back up.
 - CDs (audio CDs and PS1 discs) are read a whole block at a time instead of one sector
   at a time, so CD audio shouldn't skip.
 - Works for PS2 discs, PS1 discs, audio CDs and DVD movies.
ISO/CHD files behave exactly like normal PCSX2.

HOW TO USE
1. Back up your memory cards (Documents\PCSX2\memcards).
2. Unzip anywhere and run pcsx2-qtx64-avx2.exe.
3. It uses the same settings, BIOS and memory cards as your normal PCSX2
   (Documents\PCSX2), so there's nothing to set up.
4. Put the disc in your DVD drive and choose System > Start Disc.

REQUIREMENTS
 - Windows 10/11, 64-bit
 - A CPU with AVX2 (Intel 4th gen / AMD Ryzen or newer)

NOTES
 - Loads still take as long as your drive needs. The emulator just doesn't freeze anymore.
 - Auto-update is removed on purpose, so it won't replace itself with another build.
