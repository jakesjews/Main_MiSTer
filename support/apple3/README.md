# Apple /// disk support

The Apple-III core takes WOZ bitstreams on its Disk III drives and raw ProDOS
blocks on its block-storage card. `apple3_disk.cpp` mounts and serves the
images; `apple3_woz.cpp` builds the WOZ tracks. It uses the 2MG, DC42 and
sector-order helpers in `support/a2/iigs_fmt.cpp` and does not change them.

| Slot | Device | Formats |
|---|---|---|
| S0-S3 | Disk III drives 1-4 | WOZ, DSK, DO, PO, NIB, 2MG |
| S4-S5 | Block card drives 1-2 | PO, HDV, 2MG (DC42 read-only) |

Sector images are converted in memory to the track layout Apple's Disk III
formatter produces: synchronized tracks of 51,424 cells read at 3.875 us, so
that SOS's drive-speed check and its copy-protection key sectors behave as on a
real disk. The SOS key is added only when the volume's `SOS.INTERP` is
encrypted. A DOS 3.3 disk's own volume number goes into the address fields.

Writes: a native WOZ2 is updated in place within its existing track
allocations. A sector image is updated one track at a time, after the drive
has written its last block, and only with sectors that decode and verify. A
NIB source is rewritten a whole track at a time. WOZ1, FLUX, DC42, locked 2MG,
archived and read-only images stay read-only. Block images are written in
place behind any 2MG header.
