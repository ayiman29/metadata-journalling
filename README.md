# 1. Header files

```c
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
```

These are standard C / POSIX headers:

* `<errno.h>` – error codes set by system calls
* `<fcntl.h>` – file control flags (`open`, `O_RDWR`, etc.)
* `<stdint.h>` – fixed-width integer types (`uint32_t`, etc.)
* `<stdio.h>` – I/O (`printf`, `perror`)
* `<stdlib.h>` – memory & process control (`malloc`, `exit`)
* `<string.h>` – memory/string utilities (`memcpy`, `strcmp`)
* `<time.h>` – timestamps (`time`)
* `<unistd.h>` – POSIX system calls (`read`, `write`, `lseek`, `close`)

---

# 2. Magic numbers

```c
#define FS_MAGIC 0x56534653U
#define JOURNAL_MAGIC 0x4A524E4CU
```

* `FS_MAGIC` identifies a **valid filesystem**
* `JOURNAL_MAGIC` identifies a **valid journal**

These are written into on-disk structures to detect corruption or wrong files.

---

# 3. Filesystem layout constants

```c
#define BLOCK_SIZE        4096U
#define INODE_SIZE         128U
```

* Each disk block = **4096 bytes**
* Each inode = **128 bytes**

---

```c
#define JOURNAL_BLOCK_IDX    1U
#define JOURNAL_BLOCKS      16U
```

* Journal starts at **block 1**
* Journal spans **16 blocks**

---

```c
#define INODE_BLOCKS         2U
#define DATA_BLOCKS         64U
```

* 2 blocks of inodes
* 64 blocks of file data

---

```c
#define INODE_BMAP_IDX     (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)
#define DATA_BMAP_IDX      (INODE_BMAP_IDX + 1U)
#define INODE_START_IDX    (DATA_BMAP_IDX + 1U)
#define DATA_START_IDX     (INODE_START_IDX + INODE_BLOCKS)
#define TOTAL_BLOCKS       (DATA_START_IDX + DATA_BLOCKS)
```

This defines the **exact disk layout**:

| Block | Purpose      |
| ----- | ------------ |
| 0     | Superblock   |
| 1–16  | Journal      |
| 17    | Inode bitmap |
| 18    | Data bitmap  |
| 19–20 | Inode table  |
| 21–84 | Data blocks  |

---

```c
#define DIRECT_POINTERS     8U
#define NAME_LEN           28
#define DEFAULT_IMAGE "vsfs.img"
```

* Each inode has **8 direct data pointers**
* Directory entry names max **28 bytes**
* Default disk image name

---

# 4. Journal constants

```c
#define JOURNAL_SIZE (JOURNAL_BLOCKS * BLOCK_SIZE)
```

Total journal size in bytes.

---

```c
#define REC_DATA   1
#define REC_COMMIT 2
```

Journal record types:

* `REC_DATA` → block write
* `REC_COMMIT` → transaction boundary

---

# 5. Inode types

```c
#define INODE_FREE 0
#define INODE_FILE 1
#define INODE_DIR  2
```

Defines what an inode represents.

---

# 6. On-disk data structures

---

## Superblock

```c
struct superblock {
```

Holds global filesystem metadata.

```c
    uint32_t magic;
```

Filesystem magic number.

```c
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
```

Basic geometry of the filesystem.

```c
    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;
```

Block indices for important regions.

```c
    uint8_t  _pad[128 - 9 * 4];
};
```

Padding to make the structure **exactly 128 bytes**.

---

## Inode

```c
struct inode {
```

Represents a file or directory.

```c
    uint16_t type;
    uint16_t links;
```

* Type: file/dir/free
* Hard link count

```c
    uint32_t size;
```

File size in bytes.

```c
    uint32_t direct[DIRECT_POINTERS];
```

Direct data block pointers.

```c
    uint32_t ctime;
    uint32_t mtime;
```

Creation and modification times.

```c
    uint8_t _pad[128 - (...)]
};
```

Padding to 128 bytes.

---

## Directory entry

```c
struct dirent {
    uint32_t inode;
    char name[NAME_LEN];
};
```

Maps filename → inode number.

---

## Journal structures

```c
struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used;
};
```

* Identifies journal
* Tracks how much is filled

---

```c
struct rec_header {
    uint16_t type;
    uint16_t size;
};
```

Header for *any* journal record.

---

```c
struct data_record {
    struct rec_header hdr;
    uint32_t block_no;
    uint8_t data[BLOCK_SIZE];
};
```

Logs:

* Which block to write
* The entire block content

---

```c
struct commit_record {
    struct rec_header hdr;
};
```

Marks end of a transaction.

---

# 7. Error helper

```c
static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}
```

* Prints error with `errno`
* Terminates program

---

# 8. Block I/O helpers

---

```c
static void read_block(int fd, uint32_t block_index, void *buf)
```

Reads one full block.

```c
off_t offset = (off_t)block_index * BLOCK_SIZE;
```

Calculate byte offset.

```c
lseek(fd, offset, SEEK_SET);
read(fd, buf, BLOCK_SIZE);
```

Seek + read.

---

```c
static void write_block(...)
```

Same logic, but writes.

---

# 9. Bitmap utilities

---

```c
static int bitmap_test(const uint8_t *bitmap, uint32_t index)
```

Checks if a bit is set.

---

```c
static void bitmap_set(uint8_t *bitmap, uint32_t index)
```

Marks a bit as used.

---

```c
static uint32_t bitmap_find_free(...)
```

Finds first zero bit → free inode/block.

Returns `-1` if none found.

---

# 10. Journal read/write

---

```c
static void read_journal(...)
```

Reads **all 16 journal blocks** into memory.

---

```c
static void write_journal(...)
```

Writes memory journal back to disk.

---

# 11. Journal initialization

---

```c
static void init_journal(uint8_t *journal_buf)
```

* Clears journal
* Writes magic number
* Sets initial offset

---

```c
static int journal_is_initialized(...)
```

Checks magic number.

---

# 12. Journal append helpers

---

```c
append_data_record(...)
```

Appends:

* Record header
* Block number
* Full block contents

Advances offset.

---

```c
append_commit_record(...)
```

Appends a commit marker.

---

```c
update_journal_header(...)
```

Updates `nbytes_used`.

---

# 13. `cmd_create()` — journaling a file creation

This **does NOT modify the filesystem directly**.

It logs changes into the journal.

---

### Filename validation

```c
if (strlen(filename) >= NAME_LEN)
```

Directory entry name limit.

---

### Open image

```c
int fd = open(image_path, O_RDWR);
```

Read-write disk image.

---

### Read superblock

```c
read_block(fd, 0, &sb);
```

Block 0 is superblock.

```c
if (sb.magic != FS_MAGIC)
```

Reject invalid filesystem.

---

### Load journal

```c
journal_buf = malloc(...)
read_journal(...)
```

Initialize journal if needed.

---

### Read metadata

```c
read_block(fd, INODE_BMAP_IDX, inode_bitmap);
read_block(fd, DATA_BMAP_IDX, data_bitmap);
read_block(fd, INODE_START_IDX, inode_block);
```

Load bitmaps and root inode block.

---

### Read root directory

```c
struct inode *root_inode = ...
uint32_t root_data_blk = root_inode->direct[0];
read_block(fd, root_data_blk, root_data_block);
```

Root directory stored in first data block.

---

### Find free inode

```c
bitmap_find_free(...)
```

Fail if no inode available.

---

### Find free directory entry

```c
for (...) {
```

* Ensure filename doesn’t already exist
* Find empty slot

---

### Copy-on-write preparation

```c
memcpy(new_inode_bitmap, ...)
memcpy(new_inode_block, ...)
memcpy(new_root_data_block, ...)
```

IMPORTANT:
**Never modify original blocks directly** — journaled FS rule.

---

### Allocate inode

```c
bitmap_set(new_inode_bitmap, free_inode);
```

---

### Initialize new inode

```c
new_file_inode->type = INODE_FILE;
new_file_inode->links = 1;
new_file_inode->size = 0;
```

Empty file.

---

### Update directory entry

```c
new_entries[free_entry].inode = free_inode;
strncpy(...)
```

Adds filename → inode mapping.

---

### Update root inode size

```c
new_root_inode->size += sizeof(struct dirent);
```

Directory grew.

---

### Space check

```c
if (current_offset + total_needed > JOURNAL_SIZE)
```

Prevent journal overflow.

---

### Append journal records

```c
append_data_record(... inode bitmap ...)
append_data_record(... inode block ...)
append_data_record(... root directory ...)
append_commit_record(...)
```

Transaction = **3 metadata writes + commit**.

---

### Write journal to disk

```c
write_journal(fd, journal_buf);
```

Filesystem NOT changed yet.

---

# 14. `cmd_install()` — replay journal

---

### Load journal

```c
read_journal(...)
journal_is_initialized(...)
```

---

### First pass: count commits

```c
while (offset < nbytes_used)
```

Counts how many transactions exist.

---

### Second pass: apply writes

```c
if (hdr->type == REC_DATA)
    write_block(fd, data_rec->block_no, data_rec->data);
```

Actually modifies disk blocks.

---

### Clear journal

```c
init_journal(...)
write_journal(...)
```

Journal is reset.

---

# 15. `main()`

```c
if (argc < 2)
```

Requires command.

---

```c
if (strcmp(command, "create") == 0)
```

Calls `cmd_create`.

---

```c
else if (strcmp(command, "install") == 0)
```

Calls `cmd_install`.

---

# 16. Big-picture summary

This program demonstrates:

* **Write-ahead logging**
* **Crash-safe metadata updates**
* **Atomic filesystem transactions**
* **Copy-on-write journaling**

It mirrors how **EXT3 / EXT4 journaling** works conceptually.

If you want, I can next:

* Walk through a **full execution trace**
* Explain **crash scenarios**
* Diagram the **on-disk layout**
* Compare this with **EXT2 vs EXT3**
  Just tell me.
