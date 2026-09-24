// Apple /// 5.25" WOZ images. See apple3_woz.h.

#include <string.h>

#include "../a2/iigs_fmt.h"
#include "apple3_woz.h"

#define TRACKS      35
#define DSK_BLOCKS  13     // 512-byte blocks per converted sector-image track
#define NIB_BLOCKS  17     // per NIB track once the sync words are restored
#define GAP3_SYNC   23     // 51,424 cells a track: the formatter closes it at 22 sync nibbles
#define CELL_NS     3875   // INFO timing 31
#define NIB_CAP     32768

// Address-field sector -> DOS-order file position.
static const uint8_t DOS_POS[16] = { 0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15 };
static const uint8_t addr_prolog[] = { 0xd5, 0xaa, 0x96 }, addr_epilog[] = { 0xde, 0xaa, 0xeb };
static const uint8_t data_prolog[] = { 0xd5, 0xaa, 0xad }, data_epilog[] = { 0xde, 0xaa, 0xeb };

// SOS reads one address-field volume on each of tracks 9-16 and, when the
// tracks are synchronized and the values differ, uses them as the key.
static const uint8_t sos_key[8]        = { 0xb4, 0xc1, 0xe4, 0xf3, 0x9b, 0xbd, 0xbd, 0x7c };
static const uint8_t sos_key_sector[8] = { 2, 14, 10, 6, 2, 14, 10, 6 };

// 6-and-2 GCR: the 64 disk nibbles a data field may use.
static const uint8_t gcr62[64] = {
	0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6, 0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
	0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
	0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
	0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
};

static uint16_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// Header, INFO, TMAP and TRKS directory for 35 tracks of `blocks` blocks each.
static size_t woz_header(uint8_t *woz, size_t cap, int blocks, int synchronized)
{
	size_t size = 1536 + TRACKS * blocks * 512;
	if (cap < size) return 0;
	memset(woz, 0, size);
	memcpy(woz, "WOZ2\xff\x0a\x0d\x0a", 8);
	memcpy(woz + 12, "INFO", 4);
	wr32(woz + 16, 60);
	uint8_t *info = woz + 20;
	info[0] = 2;
	info[1] = 1;
	info[3] = synchronized;
	info[4] = 1;
	memset(info + 5, ' ', 32);
	memcpy(info + 5, "MiSTer Apple ///", 16);
	info[37] = 1;
	info[39] = synchronized ? CELL_NS / 125 : 32;
	wr16(info + 44, blocks);
	memcpy(woz + 80, "TMAP", 4);
	wr32(woz + 84, 160);
	memset(woz + 88, 255, 160);
	for (int t = 0; t < TRACKS; ++t) {
		for (int d = -1; d <= 1; ++d) if (t * 4 + d >= 0) woz[88 + t * 4 + d] = t;
		wr16(woz + 256 + t * 8, 3 + t * blocks);
		wr16(woz + 258 + t * 8, blocks);
	}
	memcpy(woz + 248, "TRKS", 4);
	wr32(woz + 252, size - 256);
	return size;
}

// A sync word is ten bits: FF followed by two zero cells.
static void put_byte(uint8_t *out, unsigned &bit, uint8_t value, int sync = 0)
{
	for (int i = 7; i >= 0; --i, ++bit) out[bit >> 3] |= ((value >> i) & 1) << (7 - (bit & 7));
	if (sync) bit += 2;
}

static void put_44(uint8_t *out, unsigned &bit, uint8_t value)
{
	put_byte(out, bit, (value >> 1) | 0xaa);
	put_byte(out, bit, value | 0xaa);
}

// A 256-byte sector as its 343 data-field nibbles: 342 and the checksum.
static void nibbilize(uint8_t *nibbles, const uint8_t *sector)
{
	uint8_t primary[256], secondary[86] = {}, prev = 0;
	for (int i = 0; i < 256; ++i) {
		primary[i] = sector[i] >> 2;
		secondary[i % 86] |= (((sector[i] & 2) >> 1) | ((sector[i] & 1) << 1)) << (i / 86 * 2);
	}
	for (int i = 0; i < 86; ++i) { nibbles[i] = gcr62[secondary[i] ^ prev]; prev = secondary[i]; }
	for (int i = 0; i < 256; ++i) { nibbles[86 + i] = gcr62[primary[i] ^ prev]; prev = primary[i]; }
	nibbles[342] = gcr62[prev];
}

// The reverse. Returns 0 on a nibble outside the table or a bad checksum.
static int denibbilize(uint8_t *sector, const uint8_t *nibbles)
{
	uint8_t primary[256], secondary[86], x = 0;
	for (int k = 0; k < 343; ++k) {
		const uint8_t *p = (const uint8_t *)memchr(gcr62, nibbles[k], sizeof(gcr62));
		if (!p) return 0;
		x ^= (uint8_t)(p - gcr62);
		if (k < 86) secondary[k] = x;
		else if (k < 342) primary[k - 86] = x;
	}
	if (x) return 0;
	for (int k = 0; k < 256; ++k) {
		uint8_t pair = (secondary[k % 86] >> (k / 86 * 2)) & 3;
		sector[k] = (primary[k] << 2) | ((pair & 1) << 1) | (pair >> 1);
	}
	return 1;
}

size_t apple3_dsk_to_woz(uint8_t *woz, size_t cap, const uint8_t *dsk, int key, uint8_t volume)
{
	size_t size = woz_header(woz, cap, DSK_BLOCKS, 1);
	if (!size) return 0;
	for (int t = 0; t < TRACKS; ++t) {
		uint8_t *out = woz + (3 + t * DSK_BLOCKS) * 512;
		unsigned bit = 0;
		for (int i = 0; i < 16; ++i) put_byte(out, bit, 255, 1);
		for (int sector = 0; sector < 16; ++sector) {
			uint8_t vol = key && t >= 9 && t <= 16 && sector == sos_key_sector[t - 9] ? sos_key[t - 9] : volume;
			for (uint8_t b : addr_prolog) put_byte(out, bit, b);
			put_44(out, bit, vol);
			put_44(out, bit, t);
			put_44(out, bit, sector);
			put_44(out, bit, vol ^ t ^ sector);
			for (uint8_t b : addr_epilog) put_byte(out, bit, b);
			for (int i = 0; i < 7; ++i) put_byte(out, bit, 255, 1);
			for (uint8_t b : data_prolog) put_byte(out, bit, b);
			uint8_t data[343];
			nibbilize(data, dsk + t * A2_TRACK_SIZE + DOS_POS[sector] * A2_SECTOR_SIZE);
			for (uint8_t b : data) put_byte(out, bit, b);
			for (uint8_t b : data_epilog) put_byte(out, bit, b);
			for (int i = 0; i < GAP3_SYNC; ++i) put_byte(out, bit, 255, 1);
		}
		wr32(woz + 260 + t * 8, bit);
		// Synchronized tracks: SOS's key sector on the next track passes the head
		// 56.5 ms after the previous one, which its 48 ms one-track seek relies on.
		uint8_t copy[DSK_BLOCKS * 512];
		const unsigned bytes = bit / 8, step = (bit * 3 / 4 - 56500000 / CELL_NS) / 8;
		memcpy(copy, out, bytes);
		unsigned rotation = (t * step) % bytes;
		for (unsigned i = 0; i < bytes; ++i) out[i] = copy[(i + rotation) % bytes];
	}
	wr32(woz + 8, woz_crc32(woz + 12, size - 12));
	return size;
}

size_t apple3_nib_to_woz(uint8_t *woz, size_t cap, const uint8_t *nib)
{
	size_t size = woz_header(woz, cap, NIB_BLOCKS, 0);
	if (!size) return 0;
	for (int t = 0; t < TRACKS; ++t) {
		const uint8_t *src = nib + t * A2_NIB_TRACK_SIZE;
		uint8_t *out = woz + (3 + t * NIB_BLOCKS) * 512;
		unsigned bit = 0;
		int field = 0;
		for (int i = 0; i < A2_NIB_TRACK_SIZE; ++i) {
			// FF is a sync word only in a gap, never inside an address or data field.
			if (!field && i + 2 < A2_NIB_TRACK_SIZE && src[i] == 0xd5 && src[i + 1] == 0xaa) {
				if (src[i + 2] == 0x96) field = 14;
				if (src[i + 2] == 0xad) field = 349;
			}
			put_byte(out, bit, src[i], !field && src[i] == 255);
			if (field) --field;
		}
		wr32(woz + 260 + t * 8, bit);
	}
	wr32(woz + 8, woz_crc32(woz + 12, size - 12));
	return size;
}

// The nibbles a Disk II shift register produces from one track, read twice so
// a sector across the splice is seen whole. Returns the count.
static int track_nibbles(const uint8_t *woz, size_t size, int track, uint8_t *nib)
{
	if (size < 1536 || memcmp(woz, "WOZ2", 4) || track < 0 || track >= TRACKS) return 0;
	const uint8_t *trks = NULL;
	for (size_t pos = 12; size - pos >= 8;) {
		uint32_t n = rd32(woz + pos + 4);
		if (n > size - pos - 8) return 0;
		if (!memcmp(woz + pos, "TRKS", 4)) { if (n < 1280) return 0; trks = woz + pos + 8; break; }
		pos += 8 + n;
	}
	if (!trks) return 0;
	const uint8_t *e = trks + track * 8;
	size_t start = (size_t)rd16(e) * 512;
	uint32_t bits = rd32(e + 4);
	if (!bits || bits > 131071 || start + (bits + 7) / 8 > size) return 0;
	uint8_t shift = 0;
	int count = 0;
	for (uint32_t i = 0; i < bits * 2 && count < NIB_CAP; ++i) {
		uint32_t pos = i % bits;
		shift = (shift << 1) | ((woz[start + pos / 8] >> (7 - (pos & 7))) & 1);
		if (shift & 0x80) { nib[count++] = shift; shift = 0; }
	}
	return count;
}

static uint8_t odd_even(uint8_t a, uint8_t b) { return ((a << 1) & 0xaa) | (b & 0x55); }

// A sector counts only when its address checksum, track number, the data field
// inside the following gap, every GCR code, the data checksum and both epilogs
// verify, and both revolutions agree.
static uint16_t verify_sectors(const uint8_t *n, int len, int track, uint8_t *dt, uint8_t *volumes)
{
	uint16_t good = 0, conflict = 0;
	if (volumes) memset(volumes, 254, 16);
	for (int i = 0; i + 13 <= len; ++i) {
		if (n[i] != 0xd5 || n[i + 1] != 0xaa || n[i + 2] != 0x96) continue;
		uint8_t vol = odd_even(n[i + 3], n[i + 4]), trk = odd_even(n[i + 5], n[i + 6]);
		uint8_t sec = odd_even(n[i + 7], n[i + 8]), chk = odd_even(n[i + 9], n[i + 10]);
		if ((vol ^ trk ^ sec) != chk || trk != track || sec >= 16) continue;
		if (n[i + 11] != 0xde || n[i + 12] != 0xaa) continue;
		int d = -1;
		for (int j = i + 13; j < i + 13 + 32 && j + 3 <= len; ++j) {
			if (n[j] != 0xd5 || n[j + 1] != 0xaa) continue;
			if (n[j + 2] == 0xad) d = j + 3;
			break;
		}
		if (d < 0 || d + 345 > len) continue;
		uint8_t sector[A2_SECTOR_SIZE];
		if (!denibbilize(sector, n + d) || n[d + 343] != 0xde || n[d + 344] != 0xaa) continue;
		int pos = DOS_POS[sec];
		uint16_t bit = 1U << pos;
		if ((good & bit) && memcmp(dt + pos * A2_SECTOR_SIZE, sector, A2_SECTOR_SIZE)) conflict |= bit;
		if (!(good & bit)) {
			memcpy(dt + pos * A2_SECTOR_SIZE, sector, A2_SECTOR_SIZE);
			good |= bit;
			if (volumes) volumes[sec] = vol;
		}
	}
	return good & ~conflict;
}

uint16_t apple3_verify_track(const uint8_t *woz, size_t size, int track, uint8_t *trk, uint8_t *volumes)
{
	static uint8_t nib[NIB_CAP];
	int count = track_nibbles(woz, size, track, nib);
	return count ? verify_sectors(nib, count, track, trk, volumes) : 0;
}

// Sixteen 416-byte sectors in DOS interleave, each after 48 gap bytes.
void apple3_nib_track(uint8_t *nib, const uint8_t *trk, int track, const uint8_t *volumes)
{
	for (int position = 0; position < 16; ++position) {
		const uint8_t sector = DOS_POS[position], vol = volumes[sector];
		const uint8_t address[4] = { vol, (uint8_t)track, sector, (uint8_t)(vol ^ track ^ sector) };
		uint8_t *o = nib + position * 416;
		memset(o, 0xff, 48);
		o += 48;
		for (uint8_t b : addr_prolog) *o++ = b;
		for (uint8_t v : address) { *o++ = (v >> 1) | 0xaa; *o++ = v | 0xaa; }
		for (uint8_t b : addr_epilog) *o++ = b;
		memset(o, 0xff, 5);
		o += 5;
		for (uint8_t b : data_prolog) *o++ = b;
		nibbilize(o, trk + DOS_POS[sector] * A2_SECTOR_SIZE);
		o += 343;
		for (uint8_t b : data_epilog) *o++ = b;
	}
}

uint8_t apple3_dos33_volume(const uint8_t *dsk)
{
	const uint8_t *vtoc = dsk + 17 * A2_TRACK_SIZE;
	if (vtoc[1] != 17 || vtoc[2] >= 16 || vtoc[3] != 3 || vtoc[0x34] != TRACKS || vtoc[0x35] != 16 ||
	    rd16(vtoc + 0x36) != A2_SECTOR_SIZE)
		return 0;
	return vtoc[6] == 0xff ? 0 : vtoc[6];
}

// SOS's DCLOOP: an XOR stream over the interpreter from load+3, the key
// rotating left one bit per 256-byte page.
void apple3_sos_crypt(uint8_t *code, size_t len, uint16_t load)
{
	uint8_t key[8], prev = 0;
	for (int i = 0; i < 8; ++i) key[i] = sos_key[7 - i];
	unsigned y = (load + 3) & 255;
	for (size_t i = 3; i < len;) {
		unsigned carry = key[0] >> 7;
		for (int x = 7; x >= 0; --x) { unsigned v = (key[x] << 1) | carry; carry = v >> 8; key[x] = v; }
		do {
			uint8_t a = key[(y & 7) ^ 2];
			prev = (uint8_t)(a + prev + key[a & 7]);
			code[i++] ^= prev;
			y = (y + 1) & 255;
		} while (y && i < len);
	}
}

// A sector image has no address fields to copy the key from, so it is rebuilt
// only when SOS.INTERP really is encrypted: a deprotected disk given the key
// stops with SYSTEM FAILURE $06. The test decodes a sample and keeps whichever
// version has the byte statistics of 6502 code.
int apple3_sos_interp_encrypted(const uint8_t *dsk)
{
	static uint8_t po[A2_525_IMAGE_SIZE];
	a2_dos_to_prodos(po, dsk);
	const uint8_t *dir = po + 2 * 512;
	unsigned per_block = dir[0x24], key_block = 0, storage = 0;
	if ((dir[4] >> 4) != 0xf || dir[0x23] != 39 || per_block < 1 || per_block > 13) return 0;
	for (unsigned block = 2, hops = 0; block && block < 280 && hops < 16 && !key_block; ++hops) {
		dir = po + block * 512;
		for (unsigned i = 0; i < per_block && !key_block; ++i) {
			const uint8_t *e = dir + 4 + i * 39;
			if ((e[0] & 15) != 10 || memcmp(e + 1, "SOS.INTERP", 10)) continue;
			storage = e[0] >> 4;
			key_block = rd16(e + 0x11);
		}
		block = rd16(dir + 2);
	}
	if (!key_block || key_block >= 280 || storage < 1 || storage > 2) return 0;
	uint8_t file[2048];
	const uint8_t *index = po + key_block * 512;
	size_t got = 0;
	for (unsigned i = 0; i < sizeof(file) / 512; ++i) {
		unsigned block = storage == 1 ? (i ? 0 : key_block) : (unsigned)(index[i] | index[256 + i] << 8);
		if (!block || block >= 280) break;
		memcpy(file + got, po + block * 512, 512);
		got += 512;
	}
	if (got < 512 || memcmp(file, "SOS NTRP", 8)) return 0;
	size_t code = 10 + rd16(file + 8) + 4;
	if (code + 256 > got) return 0;
	uint16_t load = rd16(file + code - 4);
	size_t len = got - code;
	if (rd16(file + code - 2) < len) len = rd16(file + code - 2);
	if (len < 256) return 0;
	unsigned raw[256] = {}, dec[256] = {};
	for (size_t i = 3; i < len; ++i) ++raw[file[code + i]];
	apple3_sos_crypt(file + code, len, load);
	for (size_t i = 3; i < len; ++i) ++dec[file[code + i]];
	unsigned long raw_sum = 0, dec_sum = 0;
	for (int v = 0; v < 256; ++v) { raw_sum += raw[v] * raw[v]; dec_sum += dec[v] * dec[v]; }
	return dec_sum > 2 * raw_sum;
}
