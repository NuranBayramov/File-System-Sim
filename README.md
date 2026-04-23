# Assignment 3 – File System Allocation Simulator

## Overview
This project is a simple file system simulator written in C++. It was made to demonstrate how an operating system can allocate disk blocks to files using different allocation methods.

The simulator supports three allocation strategies:

- Contiguous Allocation
- FAT (File Allocation Table)
- I-node Allocation

It also includes:

- free space tracking
- basic file operations
- hierarchical directories
- hard links
- soft links
- simple journaling for delete operations
- workload-based simulation through an input text file

---

## Objective
The purpose of this project is to compare different file allocation methods and observe their trade-offs in terms of:

- block allocation and deallocation
- fragmentation
- memory overhead
- file access behavior
- file deletion with links
- simplified crash recovery using journaling

---

## Features

### 1. Disk Representation
The disk is simulated as a linear sequence of fixed-size blocks.

- Total blocks: 64
- Block size: 512 bytes

The simulator keeps track of which blocks are free and which are allocated.

### 2. Allocation Methods
The program can run in one of three modes:

#### Contiguous Allocation
Each file is stored in one continuous range of blocks.

#### FAT Allocation
Files are stored as linked blocks using a File Allocation Table kept in memory.

#### I-node Allocation
Each file has an i-node that stores direct block addresses and supports indirect addressing for larger files.

### 3. Directory System
The simulator supports a simple hierarchical directory structure.

Examples:
- `/docs`
- `/pics`
- `/docs/file1`

### 4. Links
Two kinds of file links are implemented:

#### Hard Link
A new directory entry points to the same file/i-node.  
The file remains accessible until all hard links are removed.

#### Soft Link
A symbolic link stores the target file path.  
If the original file is deleted, the soft link becomes broken.

### 5. Journaling
Before delete-related actions are executed, the simulator records journal entries first.  
This is a simplified simulation of journaling behavior used in real systems like NTFS and ext3.

### 6. Workload Input
The simulator reads commands from a workload text file and executes them one by one.

---

## Supported Commands

### Directory Commands
- `CREATE_DIR <path>`
- `LIST <path>`

### File Commands
- `CREATE <path> <numBlocks>`
- `DELETE <path>`
- `OPEN <path>`
- `CLOSE <path>`
- `READ <path>`
- `WRITE <path> <extraBlocks>`

### Link Commands
- `HARDLINK <existingPath> <newPath>`
- `SOFTLINK <targetPath> <linkPath>`

### System Commands
- `STATUS`
- `CRASH`

---

## Example Workload

CREATE_DIR /docs
CREATE_DIR /pics
CREATE /docs/file1 5
CREATE /docs/file2 7
OPEN /docs/file1
READ /docs/file1
WRITE /docs/file1 2
CLOSE /docs/file1
HARDLINK /docs/file1 /docs/file1_hard
SOFTLINK /docs/file1 /docs/file1_soft
LIST /docs
DELETE /docs/file1
READ /docs/file1_hard
READ /docs/file1_soft
CREATE /pics/img1 6
DELETE /docs/file2
STATUS
CRASH

## Compilation

g++ fs_simulator.cpp -o simulator 

## How to run the code

./simulator <allocation_type> <workload_file>