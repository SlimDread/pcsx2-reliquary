// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CDVDdiscReader.h"
#include "CDVD/CDVD.h"
#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

const u32 sectors_per_read = 16;

static_assert(sectors_per_read > 1 && !(sectors_per_read & (sectors_per_read - 1)),
			  "sectors_per_read must by a power of 2");

struct SectorInfo
{
	u32 lsn;
	// Sectors are read in blocks, not individually
	u8 data[2352 * sectors_per_read];
};

u32 g_last_sector_block_lsn;

static std::thread s_thread;

// s_notify_lock guards the pending request. Only the newest request matters: like a real drive,
// a new seek supersedes whatever was asked for before, so we don't seek to stale sectors first.
static std::mutex s_notify_lock;
static std::condition_variable s_notify_cv;
static u32 s_request_lsn;
static bool s_request_pending = false;
// Block whose requested read failed, so pollers can stop waiting on it
static std::atomic<u32> s_failed_lsn{std::numeric_limits<u32>::max()};
// Block the game is currently reading (updated even on cache hits) so readahead can follow it
static std::atomic<u32> s_play_lsn{0};
static std::mutex s_cache_lock;

static std::atomic<bool> cdvd_is_open;

//bits: 12 would use 1<<12 entries, or 4096*16 sectors ~ 128MB
#define CACHE_SIZE 12

static constexpr u32 CacheSize = 1U << CACHE_SIZE;
static SectorInfo Cache[CacheSize];

static u32 cdvdSectorHash(u32 lsn)
{
	u32 t = 0;

	int i = 32;
	u32 m = CacheSize - 1;

	while (i >= 0)
	{
		t ^= lsn & m;
		lsn >>= CACHE_SIZE;
		i -= CACHE_SIZE;
	}

	return t & m;
}

static void cdvdCacheUpdate(u32 lsn, u8* data)
{
	std::lock_guard<std::mutex> guard(s_cache_lock);
	u32 entry = cdvdSectorHash(lsn);

	memcpy(Cache[entry].data, data, 2352 * sectors_per_read);
	Cache[entry].lsn = lsn;
}

static bool cdvdCacheCheck(u32 lsn)
{
	std::lock_guard<std::mutex> guard(s_cache_lock);
	u32 entry = cdvdSectorHash(lsn);

	return Cache[entry].lsn == lsn;
}

static bool cdvdCacheFetch(u32 lsn, u8* data)
{
	std::lock_guard<std::mutex> guard(s_cache_lock);
	u32 entry = cdvdSectorHash(lsn);

	if (Cache[entry].lsn == lsn)
	{
		memcpy(data, Cache[entry].data, 2352 * sectors_per_read);
		return true;
	}
	//printf("NOT IN CACHE\n");
	return false;
}

static void cdvdCacheReset()
{
	std::lock_guard<std::mutex> guard(s_cache_lock);
	for (u32 i = 0; i < CacheSize; i++)
	{
		Cache[i].lsn = std::numeric_limits<u32>::max();
	}
}

static bool cdvdReadBlockOfSectors(u32 sector, u8* data)
{
	u32 count = std::min(sectors_per_read, src->GetSectorCount() - sector);
	const s32 media = src->GetMediaType();

	// TODO: Is it really necessary to retry if it fails? I'm not sure the
	// second time is really going to be any better.
	for (int tries = 0; tries < 2; ++tries)
	{
		if (media >= 0)
		{
			if (src->ReadSectors2048(sector, count, data))
				return true;
		}
		else
		{
			if (src->ReadSectors2352(sector, count, data))
				return true;
		}
	}
	return false;
}

static void cdvdCallNewDiscCB()
{
	weAreInNewDiskCB = true;
	newDiscCB();
	weAreInNewDiskCB = false;
}

static bool cdvdUpdateDiscStatus()
{
	bool ready = src->DiscReady();

	if (!ready)
	{
		if (!disc_has_changed)
		{
			disc_has_changed = true;
			curDiskType = CDVD_TYPE_NODISC;
			curTrayStatus = CDVD_TRAY_OPEN;
			cdvdCallNewDiscCB();
		}
	}
	else
	{
		if (disc_has_changed)
		{
			curDiskType = CDVD_TYPE_NODISC;
			curTrayStatus = CDVD_TRAY_CLOSE;

			disc_has_changed = false;
			cdvdRefreshData();

			{
				std::lock_guard<std::mutex> request_guard(s_notify_lock);
				s_request_pending = false;
			}

			cdvdCallNewDiscCB();
		}
	}
	return !ready;
}

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
#ifdef PCSX2_CDVD_DIAG
			const auto diag_start = clock::now();
#endif
			if (cdvdReadBlockOfSectors(request_lsn, buffer))
				cdvdCacheUpdate(request_lsn, buffer);
			else
				read_ok = false;
#ifdef PCSX2_CDVD_DIAG
			const double diag_ms = std::chrono::duration<double, std::milli>(clock::now() - diag_start).count();
			if (diag_ms >= 30.0 || !read_ok)
				Console.WriteLn("[CDVD-DIAG] Drive read of sector %u (%s) took %.1f ms%s", request_lsn,
					handling_request ? "request" : (keepalive ? "keepalive" : "prefetch"), diag_ms, read_ok ? "" : " FAILED");
#endif
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

void cdvdStartThread()
{
	if (cdvd_is_open == false)
	{
		cdvd_is_open = true;
		s_thread = std::thread(cdvdThread);
	}

	cdvdCacheReset();
}

void cdvdStopThread()
{
	cdvd_is_open = false;
	s_notify_cv.notify_one();
	if (s_thread.joinable())
		s_thread.join();
}

static u8* cdvdSectorData(u8* buffer, u32 sector, u32 sector_block, s32 mode);

void cdvdRequestSector(u32 sector, s32 mode)
{
	if (sector >= src->GetSectorCount())
		return;

	// Align to cache block
	sector &= ~(sectors_per_read - 1);

	// Track the game's position even when it's cached, and wake the IO thread so readahead keeps up.
	if (s_play_lsn.exchange(sector, std::memory_order_relaxed) != sector)
		s_notify_cv.notify_one();

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
		u32 offset = 2048 * (sector - sector_block);
		return buffer + offset;
	}

	u32 offset = 2352 * (sector - sector_block);
	u8* data = buffer + offset;

	switch (mode)
	{
		case CDVD_MODE_2048:
			// Data location depends on CD mode
			return (data[15] & 3) == 2 ? data + 24 : data + 16;
		case CDVD_MODE_2328:
			return data + 24;
		case CDVD_MODE_2340:
			return data + 12;
	}
	return data;
}

s32 cdvdDirectReadSector(u32 sector, s32 mode, u8* buffer)
{
	static u8 data[2352 * sectors_per_read];

	if (src == nullptr)
		return -1;

	if (sector >= src->GetSectorCount())
		return -1;

	// Align to cache block
	u32 sector_block = sector & ~(sectors_per_read - 1);

	if (!cdvdCacheFetch(sector_block, data))
	{
		if (cdvdReadBlockOfSectors(sector_block, data))
			cdvdCacheUpdate(sector_block, data);
	}

	if (src->GetMediaType() >= 0)
	{
		u32 offset = 2048 * (sector - sector_block);
		memcpy(buffer, data + offset, 2048);
		return 0;
	}

	u32 offset = 2352 * (sector - sector_block);
	u8* bfr = data + offset;

	switch (mode)
	{
		case CDVD_MODE_2048:
			// Data location depends on CD mode
			std::memcpy(buffer, (bfr[15] & 3) == 2 ? bfr + 24 : bfr + 16, 2048);
			return 0;
		case CDVD_MODE_2328:
			memcpy(buffer, bfr + 24, 2328);
			return 0;
		case CDVD_MODE_2340:
			memcpy(buffer, bfr + 12, 2340);
			return 0;
		default:
			memcpy(buffer, bfr, 2352);
			return 0;
	}
}

static s32 cdvdGetMediaType()
{
	return src->GetMediaType();
}

void cdvdRefreshData()
{
	const char* diskTypeName = "Unknown";

	//read TOC from device
	cdvdParseTOC();

	if ((etrack == 0) || (strack > etrack))
	{
		curDiskType = CDVD_TYPE_NODISC;
	}
	else
	{
		s32 mt = cdvdGetMediaType();

		if (mt < 0)
			curDiskType = CDVD_TYPE_DETCTCD;
		else if (mt == 0)
			curDiskType = CDVD_TYPE_DETCTDVDS;
		else
			curDiskType = CDVD_TYPE_DETCTDVDD;
	}

	curTrayStatus = CDVD_TRAY_CLOSE;

	switch (curDiskType)
	{
		case CDVD_TYPE_DETCTDVDD:
			diskTypeName = "Double-Layer DVD";
			break;
		case CDVD_TYPE_DETCTDVDS:
			diskTypeName = "Single-Layer DVD";
			break;
		case CDVD_TYPE_DETCTCD:
			diskTypeName = "CD-ROM";
			break;
		case CDVD_TYPE_NODISC:
			diskTypeName = "No Disc";
			break;
	}

	printf(" * CDVD: Disk Type: %s\n", diskTypeName);

	cdvdCacheReset();
}
