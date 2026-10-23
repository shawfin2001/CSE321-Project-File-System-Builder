#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <libgen.h>
#include <sys/types.h> 

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
    uint8_t  checksum; // XOR of bytes 0..62
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
    fprintf(stderr, "Usage: mkfs_adder --input <input.img> --output <output.img> --file <file>\n");
    exit(1);
}

int main(int argc, char **argv) {
    crc32_init();
    char *input = NULL;
    char *output = NULL;
    char *add_file = NULL;

    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc) usage();
        if (strcmp(argv[i], "--input") == 0) {
            input = argv[i + 1];
        } else if (strcmp(argv[i], "--output") == 0) {
            output = argv[i + 1];
        } else if (strcmp(argv[i], "--file") == 0) {
            add_file = argv[i + 1];
        } else {
            usage();
        }
    }

    if (!input || !output || !add_file) usage();

    FILE *fin = fopen(input, "rb");
    if (!fin) {
        perror("fopen input");
        exit(1);
    }
    fseek(fin, 0, SEEK_END);
    off_t file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    if (file_size % BS != 0 || file_size < (off_t)(BS * 7)) {
        fprintf(stderr, "Invalid input image size\n");
        exit(1);
    }
    uint64_t total_blocks = (uint64_t)file_size / BS;
    uint8_t *disk = malloc((size_t)file_size);
    if (!disk) {
        perror("malloc");
        exit(1);
    }
    if (fread(disk, (size_t)file_size, 1, fin) != 1) {
        perror("fread");
        exit(1);
    }
    fclose(fin);

    superblock_t *sb = (superblock_t *)disk;
    if (sb->magic != 0x4D565346 || sb->version != 1 || sb->block_size != BS) {
        fprintf(stderr, "Invalid file system\n");
        exit(1);
    }

    FILE *ff = fopen(add_file, "rb");
    if (!ff) {
        perror("fopen file");
        exit(1);
    }
    fseek(ff, 0, SEEK_END);
    uint64_t add_size = (uint64_t)ftell(ff);
    fseek(ff, 0, SEEK_SET);
    uint64_t needed_blocks = (add_size + BS - 1) / BS;
    if (needed_blocks > DIRECT_MAX) {
        fprintf(stderr, "File too large, exceeds 12 blocks\n");
        exit(1);
    }

    uint8_t *ibm = disk + sb->inode_bitmap_start * BS;
    uint64_t max_inodes = sb->inode_count;
    int found_ino = -1;
    for (uint64_t bit = 0; bit < max_inodes; bit++) {
        uint64_t byte_idx = bit / 8;
        uint8_t bit_idx = bit % 8;
        if ((ibm[byte_idx] & (1 << bit_idx)) == 0) {
            found_ino = (int)(bit + 1);
            ibm[byte_idx] |= (1 << bit_idx);
            break;
        }
    }
    if (found_ino == -1) {
        fprintf(stderr, "No free inode\n");
        exit(1);
    }

    inode_t *inode_table = (inode_t *)(disk + sb->inode_table_start * BS);
    inode_t *new_inode = &inode_table[found_ino - 1];
    memset(new_inode, 0, sizeof(inode_t));
    new_inode->mode = 0100000;  // Regular file mode (octal)
    new_inode->links = 1;
    new_inode->uid = 0;
    new_inode->gid = 0;
    new_inode->size_bytes = add_size;
    time_t now = time(NULL);
    new_inode->atime = now;
    new_inode->mtime = now;
    new_inode->ctime = now;
    new_inode->proj_id = 4;
    new_inode->uid16_gid16 = 0;
    new_inode->xattr_ptr = 0;

    uint8_t *dbm = disk + sb->data_bitmap_start * BS;
    uint64_t max_dblocks = sb->data_region_blocks;
    uint32_t dblocks[DIRECT_MAX] = {0};
    uint64_t alloced = 0;
    for (uint64_t bit = 0; bit < max_dblocks && alloced < needed_blocks; bit++) {
        uint64_t byte_idx = bit / 8;
        uint8_t bit_idx = bit % 8;
        if ((dbm[byte_idx] & (1 << bit_idx)) == 0) {
            dblocks[alloced] = (uint32_t)(sb->data_region_start + bit);
            dbm[byte_idx] |= (1 << bit_idx);
            alloced++;
        }
    }
    if (alloced < needed_blocks) {
        fprintf(stderr, "No enough data blocks\n");
        exit(1);
    }
    for (int i = 0; i < (int)needed_blocks; i++) {
        new_inode->direct[i] = dblocks[i];
    }

    for (int i = 0; i < (int)needed_blocks; i++) {
        uint8_t *dptr = disk + (uint64_t)dblocks[i] * BS;
        memset(dptr, 0, BS);
        size_t to_read = BS;
        if (i == (int)needed_blocks - 1 && add_size % BS != 0) {
            to_read = add_size % BS;
        }
        if (fread(dptr, to_read, 1, ff) != 1) {
            perror("fread file");
            exit(1);
        }
    }
    fclose(ff);

    inode_crc_finalize(new_inode);

    inode_t *rooti = &inode_table[ROOT_INO - 1];
    char *fname = basename(add_file);
    if (strlen(fname) >= 58) {
        fprintf(stderr, "Filename too long\n");
        exit(1);
    }

    int added = 0;
    uint64_t cur_size = rooti->size_bytes;
    if (cur_size % 64 != 0) {
        fprintf(stderr, "Invalid directory size\n");
        exit(1);
    }

    for (int di = 0; di < DIRECT_MAX; di++) {
        if (rooti->direct[di] == 0) continue;
        uint8_t *dptr = disk + (uint64_t)rooti->direct[di] * BS;
        dirent64_t *dents = (dirent64_t *)dptr;
        int entries_in_block = (di < (int)(cur_size / BS)) ? 64 : (int)((cur_size % BS) / 64);
        for (int e = 0; e < entries_in_block; e++) {
            if (dents[e].inode_no != 0 && strcmp(dents[e].name, fname) == 0) {
                fprintf(stderr, "File already exists\n");
                exit(1);
            }
            if (dents[e].inode_no == 0) {
                dents[e].inode_no = (uint32_t)found_ino;
                dents[e].type = 1;
                strcpy(dents[e].name, fname);
                dirent_checksum_finalize(&dents[e]);
                added = 1;
                uint64_t this_pos = (uint64_t)di * BS + (uint64_t)e * 64;
                if (this_pos + 64 > cur_size) {
                    rooti->size_bytes = this_pos + 64;
                }
                goto added_done;
            }
        }
    }

    if (!added) {
        int last_di = -1;
        for (int i = 0; i < DIRECT_MAX; i++) if (rooti->direct[i] != 0) last_di = i;
        if (last_di == -1) {
            fprintf(stderr, "No data blocks for root\n");
            exit(1);
        }
        uint64_t offset = cur_size % BS;
        if (offset + 64 <= BS) {
            uint8_t *dptr = disk + (uint64_t)rooti->direct[last_di] * BS;
            dirent64_t *dent = (dirent64_t *)(dptr + offset);
            memset(dent, 0, 64);
            dent->inode_no = (uint32_t)found_ino;
            dent->type = 1;
            strcpy(dent->name, fname);
            dirent_checksum_finalize(dent);
            rooti->size_bytes += 64;
            added = 1;
        } else {
            int free_direct = -1;
            for (int i = 0; i < DIRECT_MAX; i++) if (rooti->direct[i] == 0) {
                free_direct = i;
                break;
            }
            if (free_direct == -1) {
                fprintf(stderr, "No more direct pointers for root\n");
                exit(1);
            }
            int found_dbit = -1;
            for (uint64_t bit = 0; bit < max_dblocks; bit++) {
                uint64_t byte_idx = bit / 8;
                uint8_t bit_idx = bit % 8;
                if ((dbm[byte_idx] & (1 << bit_idx)) == 0) {
                    found_dbit = (int)bit;
                    dbm[byte_idx] |= (1 << bit_idx);
                    break;
                }
            }
            if (found_dbit == -1) {
                fprintf(stderr, "No free data block for directory\n");
                exit(1);
            }
            uint32_t new_dblock = (uint32_t)(sb->data_region_start + found_dbit);
            rooti->direct[free_direct] = new_dblock;
            uint8_t *dptr = disk + (uint64_t)new_dblock * BS;
            memset(dptr, 0, BS);
            dirent64_t *dent = (dirent64_t *)dptr;
            dent->inode_no = (uint32_t)found_ino;
            dent->type = 1;
            strcpy(dent->name, fname);
            dirent_checksum_finalize(dent);
            rooti->size_bytes += 64;
            added = 1;
        }
    }

added_done:
    if (!added) {
        fprintf(stderr, "Could not add directory entry\n");
        exit(1);
    }

    rooti->mtime = now;
    rooti->ctime = now;
    inode_crc_finalize(rooti);

    sb->mtime_epoch = now;
    superblock_crc_finalize(sb);

    FILE *fout = fopen(output, "wb");
    if (!fout) {
        perror("fopen output");
        exit(1);
    }
    if (fwrite(disk, BS, total_blocks, fout) != total_blocks) {
        perror("fwrite");
        exit(1);
    }
    fclose(fout);
    free(disk);

    return 0;
}
