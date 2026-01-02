#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

/* =======================
   Constants
   ======================= */

#define BLOCK_SIZE     4096
#define JOURNAL_BLOCKS 16

#define FS_MAGIC       0x56534653
#define JOURNAL_MAGIC  0x4A524E4C

#define REC_DATA       1
#define REC_COMMIT     2

#define NAME_LEN       28

/* =======================
   On-disk structures
   ======================= */

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
    uint8_t  data[BLOCK_SIZE];
};

struct commit_record {
    struct rec_header hdr;
};

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
    uint32_t direct[8];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t  _pad[128 - (2+2+4 + 8*4 + 4+4)];
};

struct dirent {
    uint32_t inode;
    char     name[NAME_LEN];
};

/* =======================
   Block I/O
   ======================= */

static int read_block(int fd, uint32_t block, void *buf) {
    if (lseek(fd, (off_t)block * BLOCK_SIZE, SEEK_SET) < 0)
        return -1;
    return read(fd, buf, BLOCK_SIZE) == BLOCK_SIZE ? 0 : -1;
}

static int write_block(int fd, uint32_t block, const void *buf) {
    if (lseek(fd, (off_t)block * BLOCK_SIZE, SEEK_SET) < 0)
        return -1;
    return write(fd, buf, BLOCK_SIZE) == BLOCK_SIZE ? 0 : -1;
}

/* =======================
   Bitmap helpers
   ======================= */

static int bitmap_test(uint8_t *bm, int i) {
    return (bm[i/8] >> (i%8)) & 1;
}

static void bitmap_set(uint8_t *bm, int i) {
    bm[i/8] |= (1 << (i%8));
}

static int bitmap_find_free(uint8_t *bm, int bytes) {
    for (int i = 0; i < bytes * 8; i++)
        if (!bitmap_test(bm, i))
            return i;
    return -1;
}

/* =======================
   Journal helpers
   ======================= */

static int append_data(uint8_t *jbuf, uint32_t *off,
                       uint32_t block, const uint8_t *data) {

    uint32_t need = sizeof(struct rec_header)
                  + sizeof(uint32_t)
                  + BLOCK_SIZE;

    if (*off + need > BLOCK_SIZE * JOURNAL_BLOCKS)
        return -1;

    struct data_record *r = (struct data_record *)(jbuf + *off);
    r->hdr.type = REC_DATA;
    r->hdr.size = need;
    r->block_no = block;
    memcpy(r->data, data, BLOCK_SIZE);

    *off += need;
    return 0;
}

static int append_commit(uint8_t *jbuf, uint32_t *off) {
    if (*off + sizeof(struct commit_record) >
        BLOCK_SIZE * JOURNAL_BLOCKS)
        return -1;

    struct commit_record *c =
        (struct commit_record *)(jbuf + *off);

    c->hdr.type = REC_COMMIT;
    c->hdr.size = sizeof(*c);

    *off += sizeof(*c);
    return 0;
}

static int clear_journal(int fd, uint32_t jblk) {
    uint8_t buf[BLOCK_SIZE];
    
    // First block: write journal header
    memset(buf, 0, BLOCK_SIZE);
    struct journal_header h = {
        .magic = JOURNAL_MAGIC,
        .nbytes_used = sizeof(h)
    };
    memcpy(buf, &h, sizeof(h));
    
    if (write_block(fd, jblk, buf) < 0)
        return -1;

    // Remaining blocks: write zeros
    memset(buf, 0, BLOCK_SIZE);
    for (int i = 1; i < JOURNAL_BLOCKS; i++)
        if (write_block(fd, jblk + i, buf) < 0)
            return -1;

    return 0;
}

/* =======================
   CREATE
   ======================= */

static int cmd_create(int fd, struct superblock *sb,
                      const char *name) {

    /* ---- read on-disk metadata ---- */
    uint8_t inode_bitmap[BLOCK_SIZE];
    uint8_t inode_blk[BLOCK_SIZE];
    uint8_t root_blk[BLOCK_SIZE];

    read_block(fd, sb->inode_bitmap, inode_bitmap);
    read_block(fd, sb->inode_start, inode_blk);
    read_block(fd, sb->data_start, root_blk);

    /* ---- read and replay journal to get current state ---- */
    uint8_t jbuf[BLOCK_SIZE * JOURNAL_BLOCKS];
    for (int i = 0; i < JOURNAL_BLOCKS; i++)
        read_block(fd, sb->journal_block + i,
                   jbuf + i * BLOCK_SIZE);

    struct journal_header *jh_temp = (void *)jbuf;
    if (jh_temp->magic == JOURNAL_MAGIC) {
        size_t off = sizeof(*jh_temp);
        printf("DEBUG: Replaying journal (nbytes_used=%u)\n", jh_temp->nbytes_used);
        while (off + sizeof(struct rec_header) <= jh_temp->nbytes_used) {
            struct rec_header *rh = (void *)(jbuf + off);
            if (rh->type == REC_DATA) {
                struct data_record *r = (void *)(jbuf + off);
                printf("DEBUG: Replaying record for block %u\n", r->block_no);
                if (r->block_no == sb->inode_bitmap)
                    memcpy(inode_bitmap, r->data, BLOCK_SIZE);
                else if (r->block_no == sb->inode_start)
                    memcpy(inode_blk, r->data, BLOCK_SIZE);
                else if (r->block_no == sb->data_start)
                    memcpy(root_blk, r->data, BLOCK_SIZE);
            }
            off += rh->size;
        }
    }

    /* ---- find free inode ---- */
    int ino = bitmap_find_free(inode_bitmap, BLOCK_SIZE);
    if (ino < 0) {
        fprintf(stderr, "no free inodes\n");
        return -1;
    }
    
    printf("DEBUG: Found free inode: %d\n", ino);

    /* ---- prepare new metadata copies ---- */
    uint8_t new_inode_bitmap[BLOCK_SIZE];
    uint8_t new_inode_blk[BLOCK_SIZE];
    uint8_t new_root_blk[BLOCK_SIZE];

    memcpy(new_inode_bitmap, inode_bitmap, BLOCK_SIZE);
    memcpy(new_inode_blk, inode_blk, BLOCK_SIZE);
    memcpy(new_root_blk, root_blk, BLOCK_SIZE);

    bitmap_set(new_inode_bitmap, ino);

    struct inode *ip = (struct inode *)new_inode_blk + ino;
    memset(ip, 0, sizeof(*ip));
    ip->type = 1;
    ip->links = 1;
    ip->ctime = ip->mtime = time(NULL);

    struct dirent *d = (struct dirent *)new_root_blk;
    int found_slot = 0;
    for (int i = 0; i < BLOCK_SIZE / sizeof(*d); i++) {
        if (d[i].name[0] == '\0') {  // Check for empty name, not inode
            printf("DEBUG: Adding entry '%s' at slot %d (inode %d)\n", name, i, ino);
            d[i].inode = ino;
            strncpy(d[i].name, name, NAME_LEN - 1);
            d[i].name[NAME_LEN - 1] = '\0';
            found_slot = 1;
            break;
        } else {
            printf("DEBUG: Slot %d occupied by '%s' (inode %u)\n", i, d[i].name, d[i].inode);
        }
    }
    
    if (!found_slot) {
        fprintf(stderr, "Error: No free directory entry found\n");
        return -1;
    }

    /* ---- jbuf already loaded above, now check/initialize header ---- */
    struct journal_header *jh = (void *)jbuf;
    
    /* ---- initialize journal if needed ---- */
    if (jh->magic != JOURNAL_MAGIC) {
        jh->magic = JOURNAL_MAGIC;
        jh->nbytes_used = sizeof(*jh);
    }
    
    uint32_t off = jh->nbytes_used;

    /* ---- space check (NOT emptiness check) ---- */
    uint32_t need =
        3 * (sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE)
        + sizeof(struct rec_header);

    if (off + need > BLOCK_SIZE * JOURNAL_BLOCKS) {
        fprintf(stderr, "journal full, run install\n");
        return -1;
    }

    /* ---- append transaction ---- */
    append_data(jbuf, &off, sb->inode_bitmap, new_inode_bitmap);
    append_data(jbuf, &off, sb->inode_start, new_inode_blk);
    append_data(jbuf, &off, sb->data_start, new_root_blk);
    append_commit(jbuf, &off);

    jh->nbytes_used = off;

    /* ---- write journal ---- */
    for (int i = 0; i < JOURNAL_BLOCKS; i++)
        write_block(fd, sb->journal_block + i,
                    jbuf + i * BLOCK_SIZE);

    printf("Created '%s' (journaled)\n", name);
    return 0;
}

/* =======================
   INSTALL
   ======================= */

static int cmd_install(int fd, struct superblock *sb) {

    uint8_t jbuf[BLOCK_SIZE * JOURNAL_BLOCKS];
    for (int i = 0; i < JOURNAL_BLOCKS; i++)
        read_block(fd, sb->journal_block + i,
                   jbuf + i * BLOCK_SIZE);

    struct journal_header *jh = (void *)jbuf;
    
    /* ---- handle uninitialized journal ---- */
    if (jh->magic != JOURNAL_MAGIC) {
        printf("Journal is empty (uninitialized or already installed)\n");
        return 0;
    }

    printf("Journal header: magic=%08x, nbytes_used=%u\n", 
           jh->magic, jh->nbytes_used);

    size_t off = sizeof(*jh);
    int records_applied = 0;

    while (off + sizeof(struct rec_header) <= jh->nbytes_used) {
        struct rec_header *rh = (void *)(jbuf + off);

        printf("Record at offset %zu: type=%u, size=%u\n", 
               off, rh->type, rh->size);

        if (rh->type == REC_DATA) {
            struct data_record *r = (void *)(jbuf + off);
            printf("  Writing data to block %u\n", r->block_no);
            write_block(fd, r->block_no, r->data);
            records_applied++;
        }
        else if (rh->type == REC_COMMIT) {
            printf("  Commit record found\n");
        }
        else {
            printf("  Unknown record type, stopping\n");
            break;
        }

        off += rh->size;
    }

    printf("Applied %d data records\n", records_applied);

    clear_journal(fd, sb->journal_block);
    
    /* DEBUG: Read root directory after install to verify */
    uint8_t verify_root[BLOCK_SIZE];
    read_block(fd, sb->data_start, verify_root);
    struct dirent *verify_d = (struct dirent *)verify_root;
    printf("Root directory after install:\n");
    for (int i = 0; i < 10; i++) {
        if (verify_d[i].name[0] != '\0') {
            printf("  [%d] inode=%u name='%s'\n", i, verify_d[i].inode, verify_d[i].name);
        }
    }
    
    printf("Journal installed successfully\n");
    return 0;
}

/* =======================
   MAIN
   ======================= */

int main(int argc, char **argv) {
    if (argc < 2) return 1;

    int fd = open("vsfs.img", O_RDWR);
    if (fd < 0) return 1;

    uint8_t sbuf[BLOCK_SIZE];
    read_block(fd, 0, sbuf);

    struct superblock sb;
    memcpy(&sb, sbuf, sizeof(sb));
    if (sb.magic != FS_MAGIC) return 1;

    if (!strcmp(argv[1], "create") && argc == 3)
        cmd_create(fd, &sb, argv[2]);
    else if (!strcmp(argv[1], "install"))
        cmd_install(fd, &sb);

    close(fd);
    return 0;
}
