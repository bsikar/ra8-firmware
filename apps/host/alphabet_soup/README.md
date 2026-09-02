# Alphabet Soup

Word search puzzle answer key generator for newspaper puzzles using the architecture-neutral `fw_if_fs` POSIX filesystem adapter and `ra8_io_stream` byte stream facade.

## Overview

Finds target words within an ASCII character grid across all 8 directions (horizontal, vertical, diagonal, forward, and backward). Words that contain spaces in the word list are matched against consecutive non-space characters in the grid, while retaining original word labels in the generated answer key.

## Input File Format

The puzzle input file contains three sections:
1. **Dimensions**: First line specifies `Rows` x `Cols` separated by `x` (e.g. `3x3` or `5x5`).
2. **Grid Matrix**: The next `Rows` lines each contain `Cols` characters separated by space.
3. **Word List**: The remaining lines list the target words to locate in the grid.

### 3x3 Sample Dataset (`resources/sample_3x3.txt`)

```
3x3
A B C
D E F
G H I
ABC
AEI
```

**Expected Output:**
```
ABC 0:0 0:2
AEI 0:0 2:2
```

### 5x5 Sample Dataset (`resources/sample_5x5.txt`)

```
5x5
H A S D F
G E Y B H
J K L Z X
C V B L N
G O O D O
HELLO
GOOD
BYE
```

**Expected Output:**
```
HELLO 0:0 4:4
GOOD 4:0 4:3
BYE 1:3 1:1
```

## Building and Running

```bash
# Build the CLI application and unit test suite
just apps::host::build alphabet_soup

# Run against sample datasets
./apps/host/alphabet_soup/build/alphabet_soup apps/host/alphabet_soup/resources/sample_3x3.txt
./apps/host/alphabet_soup/build/alphabet_soup apps/host/alphabet_soup/resources/sample_5x5.txt

# Run the test suite
just tests::alphabet_soup
```
