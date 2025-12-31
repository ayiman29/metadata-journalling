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

static void read_block(int fd, uint32_t block_index, void *buf) {
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    lseek(fd, offset, SEEK_SET);
    read(fd, buf, BLOCK_SIZE);
}

static void write_block(int fd, uint32_t block_index, const void *buf) {
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    lseek(fd, offset, SEEK_SET);
    write(fd, buf, BLOCK_SIZE);
}

static int bitmap_test(const uint8_t *bitmap, uint32_t index) {
    return (bitmap[index / 8] >> (index % 8)) & 0x1;
}

static void bitmap_set(uint8_t *bitmap, uint32_t index) {
    bitmap[index / 8] |= (uint8_t)(1U << (index % 8));
}

static uint32_t bitmap_find_free(const uint8_t *bitmap, uint32_t max_bits) {
    for (uint32_t i = 0; i < max_bits; i++) {
        if (!bitmap_test(bitmap, i)) {
            return i;
        }
    }
    return (uint32_t)-1;
}

static void read_journal(int fd, uint8_t *journal_buf) {
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++) {
        read_block(fd, JOURNAL_BLOCK_IDX + i, journal_buf + (i * BLOCK_SIZE));
    }
}

static void write_journal(int fd, const uint8_t *journal_buf) {
    for (uint32_t i = 0; i < JOURNAL_BLOCKS; i++) {
        write_block(fd, JOURNAL_BLOCK_IDX + i, journal_buf + (i * BLOCK_SIZE));
    }
}

static void init_journal(uint8_t *journal_buf) {
    memset(journal_buf, 0, JOURNAL_SIZE);
    struct journal_header *jhdr = (struct journal_header *)journal_buf;
    jhdr->magic = JOURNAL_MAGIC;
    jhdr->nbytes_used = sizeof(struct journal_header);
}

static int journal_is_initialized(const uint8_t *journal_buf) {
    struct journal_header *jhdr = (struct journal_header *)journal_buf;
    return jhdr->magic == JOURNAL_MAGIC;
}

static void append_data_record(uint8_t *journal_buf, uint32_t *offset, 
                                uint32_t block_no, const uint8_t *block_data) {
    struct data_record *rec = (struct data_record *)(journal_buf + *offset);
    rec->hdr.type = REC_DATA;
    rec->hdr.size = sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE;
    rec->block_no = block_no;
    memcpy(rec->data, block_data, BLOCK_SIZE);
    *offset += rec->hdr.size;
}

static void append_commit_record(uint8_t *journal_buf, uint32_t *offset) {
    struct commit_record *rec = (struct commit_record *)(journal_buf + *offset);
    rec->hdr.type = REC_COMMIT;
    rec->hdr.size = sizeof(struct rec_header);
    *offset += rec->hdr.size;
}

static void update_journal_header(uint8_t *journal_buf, uint32_t nbytes_used) {
    struct journal_header *jhdr = (struct journal_header *)journal_buf;
    jhdr->nbytes_used = nbytes_used;
}

static void cmd_create(const char *image_path, const char *filename) {
    if (strlen(filename) >= NAME_LEN) {
        fprintf(stderr, "Error: filename too long (max %d chars)\n", NAME_LEN - 1);
        exit(EXIT_FAILURE);
    }

    int fd = open(image_path, O_RDWR);
    if (fd < 0) {
        die("open");
    }

    struct superblock sb;
    read_block(fd, 0, &sb);

    if (sb.magic != FS_MAGIC) {
        fprintf(stderr, "Error: invalid filesystem magic\n");
        close(fd);
        exit(EXIT_FAILURE);
    }

    uint8_t *journal_buf = malloc(JOURNAL_SIZE);
    if (!journal_buf) {
        die("malloc journal");
    }
    read_journal(fd, journal_buf);

    if (!journal_is_initialized(journal_buf)) {
        init_journal(journal_buf);
    }

    struct journal_header *jhdr = (struct journal_header *)journal_buf;
    uint32_t current_offset = jhdr->nbytes_used;

    uint8_t inode_bitmap[BLOCK_SIZE];
    uint8_t data_bitmap[BLOCK_SIZE];
    uint8_t inode_block[BLOCK_SIZE];
    uint8_t root_data_block[BLOCK_SIZE];

    read_block(fd, INODE_BMAP_IDX, inode_bitmap);
    read_block(fd, DATA_BMAP_IDX, data_bitmap);
    read_block(fd, INODE_START_IDX, inode_block);
    
    struct inode *root_inode = (struct inode *)inode_block;
    uint32_t root_data_blk = root_inode->direct[0];
    read_block(fd, root_data_blk, root_data_block);

    uint32_t free_inode = bitmap_find_free(inode_bitmap, sb.inode_count);
    if (free_inode == (uint32_t)-1) {
        fprintf(stderr, "Error: no free inodes\n");
        free(journal_buf);
        close(fd);
        exit(EXIT_FAILURE);
    }

    struct dirent *entries = (struct dirent *)root_data_block;
    uint32_t max_entries = BLOCK_SIZE / sizeof(struct dirent);
    uint32_t free_entry = (uint32_t)-1;
    
    for (uint32_t i = 0; i < max_entries; i++) {
        if (entries[i].inode == 0 && entries[i].name[0] == '\0') {
            free_entry = i;
            break;
        }
        if (strcmp(entries[i].name, filename) == 0) {
            fprintf(stderr, "Error: file '%s' already exists\n", filename);
            free(journal_buf);
            close(fd);
            exit(EXIT_FAILURE);
        }
    }

    if (free_entry == (uint32_t)-1) {
        fprintf(stderr, "Error: root directory is full\n");
        free(journal_buf);
        close(fd);
        exit(EXIT_FAILURE);
    }


    uint8_t new_inode_bitmap[BLOCK_SIZE];
    uint8_t new_inode_block[BLOCK_SIZE];
    uint8_t new_root_data_block[BLOCK_SIZE];

    memcpy(new_inode_bitmap, inode_bitmap, BLOCK_SIZE);
    memcpy(new_inode_block, inode_block, BLOCK_SIZE);
    memcpy(new_root_data_block, root_data_block, BLOCK_SIZE);

    bitmap_set(new_inode_bitmap, free_inode);

    uint32_t inode_block_idx = free_inode / (BLOCK_SIZE / INODE_SIZE);
    uint32_t inode_offset = free_inode % (BLOCK_SIZE / INODE_SIZE);
    
    if (inode_block_idx != 0) {
        read_block(fd, INODE_START_IDX + inode_block_idx, new_inode_block);
    }
    
    struct inode *new_file_inode = (struct inode *)(new_inode_block + inode_offset * INODE_SIZE);
    new_file_inode->type = INODE_FILE;
    new_file_inode->links = 1;
    new_file_inode->size = 0;
    memset(new_file_inode->direct, 0, sizeof(new_file_inode->direct));
    time_t now = time(NULL);
    new_file_inode->ctime = (uint32_t)now;
    new_file_inode->mtime = (uint32_t)now;

    struct dirent *new_entries = (struct dirent *)new_root_data_block;
    new_entries[free_entry].inode = free_inode;
    strncpy(new_entries[free_entry].name, filename, NAME_LEN - 1);
    new_entries[free_entry].name[NAME_LEN - 1] = '\0';

    struct inode *new_root_inode = (struct inode *)new_inode_block;
    if (inode_block_idx == 0) {
        new_root_inode->size += sizeof(struct dirent);
        new_root_inode->mtime = (uint32_t)now;
    }

    uint32_t record_size = sizeof(struct rec_header) + sizeof(uint32_t) + BLOCK_SIZE;
    uint32_t num_data_records = 3;
    uint32_t commit_size = sizeof(struct rec_header);
    uint32_t total_needed = (num_data_records * record_size) + commit_size;

    if (current_offset + total_needed > JOURNAL_SIZE) {
        fprintf(stderr, "Error: insufficient journal space. Please run './journal install' first.\n");
        free(journal_buf);
        close(fd);
        exit(EXIT_FAILURE);
    }

    append_data_record(journal_buf, &current_offset, INODE_BMAP_IDX, new_inode_bitmap);
    append_data_record(journal_buf, &current_offset, INODE_START_IDX + inode_block_idx, new_inode_block);
    append_data_record(journal_buf, &current_offset, root_data_blk, new_root_data_block);

    append_commit_record(journal_buf, &current_offset);

    update_journal_header(journal_buf, current_offset);

    write_journal(fd, journal_buf);

    free(journal_buf);
    close(fd);

    printf("Created file '%s' in journal (not yet applied to filesystem).\n", filename);
    printf("Run './journal install' to apply changes.\n");
}

static void cmd_install(const char *image_path) {
    int fd = open(image_path, O_RDWR);
    if (fd < 0) {
        die("open");
    }

    uint8_t *journal_buf = malloc(JOURNAL_SIZE);
    if (!journal_buf) {
        die("malloc journal");
    }
    read_journal(fd, journal_buf);

    if (!journal_is_initialized(journal_buf)) {
        fprintf(stderr, "Error: journal does not exist or is not initialized\n");
        free(journal_buf);
        close(fd);
        exit(EXIT_FAILURE);
    }

    struct journal_header *jhdr = (struct journal_header *)journal_buf;
    uint32_t offset = sizeof(struct journal_header);
    uint32_t nbytes_used = jhdr->nbytes_used;

    int transactions_replayed = 0;

    while (offset < nbytes_used) {
        struct rec_header *hdr = (struct rec_header *)(journal_buf + offset);
        
        if (offset + sizeof(struct rec_header) > nbytes_used) {
            break;
        }

        if (hdr->type == REC_DATA) {
            offset += hdr->size;
        } else if (hdr->type == REC_COMMIT) {
            transactions_replayed++;
            offset += hdr->size;
        } else {
            fprintf(stderr, "Warning: unknown record type %u at offset %u\n", hdr->type, offset);
            break;
        }
    }

    offset = sizeof(struct journal_header);
    while (offset < nbytes_used) {
        struct rec_header *hdr = (struct rec_header *)(journal_buf + offset);
        
        if (offset + sizeof(struct rec_header) > nbytes_used) {
            break;
        }

        if (hdr->type == REC_DATA) {
            struct data_record *data_rec = (struct data_record *)(journal_buf + offset);
            write_block(fd, data_rec->block_no, data_rec->data);
            offset += hdr->size;
        } else if (hdr->type == REC_COMMIT) {
            offset += hdr->size;
        } else {
            break;
        }
    }

    init_journal(journal_buf);
    write_journal(fd, journal_buf);

    free(journal_buf);
    close(fd);

    printf("Replayed %d transaction(s) and cleared journal.\n", transactions_replayed);
    printf("Filesystem metadata has been updated.\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <create|install> [filename]\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *command = argv[1];
    const char *image_path = DEFAULT_IMAGE;

    if (strcmp(command, "create") == 0) {
        if (argc < 3) {
            exit(EXIT_FAILURE);
        }
        cmd_create(image_path, argv[2]);
    } else if (strcmp(command, "install") == 0) {
        cmd_install(image_path);
    } else {
        exit(EXIT_FAILURE);
    }

    return 0;
}