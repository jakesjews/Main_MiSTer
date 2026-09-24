// Apple /// core: Disk III drives on S0-S3, block-storage card on S4/S5.
// Hooked from user_io.cpp like the Mac support code.

#ifndef APPLE3_DISK_H
#define APPLE3_DISK_H

#include <stdint.h>

struct fileTYPE;

char is_apple3();

// user_io_file_mount: validate or convert the image for its slot.
// Returns 0 after closing the file when the image is rejected, 1 otherwise.
int apple3_mount_hook(int index, const char *name, fileTYPE *f, int *writable);
void apple3_unmount(int index);

// SD block service: 1 = transfer done, -1 = nothing to do, 0 = not ours.
int apple3_sd_service(int disk, fileTYPE *f, int op, uint64_t lba, int sz, int ack);

#endif
