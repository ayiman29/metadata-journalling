#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FS_MAGIC 0x56534653U
#define JOURNAL_MAGIC 0x4A524E4CU

#define BLOCK_SIZE        4096U
#define INODE_SIZE         128U
#define JOURNAL_BLOCK_IDX    1U
#define JOURNAL_BLOCKS      16U
#define INODE_BLOCKS         2U
#define DATA_BLOCKS         64U
#define INODE_BMAP_IDX     (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)
#define DATA_BMAP_IDX      (INODE_BMAP_IDX + 1U)
#define INODE_START_IDX    (DATA_BMAP_IDX + 1U)
#define DATA_START_IDX     (INODE_START_IDX + INODE_BLOCKS)
#define TOTAL_BLOCKS       (DATA_START_IDX + DATA_BLOCKS)
#define DIRECT_POINTERS     8U
#define NAME_LEN           28
#define DEFAULT_IMAGE "vsfs.img"

#define JOURNAL_SIZE (JOURNAL_BLOCKS * BLOCK_SIZE)

#define REC_DATA   1
#define REC_COMMIT 2

#define INODE_FREE 0
#define INODE_FILE 1
#define INODE_DIR  2

struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;
    uint8_t  _pad[128 - 9 * 4];
};

struct inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;
    uint32_t direct[DIRECT_POINTERS];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t _pad[128 - (2 + 2 + 4 + DIRECT_POINTERS * 4 + 4 + 4)];
};

struct dirent {
    uint32_t inode;
    char name[NAME_LEN];
};

struct journal_header {
    uint32_t magic;
    uint32_t nbytes_used;
};

struct rec_header {
    uint16_t type;
    uint16_t size;
};

struct data_record {
    struct rec_header hdr;
    uint32_t block_no;
    uint8_t data[BLOCK_SIZE];
};

struct commit_record {
    struct rec_header hdr;
};

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void read_block(int fd, uint32_t idx, void *buf) {
    if (lseek(fd, (off_t)idx * BLOCK_SIZE, SEEK_SET) < 0) die("lseek");
    if (read(fd, buf, BLOCK_SIZE) != BLOCK_SIZE) die("read");
}

static void write_block(int fd, uint32_t idx, const void *buf) {
    if (lseek(fd, (off_t)idx * BLOCK_SIZE, SEEK_SET) < 0) die("lseek");
    if (write(fd, buf, BLOCK_SIZE) != BLOCK_SIZE) die("write");
}

static int bitmap_test(const uint8_t *bm, uint32_t i) {
    return (bm[i / 8] >> (i % 8)) & 1;
}

static void bitmap_set(uint8_t *bm, uint32_t i) {
    bm[i / 8] |= (1U << (i % 8));
}

static uint32_t bitmap_find_free(const uint8_t *bm, uint32_t max) {
    for (uint32_t i = 0; i < max; i++)
        if (!bitmap_test(bm, i)) return i;
    return (uint32_t)-1;
}

static void read_journal(int fd, uint8_t *buf) {
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++)
        read_block(fd, JOURNAL_BLOCK_IDX + i, buf + i * BLOCK_SIZE);
}

static void write_journal(int fd, const uint8_t *buf) {
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++)
        write_block(fd, JOURNAL_BLOCK_IDX + i, buf + i * BLOCK_SIZE);
}

static void init_journal(uint8_t *buf) {
    memset(buf, 0, JOURNAL_SIZE);
    struct journal_header *h = (void *)buf;
    h->magic = JOURNAL_MAGIC;
    h->nbytes_used = sizeof(*h);
}

static void append_data(uint8_t *buf, uint32_t *off, uint32_t blk, const uint8_t *data) {
    struct data_record *r = (void *)(buf + *off);
    r->hdr.type = REC_DATA;
    r->hdr.size = sizeof(*r);
    r->block_no = blk;
    memcpy(r->data, data, BLOCK_SIZE);
    *off += r->hdr.size;
}

static void append_commit(uint8_t *buf, uint32_t *off) {
    struct commit_record *r = (void *)(buf + *off);
    r->hdr.type = REC_COMMIT;
    r->hdr.size = sizeof(*r);
    *off += r->hdr.size;
}

static void cmd_create(const char *img, const char *name) {
    if (strlen(name) >= NAME_LEN) {
        fprintf(stderr, "Error: filename too long (max %d chars)\n", NAME_LEN - 1);
        exit(EXIT_FAILURE);
    }

    int fd = open(img, O_RDWR);
    if (fd < 0) die("open");

    struct superblock sb;
    read_block(fd, 0, &sb);
    if (sb.magic != FS_MAGIC) {
        fprintf(stderr, "Error: invalid filesystem magic\n");
        close(fd);
        exit(EXIT_FAILURE);
    }

    uint8_t *journal = malloc(JOURNAL_SIZE);
    read_journal(fd, journal);
    struct journal_header *jh = (void *)journal;
    if (jh->magic != JOURNAL_MAGIC) init_journal(journal);

    uint8_t inode_bm[BLOCK_SIZE];
    uint8_t root_data_blk[BLOCK_SIZE];

    read_block(fd, INODE_BMAP_IDX, inode_bm);

    uint32_t ino = bitmap_find_free(inode_bm, sb.inode_count);
    if (ino == (uint32_t)-1) {
        fprintf(stderr, "Error: no free inodes\n");
        free(journal);
        close(fd);
        exit(EXIT_FAILURE);
    }

    uint32_t inode_block_idx = ino / (BLOCK_SIZE / INODE_SIZE);
    uint32_t inode_offset = ino % (BLOCK_SIZE / INODE_SIZE);

    // Read the inode block that contains the new inode
    uint8_t inode_blk[BLOCK_SIZE];
    read_block(fd, INODE_START_IDX + inode_block_idx, inode_blk);

    // Root is always at offset 0 in the first inode block
    struct inode *root = (void *)inode_blk;
    read_block(fd, root->direct[0], root_data_blk);

    struct dirent *de = (void *)root_data_blk;
    uint32_t slot = (uint32_t)-1;
    for (uint32_t i = 0; i < BLOCK_SIZE / sizeof(*de); i++) {
        if (de[i].inode == 0) { 
            if (slot == (uint32_t)-1) slot = i;
        } else if (!strcmp(de[i].name, name)) {
            fprintf(stderr, "Error: file '%s' already exists\n", name);
            free(journal);
            close(fd);
            exit(EXIT_FAILURE);
        }
    }
    if (slot == (uint32_t)-1) {
        fprintf(stderr, "Error: root directory is full\n");
        free(journal);
        close(fd);
        exit(EXIT_FAILURE);
    }

    uint8_t new_inode_bm[BLOCK_SIZE];
    memcpy(new_inode_bm, inode_bm, BLOCK_SIZE);
    bitmap_set(new_inode_bm, ino);

    // Initialize the new inode
    struct inode *ni = (void *)(inode_blk + inode_offset * INODE_SIZE);
    memset(ni, 0, sizeof(*ni));
    ni->type = INODE_FILE;
    ni->links = 1;
    ni->ctime = ni->mtime = time(NULL);

    // Add directory entry
    de[slot].inode = ino;
    strncpy(de[slot].name, name, NAME_LEN - 1);
    de[slot].name[NAME_LEN - 1] = '\0';

    // Update root inode
    root->size += sizeof(*de);
    root->mtime = time(NULL);

    uint32_t off = jh->nbytes_used;
    
    // Check if we have enough space in journal
    uint32_t needed = sizeof(struct data_record) * 3 + sizeof(struct commit_record);
    if (off + needed > JOURNAL_SIZE) {
        fprintf(stderr, "Error: not enough journal space\n");
        free(journal);
        close(fd);
        exit(EXIT_FAILURE);
    }

    // Write inode bitmap
    append_data(journal, &off, INODE_BMAP_IDX, new_inode_bm);
    
    // Write the inode block (contains both root and possibly the new inode)
    append_data(journal, &off, INODE_START_IDX + inode_block_idx, inode_blk);
    
    // If new inode is in a different block than root, need to write root block separately
    if (inode_block_idx != 0) {
        uint8_t root_inode_blk[BLOCK_SIZE];
        read_block(fd, INODE_START_IDX, root_inode_blk);
        struct inode *root_separate = (void *)root_inode_blk;
        root_separate->size = root->size;
        root_separate->mtime = root->mtime;
        
        // Recalculate space needed
        needed = sizeof(struct data_record) * 4 + sizeof(struct commit_record);
        if (jh->nbytes_used + needed > JOURNAL_SIZE) {
            fprintf(stderr, "Error: not enough journal space\n");
            free(journal);
            close(fd);
            exit(EXIT_FAILURE);
        }
        
        append_data(journal, &off, INODE_START_IDX, root_inode_blk);
    }
    
    // Write root data block
    append_data(journal, &off, root->direct[0], root_data_blk);
    
    append_commit(journal, &off);

    jh->nbytes_used = off;
    write_journal(fd, journal);

    free(journal);
    close(fd);
    
    printf("Created file '%s' (inode %u) in journal\n", name, ino);
    printf("Run './journal install' to apply changes\n");
}

static void cmd_install(const char *img) {
    int fd = open(img, O_RDWR);
    if (fd < 0) die("open");

    uint8_t *journal = malloc(JOURNAL_SIZE);
    read_journal(fd, journal);
    struct journal_header *jh = (void *)journal;
    if (jh->magic != JOURNAL_MAGIC) {
        fprintf(stderr, "Error: invalid journal magic\n");
        free(journal);
        close(fd);
        exit(EXIT_FAILURE);
    }

    uint32_t off = sizeof(*jh);
    int commits = 0;

    while (off < jh->nbytes_used) {
        struct rec_header *h = (void *)(journal + off);
        if (h->type == REC_DATA) {
            struct data_record *r = (void *)(journal + off);
            write_block(fd, r->block_no, r->data);
        } else if (h->type == REC_COMMIT) {
            commits++;
        }
        off += h->size;
    }

    init_journal(journal);
    write_journal(fd, journal);

    free(journal);
    close(fd);

    printf("Replayed %d transaction(s)\n", commits);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <create|install> [args]\n", argv[0]);
        return 1;
    }
    if (!strcmp(argv[1], "create")) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s create <filename>\n", argv[0]);
            return 1;
        }
        cmd_create(DEFAULT_IMAGE, argv[2]);
    } else if (!strcmp(argv[1], "install")) {
        cmd_install(DEFAULT_IMAGE);
    } else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
    return 0;
}