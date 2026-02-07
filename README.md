# Metadata Journalling File System

A lightweight file system implementation with journal-based crash recovery, designed for CSE321 (Operating Systems). Only part I had to work on is [journal.c](https://github.com/ayiman29/metadata-journalling/blob/main/journal.c)

## Overview

This project implements a simple file system with **metadata journalling** to ensure consistency and provide crash recovery. The system uses a write-ahead journal to log all metadata changes before they are applied to the main file system structures.

### Key Concepts

**Journalling**: All metadata modifications (file creation, inode updates, bitmap changes) are first written to a journal as atomic transactions. Only after a commit record is written does the system apply these changes to the actual file system. This ensures that the system can recover to a consistent state after a crash.

**Crash Consistency**: If the system crashes before a commit, the incomplete transaction in the journal is discarded. If it crashes after a commit, the transactions can be replayed during recovery.

## Project Structure

```
├── mkfs.c          - Creates a new file system image with journal support
├── journal.c       - Manages journalling: file creation and transaction installation
├── validator.c     - Validates and verifies file system consistency
└── vsfs.img        - File system image (created by mkfs)
```

## File System Layout

The file system is organized into **84 blocks** (4096 bytes each):

| Blocks | Purpose |
|--------|---------|
| 0 | Superblock (metadata about the file system) |
| 1-16 | Journal (16 blocks for write-ahead logging) |
| 17 | Inode Bitmap (tracks which inodes are in use) |
| 18 | Data Bitmap (tracks which data blocks are in use) |
| 19-20 | Inode Table (stores file metadata) |
| 21-84 | Data Blocks (actual file content) |

## Superblock Structure

Contains essential file system information:
- Magic number (`0x56534653`) for validation
- Block size and total block count
- Inode and data bitmap locations
- Inode and data block start addresses

## Data Structures

### Inode
Metadata for files/directories:
- Type (file=1, directory=2, free=0)
- Link count
- File size
- 8 direct block pointers
- Creation and modification timestamps

### Journal Records

**Data Record**: Stores a modified block
- Record type (REC_DATA = 1)
- Block number
- Block data (4096 bytes)

**Commit Record**: Marks end of transaction
- Record type (REC_COMMIT = 2)
- Marks the point where changes become durable

### Journal Header
- Magic number (`0x4A524E4C`) indicates initialized journal
- Bytes used (current journal size)

## Building

```bash
gcc -o mkfs mkfs.c
gcc -o journal journal.c
gcc -o validator validator.c
```

## Usage

### 1. Create a New File System

```bash
./mkfs
```

This creates `vsfs.img` with:
- Initialized superblock
- Empty journal
- Root directory inode
- Bitmap structures

### 2. Create Files (with Journalling)

```bash
./journal create myfile.txt
```

This demonstrates the journalling process:
1. **Log Phase**: Write all modifications (inode bitmap, inode, directory entry) to the journal as data records
2. **Commit Phase**: Write a commit record marking the transaction as complete
3. **Consistency**: If crash occurs before commit, transaction is discarded; if after, it can be replayed

The file creation logs:
- Updated inode bitmap
- New inode metadata
- Updated root directory entry

### 3. Install Committed Transactions

```bash
./journal install
```

This applies all committed transactions from the journal to the actual file system:
1. Scans the journal for complete transactions (ending with commit records)
2. Replays data records to their target blocks
3. Clears the journal after successful installation

**Simulating Recovery**: If you edit `vsfs.img` manually or simulate a crash, running `./journal install` again will apply any pending transactions.

### 4. Validate File System Consistency

```bash
./validator
```

Checks:
- Superblock magic number validity
- Inode bitmap consistency with actual inodes
- Data bitmap consistency with allocated data blocks
- Inode structure validity
- Root directory integrity
- Journal state and completeness

## How Crash Recovery Works

### Scenario 1: Crash During Logging
```
1. update inode bitmap → write to journal
2. update inode → write to journal
3. CRASH (before commit record)
```
**Recovery**: run `./journal install` → nothing happens (no commit record found)

### Scenario 2: Crash After Commit
```
1. write inode bitmap to journal
2. write inode to journal
3. write commit record to journal
4. CRASH (before installation)
```
**Recovery**: run `./journal install` → replays all logged blocks to file system

### Scenario 3: Normal Operation
```
1. write all changes to journal
2. write commit record
3. run install → apply to file system
4. clear journal
```

## Key Features

✅ **Atomic Transactions**: Changes are all-or-nothing  
✅ **Write-Ahead Logging**: Journal logs changes before applying them  
✅ **Crash Consistency**: System can recover to consistent state  
✅ **Bitmap Management**: Tracks free inodes and data blocks  
✅ **File Creation**: Creates files as atomic transactions  
✅ **Validation**: Checks file system integrity  


## Concepts Demonstrated

- **File System Structures**: Superblock, inode, bitmaps, directories
- **Journalling**: Write-ahead logging, transaction records
- **Crash Recovery**: Redo logging, atomic operations
- **Bitmap Operations**: Efficient bit manipulation for space tracking
- **Disk I/O**: Block-oriented file operations
