// Apple /// 5.25" WOZ images: synchronized track layout, SOS protection key,
// strict sector decoding for write-back. No MiSTer dependencies.

#ifndef APPLE3_WOZ_H
#define APPLE3_WOZ_H

#include <stdint.h>
#include <stddef.h>

// DOS-order 140K image -> WOZ2 with the Disk /// formatter's synchronized
// tracks. key adds the SOS protection key to the address fields of tracks 9-16.
// Returns bytes written, 0 if cap is too small.
size_t apple3_dsk_to_woz(uint8_t *woz, size_t cap, const uint8_t *dsk, int key, uint8_t volume);

// 232960-byte NIB -> WOZ2, keeping every nibble; FF gaps become sync words.
size_t apple3_nib_to_woz(uint8_t *woz, size_t cap, const uint8_t *nib);

// Decode one track of a WOZ built above into trk (4096 bytes, DOS order).
// Returns the mask of DOS-order positions whose sector verified completely;
// volumes (16 bytes) receives their address-field volume numbers.
uint16_t apple3_verify_track(const uint8_t *woz, size_t size, int track, uint8_t *trk, uint8_t *volumes);

// One 6656-byte track in the standard NIB layout from a DOS-order track, with
// each sector's address-field volume (16 bytes, by sector number).
void apple3_nib_track(uint8_t *nib, const uint8_t *trk, int track, const uint8_t *volumes);

// Volume number in the VTOC of a DOS-order DOS 3.3 image, or 0.
uint8_t apple3_dos33_volume(const uint8_t *dsk);

// SOS interpreter cipher (its own inverse) over code loaded at `load`.
void apple3_sos_crypt(uint8_t *code, size_t len, uint16_t load);
// 1 if the DOS-order image is a SOS boot volume whose SOS.INTERP is encrypted.
int apple3_sos_interp_encrypted(const uint8_t *dsk);

#endif
