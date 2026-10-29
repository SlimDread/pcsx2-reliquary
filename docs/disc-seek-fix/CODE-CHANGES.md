# Disc Seek Fix: Code Changes, Explained

This document lists every line of code changed between official PCSX2 and the fix, in plain language.

| Version | What it is |
|---|---|
| **Base** | Official PCSX2 v2.9.131 (commit `5b6e6e23e`) |
| **v1** | First fix: the emulator stops freezing while the drive seeks (PS2 discs) |
| **v2** | v1 plus: reads ahead of the game, keeps the drive spinning, and PS1 discs get the fix too |
| **v2.1** | v2 plus: a clear message when the BIOS can't boot because of an encrypted HDD or memory card program |
| **v3** | Built on **pcsx2-reliquary** (which already has v2). Adds whole-block CD reads (audio CD / PS1). No BIOS message. |

Only the **real disc drive** path is changed. ISO files work exactly like official PCSX2.

---

## A few words first

- **Sector**: the smallest piece of a disc (2048 bytes of data on a DVD).
- **Block**: PCSX2 reads the disc 16 sectors at a time. That group is a block.
- **Cache**: memory where PCSX2 keeps blocks it has already read, so it doesn't have to ask the drive again.
- **Emulation thread**: the part of PCSX2 that runs the PS2 itself. If it stops, the game, sound and picture all freeze.
- **IO thread**: a helper that runs alongside the emulator and does the slow work of talking to the DVD drive.
- **`-2`**: the code PCSX2 uses internally for "the data isn't ready yet, try again later".

**The core problem in one sentence:** official PCSX2 made the emulation thread wait on the DVD drive
itself, so every time the drive seeked (100 ms or more), the whole PS2 froze.

**A real PS2 works differently:** the console keeps running while its drive is busy. The game just
sees "drive busy" until the data is ready.

---

# Part 1: Base → v1

## Change 1. Stop freezing while waiting for data (PS2)

**File:** `pcsx2/CDVD/CDVD.cpp`, function `cdvdReadInterrupt()`

This is where the emulated PS2 drive hands a finished sector to the game.

### Base

```cpp
if (cdvd.ReadErr == 0)
{
    while ((cdvd.ReadErr = DoCDVDgetBuffer(&cdr.Transfer[0])), cdvd.ReadErr == -2)
    {
        // not finished yet ... block on the read until it finishes.
        Threading::Sleep(0);
        Threading::SpinWait();
    }
}
```

### v1

```cpp
if (cdvd.ReadErr == 0 || cdvd.ReadErr == -2)
{
    cdvd.ReadErr = DoCDVDgetBuffer(&cdr.Transfer[0]);
    if (cdvd.ReadErr == -2)
    {
        // Host read not finished yet. Rather than freezing the emulator until it lands,
        // keep the drive busy and check again a sector-time later, the way a real drive
        // keeps the console running while it waits on the disc. Needs proper testing across games.
        CDVDREAD_INT(cdvd.ReadTime);
        return;
    }
}
```

### What changed

- **Before:** the `while` loop asked "is the data ready?" over and over, and refused to move on until
  it was. Nothing else in the emulator ran during that time, so you saw a freeze.
- **After:** it asks once. If the answer is "not yet" (`-2`), it sets a reminder to ask again in one
  sector's worth of time (`CDVDREAD_INT(cdvd.ReadTime)`) and returns. The emulated PS2 keeps running,
  and the game sees the drive as "busy, reading", just like real hardware.
- **Why the `if` line changed:** `ReadErr` is set to `-2` while waiting, so the next check has to
  accept `-2` as "still waiting, ask again", not only `0`.

---

## Change 2. The emulator never reads the drive directly

**File:** `pcsx2/CDVD/CDVDdiscReader.cpp`, function `DISCgetBuffer()`

This is what Change 1 calls to get the sector data from the real drive.

### Base

```cpp
memcpy(dest, cdvdGetSector(csector, cmode), csize);

return 0;
```

### v1

```cpp
// Don't read the drive on the emulation thread: if the IO thread hasn't fetched the block yet,
// report "not ready" so the emulated drive stays busy while the console keeps running.
bool failed;
u8* data = cdvdTryGetSector(csector, cmode, failed);
if (!data)
{
    if (!failed)
        return -2;

    // The IO thread's read failed; retry synchronously like before so error behavior is unchanged.
    data = cdvdGetSector(csector, cmode);
}

memcpy(dest, data, csize);

return 0;
```

### What changed

- **Before:** `cdvdGetSector()` checked the cache, and if the block wasn't there, **read it from the
  drive right there on the emulation thread**. This was the real freeze: the emulator had to wait for
  the drive to seek. The IO thread was often reading the same block at the same moment, so the drive
  got two requests and had to seek twice.
- **After:** `cdvdTryGetSector()` (new, Change 3) only looks in the cache. If the data is there, it's
  copied. If not, the function returns `-2` and Change 1 checks again shortly.
- **Safety net:** if the IO thread tried and *failed* to read that block (scratched disc, etc.), it
  falls back to the old direct read. Error behavior is exactly the same as official PCSX2.

**File:** `pcsx2/CDVD/CDVDdiscReader.h` declares the new function:

```cpp
u8* cdvdGetSector(u32 sector, s32 mode);
u8* cdvdTryGetSector(u32 sector, s32 mode, bool& failed);   // added
```

---

## Change 3. New "look in the cache only" function

**File:** `pcsx2/CDVD/CDVDdiscThread.cpp`

### Base

```cpp
u8* cdvdGetSector(u32 sector, s32 mode)
{
    static u8 buffer[2352 * sectors_per_read];

    // Align to cache block
    u32 sector_block = sector & ~(sectors_per_read - 1);

    if (!cdvdCacheFetch(sector_block, buffer))
        if (cdvdReadBlockOfSectors(sector_block, buffer))
            cdvdCacheUpdate(sector_block, buffer);

    if (src->GetMediaType() >= 0)
    {
        ... (work out where the sector is inside the block and return it)
```

### v1

```cpp
static u8* cdvdSectorData(u8* buffer, u32 sector, u32 sector_block, s32 mode);

u8* cdvdTryGetSector(u32 sector, s32 mode, bool& failed)
{
    static u8 buffer[2352 * sectors_per_read];

    failed = false;

    // Align to cache block
    u32 sector_block = sector & ~(sectors_per_read - 1);

    if (!cdvdCacheFetch(sector_block, buffer))
    {
        if (s_failed_lsn.load(std::memory_order_acquire) == sector_block)
        {
            failed = true;
            return nullptr;
        }

        // Re-post in case a later request superseded this one; a no-op if it's already being read.
        cdvdRequestSector(sector, mode);
        return nullptr;
    }

    return cdvdSectorData(buffer, sector, sector_block, mode);
}

u8* cdvdGetSector(u32 sector, s32 mode)
{
    static u8 buffer[2352 * sectors_per_read];

    // Align to cache block
    u32 sector_block = sector & ~(sectors_per_read - 1);

    if (!cdvdCacheFetch(sector_block, buffer))
        if (cdvdReadBlockOfSectors(sector_block, buffer))
            cdvdCacheUpdate(sector_block, buffer);

    return cdvdSectorData(buffer, sector, sector_block, mode);
}

static u8* cdvdSectorData(u8* buffer, u32 sector, u32 sector_block, s32 mode)
{
    if (src->GetMediaType() >= 0)
    {
        ... (unchanged: work out where the sector is inside the block and return it)
```

### What changed

- **`cdvdTryGetSector()` is new.** It does three things:
  1. If the block is in the cache, it returns the sector.
  2. If the IO thread marked that block as failed, it says "failed" so Change 2 can fall back.
  3. Otherwise it asks the IO thread for the block again and returns "not yet". Asking again matters
     because the game may have asked for something else in between (see Change 4).
- **`cdvdSectorData()`** is not new logic. The part of `cdvdGetSector()` that finds a sector inside
  a block was moved into its own function, so both functions can share it. `cdvdGetSector()` now
  only runs in the failure fallback.

---

## Change 4. One "latest request" slot instead of a queue

**File:** `pcsx2/CDVD/CDVDdiscThread.cpp`

### Base (shared variables)

```cpp
static std::mutex s_notify_lock;
static std::condition_variable s_notify_cv;
static std::mutex s_request_lock;
static std::queue<u32> s_request_queue;
static std::mutex s_cache_lock;
```

### v1 (shared variables)

```cpp
// s_notify_lock guards the pending request. Only the newest request matters: like a real drive,
// a new seek supersedes whatever was asked for before, so we don't seek to stale sectors first.
static std::mutex s_notify_lock;
static std::condition_variable s_notify_cv;
static u32 s_request_lsn;
static bool s_request_pending = false;
// Block whose requested read failed, so pollers can stop waiting on it
static std::atomic<u32> s_failed_lsn{std::numeric_limits<u32>::max()};
static std::mutex s_cache_lock;
```

### Base (asking the IO thread for a block)

```cpp
void cdvdRequestSector(u32 sector, s32 mode)
{
    if (sector >= src->GetSectorCount())
        return;

    // Align to cache block
    sector &= ~(sectors_per_read - 1);

    if (cdvdCacheCheck(sector))
        return;

    {
        std::lock_guard<std::mutex> guard(s_request_lock);
        s_request_queue.push(sector);
    }

    s_notify_cv.notify_one();
}
```

### v1

```cpp
void cdvdRequestSector(u32 sector, s32 mode)
{
    if (sector >= src->GetSectorCount())
        return;

    // Align to cache block
    sector &= ~(sectors_per_read - 1);

    if (cdvdCacheCheck(sector))
        return;

    {
        std::lock_guard<std::mutex> guard(s_notify_lock);
        s_request_lsn = sector;
        s_request_pending = true;
        if (s_failed_lsn.load(std::memory_order_relaxed) == sector)
            s_failed_lsn.store(std::numeric_limits<u32>::max(), std::memory_order_relaxed);
    }

    s_notify_cv.notify_one();
}
```

The disc-change code was updated to match (it used to empty the queue, now it clears the slot):

```cpp
// Base
{
    std::lock_guard<std::mutex> request_guard(s_request_lock);
    s_request_queue = decltype(s_request_queue)();
}
// v1
{
    std::lock_guard<std::mutex> request_guard(s_notify_lock);
    s_request_pending = false;
}
```

### What changed

- **Before:** every request went into a line (`std::queue`), and the IO thread worked through them in
  order. If the game asked for block A and then quickly jumped to block B, the drive still went to A
  first, which was a wasted seek.
- **After:** there is one slot holding **only the newest request**. A new request overwrites the old
  one, like a real drive abandoning a seek when told to go somewhere else.
- **`s_failed_lsn`** remembers which block couldn't be read, so Change 3 can stop waiting for it. It's
  cleared when that block is requested again, so a retry gets a fresh chance.
- **Why one lock now:** the slot is protected by the same lock (`s_notify_lock`) the IO thread uses
  when it sleeps. That's what makes Change 5's "never miss a wake-up" work.

---

## Change 5. IO thread rework

**File:** `pcsx2/CDVD/CDVDdiscThread.cpp`, function `cdvdThread()`

### Base

```cpp
static void cdvdThread()
{
    u8 buffer[2352 * sectors_per_read];
    u32 prefetches_left = 0;

    printf(" * CDVD: IO thread started...\n");
    std::unique_lock<std::mutex> guard(s_notify_lock);

    while (cdvd_is_open)
    {
        if (cdvdUpdateDiscStatus())
        {
            // Need to sleep some to avoid an aggressive spin that sucks the cpu dry.
            s_notify_cv.wait_for(guard, std::chrono::milliseconds(10));
            prefetches_left = 0;
            continue;
        }

        if (prefetches_left == 0)
            s_notify_cv.wait_for(guard, std::chrono::milliseconds(250));

        // check again to make sure we're not done here...
        if (!cdvd_is_open)
            break;

        // Read request
        bool handling_request = false;
        u32 request_lsn;

        {
            std::lock_guard<std::mutex> request_guard(s_request_lock);
            if (!s_request_queue.empty())
            {
                request_lsn = s_request_queue.front();
                s_request_queue.pop();
                handling_request = true;
            }
        }

        if (!handling_request)
        {
            if (prefetches_left == 0)
                continue;

            --prefetches_left;

            u32 next_prefetch_lsn = g_last_sector_block_lsn + sectors_per_read;
            request_lsn = next_prefetch_lsn;
        }

        // Handle request
        if (!cdvdCacheCheck(request_lsn))
        {
            if (cdvdReadBlockOfSectors(request_lsn, buffer))
            {
                cdvdCacheUpdate(request_lsn, buffer);
            }
            else
            {
                // If the read fails, further reads are likely to fail too.
                prefetches_left = 0;
                continue;
            }
        }

        g_last_sector_block_lsn = request_lsn;

        if (!handling_request)
            continue;

        // Prefetch
        u32 next_prefetch_lsn = g_last_sector_block_lsn + sectors_per_read;
        if (next_prefetch_lsn >= src->GetSectorCount())
        {
            prefetches_left = 0;
        }
        else
        {
            const u32 max_prefetches = 16;
            u32 remaining = src->GetSectorCount() - next_prefetch_lsn;
            prefetches_left = std::min((remaining + sectors_per_read - 1) / sectors_per_read, max_prefetches);
        }
    }
    printf(" * CDVD: IO thread finished.\n");
}
```

### v1

```cpp
static void cdvdThread()
{
    u8 buffer[2352 * sectors_per_read];
    u32 prefetches_left = 0;

    printf(" * CDVD: IO thread started...\n");
    std::unique_lock<std::mutex> guard(s_notify_lock);
    auto last_status_check = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    while (cdvd_is_open)
    {
        // Polling the drive status costs an IOCTL round trip, so don't do it in front of every
        // block while the game is streaming; once a second (or when idle) is enough to notice ejects.
        const auto now = std::chrono::steady_clock::now();
        if ((!s_request_pending && prefetches_left == 0) || now - last_status_check >= std::chrono::seconds(1))
        {
            last_status_check = now;
            guard.unlock();
            const bool not_ready = cdvdUpdateDiscStatus();
            guard.lock();
            if (not_ready)
            {
                // Need to sleep some to avoid an aggressive spin that sucks the cpu dry.
                s_notify_cv.wait_for(guard, std::chrono::milliseconds(10));
                prefetches_left = 0;
                continue;
            }
        }

        // Predicate wait: a request posted before we got here is never missed.
        if (prefetches_left == 0)
            s_notify_cv.wait_for(guard, std::chrono::milliseconds(250), [] { return s_request_pending || !cdvd_is_open; });

        // check again to make sure we're not done here...
        if (!cdvd_is_open)
            break;

        // Read request
        const bool handling_request = s_request_pending;
        u32 request_lsn;

        if (handling_request)
        {
            request_lsn = s_request_lsn;
            s_request_pending = false;
        }
        else
        {
            if (prefetches_left == 0)
                continue;

            --prefetches_left;

            request_lsn = g_last_sector_block_lsn + sectors_per_read;
            if (request_lsn >= src->GetSectorCount())
            {
                prefetches_left = 0;
                continue;
            }
        }

        // Drop the lock while the drive works so the emulator can post new requests without waiting.
        guard.unlock();
        bool read_ok = true;
        if (!cdvdCacheCheck(request_lsn))
        {
            if (cdvdReadBlockOfSectors(request_lsn, buffer))
                cdvdCacheUpdate(request_lsn, buffer);
            else
                read_ok = false;
        }
        guard.lock();

        if (!read_ok)
        {
            if (handling_request)
                s_failed_lsn.store(request_lsn, std::memory_order_release);
            // If the read fails, further reads are likely to fail too.
            prefetches_left = 0;
            continue;
        }

        g_last_sector_block_lsn = request_lsn;

        if (!handling_request)
            continue;

        // Prefetch (unchanged from base)
        u32 next_prefetch_lsn = g_last_sector_block_lsn + sectors_per_read;
        if (next_prefetch_lsn >= src->GetSectorCount())
        {
            prefetches_left = 0;
        }
        else
        {
            const u32 max_prefetches = 16;
            u32 remaining = src->GetSectorCount() - next_prefetch_lsn;
            prefetches_left = std::min((remaining + sectors_per_read - 1) / sectors_per_read, max_prefetches);
        }
    }
    printf(" * CDVD: IO thread finished.\n");
}
```

### What changed

1. **The drive status check runs less often.** Base asked the drive "is a disc still in there?"
   (`cdvdUpdateDiscStatus()`) before *every* block, which added a delay to each read. v1 asks
   at most once a second while busy, and every time when idle. That's still plenty to notice an eject.
2. **A missed wake-up can't happen any more.** In base, the request queue had its own lock, separate
   from the lock the thread sleeps on. If a request arrived at just the wrong moment, the "wake up"
   signal was missed and the thread slept for up to **250 ms** with work waiting. v1 sleeps with a
   condition (`[] { return s_request_pending || ... }`): before sleeping it checks whether there's
   already a request, under the same lock the request is posted with. A request can't slip through.
3. **The lock is released while the drive is reading** (`guard.unlock()` / `guard.lock()` around the
   read). The emulator can post a new request instantly instead of waiting for the read to finish.
4. **Failures are reported.** When the game's own request fails, `s_failed_lsn` is set so Change 3
   stops waiting and falls back.
5. **A bounds check was added** so readahead never tries to read past the end of the disc.

The readahead itself (16 blocks after each request) is still the same as base in v1. That turned out
to be the cause of the last stutter, fixed in v2.

---

# Part 2: v1 → v2

## What the v1 diagnostic log showed (Naruto)

```
261.19  Drive read of sector 1799680 (prefetch) took 106 ms
        ...the drive does nothing for about 4 seconds...
265.18  Drive read of sector 1799808 (request) took 1265.6 ms
265.18  Waited 1266.1 ms on drive
```

Naruto streams a small chunk about every 2 seconds. v1 only read 16 blocks ahead after a cache miss.
The game then read from the cache, made no new requests, and readahead stopped. The PC drive sat idle,
**slowed its spindle after about 4 seconds**, and the next read had to wait **1.27 s** for it to spin
back up. That was the stutter. A real PS2 drive keeps spinning and keeps its buffer full while a game
is running.

## Change 6. Rolling readahead and keeping the drive spinning

**File:** `pcsx2/CDVD/CDVDdiscThread.cpp`

### New shared variable

```cpp
// Block the game is currently reading (updated even on cache hits) so readahead can follow it
static std::atomic<u32> s_play_lsn{0};
```

### `cdvdRequestSector()`: v1 → v2 (two lines added before the cache check)

```cpp
    // Align to cache block
    sector &= ~(sectors_per_read - 1);

    // Track the game's position even when it's cached, and wake the IO thread so readahead keeps up.
    if (s_play_lsn.exchange(sector, std::memory_order_relaxed) != sector)
        s_notify_cv.notify_one();

    if (cdvdCacheCheck(sector))
        return;
```

**What this does:** every time the game moves on to a new block, even one already in the cache, the
IO thread is told where the game is now. v1 only knew when the game *missed* the cache.

### `cdvdThread()`: v2 (full function)

(The `#ifdef PCSX2_CDVD_DIAG` timing lines around the drive read are left out here for readability.
They're shown in the Diagnostics section at the end.)

```cpp
static void cdvdThread()
{
    using clock = std::chrono::steady_clock;

    // Keep this much read ahead of where the game is reading, like the PS2 drive keeping its buffer
    // full while a game streams. 128 blocks = 2048 sectors (4MB on DVD).
    constexpr u32 readahead_sectors = 128 * sectors_per_read;
    // PC drives slow their spindle after a few seconds without commands and take over a second to spin
    // back up (seen as 1.27s stalls in Naruto streaming). A real PS2 drive keeps spinning, so touch the
    // disc before that idle timer runs out.
    constexpr auto keepalive_interval = std::chrono::seconds(2);
    // Let the drive rest once the game hasn't asked for anything in a long while (paused, idle menu).
    constexpr auto keepalive_window = std::chrono::minutes(10);

    u8 buffer[2352 * sectors_per_read];
    u32 prefetch_lsn = 0;
    bool prefetch_enabled = false;

    printf(" * CDVD: IO thread started...\n");
    std::unique_lock<std::mutex> guard(s_notify_lock);
    auto last_status_check = clock::now() - std::chrono::seconds(1);
    auto last_drive_io = clock::now();
    auto last_request = clock::now() - keepalive_window;

    // Follow the game: if it has read past our readahead (cache hits) or jumped far back into cached
    // data, continue readahead from where it is now.
    const auto sync_prefetch = [&]() {
        if (!prefetch_enabled)
            return;
        const u32 play = s_play_lsn.load(std::memory_order_relaxed);
        if (play > prefetch_lsn || play + 2 * readahead_sectors < prefetch_lsn)
            prefetch_lsn = play;
    };
    const auto want_prefetch = [&]() {
        return prefetch_enabled && prefetch_lsn < src->GetSectorCount() &&
            prefetch_lsn < s_play_lsn.load(std::memory_order_relaxed) + readahead_sectors;
    };

    while (cdvd_is_open)
    {
        sync_prefetch();

        // Polling the drive status costs an IOCTL round trip, so don't do it in front of every
        // block while the game is streaming; once a second (or when idle) is enough to notice ejects.
        const auto now = clock::now();
        if ((!s_request_pending && !want_prefetch()) || now - last_status_check >= std::chrono::seconds(1))
        {
            last_status_check = now;
            guard.unlock();
            const bool not_ready = cdvdUpdateDiscStatus();
            guard.lock();
            if (not_ready)
            {
                // Need to sleep some to avoid an aggressive spin that sucks the cpu dry.
                s_notify_cv.wait_for(guard, std::chrono::milliseconds(10));
                prefetch_enabled = false;
                continue;
            }
        }

        // Predicate wait: a request posted before we got here is never missed.
        if (!s_request_pending && !want_prefetch())
        {
            s_notify_cv.wait_for(guard, std::chrono::milliseconds(250),
                [&] { return s_request_pending || !cdvd_is_open || want_prefetch(); });
            sync_prefetch();
        }

        // check again to make sure we're not done here...
        if (!cdvd_is_open)
            break;

        const auto wake = clock::now();
        const bool handling_request = s_request_pending;
        bool keepalive = false;
        u32 request_lsn;

        if (handling_request)
        {
            request_lsn = s_request_lsn;
            s_request_pending = false;
            last_request = wake;
        }
        else if (want_prefetch())
        {
            request_lsn = prefetch_lsn;
        }
        else if (wake - last_drive_io >= keepalive_interval && wake - last_request < keepalive_window && src->GetSectorCount())
        {
            // Read the next block past the readahead window: useful data, and it's new to the drive so it
            // really has to go to the disc. At the end of the disc, re-read the last block instead.
            keepalive = true;
            request_lsn = (prefetch_enabled && prefetch_lsn < src->GetSectorCount()) ? prefetch_lsn : g_last_sector_block_lsn;
        }
        else
        {
            continue;
        }

        // Drop the lock while the drive works so the emulator can post new requests without waiting.
        guard.unlock();
        bool read_ok = true;
        bool touched_drive = false;
        if (keepalive || !cdvdCacheCheck(request_lsn))
        {
            touched_drive = true;
            if (cdvdReadBlockOfSectors(request_lsn, buffer))
                cdvdCacheUpdate(request_lsn, buffer);
            else
                read_ok = false;
        }
        guard.lock();

        if (touched_drive)
            last_drive_io = clock::now();

        if (!read_ok)
        {
            if (handling_request)
                s_failed_lsn.store(request_lsn, std::memory_order_release);
            // If the read fails, further reads are likely to fail too.
            prefetch_enabled = false;
            continue;
        }

        g_last_sector_block_lsn = request_lsn;

        if (handling_request)
        {
            // New seek: restart readahead right after it
            prefetch_lsn = request_lsn + sectors_per_read;
            prefetch_enabled = true;
        }
        else if (request_lsn == prefetch_lsn)
        {
            prefetch_lsn += sectors_per_read;
        }
    }
    printf(" * CDVD: IO thread finished.\n");
}
```

### What changed from v1

The loop now decides each turn between **three jobs, in priority order:**

1. **The game asked for something** (`handling_request`). Read it. This always comes first.
2. **Readahead** (`want_prefetch()`). If we're less than 4 MB ahead of where the game is reading,
   read the next block. This is a "rolling" window: as the game moves forward, the window moves with it.
3. **Keep the drive spinning** (`keepalive`). If the drive hasn't been asked to do anything for
   **2 seconds**, read one more block past the window. That stops the drive deciding it's idle and
   slowing down. The block is new data the drive hasn't read yet, so it has to go to the disc, and it's
   useful readahead anyway.

The new pieces:

| Code | What it means |
|---|---|
| `readahead_sectors = 128 * 16` | Keep 2048 sectors (4 MB) ready ahead of the game. Base/v1 read 256 sectors and then stopped. |
| `keepalive_interval = 2 s` | Touch the drive if it's been idle this long. The drive slowed down after about 4 s in the log, so 2 s stays well under that. |
| `keepalive_window = 10 min` | Stop keeping it spinning if the game hasn't asked for anything in 10 minutes (paused emulator, idle menu), so the drive can rest. |
| `prefetch_lsn` | The next block readahead will read. |
| `sync_prefetch()` | If the game got ahead of readahead, or jumped far back to data already in the cache, restart readahead from where the game is now. |
| `want_prefetch()` | "Is the window not full yet?" |
| `last_drive_io` | When the drive last actually did a read. Drives the 2-second keep-spinning timer. |
| `last_request` | When the game last asked for something the cache didn't have. Drives the 10-minute cutoff. |
| `touched_drive` | Only real drive reads count for the 2-second timer, not cache hits. |
| After a request: `prefetch_lsn = request_lsn + 16` | After a seek, readahead restarts right after the new position. |

The old `prefetches_left` counter is gone; the window replaces it.

---

## Change 7. PS1 discs stop freezing too

**File:** `pcsx2/CDVD/Ps1CD.cpp`, function `cdrReadInterrupt()`

This is the same idea as Change 1, for the PS1 drive emulation.

### Base / v1

```cpp
CDVD_LOG("KEY END");

cdr.OCUP = 1;
SetResultSize(1);
cdr.StatP &= ~STATUS_SEEK;
cdr.StatP |= STATUS_READ;
cdr.Result[0] = cdr.StatP;

if (cdr.RErr == 0)
{
    while ((cdr.RErr = DoCDVDgetBuffer(cdr.Transfer)), cdr.RErr == -2)
    {
        // not finished yet ... block on the read until it finishes.
        Threading::Sleep(0);
        Threading::SpinWait();
    }
}
```

### v2

```cpp
CDVD_LOG("KEY END");

if (cdr.RErr == 0 || cdr.RErr == -2)
{
    cdr.RErr = DoCDVDgetBuffer(cdr.Transfer);
    if (cdr.RErr == -2)
    {
        // Host drive hasn't delivered the sector yet. Instead of freezing the emulator until it does,
        // check again a sector-time later and let the console keep running, like real hardware.
        // Needs proper testing across PS1 games.
        CDREAD_INT((cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime);
        return;
    }
}

cdr.OCUP = 1;
SetResultSize(1);
cdr.StatP &= ~STATUS_SEEK;
cdr.StatP |= STATUS_READ;
cdr.Result[0] = cdr.StatP;
```

### What changed

- The freeze loop is replaced by "ask once, and if it's not ready, check again in one sector-time",
  exactly like Change 1. `(cdr.Mode & 0x80) ? (cdReadTime / 2) : cdReadTime` is one sector-time at
  the PS1 drive's current speed (single or double).
- **The check was moved above the `cdr.OCUP = 1; SetResultSize(1); ...` lines.** Those lines tell
  the game "a sector is ready". They must not run while we're still waiting, otherwise the game could
  see "ready" before the data is there. Now they only run once the data really is ready.
- This was not tested on real PS1 discs (no PS1 game was in the test set).

---

# Part 3: v2 → v2.1

## Change 8. Explain why the game won't boot instead of hanging silently

**File:** `pcsx2/CDVD/CDVD.cpp`

**The situation:** a user's PCSX2 (Europe BIOS v1.90, emulated HDD turned on) couldn't boot a disc
with fast boot off. Before the game, the BIOS tried to start an **encrypted** PS2 program, most likely
a boot program on his HDD image. Real PS2s decrypt these with secret Sony keys. PCSX2 doesn't have
those keys, so it can't run the program, and the BIOS just stops. Nobody could tell why.

This can't be properly fixed without Sony's keys, so instead the user now gets a clear message.

### Base

```cpp
static __fi void fail_pol_cal()
{
    Console.Error("[MG] ERROR - Make sure the file is already decrypted!!!");
    cdvd.SCMDResultBuff[0] = 0x80;
}
```

### v2.1

```cpp
// near the top of the file:
// Shown once per boot when the BIOS fails to start an encrypted program before any game has loaded.
static bool s_mg_boot_warning_shown = false;

// in cdvdReset():
s_mg_boot_warning_shown = false;

static __fi void fail_pol_cal()
{
    Console.Error("[MG] ERROR - Make sure the file is already decrypted!!!");
    cdvd.SCMDResultBuff[0] = 0x80;

    // PCSX2 can't decrypt MagicGate (KELF) programs. If the BIOS hits one during its own startup
    // (HDD boot program, memory card system update), it stalls there and the disc never boots, which
    // otherwise looks like a silent hang. Tell the user why and how to get past it.
    if (!s_mg_boot_warning_shown && VMManager::GetCurrentCRC() == 0)
    {
        s_mg_boot_warning_shown = true;
        const char* source = EmuConfig.DEV9.HddEnable ?
            "most likely from the emulated HDD (a boot program on the HDD image), or a memory card system update such as FreeMcBoot" :
            "most likely from a memory card system update such as FreeMcBoot";
        const std::string message = fmt::format(
            "The PS2 BIOS tried to start an encrypted (MagicGate) program before your game, {}.\n\n"
            "PCSX2 can't decrypt these, so the BIOS stops here and the game won't start.\n\n"
            "To fix it, do one of these:\n"
            "{}"
            " - Enable Fast Boot (Settings > BIOS)\n"
            " - Use a different BIOS\n"
            " - Use a memory card without a system update",
            source,
            EmuConfig.DEV9.HddEnable ? " - Disable the HDD (Settings > Network & HDD > uncheck Enable HDD)\n" : "");
        Console.Error("[MG] %s", message.c_str());
        Host::ReportErrorAsync("Game can't boot with this BIOS setup", message);
    }
}
```

### What changed

- `fail_pol_cal()` is what PCSX2 runs when an encrypted program can't be handled. Before, it only
  wrote one cryptic line to the log.
- Now, **if no game has started yet** (`GetCurrentCRC() == 0` means "still in the BIOS"), it also shows
  a pop-up titled **"Game can't boot with this BIOS setup"** that explains what happened and how to fix it.
- If the HDD is turned on, the message says the HDD is the likely cause and lists "Disable the HDD"
  first. Otherwise it points at the memory card.
- `s_mg_boot_warning_shown` makes sure the pop-up appears **once per boot**, not every time the BIOS
  retries. `cdvdReset()` clears it when the virtual PS2 restarts.
- Once a game is running, failures don't trigger the pop-up, so games are unaffected.

---

# Part 4: Reliquary (v3)

From 2026-10-28 the base is **SlimDread/pcsx2-reliquary**, which already contains Changes 1–7.
Change 8 (the BIOS message) is not used there. v3 adds:

## Change 9. Read a whole CD block in one drive command

**File:** `pcsx2/CDVD/Windows/IOCtlSrc.cpp`, function `ReadSectors2352()`

**The situation:** audio CDs (and PS1 discs) still skipped. CDs aren't read with normal file reads;
PCSX2 sends the drive a SCSI **READ CD** command. Upstream sent **one command per sector**, so 16
commands for every 16-sector block. Most drives don't read ahead on audio, so after each command the
drive has to wait for the next sector to spin round again. That limits it to roughly 1x speed, which
is exactly what CD audio needs, with no room to spare, so any hiccup makes it skip.

### Base (reliquary / upstream)

```cpp
// Read sectors one by one to avoid reading data from 2 tracks of different
// types in the same read (which will fail).
for (u32 n = 0; n < count; ++n)
{
    u32 current_sector = sector + n;
    sptd.info.Cdb[2] = (current_sector >> 24) & 0xFF;
    sptd.info.Cdb[3] = (current_sector >> 16) & 0xFF;
    sptd.info.Cdb[4] = (current_sector >> 8) & 0xFF;
    sptd.info.Cdb[5] = current_sector & 0xFF;
    sptd.info.DataTransferLength = 2352;
    sptd.info.DataBuffer = buffer + 2352 * n;
    sptd.info.SenseInfoLength = sizeof(sptd.sense_buffer);

    DWORD unused;
    if (DeviceIoControl(m_device, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd,
                        sizeof(sptd), &sptd, sizeof(sptd), &unused, nullptr))
    {
        if (sptd.info.DataTransferLength == 2352)
            continue;
    }
    printf(" * CDVD: SPTI failed reading sector %u; SENSE %u -", current_sector, sptd.info.SenseInfoLength);
    for (const auto& c : sptd.sense_buffer)
        printf(" %02X", c);
    putchar('\n');
    return false;
}
```

### v3

```cpp
// top of file
#include <atomic>
// Consecutive multi-sector READ CD failures; reset on success and when a disc is (re)opened
static std::atomic<int> s_multi_read_failures{0};

// in Reopen(), after closing the old handle
s_multi_read_failures.store(0, std::memory_order_relaxed);

// in ReadSectors2352()
const auto read_range = [&](u32 first_sector, u32 n, u8* dest, bool report) -> bool {
    sptd.info.Cdb[2] = (first_sector >> 24) & 0xFF;
    sptd.info.Cdb[3] = (first_sector >> 16) & 0xFF;
    sptd.info.Cdb[4] = (first_sector >> 8) & 0xFF;
    sptd.info.Cdb[5] = first_sector & 0xFF;
    sptd.info.Cdb[6] = (n >> 16) & 0xFF;
    sptd.info.Cdb[7] = (n >> 8) & 0xFF;
    sptd.info.Cdb[8] = n & 0xFF;
    sptd.info.DataTransferLength = 2352 * n;
    sptd.info.DataBuffer = dest;
    sptd.info.SenseInfoLength = sizeof(sptd.sense_buffer);

    DWORD unused;
    if (DeviceIoControl(m_device, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd,
                        sizeof(sptd), &sptd, sizeof(sptd), &unused, nullptr))
    {
        if (sptd.info.DataTransferLength == 2352 * n)
            return true;
    }
    if (report)
    {
        printf(" * CDVD: SPTI failed reading sector %u; SENSE %u -", first_sector, sptd.info.SenseInfoLength);
        for (const auto& c : sptd.sense_buffer)
            printf(" %02X", c);
        putchar('\n');
    }
    return false;
};

// Read the whole block in one command. ... Needs proper testing across drives.
constexpr int max_multi_failures = 8;
if (count > 1 && s_multi_read_failures.load(std::memory_order_relaxed) < max_multi_failures)
{
    if (read_range(sector, count, buffer, false))
    {
        s_multi_read_failures.store(0, std::memory_order_relaxed);
        return true;
    }
    // Expected occasionally: a single read can't span 2 tracks of different types.
    s_multi_read_failures.fetch_add(1, std::memory_order_relaxed);
}

// Fall back to reading sectors one by one.
for (u32 n = 0; n < count; ++n)
{
    if (!read_range(sector + n, 1, buffer + 2352 * n, true))
        return false;
}
```

### What changed

- `read_range()` sends **one READ CD command for several sectors**. Bytes 6–8 of the command
  (`Cdb[6..8]`) hold how many sectors to read; the base code always put `1` there.
- The whole 16-sector block is tried in one command first. The drive can then stream the sectors
  back to back, like a real PS2 drive, instead of stopping after each one.
- **Fallback:** a single read can't cross from a data track into an audio track (the drive rejects it).
  When the big read fails, that block is read one sector at a time, exactly like the base code, so
  nothing that worked before breaks.
- **Bad-drive guard:** if the big read fails 8 times in a row, the drive probably doesn't support it,
  so we stop trying and go straight to one-at-a-time (no wasted command per block). The counter
  resets after any successful big read and whenever a disc is opened.
- **DVD movies didn't need this.** DVDs are read with normal file reads of the whole block at once,
  and they already go through the fixed non-blocking path and readahead.

## Change 11. Don't ask the drive for the play position on the emulation thread

**File:** `pcsx2/CDVD/CDVDdiscReader.cpp`, function `DISCreadSubQ()`

**The situation (found with the v3 DIAG build):** when a CD started playing there was heavy lag for a
few seconds, then everything was smooth. The log showed slow frames of 50–165 ms, each one landing
exactly when a drive read finished, while readahead was filling its 4 MB at the start. The game
itself barely waited for data.

**Cause:** the PS2 CD player constantly asks for the current track and position ("subchannel Q").
Upstream answered part of that by **asking the real drive** (`src->ReadTrackSubQ()`), on the emulation
thread. While the IO thread had a 56 ms read in progress, that request had to wait behind it, so the
whole emulator stalled. Once readahead was full, the drive was mostly idle, so the stalls stopped.

### Base

```cpp
if (!src->ReadTrackSubQ(subq))
{
    subq->adr = 1;
    subq->trackNum = i;
    subq->trackIndex = 1;
}
```

### v3

```cpp
// Don't ask the drive for them, though: this runs on the emulation thread, ...
subq->adr = 1;
subq->trackNum = i;
subq->trackIndex = 1;
```

### What changed

- The values are now always worked out from the CD's track list, which was the old fallback. The
  drive is never asked from the emulation thread.
- The minutes and seconds shown by the CD player were already calculated this way. Only the
  ADR/track/index came from the drive.
- The drive's answer was also unreliable: it reports where the drive's head is (readahead), not
  where the game is reading.
- The only difference is the track "index" inside the short gaps between tracks: it now always
  reads 1 instead of the drive's (head-based) 0.

## Change 10. PS1 diagnostic line

**File:** `pcsx2/CDVD/Ps1CD.cpp` (DIAG builds only). This is the same stopwatch as the PS2 one, but it
logs `[CDVD-DIAG] PS1: waited X ms on drive` so PS1 disc tests can be checked.

---

# Diagnostics (not in the release builds)

These lines are wrapped in `#ifdef PCSX2_CDVD_DIAG`, so normal builds (and the v2 zip) don't include
them. They're only compiled into the DIAG build that was used to find the Naruto stutter.

**`CDVD.cpp` `cdvdReadInterrupt()`**: measures how long the game actually waited on the drive:

```cpp
#ifdef PCSX2_CDVD_DIAG
static Common::Timer::Value s_diag_wait_start = 0;
static u32 s_diag_polls = 0;
if (cdvd.ReadErr == -2)
{
    if (s_diag_polls++ == 0)
        s_diag_wait_start = Common::Timer::GetCurrentValue();
}
else if (s_diag_polls)
{
    const double ms = Common::Timer::ConvertValueToMilliseconds(Common::Timer::GetCurrentValue() - s_diag_wait_start);
    if (ms >= 2.0)
        Console.Warning("[CDVD-DIAG] Waited %.1f ms on drive for sector %u (%u polls)", ms, cdvd.CurrentSector, s_diag_polls);
    s_diag_polls = 0;
}
#endif
```

It starts a stopwatch on the first "not ready" and logs the total once the data arrives (if it took
2 ms or more). `CDVD.cpp` also got `#include "common/Timer.h"` for this.

**`CDVD.cpp` `cdvdStartSeek()`**: logs long seeks the game itself asks for (a real PS2 takes that time too):

```cpp
#ifdef PCSX2_CDVD_DIAG
if (isSeeking || drive_speed_change_cycles || seektime >= PSXCLK / 50)
    Console.WriteLn("[CDVD-DIAG] Emulated seek %u -> %u (%u sectors): %.1f ms", cdvd.CurrentSector, newsector, delta,
        static_cast<double>(seektime) * 1000.0 / static_cast<double>(PSXCLK));
#endif
```

**`CDVDdiscThread.cpp` `cdvdThread()`**: times each real drive read and logs slow (30 ms or more) or failed ones:

```cpp
#ifdef PCSX2_CDVD_DIAG
const double diag_ms = std::chrono::duration<double, std::milli>(clock::now() - diag_start).count();
if (diag_ms >= 30.0 || !read_ok)
    Console.WriteLn("[CDVD-DIAG] Drive read of sector %u (%s) took %.1f ms%s", request_lsn,
        handling_request ? "request" : (keepalive ? "keepalive" : "prefetch"), diag_ms, read_ok ? "" : " FAILED");
#endif
```

(It also added `#include "common/Console.h"` at the top of the file.)

**`PerformanceMetrics.cpp` `Update()`**: logs frames the emulator itself was slow to make
(25 ms or more), which tells emulator hitches apart from disc waits:

```cpp
#ifdef PCSX2_CDVD_DIAG
if (frame_time >= 25.0f && s_frame_number > 60)
    Console.Warning("[FRAME-DIAG] Slow frame: %.1f ms (frame %llu)", frame_time, static_cast<unsigned long long>(s_frame_number));
#endif
```

---

## Summary

| # | Change | File | Version | Fixes |
|---|---|---|---|---|
| 1 | Ask once, retry later instead of waiting (PS2) | `CDVD.cpp` | v1 | The freeze itself |
| 2 | Emulator never reads the drive directly | `CDVDdiscReader.cpp/.h` | v1 | The freeze, and double seeks |
| 3 | Cache-only lookup (`cdvdTryGetSector`) | `CDVDdiscThread.cpp` | v1 | Supports 2 |
| 4 | One "latest request" slot | `CDVDdiscThread.cpp` | v1 | Wasted seeks to stale requests |
| 5 | IO thread: no missed wake-ups, fewer status checks, lock released while reading | `CDVDdiscThread.cpp` | v1 | Up to 250 ms delays, slower reads |
| 6 | Rolling 4 MB readahead and keeping the drive spinning | `CDVDdiscThread.cpp` | v2 | 1.27 s spin-up stalls in streaming games |
| 7 | Ask once, retry later instead of waiting (PS1) | `Ps1CD.cpp` | v2 | PS1 disc freezes |
| 8 | Clear pop-up when the BIOS can't start an encrypted program | `CDVD.cpp` | v2.1 (official only) | Silent boot hang with HDD/BIOS setups |
| 9 | One READ CD command per block instead of per sector | `Windows/IOCtlSrc.cpp` | v3 (reliquary) | Audio CD skipping, slow PS1 disc reads |
| 10 | PS1 wait diagnostic | `Ps1CD.cpp` | DIAG builds | Checking PS1 disc tests |
| 11 | Play position (subchannel Q) from the track list, not the drive | `CDVDdiscReader.cpp` | v3 | Heavy lag when a CD starts playing |
| - | Diagnostics (`PCSX2_CDVD_DIAG` only) | several | DIAG builds | Finding problems |
