# Porting the Disc Seek Fix to a newer PCSX2

How to reapply this fix to a future PCSX2 version, build it and package it.
`<repo>` is your local checkout and `<VS>` your Visual Studio install folder.

## In pcsx2-reliquary

All of the changes (1–7, 9–11 in `CODE-CHANGES.md`) are committed in
https://github.com/SlimDread/pcsx2-reliquary. Merging upstream PCSX2 into reliquary keeps them, and
git reports a conflict only where upstream touched the same lines. Resolve those with the per-file
checklist below.

To carry the fix into a different PCSX2 fork, export it from reliquary's history:

```bash
cd "<repo>"
git log --oneline -- pcsx2/CDVD pcsx2/PerformanceMetrics.cpp   # find the disc-fix commits
git format-patch -o disc-fix-patches <first-commit>^..<last-commit> -- pcsx2
# in the other fork:
git am -3 disc-fix-patches/*.patch
```

Reliquary vendors libusb as `3rdparty\libusb\libusb.vcxproj`, so the MSBuild build works with the
official `pcsx2-windows-dependencies` pack in `deps\` (its CMake/CI path builds libusb into deps
instead).

The sections below describe porting onto **official** PCSX2 and the build, DIAG, test and package
steps, which are the same for reliquary.

## 1. Get the new PCSX2 source

Check out the PCSX2 version you want to port to, with submodules:

```bash
git clone --recurse-submodules https://github.com/PCSX2/pcsx2.git <new-checkout>
```

## 2. Apply the changes

Apply the patches exported in "In pcsx2-reliquary" above:

```bash
cd "<new-checkout>"
git am -3 disc-fix-patches/*.patch
```

If they apply cleanly, go to step 3. If there are conflicts, the changes are small and self-contained,
so reapply them by hand using the checklist below and `CODE-CHANGES.md` as reference.

### Manual checklist (what to look for in each file)

1. **`pcsx2/CDVD/CDVD.cpp` → `cdvdReadInterrupt()`**
   - Find the loop `while ((cdvd.ReadErr = DoCDVDgetBuffer(...)), cdvd.ReadErr == -2) { Sleep/SpinWait }`.
   - Replace it with a single `DoCDVDgetBuffer` call. On `-2`, `CDVDREAD_INT(cdvd.ReadTime); return;`.
   - Change the guard from `if (cdvd.ReadErr == 0)` to `if (cdvd.ReadErr == 0 || cdvd.ReadErr == -2)`.
   - Optional diagnostics: the `#ifdef PCSX2_CDVD_DIAG` blocks (wait timer, plus seek log in
     `cdvdStartSeek()` after `seektime += drive_speed_change_cycles;`) and `#include "common/Timer.h"`.
2. **`pcsx2/CDVD/Ps1CD.cpp` → `cdrReadInterrupt()`**
   - Same idea: poll `DoCDVDgetBuffer` once, and on `-2` call `CDREAD_INT(<sector time>)` and return.
   - Do the poll **before** `cdr.OCUP = 1; SetResultSize(1); ...` so the registers aren't touched
     while waiting.
3. **`pcsx2/CDVD/CDVDdiscReader.cpp` → `DISCgetBuffer()`**
   - Replace `memcpy(dest, cdvdGetSector(csector, cmode), csize)` with `cdvdTryGetSector(...)`.
     Return `-2` if it's not ready yet. If `failed` is set, fall back to `cdvdGetSector`.
4. **`pcsx2/CDVD/CDVDdiscReader.h`**: declare
   `u8* cdvdTryGetSector(u32 sector, s32 mode, bool& failed);`
5. **`pcsx2/CDVD/CDVDdiscThread.cpp`**: this is the largest change.
   - Globals: remove `s_request_lock`/`s_request_queue`. Add `s_request_lsn`, `s_request_pending`
     (guarded by `s_notify_lock`), `std::atomic<u32> s_failed_lsn`, `std::atomic<u32> s_play_lsn`.
   - `cdvdUpdateDiscStatus()`: clear `s_request_pending` under `s_notify_lock`.
   - `cdvdThread()`: replace it entirely with the patched version. It does rolling readahead,
     keep-spinning, unlocks the mutex during drive reads, uses a predicate wait, and rate-limits
     the status check.
   - `cdvdRequestSector()`: update `s_play_lsn` (and notify) *before* the cache check. Post the
     newest request under `s_notify_lock` and clear `s_failed_lsn` for that block.
   - Add `cdvdTryGetSector()`, and factor the sector-offset/mode code out of `cdvdGetSector()` into
     `cdvdSectorData()`.
6. **`pcsx2/CDVD/Windows/IOCtlSrc.cpp` → `ReadSectors2352()`** (Change 9): read the whole block with
   one `READ CD` (sector count in `Cdb[6..8]`). On failure, fall back to one sector at a time. Add the
   `s_multi_read_failures` counter (stop trying after 8 failures in a row) and reset it in `Reopen()`.
7. **`pcsx2/CDVD/CDVDdiscReader.cpp` → `DISCreadSubQ()`** (Change 11): don't call `src->ReadTrackSubQ()`.
   Always set `adr = 1`, `trackNum = i`, `trackIndex = 1` from the TOC.
8. *(Official-PCSX2 builds only, not in reliquary)* **`pcsx2/CDVD/CDVD.cpp` → `fail_pol_cal()`** (v2.1): add `static bool s_mg_boot_warning_shown`
   near the other file-level statics (it must be declared before `cdvdReset()`) and reset it in
   `cdvdReset()`. In `fail_pol_cal()`, if it's not shown yet and `VMManager::GetCurrentCRC() == 0`,
   call `Host::ReportErrorAsync("Game can't boot with this BIOS setup", ...)` with the fix list.
9. **`pcsx2/PerformanceMetrics.cpp`** (diagnostics only): the `[FRAME-DIAG]` slow-frame log in `Update()`.

Things to re-check if upstream changed them:
- The `-2` contract: `DoCDVDgetBuffer` returning `-2` must still mean "not ready, try again".
- Whether `cdvdFreeze()` still re-issues `DoCDVDreadTrack` on load (needed so a savestate taken
  mid-wait resumes correctly).
- Whether `DISCreadTrack` still calls `cdvdRequestSector` (that's how `s_play_lsn` is fed).

After a successful port, commit it and update the base version at the top of `README.md`.

## 3. Build (Windows)

Requirements:
- Visual Studio 2022 17.10+ or Build Tools 2026 with the C++ workload (`<VS>` below is its install folder)
- 7-Zip at `C:\Program Files\7-Zip\7z.exe`
- The PCSX2 dependency pack extracted to `pcsx2\deps\`. **Update it when PCSX2 updates**: download
  `pcsx2-windows-dependencies.7z` from
  https://github.com/PCSX2/pcsx2-windows-dependencies/releases/tag/latest-windows-dependencies
  and extract it into the repo root, replacing `deps\`.

Release build (AVX2; fine for Intel 4th gen / Ryzen and newer):

```powershell
& "<VS>\MSBuild\Current\Bin\amd64\MSBuild.exe" "<repo>\PCSX2_qt.slnx" /m /v:m "/p:Configuration=Release AVX2" /p:Platform=x64
```

The output goes to `pcsx2\bin\pcsx2-qtx64-avx2.exe`. For CPUs without AVX2, use `/p:Configuration=Release`
(output `pcsx2-qtx64.exe`).

**patches.zip:** local builds don't generate `bin\resources\patches.zip` (official CI builds it from
the separate pcsx2_patches repo). Copy it from an installed official PCSX2 of the same era, e.g.
``resources\patches.zip` in its install folder. Without it,
built-in game patches don't load.

### Diagnostic build

The cl.exe `CL` environment variable adds the define without editing project files. Touch the patched
files so MSBuild recompiles them:

```powershell
$repo = "<repo>"
$env:CL = "/DPCSX2_CDVD_DIAG"
foreach ($f in "pcsx2\CDVD\CDVD.cpp","pcsx2\CDVD\CDVDdiscThread.cpp","pcsx2\PerformanceMetrics.cpp") { (Get-Item "$repo\$f").LastWriteTime = Get-Date }
& "<VS>\MSBuild\Current\Bin\amd64\MSBuild.exe" "$repo\PCSX2_qt.slnx" /m /v:m "/p:Configuration=Release AVX2" /p:Platform=x64
# copy bin\pcsx2-qtx64-avx2.exe somewhere as pcsx2-qtx64-avx2-DIAG.exe, then rebuild normally:
$env:CL = $null
foreach ($f in "pcsx2\CDVD\CDVD.cpp","pcsx2\CDVD\CDVDdiscThread.cpp","pcsx2\PerformanceMetrics.cpp") { (Get-Item "$repo\$f").LastWriteTime = Get-Date }
& "<VS>\MSBuild\Current\Bin\amd64\MSBuild.exe" "$repo\PCSX2_qt.slnx" /m /v:m "/p:Configuration=Release AVX2" /p:Platform=x64
```

**Precompiled header warning:** never do the *first* (from-scratch) build with `CL` set. The flag
gets baked into pcsx2's precompiled header and then leaks into every file, including later release
builds. Build the release first, then the DIAG. If it does happen, kill MSBuild nodes
(`Get-Process MSBuild | Stop-Process`), clear `CL`, touch `pcsx2\PrecompiledHeader.cpp` and rebuild
with `/nr:false`. Seeing warning C4651 in a release build means the precompiled header still has the flag.

Check that the release exe has no diagnostics (this should print `False`):

```powershell
Select-String -Path "<repo>\bin\pcsx2-qtx64-avx2.exe" -Pattern "CDVD-DIAG" -SimpleMatch -Quiet
```

## 4. Test

1. Back up memory cards (`Documents\PCSX2\memcards`).
2. Run the DIAG exe, boot a disc from the DVD drive, and play through loads and streaming sections.
3. Read `Documents\PCSX2\logs\emuLog.txt` (it's overwritten on each launch):
   - There should be no `Waited` entries close to 1000 ms or more. Those mean the drive spun down, so
     re-check the keep-spinning code.
   - `Slow frame` entries without disc activity around them are emulator-side hitches, not this fix.
4. Good test games: Naruto Ultimate Hero (SLKA-25232; streams, and showed the spin-down stall), DOA2
   Hardcore, Guilty Gear Isuka.

## 5. Package for sharing

Copy `bin\` without debug and updater files, add the README, and zip it:

```powershell
$repo = "<repo>"; $name = "PCSX2 Disc Seek Fix vN"
$stage = "$repo\releases\$name\$name"
robocopy "$repo\bin" $stage /E /XF *.pdb *.exp *.lib updater.exe
Copy-Item "$repo\docs\disc-seek-fix\RELEASE-README.txt" "$stage\README.txt"
& "C:\Program Files\7-Zip\7z.exe" a -tzip -mx=7 "$repo\releases\$name\$name.zip" $stage
```

`updater.exe` is left out on purpose, so the build can't auto-update itself back to official PCSX2.
