# PCSX2 Disc Seek Fix

Stops PCSX2 from freezing every time a game seeks when playing from a **real disc in a PC DVD drive**,
and makes the drive behave more like a real PS2's.

- **Current base (from 2026-10-28): [SlimDread/pcsx2-reliquary](https://github.com/SlimDread/pcsx2-reliquary)**
  (master `efa06a63f`). Reliquary already contains the v2 disc fix. On top of it we add the
  multi-sector CD read (v3, see below) and the diagnostic hooks. The v2.1 BIOS message is **not**
  used on reliquary.
- Original base: official PCSX2 **v2.9.131** (master `5b6e6e23e`, 2026-10-19). It is no longer used.
- The changes themselves are in this repository's commit history.
- Applying it to a newer PCSX2: [`PORTING.md`](PORTING.md)
- Every changed line, base → v1 → v2, explained in plain language: [`CODE-CHANGES.md`](CODE-CHANGES.md)
- Release builds are packaged into `releases` (not tracked in git).

ISO/CHD/CSO playback is untouched: it behaves exactly like official PCSX2.

## Results

Tested on DOA2 Hardcore (SLUS-20071), Guilty Gear Isuka (SLPM-65644) and Naruto Narutimate Hero
International (SLKA-25232) from a PC DVD drive. No more freezes on seek. The last stutter left in Naruto was found with
the diagnostic build and fixed (see "Fix 3" below). The retest had no stutter.

## Root causes (official PCSX2)

1. **The emulator read the drive itself.** When the emulated drive finished a seek, `DISCgetBuffer`
   called `cdvdGetSector`. If the IO thread hadn't fetched that block yet, it did a synchronous
   `ReadFile` on the **emulation thread**. Every optical seek (100 ms or more, seconds on spin-up)
   froze the whole emulator: video, audio and input.
2. **Two readers fought over the drive.** The emulation thread's direct read and the IO thread's
   read of the same block ran back to back, making the read head seek twice.
3. **Lost wakeup.** `cdvdRequestSector` called `notify_one` without holding the CV mutex, while the
   IO thread held that mutex for its whole loop. A request posted between the thread's queue check
   and its `wait_for` slept for up to 250 ms.
4. **Stale requests ran first.** Requests went into a FIFO queue. After the game jumped elsewhere,
   the drive still seeked to positions it no longer needed before reaching the one it did.
5. **Status IOCTL before every block.** `IOCTL_STORAGE_CHECK_VERIFY` ran in front of every 16-sector
   read, adding a round trip to each one.
6. **Readahead stopped while the game was streaming.** Readahead was 16 blocks (256 sectors), and only
   after a cache *miss*. A streaming game kept reading from the cache, so no new requests came in and
   readahead never restarted. The drive went idle, the PC drive slowed its spindle after about 4 s,
   and the next read took about 1.27 s to spin back up (seen twice in the Naruto log).

## What the patch changes

### Fix 1: Non-blocking reads (PS2 and PS1)

- `CDVD.cpp` `cdvdReadInterrupt`: when `DoCDVDgetBuffer` returns `-2` ("not ready"), the patch
  reschedules `IopEvt_CdvdRead` one sector-time later (`CDVDREAD_INT(cdvd.ReadTime)`) and returns,
  instead of spin-waiting. The emulated drive stays BUSY/READ, the console keeps running, and aborts
  are still honored on the next poll. `ReadErr == -2` is accepted as "keep polling".
- `Ps1CD.cpp` `cdrReadInterrupt`: same change for PS1 discs. The poll happens *before* the result
  registers are touched, so nothing is half-updated while waiting.
- Savestates are unaffected: `cdvdFreeze` re-issues `DoCDVDreadTrack` on load when `cdvd.Reading`,
  which resets `ReadErr` to 0. The savestate format is unchanged.

### Fix 2: Disc IO thread rework (`CDVDdiscThread.cpp`, `CDVDdiscReader.cpp/.h`)

- `DISCgetBuffer` uses the new `cdvdTryGetSector()`. It never touches the drive: a cache hit returns
  the data, and a miss re-posts the request and returns `-2`. If the IO thread's read *failed*, it
  falls back to the old synchronous `cdvdGetSector()`, so error behavior is unchanged.
- The request FIFO is replaced by a single **newest request** (`s_request_lsn`/`s_request_pending`)
  guarded by `s_notify_lock`. A new seek replaces the previous one, as on a real drive.
- The IO thread releases `s_notify_lock` while the drive is reading, and waits with a **predicate**,
  so wakeups can't be lost.
- `s_failed_lsn` records a failed requested block, so pollers stop waiting and fall back.
- The disc status IOCTL runs at most once a second while busy (every iteration when idle).

### Fix 3: Rolling readahead and keeping the drive spinning (`CDVDdiscThread.cpp`)

- `s_play_lsn` tracks the block the game is reading, **updated even on cache hits**, and wakes the IO
  thread when it changes.
- Readahead keeps **2048 sectors (4 MB)** read ahead of the game, refilling as the game moves (like the
  PS2 drive keeping its buffer full). After a seek it restarts just past the seek target. If the game
  overtakes it or jumps far back into cached data, it continues from the game's position.
- **Keep-spinning:** if the drive has had no command for **2 s**, the thread reads the next block past
  the readahead window. That data is new to the drive, so the drive really has to read the disc, and
  it's useful readahead anyway. This stops the PC drive slowing its spindle; a PS2 drive doesn't slow
  down while a game is running. It stops after **10 min** with no game reads (paused emulator, idle menu).

Tunables (top of `cdvdThread()`): `readahead_sectors`, `keepalive_interval`, `keepalive_window`.

### v3 (reliquary): Multi-sector reads for CDs (`Windows/IOCtlSrc.cpp` `ReadSectors2352()`)

CDs (audio CDs and PS1 discs, anything where `GetMediaType() < 0`) are read with SCSI `READ CD`
through `ReadSectors2352()`. Upstream sends **one command per sector**, so each 16-sector block
costs 16 round trips. Most drives don't read ahead on audio tracks, so each command waits for the
sector to come round again. That caps throughput near 1x, which is not enough for steady 75
sectors/s CD audio, so audio CDs skip. (Reliquary tried a version of this on 2026-10-27 and
reverted it the next day.)

v3 reads the whole block with one `READ CD` command. If that fails (a read can't span two tracks of
different types, e.g. the data → audio boundary), it falls back to one sector at a time for that
block. After 8 failed multi-sector reads in a row (a drive that rejects them), it stops trying until
the disc is reopened, so a bad drive doesn't pay for a failed command on every block.

v3 also stops `DISCreadSubQ()` from calling `src->ReadTrackSubQ()` (an `IOCTL_CDROM_READ_Q_CHANNEL`
on the emulation thread). The CD player polls it constantly, and it queued behind in-flight IO-thread
reads, causing 50–165 ms frame stalls while readahead filled at the start of CD audio. ADR, track
and index now come from the TOC (the old fallback).

DVD-Video uses `N_DVD_READ` → `DoCDVDreadTrack`/`cdvdReadInterrupt` → `ReadSectors2048` (one
`ReadFile` per block). That's already on the fixed non-blocking path with readahead, so it needed
no extra change.

### v2.1: Message when the BIOS can't boot (official-PCSX2 builds only; not used on reliquary) (`CDVD.cpp` `fail_pol_cal()`)

With fast boot off, some setups never reach the game: the BIOS tries to start an encrypted
(MagicGate/KELF) program first, such as an HDD boot program on the DEV9 HDD image or a memory card
system update. PCSX2 can't decrypt these (it would need Sony's keys), so the BIOS stalls silently.
This was seen with a user's Europe v1.90 BIOS and DEV9 HDD enabled: the log showed
`[MG] ... zones=USA` and then `[MG] ERROR - Make sure the file is already decrypted!!!` right after
the ATA init, and no game ELF was ever loaded. Another BIOS worked.

A real fix isn't possible, so v2.1 shows a one-time error dialog ("Game can't boot with this BIOS
setup") when a MagicGate check fails **before any game has loaded** (`VMManager::GetCurrentCRC() == 0`).
It explains the cause and lists the fixes: disable the HDD (only listed if the HDD is enabled), enable
Fast Boot, use a different BIOS, or use a memory card without a system update. It's reset in
`cdvdReset()` so it shows once per boot. This is untested against a real failing setup.

### Diagnostics (compiled out by default)

The diagnostics are only compiled in with `PCSX2_CDVD_DIAG` defined. When enabled they write to `emuLog.txt`:

| Log line | Meaning |
|---|---|
| `[CDVD-DIAG] Waited X ms on drive for sector N` | Real time the game waited on the host drive beyond emulated timing (only logged at 2 ms or more) |
| `[CDVD-DIAG] PS1: waited X ms on drive` | The same, for PS1 discs (`Ps1CD.cpp`; added in v3) |
| `[CDVD-DIAG] Drive read of sector N (request/prefetch/keepalive) took X ms` | Slow (30 ms or more) or failed physical drive read |
| `[CDVD-DIAG] Emulated seek A -> B: X ms` | Long seek the game asked for (game-side loading; a real PS2 takes that time too) |
| `[FRAME-DIAG] Slow frame: X ms` | The emulator itself took 25 ms or more for a frame (an emulator hitch, not the disc) |

How to build with diagnostics: see [`PORTING.md`](PORTING.md#diagnostic-build).

## Not covered / known limits

- **Open issue: PS1 discs and DVD-Video black-screen after booting from a physical drive** (tested
  2026-10-28 with one PS1 disc, SCPS-10118, and DVD-Video discs). In the PS1 case the DIAG log shows
  the first block (sectors 0–15) delivered without errors, after which the PS1 side stops requesting
  data. The cause hasn't been determined. It may be existing PCSX2 PS1/DVD-player behaviour or
  related to these changes; no baseline comparison has been done yet.
- Loads still take as long as the drive needs. If the game must wait for data, you get a loading pause,
  as on real hardware. The difference is that the emulator keeps running.
- Disc reads at boot and during disc hashing (`DoCDVDreadSector`: game list serial/CRC, ISO hashing)
  are still synchronous. They don't happen during gameplay.
- Linux/macOS share `CDVDdiscThread.cpp`, so they get the same thread changes, but they were not built or
  tested. Only Windows (`Windows/IOCtlSrc.cpp`) was tested.
- An earlier experiment also changed ISO reading (`ThreadedFileReader`/`InputIsoFile`). It was reverted
  on request, because the fix is meant only for disc drives.
