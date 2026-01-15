# mmap_alloc

Drop-in replacement for `malloc`/`realloc`/`free`/`calloc` that uses a memory-mapped temp file instead of RAM.

- Statically links with executable
- A temporary file is created on disk at startup
- Memory is allocated in chunks mapped from this file
- The OS pages data in/out of RAM automatically — only the working set stays resident

Supports Windows and Linux, 64-bit only.

## Usage

```bash
# build mmap_alloc
clang -O2 -c mmap_alloc.c -o mmap_alloc.o

# link with mmap_alloc
clang -O3 -o myapp mmap_alloc.o myapp.c
```

`mmap_alloc.o` must come before other object files so its symbols override the C runtime's `malloc`/`free`.

## Environment variables

| Variable | Default | Description |
|---|---|---|
| `MMAP_HEAP_DIR` | platform temp dir | Directory for the backing file. Falls back to `GetTempPath` (Windows) or `$TMPDIR` / `/tmp` (Linux). |
| `MMAP_ALLOC_MAX_RSS` | `1073741824` (1 GB) | Working set limit in bytes. `0` = unlimited. On Linux this sets `RLIMIT_RSS` (mostly advisory — use cgroup `memory.max` for strict enforcement). On Windows uses `SetProcessWorkingSetSizeEx`. |
| `MMAP_ALLOC_MAX_FILE_SIZE` | `0` (unlimited) | Max backing file size in bytes. `malloc` returns `NULL` when the limit is reached. |
| `MMAP_ALLOC_DEBUG` | `1` | Print allocation stats. `0` = disabled. Only works when compiled with `MMAP_ALLOC_DEBUG=1`. |

## Compile-time options

| Define | Default | Description |
|---|---|---|
| `MMAP_ALLOC_DEBUG` | `1` | Include debug stats code. Set to `0` to strip it entirely. |
| `MMAP_ALLOC_DEBUG_DEFAULT_ENABLED` | `1` | Default for `MMAP_ALLOC_DEBUG` env var. |
| `MMAP_ALLOC_MAX_RSS` | `1073741824` | Default working set limit. |
| `MMAP_ALLOC_MAX_FILE_SIZE` | `0` | Default file size limit. |
| `TMP_FILE_PREFIX` | `"mmap_alloc"` | Prefix for the temp file name. |
| `MMAP_ALLOC_BOOTSTRAP_SIZE` | `65536` | Static buffer size for allocations during init (before the backing file is ready). |

## Linux / Docker

cgroup memory limit is the real hard cap — `MMAP_ALLOC_MAX_RSS` cannot override it.

Works in Linux containers. If `/tmp` is a small tmpfs, point the backing file elsewhere:

```bash
docker run -v /host/path:/data -e MMAP_HEAP_DIR=/data myimage
```

## Misc

LD_PRELOAD on Linux (not tested)

```bash
clang -O2 -shared -fPIC -o mmap_alloc.so mmap_alloc.c
LD_PRELOAD=./mmap_alloc.so ./myapp
```
