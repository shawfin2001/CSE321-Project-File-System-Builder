# CSE321 File System Image Builder

A C systems-programming project that creates a custom file-system image and adds files to that image.

## Project overview

The project contains two C programs:

- **`mkfs_builder.c`** — creates a file-system image with a superblock, inode bitmap, data-block bitmap, inode table, data region, and root directory.
- **`mkfs_adder.c`** — reads an existing image, allocates an inode and data blocks for a file, adds a directory entry, updates metadata/checksums, and writes a new image.

The implementation uses 4096-byte blocks, 128-byte inodes, 64-byte directory entries, CRC32 checksums, and up to 12 direct data-block pointers per inode.

## Requirements

- GCC or another compatible C compiler
- Linux, macOS, or Windows Subsystem for Linux (WSL) recommended

## Compile

Run these commands from the project directory:

```bash
gcc -Wall -Wextra -std=c11 mkfs_builder.c -o mkfs_builder
gcc -Wall -Wextra -std=c11 mkfs_adder.c -o mkfs_adder
```

## Run the programs

### 1. Build a file-system image

```bash
./mkfs_builder --image filesystem.img --size-kib 256 --inodes 128
```

Options:
- `--image`: output image filename
- `--size-kib`: image size in KiB (180–4096, and must be a multiple of 4)
- `--inodes`: number of inodes (128–512)

### 2. Add a file to the image

Use one of the included sample text files, or create your own:

```bash
./mkfs_adder --input filesystem.img --output filesystem_with_file.img --file file_18.txt
```

Options:
- `--input`: existing file-system image
- `--output`: output image filename
- `--file`: file to add; its basename becomes the directory entry name

The adder supports files that need no more than 12 data blocks. With 4096-byte blocks, this corresponds to a maximum of 49,152 bytes of file data.

## File-system layout

The image is arranged with the following structures:

1. Superblock at block 0
2. Inode bitmap at block 1
3. Data-block bitmap at block 2
4. Inode table starting at block 3
5. Data region after the inode table

The root directory is initialized with `.` and `..` entries. When adding a file, the adder updates the allocation bitmaps, inode metadata, root directory, timestamps, and checksums.

## Repository structure

```text
.
├── mkfs_builder.c
├── mkfs_adder.c
├── file_2.txt
├── file_18.txt
├── file_29.txt
└── file_31.txt
```

The included `.txt` files are small sample inputs that can be used to test adding files to an image.

## Notes

- This is an educational file-system format, not a production file system.
- The commands above are example usage.
- Keep an untouched copy of any image you need to preserve because the programs write binary image files.
