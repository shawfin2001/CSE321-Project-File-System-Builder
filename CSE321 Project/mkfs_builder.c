#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#define BS 4096u      
#define INODE_SIZE 128u
#define ROOT_INO 1u
#define DIRECT_MAX 12

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint64_t total_blocks;
    uint64_t inode_count;
    uint64_t inode_bitmap_start;
    uint64_t inode_bitmap_blocks;
    uint64_t data_bitmap_start;
    uint64_t data_bitmap_blocks;
    uint64_t inode_table_start;
    uint64_t inode_table_blocks;
    uint64_t data_region_start;
    uint64_t data_region_blocks;
    uint64_t root_inode;
    uint64_t mtime_epoch;
    uint32_t flags;
    uint32_t checksum;    
} superblock_t;
#pragma pack(pop)
_Static_assert(sizeof(superblock_t) == 116, "superblock must fit in one block");

#pragma pack(push,1)
typedef struct {
    uint16_t mode;
    uint16_t links;
    uint32_t uid;
    uint32_t gid;
    uint64_t size_bytes;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t direct[12];
    uint32_t reserved_0;
    uint32_t reserved_1;
    uint32_t reserved_2;
    uint32_t proj_id;
    uint32_t uid16_gid16;
    uint64_t xattr_ptr;
    uint64_t inode_crc; 
} inode_t;
#pragma pack(pop)
_Static_assert(sizeof(inode_t)==INODE_SIZE, "inode size mismatch");

#pragma pack(push,1)
typedef struct {
    uint32_t inode_no;
    uint8_t type;
    char name[58];
    uint8_t  checksum;
} dirent64_t;
#pragma pack(pop)
_Static_assert(sizeof(dirent64_t)==64, "dirent size mismatch");

uint32_t CRC32_TAB[256];
void crc32_init(void){
    for (uint32_t i=0;i<256;i++){
        uint32_t c=i;
        for(int j=0;j<8;j++) c = (c&1)?(0xEDB88320u^(c>>1)):(c>>1);
        CRC32_TAB[i]=c;
    }
}
uint32_t crc32(const void* data, size_t n){
    const uint8_t* p=(const uint8_t*)data; uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<n;i++) c = CRC32_TAB[(c^p[i])&0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t superblock_crc_finalize(superblock_t *sb) {
    sb->checksum = 0;
    uint32_t s = crc32((void *) sb, BS - 4);
    sb->checksum = s;
    return s;
}

void inode_crc_finalize(inode_t* ino){
    uint8_t tmp[INODE_SIZE]; memcpy(tmp, ino, INODE_SIZE);
    memset(&tmp[120], 0, 8);
    uint32_t c = crc32(tmp, 120);
    ino->inode_crc = (uint64_t)c;
}

void dirent_checksum_finalize(dirent64_t* de) {
    const uint8_t* p = (const uint8_t*)de;
    uint8_t x = 0;
    for (int i = 0; i < 63; i++) x ^= p[i];
    de->checksum = x;
}

static void usage() {
    fprintf(stderr, "Usage: mkfs_builder --image <output.img> --size-kib <180..4096, multiple of 4> --inodes <128..512>\n");
    exit(1);
}

int main(int argc, char **argv) {
    crc32_init();

    char *image = NULL;
    uint64_t size_kib = 0;
    uint64_t inode_count = 0;

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) usage();
        if (strcmp(argv[i], "--image") == 0) {
            image = argv[i + 1];
        } else if (strcmp(argv[i], "--size-kib") == 0) {
            size_kib = strtoull(argv[i + 1], NULL, 10);
        } else if (strcmp(argv[i], "--inodes") == 0) {
            inode_count = strtoull(argv[i + 1], NULL, 10);
        } else {
            usage();
        }
    }

    if (!image || size_kib == 0 || inode_count == 0) usage();

    if (size_kib < 180 || size_kib > 4096 || size_kib % 4 != 0) {
        fprintf(stderr, "Invalid size-kib\n");
        exit(1);
    }
    if (inode_count < 128 || inode_count > 512) {
        fprintf(stderr, "Invalid inodes\n");
        exit(1);
    }

    uint64_t total_blocks = size_kib / 4;
    uint64_t inode_table_blocks = (inode_count * INODE_SIZE + BS - 1) / BS;
    if (3 + inode_table_blocks + 1 > total_blocks) {
        fprintf(stderr, "Not enough space for file system\n");
        exit(1);
    }

    time_t now = time(NULL);

    uint8_t *disk = calloc(total_blocks, BS);
    if (!disk) {
        perror("calloc");
        exit(1);
    }

    superblock_t *sb = (superblock_t *)disk;
    sb->magic = 0x4D565346;
    sb->version = 1;
    sb->block_size = BS;
    sb->total_blocks = total_blocks;
    sb->inode_count = inode_count;
    sb->inode_bitmap_start = 1;
    sb->inode_bitmap_blocks = 1;
    sb->data_bitmap_start = 2;
    sb->data_bitmap_blocks = 1;
    sb->inode_table_start = 3;
    sb->inode_table_blocks = inode_table_blocks;
    sb->data_region_start = 3 + inode_table_blocks;
    sb->data_region_blocks = total_blocks - sb->data_region_start;
    sb->root_inode = ROOT_INO;
    sb->mtime_epoch = now;
    sb->flags = 0;
    superblock_crc_finalize(sb);

    uint8_t *ibm = disk + 1 * BS;
    ibm[0] |= 0x01;

    uint8_t *dbm = disk + 2 * BS;
    dbm[0] |= 0x01; 

    uint64_t root_data_block = sb->data_region_start;

    inode_t *inode_table = (inode_t *)(disk + sb->inode_table_start * BS);
    inode_t *root = &inode_table[0];
    root->mode = 0040000;  
    root->links = 2;
    root->uid = 0;
    root->gid = 0;
    root->size_bytes = 2 * sizeof(dirent64_t);
    root->atime = now;
    root->mtime = now;
    root->ctime = now;
    root->direct[0] = (uint32_t)root_data_block;
    root->proj_id = 4;
    root->uid16_gid16 = 0;
    root->xattr_ptr = 0;
    inode_crc_finalize(root);

    dirent64_t *dirblock = (dirent64_t *)(disk + root_data_block * BS);

    dirent64_t *dot = &dirblock[0];
    dot->inode_no = ROOT_INO;
    dot->type = 2;
    strcpy(dot->name, ".");
    dirent_checksum_finalize(dot);

    dirent64_t *dotdot = &dirblock[1];
    dotdot->inode_no = ROOT_INO;
    dotdot->type = 2;
    strcpy(dotdot->name, "..");
    dirent_checksum_finalize(dotdot);

    FILE *f = fopen(image, "wb");
    if (!f) {
        perror("fopen");
        exit(1);
    }
    if (fwrite(disk, BS, total_blocks, f) != total_blocks) {
        perror("fwrite");
        exit(1);
    }
    fclose(f);
    free(disk);

    return 0;
}
