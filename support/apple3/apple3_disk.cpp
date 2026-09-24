// Apple /// core disk support. See apple3_disk.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../../file_io.h"
#include "../../user_io.h"
#include "../../spi.h"
#include "../../menu.h"

#include "../a2/iigs_fmt.h"
#include "apple3_woz.h"
#include "apple3_disk.h"

#define SLOTS        6   // S0-S3 Disk III drives, S4-S5 block card
#define FLOPPY_SLOTS 4

static struct {
	int      mode;      // 0 empty, 1 block image, 2 floppy served from RAM
	int      readonly;
	int      native;    // 2: WOZ file, written in place
	int      nib;       // 2: NIB source, stored a whole track at a time
	int      prodos;    // 2: sector source in ProDOS order
	int      pending;   // 2: track with a save in progress, or -1
	int64_t  off;       // payload offset in the source file
	uint8_t *woz;
	size_t   woz_sz;
} g[SLOTS];

static uint8_t buf[UIO_BUFFER_SIZE];

// DOS-order position -> ProDOS-order position within a track.
static const uint8_t DOS_TO_PRODOS[16] = { 0, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 15 };

static int eqi(const char *a, const char *b) { return a && b && !strcasecmp(a, b); }

char is_apple3()
{
	return !strcasecmp(user_io_get_core_name(0), "Apple-III") ||
	       !strcasecmp(user_io_get_core_name(1), "Apple-III");
}

void apple3_unmount(int index)
{
	if (index < 0 || index >= SLOTS) return;
	free(g[index].woz);
	memset(&g[index], 0, sizeof(g[index]));
	g[index].pending = -1;
}

static uint8_t *read_all(fileTYPE *f, size_t *out_len)
{
	if (f->size <= 0 || f->size > 64 * 1024 * 1024) return NULL;
	size_t n = (size_t)f->size;
	uint8_t *b = (uint8_t *)malloc(n);
	if (!b) return NULL;
	FileSeek(f, 0, SEEK_SET);
	size_t got = 0;
	while (got < n) {
		int r = FileReadAdv(f, b + got, n - got);
		if (r <= 0) break;
		got += r;
	}
	FileSeek(f, 0, SEEK_SET);
	if (got != n) { free(b); return NULL; }
	*out_len = n;
	return b;
}

// SOS volume directory header: storage type F, 39-byte entries, 12 (early
// SOS) or 13 entries a block. The shared probe knows 13 only.
static int sos_directory(const uint8_t *block)
{
	return (block[4] >> 4) == 0xf && block[4 + 0x1f] == 39 &&
	       (block[4 + 0x20] == 12 || block[4 + 0x20] == 13);
}

static int prodos_order(const uint8_t *pay, const char *ext)
{
	A2SectorOrder order = a2_detect_525_order(pay, A2_525_IMAGE_SIZE);
	if (order != A2_ORDER_UNKNOWN) return order == A2_ORDER_PRODOS;
	if (sos_directory(pay + 2 * 512)) return 1;
	uint8_t block[512];
	memcpy(block, pay + 11 * 256, 256);        // block 2 of a DOS-order image
	memcpy(block + 256, pay + 10 * 256, 256);
	if (sos_directory(block)) return 0;
	return eqi(ext, "po");
}

static const char *mount_block(int index, fileTYPE *f, const uint8_t *head, const char *ext,
                               const TwoMG *m, int dc42, int locked, int *writable)
{
	if (!memcmp(head, "WOZ", 3) || f->size == A2_NIB_IMAGE_SIZE) return "Block device needs a ProDOS block image, not a floppy.";
	if (m && m->format != 1) return "This 2MG is not a ProDOS-order block image.";
	if (!m && !dc42 && (eqi(ext, "dsk") || eqi(ext, "do"))) return "Use a ProDOS-order PO/HDV image in a block-device slot.";
	int64_t off = 0, payload = f->size;
	if (m) { off = m->data_offset; payload = m->data_len; }
	else if (dc42) { DC42 d; dc42_parse(head, f->size, &d); off = 84; payload = d.data_size; }
	if (payload <= 0 || (payload & 511)) return "Block image length must be a multiple of 512.";
	g[index].mode = 1;
	g[index].off = off;
	g[index].readonly = locked || dc42;   // DC42 checksums would go stale
	f->size = payload;
	*writable = !g[index].readonly;
	return NULL;
}

// A WOZ chunk's data, or NULL if it is missing or runs past the end of the file.
static const uint8_t *woz_chunk(const uint8_t *woz, size_t size, const char *id)
{
	for (size_t pos = 12; pos + 8 <= size;) {
		const uint8_t *p = woz + pos;
		uint32_t n = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
		if (n > size - pos - 8) return NULL;
		if (!memcmp(p, id, 4)) return p + 8;
		pos += 8 + n;
	}
	return NULL;
}

static const char *mount_floppy(int index, fileTYPE *f, const uint8_t *head, const char *ext,
                                const TwoMG *m, int dc42, int locked, int *writable)
{
	if (!memcmp(head, "WOZ", 3) || eqi(ext, "woz")) {
		size_t n = 0;
		uint8_t *woz = read_all(f, &n);
		const uint8_t *info = woz && n >= 12 && !memcmp(woz, "WOZ", 3) && (woz[3] == '1' || woz[3] == '2') ?
		                      woz_chunk(woz, n, "INFO") : NULL;
		if (!info || info + 3 > woz + n) { free(woz); return "Invalid WOZ image."; }
		if (info[1] != 1) { free(woz); return "Disk III needs a 5.25\" WOZ."; }
		g[index].mode = 2;
		g[index].native = 1;
		g[index].woz = woz;
		g[index].woz_sz = n;
		// Only a WOZ2 of bitstream tracks is written, and never one marked write protected.
		g[index].readonly = locked || woz[3] != '2' || info[2] || woz_chunk(woz, n, "FLUX");
		f->size = n;
		*writable = !g[index].readonly;
		return NULL;
	}

	size_t raw_len = 0;
	uint8_t *raw = read_all(f, &raw_len);
	if (!raw) return "Could not read the disk image.";
	const uint8_t *pay = raw;
	size_t pay_len = raw_len;
	int prodos = 0, nib = 0, volume = 0;
	if (m) {
		pay = raw + m->data_offset;
		pay_len = m->data_len;
		prodos = m->format == 1;
		nib = m->format == 2;
		if (m->flags & 0x100) volume = m->flags & 255;
	} else if (dc42) {
		DC42 d;
		dc42_parse(raw, raw_len, &d);
		pay = raw + 84;
		pay_len = d.data_size;
		prodos = 1;
	} else if (raw_len == A2_NIB_IMAGE_SIZE) {
		nib = 1;
	} else if (raw_len == A2_525_IMAGE_SIZE) {
		prodos = prodos_order(raw, ext);
	}

	size_t cap = 512 * 1024, n = 0;
	uint8_t *woz = (uint8_t *)malloc(cap);
	if (woz && nib && pay_len == A2_NIB_IMAGE_SIZE) {
		n = apple3_nib_to_woz(woz, cap, pay);
	} else if (woz && !nib && pay_len == A2_525_IMAGE_SIZE) {
		static uint8_t dsk[A2_525_IMAGE_SIZE];
		if (prodos) a2_prodos_to_dos(dsk, pay);
		else memcpy(dsk, pay, sizeof(dsk));
		// A sector image has no address fields. A DOS 3.3 disk's own volume
		// number goes there, because the DOS it saved asks for it.
		if (!volume) volume = apple3_dos33_volume(dsk);
		n = apple3_dsk_to_woz(woz, cap, dsk, apple3_sos_interp_encrypted(dsk), volume ? volume : 254);
	}
	int64_t off = pay - raw;
	free(raw);
	if (!n) { free(woz); return "Disk III needs a 140K DSK/DO/PO, a NIB or a WOZ image."; }
	g[index].mode = 2;
	g[index].nib = nib;
	g[index].prodos = prodos;
	g[index].off = off;
	g[index].woz = woz;
	g[index].woz_sz = n;
	g[index].readonly = locked || dc42;
	f->size = n;
	*writable = !g[index].readonly;
	return NULL;
}

int apple3_mount_hook(int index, const char *name, fileTYPE *f, int *writable)
{
	if (!is_apple3() || index < 0 || index >= SLOTS) return 1;
	apple3_unmount(index);

	uint8_t head[128] = {};
	size_t hn = f->size < (int64_t)sizeof(head) ? (size_t)f->size : sizeof(head);
	FileSeek(f, 0, SEEK_SET);
	FileReadAdv(f, head, hn);
	FileSeek(f, 0, SEEK_SET);
	const char *dot = strrchr(name, '.');
	const char *ext = dot ? dot + 1 : "";
	TwoMG m;
	const int is_2mg = twomg_parse(head, f->size, &m);
	const int dc42 = dc42_probe(head, f->size);
	const int locked = f->zip || !FileCanWrite(name) || (is_2mg && m.write_protected);

	const char *msg;
	if ((!memcmp(head, "2IMG", 4) && !is_2mg) || (is_2mg && m.format > 2) ||
	    ((eqi(ext, "2mg") || eqi(ext, "2img")) && !is_2mg))
		msg = "Invalid or unsupported 2MG header.";
	else if (index >= FLOPPY_SLOTS)
		msg = mount_block(index, f, head, ext, is_2mg ? &m : NULL, dc42, locked, writable);
	else
		msg = mount_floppy(index, f, head, ext, is_2mg ? &m : NULL, dc42, locked, writable);
	if (!msg) {
		printf("Apple ///: slot %d %s, %lld bytes, %s\n", index,
		       g[index].mode == 1 ? "block image" : g[index].native ? "WOZ" : "converted to WOZ",
		       (long long)f->size, *writable ? "read-write" : "read-only");
		return 1;
	}
	apple3_unmount(index);
	printf("Apple ///: slot %d rejected: %s\n", index, msg);
	InfoMessage(msg, 5000, "Apple ///");
	FileClose(f);
	return 0;
}

// Store the track whose save has finished. Only sectors that verify replace
// the source's, in one write per track: the image is opened O_SYNC, and
// sixteen separate writes kept the drive waiting long enough to break SOS's
// formatter. A NIB track is rewritten whole, so all sixteen must verify.
static void store_pending(int disk, fileTYPE *f)
{
	int t = g[disk].pending;
	g[disk].pending = -1;
	if (t < 0 || g[disk].readonly || !g[disk].woz) return;
	uint8_t trk[A2_TRACK_SIZE], cur[A2_TRACK_SIZE], volumes[16];
	uint16_t good = apple3_verify_track(g[disk].woz, g[disk].woz_sz, t, trk, volumes);
	int failed = 0;
	if (g[disk].nib) {
		if (good == 0xffff) {
			static uint8_t nibtrk[A2_NIB_TRACK_SIZE];
			apple3_nib_track(nibtrk, trk, t, volumes);
			failed = !FileSeek(f, g[disk].off + (int64_t)t * A2_NIB_TRACK_SIZE, SEEK_SET) ||
			         FileWriteAdv(f, nibtrk, A2_NIB_TRACK_SIZE) != A2_NIB_TRACK_SIZE;
		}
	} else {
		int64_t base = g[disk].off + (int64_t)t * A2_TRACK_SIZE;
		failed = !FileSeek(f, base, SEEK_SET) || FileReadAdv(f, cur, A2_TRACK_SIZE) != A2_TRACK_SIZE;
		int changed = 0;
		for (int s = 0; s < 16 && !failed; s++) {
			int pos = (g[disk].prodos ? DOS_TO_PRODOS[s] : s) * A2_SECTOR_SIZE;
			if (!(good & (1 << s)) || !memcmp(cur + pos, trk + s * A2_SECTOR_SIZE, A2_SECTOR_SIZE)) continue;
			memcpy(cur + pos, trk + s * A2_SECTOR_SIZE, A2_SECTOR_SIZE);
			changed = 1;
		}
		if (changed && !failed)
			failed = !FileSeek(f, base, SEEK_SET) || FileWriteAdv(f, cur, A2_TRACK_SIZE) != A2_TRACK_SIZE;
	}
	if (failed) InfoMessage("Could not save to the disk image.", 5000, "Disk write error");
	else if (good != 0xffff) {
		static char msg[64];
		snprintf(msg, sizeof(msg), "Track %d: %d unreadable sector(s) not saved.", t, 16 - __builtin_popcount(good));
		InfoMessage(msg, 5000, "Disk write warning");
	}
}

// The drive saves a track one block at a time; decode it once the last arrives.
static void converted_write(int disk, fileTYPE *f, uint64_t lba, const uint8_t *block)
{
	memcpy(g[disk].woz + lba * 512, block, 512);
	int t = a2_woz_track_for_lba(g[disk].woz, g[disk].woz_sz, (uint32_t)lba);
	if (t < 0) return;
	if (g[disk].pending >= 0 && g[disk].pending != t) store_pending(disk, f);
	g[disk].pending = t;
	if (a2_woz_track_for_lba(g[disk].woz, g[disk].woz_sz, (uint32_t)lba + 1) != t) store_pending(disk, f);
}

// Bits inside existing track allocations change; the chunk directory, other
// chunks and the file length do not. A zero CRC means "not calculated".
static void native_write(int disk, fileTYPE *f, uint64_t lba, int sz)
{
	for (int i = 0; i < sz / 512; ++i)
		if (a2_woz_track_for_lba(g[disk].woz, g[disk].woz_sz, (uint32_t)lba + i) < 0) return;
	uint8_t zero[4] = {};
	if (memcmp(g[disk].woz + 8, zero, 4)) {
		if (!FileSeek(f, 8, SEEK_SET) || FileWriteAdv(f, zero, 4) != 4) return;
		memset(g[disk].woz + 8, 0, 4);
	}
	uint64_t off = lba * 512;
	if (FileSeek(f, off, SEEK_SET) && FileWriteAdv(f, buf, sz) == sz) memcpy(g[disk].woz + off, buf, sz);
	else InfoMessage("Could not save to the WOZ image.", 5000, "Disk write error");
}

static void read_blocks(int disk, fileTYPE *f, uint64_t lba, int sz, int ack)
{
	if (g[disk].pending >= 0) store_pending(disk, f);   // the drive has moved on
	if (sz > (int)sizeof(buf)) sz = sizeof(buf);
	memset(buf, 0, sz);
	uint64_t off = lba * 512;
	if (lba <= UINT64_MAX / 512 && off < (uint64_t)f->size) {
		size_t n = (uint64_t)f->size - off < (unsigned)sz ? (size_t)((uint64_t)f->size - off) : (size_t)sz;
		if (g[disk].mode == 2) {
			if (g[disk].woz && off + n <= g[disk].woz_sz) memcpy(buf, g[disk].woz + off, n);
		} else if (FileSeek(f, off + g[disk].off, SEEK_SET)) {
			FileReadAdv(f, buf, n);
		}
	}
	EnableIO();
	spi_w(UIO_SECTOR_RD | ack);
	spi_block_write(buf, user_io_get_width(), sz);
	DisableIO();
}

static void write_blocks(int disk, fileTYPE *f, uint64_t lba, int sz, int ack)
{
	if (sz <= 0 || sz > (int)sizeof(buf) || (sz & 511)) return;
	EnableIO();
	spi_w(UIO_SECTOR_WR | ack);
	spi_block_read(buf, user_io_get_width(), sz);
	DisableIO();
	// A protected image acknowledges the write and drops it.
	if (g[disk].readonly || lba > UINT64_MAX / 512) return;
	uint64_t off = lba * 512;
	if (off > (uint64_t)f->size || (unsigned)sz > (uint64_t)f->size - off) return;
	if (g[disk].mode == 1) {
		if (FileSeek(f, off + g[disk].off, SEEK_SET)) FileWriteAdv(f, buf, sz);
	} else if (g[disk].native) {
		native_write(disk, f, lba, sz);
	} else {
		for (int i = 0; i < sz; i += 512) converted_write(disk, f, lba + i / 512, buf + i);
	}
}

int apple3_sd_service(int disk, fileTYPE *f, int op, uint64_t lba, int sz, int ack)
{
	if (!is_apple3() || disk < 0 || disk >= SLOTS || !g[disk].mode) return 0;
	if (!f->opened()) { apple3_unmount(disk); return 0; }
	if (op == 2) write_blocks(disk, f, lba, sz, ack);
	else if (op & 1) read_blocks(disk, f, lba, sz, ack);
	else return -1;
	return 1;
}
