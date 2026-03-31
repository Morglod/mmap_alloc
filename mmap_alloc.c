//
// mmap_alloc.c — Memory-mapped file allocator.
//
// Overrides malloc/realloc/free/calloc to use memory-mapped file chunks
// instead of RAM. All allocations are served from a single backing temp file.
//

// --------------------------------------
//               How to use
// --------------------------------------

// 1. build it
// clang -O2 -c mmap_alloc.c -o mmap_alloc.o

// 2. link it before your code
// clang -O3 -Wno-override-module -lm -o main mmap_alloc.o main.c

// --------------------------------------
//   Configurable through env variables
// --------------------------------------

// MMAP_ALLOC_MAX_RSS
// max ram could be used by program
// 1073741824 - 1gb, default
// 0 - unlimited

// MMAP_ALLOC_MAX_FILE_SIZE
// max backing file size in bytes
// 0 - unlimited, default
// 10737418240 - 10gb

// MMAP_HEAP_DIR
// directory for the backing file
// not set - uses platform default (GetTempPath on Windows, $TMPDIR or /tmp on Linux)
// "/mnt/data" - use specified directory

// MMAP_ALLOC_DEBUG
// print debug stats, works only when compiled with MMAP_ALLOC_DEBUG=1 (default)
// 1 - print debug stats, default
// 0 - disabled

// --------------------------------------
//             Build configs
// --------------------------------------

// MMAP_ALLOC_DEBUG=1 turns on debug stats feature (1 by default)
// MMAP_ALLOC_DEBUG_DEFAULT_ENABLED=1 specifies default for MMAP_ALLOC_DEBUG env
// MMAP_ALLOC_MAX_RSS=123 specifies default for MMAP_ALLOC_MAX_RSS env
// MMAP_ALLOC_MAX_FILE_SIZE=0 specifies default for MMAP_ALLOC_MAX_FILE_SIZE env
// TMP_FILE_PREFIX="mmap_alloc" specifies file prefix
// MMAP_ALLOC_BOOTSTRAP_SIZE=65536 specifies memory used for bootstrap

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* DiscardVirtualMemory (Windows 8.1+) may not be declared in all SDK versions */
DWORD WINAPI DiscardVirtualMemory(PVOID VirtualAddress, SIZE_T Size);
#include <psapi.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>
/* MADV_PAGEOUT actively pages dirty MAP_SHARED pages to the backing file (Linux 5.4+).
   Fall back to MADV_DONTNEED on older kernels — handled at runtime via errno. */
#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT 21
#endif
#endif

/* ── Constants ── */
#ifndef TMP_FILE_PREFIX
#define TMP_FILE_PREFIX  "mmap_alloc"
#endif

#define CHUNK_SIZE       (64 * 1024 * 1024)   /* 64 MB */
#define MAX_CHUNKS       1024
#define MIN_ALLOC_SIZE   32
#define ALIGN            16
#define ALLOC_GRAN       (64 * 1024)          /* 64 KB — Windows MapViewOfFile granularity */
#define MMAP_PAGE_SIZE   4096

#ifndef MMAP_ALLOC_BOOTSTRAP_SIZE
#define MMAP_ALLOC_BOOTSTRAP_SIZE   65536
#endif

#ifndef MMAP_ALLOC_MAX_RSS
#define MMAP_ALLOC_MAX_RSS (1ULL * 1024 * 1024 * 1024)  /* 1 GB default; 0 to disable */
#endif

#ifndef MMAP_ALLOC_MAX_FILE_SIZE
#define MMAP_ALLOC_MAX_FILE_SIZE 0  /* 0 = unlimited */
#endif

#ifndef MMAP_ALLOC_DEBUG
#define MMAP_ALLOC_DEBUG 1
#endif
#ifndef MMAP_ALLOC_DEBUG_DEFAULT_ENABLED
#define MMAP_ALLOC_DEBUG_DEFAULT_ENABLED 1
#endif

/* ── Types ── */
typedef struct {
    uint64_t size;
    int32_t  chunk_index;
    int32_t  _pad;
} AllocHeader;

typedef struct {
    void*  base;
    size_t file_offset;
    size_t mapped_size;
    int    alloc_count;
    int    is_oversized;
} ChunkInfo;

typedef struct FreeBlock {
    struct FreeBlock* next;
} FreeBlock;

/* ── Global state ── */
static int       initialized  = 0;
static int       initializing = 0;

#ifdef _WIN32
static HANDLE    backing_handle = INVALID_HANDLE_VALUE;
#else
static int       backing_fd = -1;
#endif

static ChunkInfo chunks[MAX_CHUNKS];
static int       chunk_count      = 0;
static int       active_chunk     = -1;
static size_t    current_offset   = 0;
static size_t    backing_file_size = 0;
static char      backing_path[512];

static size_t    max_file_size   = 0;  /* 0 = unlimited; set from env/define in init */

static FreeBlock* free_list_head = NULL;

static size_t reusable_offsets[MAX_CHUNKS];
static int    reusable_count = 0;

/* ── Debug stats ── */
#if MMAP_ALLOC_DEBUG == 1
static int    dbg_enabled        = MMAP_ALLOC_DEBUG_DEFAULT_ENABLED;   /* runtime toggle; overridden by env MMAP_ALLOC_DEBUG */
static size_t dbg_file_allocated = 0;   /* total bytes allocated in backing file */
static size_t dbg_in_use         = 0;   /* bytes currently in use by program */

static size_t dbg_get_rss(void) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return (size_t)pmc.WorkingSetSize;
    return 0;
#else
    /* Use raw syscalls to avoid fopen/fclose calling our malloc */
    int fd = open("/proc/self/statm", O_RDONLY);
    if (fd < 0) return 0;
    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    unsigned long dummy = 0, rss_pages = 0;
    sscanf(buf, "%lu %lu", &dummy, &rss_pages);
    return (size_t)rss_pages * (size_t)sysconf(_SC_PAGESIZE);
#endif
}

static void dbg_print_stats(const char* label) {
    if (!dbg_enabled) return;
    size_t rss = dbg_get_rss();
    printf("mmap_alloc [%s]: file=%.3f GB, file_used=%.3f GB, ram_used=%.3f GB\n",
           label,
           (double)dbg_file_allocated / (1024.0 * 1024.0 * 1024.0),
           (double)dbg_in_use / (1024.0 * 1024.0 * 1024.0),
           (double)rss / (1024.0 * 1024.0 * 1024.0));
}
#endif

/* ── Bootstrap buffer ── */
static char   bootstrap_buf[MMAP_ALLOC_BOOTSTRAP_SIZE];
static size_t bootstrap_offset = 0;

/* ── Bootstrap allocator ── */
static void* bootstrap_alloc(size_t size) {
    size = (size + ALIGN - 1) & ~((size_t)(ALIGN - 1));
    if (bootstrap_offset + size > MMAP_ALLOC_BOOTSTRAP_SIZE) {
        puts("bootstrap_alloc failed\n");
        printf("bootstrap_alloc failed: MMAP_ALLOC_BOOTSTRAP_SIZE=%i, current=%zu, requested=%zu\n", MMAP_ALLOC_BOOTSTRAP_SIZE, bootstrap_offset, size);
        return NULL;
    }
    void* p = bootstrap_buf + bootstrap_offset;
    bootstrap_offset += size;
    return p;
}

static int is_bootstrap_ptr(void* ptr) {
    return (char*)ptr >= bootstrap_buf &&
           (char*)ptr < bootstrap_buf + MMAP_ALLOC_BOOTSTRAP_SIZE;
}

/* ── Platform abstraction ── */
#ifdef _WIN32

static void platform_file_create(void) {
    char tmp_dir[MAX_PATH];
    const char* heap_dir = getenv("MMAP_HEAP_DIR");
    if (heap_dir && heap_dir[0]) {
        strncpy(tmp_dir, heap_dir, MAX_PATH - 1);
        tmp_dir[MAX_PATH - 1] = '\0';
    } else {
        DWORD len = GetTempPathA(MAX_PATH, tmp_dir);
        if (len == 0 || len >= MAX_PATH) {
            fprintf(stderr, "mmap_alloc: platform_file_create: failed get temp path\n");
            return;
        }
    }
    printf("mmap_alloc: platform_file_create: %s\n", backing_path);
    if (GetTempFileNameA(tmp_dir, TMP_FILE_PREFIX, 0, backing_path) == 0) {
        fprintf(stderr, "mmap_alloc: platform_file_create: failed get temp file name at dir=\"%s\" prefix=\"%s\" backing_path=\"%s\"\n", tmp_dir, TMP_FILE_PREFIX, backing_path);
        return;
    }
    backing_handle = CreateFileA(
        backing_path,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        NULL
    );
}

static int platform_file_extend(size_t new_size) {
    if (new_size > backing_file_size) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)new_size;
        if (!SetFilePointerEx(backing_handle, li, NULL, FILE_BEGIN) ||
            !SetEndOfFile(backing_handle)) {
            fprintf(stderr, "mmap_alloc: cannot grow backing file to %.3f GB (error %lu)\n",
                    (double)new_size / (1024.0 * 1024.0 * 1024.0), GetLastError());
            fflush(stderr);
            abort();
        }
        backing_file_size = new_size;
    }
    return 0;
}

static void* platform_chunk_map(size_t offset, size_t size) {
    LARGE_INTEGER li_max;
    li_max.QuadPart = (LONGLONG)backing_file_size;
    HANDLE mapping = CreateFileMappingA(
        backing_handle,
        NULL,
        PAGE_READWRITE,
        li_max.HighPart,
        li_max.LowPart,
        NULL
    );
    if (!mapping) return NULL;
    LARGE_INTEGER li_off;
    li_off.QuadPart = (LONGLONG)offset;
    void* ptr = MapViewOfFile(
        mapping,
        FILE_MAP_ALL_ACCESS,
        li_off.HighPart,
        li_off.LowPart,
        size
    );
    CloseHandle(mapping);
    return ptr;
}

static void platform_chunk_unmap(void* ptr, size_t size) {
    (void)size;
    UnmapViewOfFile(ptr);
}

static void platform_file_close(void) {
    if (backing_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(backing_handle);
        backing_handle = INVALID_HANDLE_VALUE;
    }
}

#else /* Linux */

static void platform_file_create(void) {
    const char* tmpdir = getenv("MMAP_HEAP_DIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = getenv("TMPDIR");
    if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
    snprintf(backing_path, sizeof(backing_path), "%s/" TMP_FILE_PREFIX "_heap_XXXXXX", tmpdir);
    printf("mmap_alloc: platform_file_create: %s\n", backing_path);
    backing_fd = mkstemp(backing_path);
    if (backing_fd < 0) {
        fprintf(stderr, "mmap_alloc: platform_file_create: failed mkstemp tmpdir=\"%s\" prefix=\"%s\" backing_path=\"%s\"\n", tmpdir, TMP_FILE_PREFIX, backing_path);
        return;
    }
    unlink(backing_path);
    backing_path[0] = '\0';
}

static int platform_file_extend(size_t new_size) {
    if (new_size > backing_file_size) {
        /* posix_fallocate actually reserves pages on the filesystem (including
           tmpfs), so it returns ENOSPC immediately when /tmp is full.
           ftruncate only sets file size metadata and succeeds even when there
           is no space; the real failure then arrives as SIGBUS on first page
           write, killing the process silently before any error can be printed. */
        int ret = posix_fallocate(backing_fd, (off_t)backing_file_size,
                                  (off_t)(new_size - backing_file_size));
        if (ret != 0) {
            /* posix_fallocate returns the error code directly, not via errno */
            fprintf(stderr, "mmap_alloc: cannot grow backing file to %.3f GB: %s\n",
                    (double)new_size / (1024.0 * 1024.0 * 1024.0), strerror(ret));
            fflush(stderr);
            abort();
        }
        backing_file_size = new_size;
    }
    return 0;
}

static void* platform_chunk_map(size_t offset, size_t size) {
    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, backing_fd, (off_t)offset);
}

static void platform_chunk_unmap(void* ptr, size_t size) {
    munmap(ptr, size);
}

static void platform_file_close(void) {
    if (backing_fd >= 0) {
        close(backing_fd);
        backing_fd = -1;
    }
}

#endif

/* ── Chunk management ── */
static size_t align_up(size_t val, size_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

static int map_new_chunk(size_t size, int oversized) {
    if (chunk_count >= MAX_CHUNKS) return -1;

    size_t mapped_size = oversized ? align_up(size, ALLOC_GRAN) : CHUNK_SIZE;
    size_t file_offset;

    if (!oversized && reusable_count > 0) {
        file_offset = reusable_offsets[--reusable_count];
    } else {
        file_offset = align_up(backing_file_size, ALLOC_GRAN);
        if (max_file_size > 0 && file_offset + mapped_size > max_file_size) {
            fprintf(stderr, "mmap_alloc: file size limit reached (%.3f GB)\n",
                    (double)max_file_size / (1024.0 * 1024.0 * 1024.0));
            return -1;
        }
        if (platform_file_extend(file_offset + mapped_size) < 0) return -1;
    }

    void* base = platform_chunk_map(file_offset, mapped_size);
    if (!base || base == (void*)(intptr_t)-1) return -1;

    int idx = chunk_count++;
    chunks[idx].base        = base;
    chunks[idx].file_offset = file_offset;
    chunks[idx].mapped_size = mapped_size;
    chunks[idx].alloc_count = 0;
    chunks[idx].is_oversized = oversized;
#if MMAP_ALLOC_DEBUG == 1
    dbg_file_allocated += mapped_size;
    dbg_print_stats("new_chunk");
#endif
    return idx;
}

/* ── Cleanup ── */
static void cleanup_allocator(void) {
#if MMAP_ALLOC_DEBUG == 1
    dbg_print_stats("cleanup");
#endif
    for (int i = 0; i < chunk_count; i++) {
        if (chunks[i].base) {
            platform_chunk_unmap(chunks[i].base, chunks[i].mapped_size);
            chunks[i].base = NULL;
        }
    }
    platform_file_close();
#if MMAP_ALLOC_DEBUG == 1
    if (dbg_enabled) {
        printf("mmap_alloc: cleanup ended\n");
    }
#endif
}

/* ── Initialization ── */
static void init_allocator(void) {
    /* Read env overrides before anything else */
#if MMAP_ALLOC_DEBUG == 1
    {
        const char* env_dbg = getenv("MMAP_ALLOC_DEBUG");
        if (env_dbg) dbg_enabled = (env_dbg[0] != '0');
    }
#endif

    size_t max_rss = (size_t)MMAP_ALLOC_MAX_RSS;
    {
        const char* env_rss = getenv("MMAP_ALLOC_MAX_RSS");
        if (env_rss) {
            unsigned long long val = strtoull(env_rss, NULL, 0);
            max_rss = (size_t)val;
        }
    }

    max_file_size = (size_t)MMAP_ALLOC_MAX_FILE_SIZE;
    {
        const char* env_fs = getenv("MMAP_ALLOC_MAX_FILE_SIZE");
        if (env_fs) {
            unsigned long long val = strtoull(env_fs, NULL, 0);
            max_file_size = (size_t)val;
        }
    }

    platform_file_create();
#ifdef _WIN32
    if (backing_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "mmap_alloc: failed to create backing file\n");
        return;
    }
#else
    if (backing_fd < 0) {
        fprintf(stderr, "mmap_alloc: failed to create backing file\n");
        return;
    }
#endif
    int idx = map_new_chunk(CHUNK_SIZE, 0);
    if (idx >= 0) {
        active_chunk   = idx;
        current_offset = 0;
    }
    atexit(cleanup_allocator);

    /* Apply working set hard cap (best-effort) */
    if (max_rss > 0) {
#ifdef _WIN32
        /* Requires Windows 8.1+ / Server 2012 R2+ */
        SetProcessWorkingSetSizeEx(
            GetCurrentProcess(),
            MIN_ALLOC_SIZE,
            (SIZE_T)max_rss,
            QUOTA_LIMITS_HARDWS_MAX_ENABLE
        );
#else
        /* Note: modern Linux kernels largely ignore RLIMIT_RSS.
           For strict enforcement, use cgroups (memory.max) externally. */
        printf("mmap_alloc: RLIMIT_RSS works only on Linux <= 2.6.\n");
        printf("mmap_alloc: Instead use this: systemd-run --scope -p MemoryMax=8G -- bash -c \"your_app_here\"\n");
        {
            struct rlimit rl;
            getrlimit(RLIMIT_RSS, &rl);
            rl.rlim_cur = (rlim_t)max_rss;
            setrlimit(RLIMIT_RSS, &rl);
        }
#endif
    }
}

/* ── Free list helpers ── */
static AllocHeader* get_header(void* ptr) {
    return (AllocHeader*)((char*)ptr - sizeof(AllocHeader));
}

static void free_list_remove_chunk(int chunk_index) {
    FreeBlock** pp = &free_list_head;
    while (*pp) {
        AllocHeader* hdr = get_header(*pp);
        if (hdr->chunk_index == chunk_index) {
            *pp = (*pp)->next;
        } else {
            pp = &(*pp)->next;
        }
    }
}

/* ── Public API ── */
void* malloc(size_t size) {
    if (size == 0) size = 1;

    if (!initialized && !initializing) {
        initializing = 1;
        init_allocator();
        initialized  = 1;
        initializing = 0;
    }
    if (!initialized) {
        return bootstrap_alloc(size);
    }

    if (size < MIN_ALLOC_SIZE) size = MIN_ALLOC_SIZE;

    FreeBlock** pp = &free_list_head;
    while (*pp) {
        AllocHeader* hdr = get_header(*pp);
        if ((size_t)hdr->size >= size) {
            void* result = *pp;
            *pp = (*pp)->next;
            chunks[hdr->chunk_index].alloc_count++;
#if MMAP_ALLOC_DEBUG == 1
            dbg_in_use += (size_t)hdr->size;
#endif
            return result;
        }
        pp = &(*pp)->next;
    }

    size_t total = sizeof(AllocHeader) + size;
    if (total > CHUNK_SIZE) {
        int idx = map_new_chunk(total, 1);
        if (idx < 0) return NULL;
        AllocHeader* hdr = (AllocHeader*)chunks[idx].base;
        hdr->size        = (uint64_t)size;
        hdr->chunk_index = (int32_t)idx;
        hdr->_pad        = 0;
        chunks[idx].alloc_count = 1;
#if MMAP_ALLOC_DEBUG == 1
        dbg_in_use += size;
#endif
        return (char*)hdr + sizeof(AllocHeader);
    }

    size_t aligned_off = align_up(current_offset, ALIGN);
    if (active_chunk < 0 || aligned_off + sizeof(AllocHeader) + size > CHUNK_SIZE) {
#ifndef _WIN32
        /* Page out the unused tail of the retiring active chunk so the OS can
           reclaim those physical pages back to the file immediately. */
        if (active_chunk >= 0) {
            uintptr_t tail_start = ((uintptr_t)chunks[active_chunk].base + current_offset
                                    + MMAP_PAGE_SIZE - 1)
                                   & ~((uintptr_t)(MMAP_PAGE_SIZE - 1));
            uintptr_t tail_end   = (uintptr_t)chunks[active_chunk].base + CHUNK_SIZE;
            if (tail_end > tail_start) {
                if (madvise((void*)tail_start, tail_end - tail_start, MADV_PAGEOUT) < 0)
                    madvise((void*)tail_start, tail_end - tail_start, MADV_DONTNEED);
            }
        }
#endif
        int idx = map_new_chunk(CHUNK_SIZE, 0);
        if (idx < 0) return NULL;
        active_chunk = idx;
        current_offset = 0;
        aligned_off = 0;
    }

    char* base = (char*)chunks[active_chunk].base;
    AllocHeader* hdr = (AllocHeader*)(base + aligned_off);
    hdr->size        = (uint64_t)size;
    hdr->chunk_index = (int32_t)active_chunk;
    hdr->_pad        = 0;
    chunks[active_chunk].alloc_count++;
    current_offset = aligned_off + sizeof(AllocHeader) + size;
#if MMAP_ALLOC_DEBUG == 1
    dbg_in_use += size;
#endif
    return (char*)hdr + sizeof(AllocHeader);
}

void free(void* ptr) {
    if (!ptr) return;
    if (is_bootstrap_ptr(ptr)) return;

    AllocHeader* hdr = get_header(ptr);
    int ci = hdr->chunk_index;
    chunks[ci].alloc_count--;
#if MMAP_ALLOC_DEBUG == 1
    dbg_in_use -= (size_t)hdr->size;
#endif

    if (chunks[ci].alloc_count == 0 && ci != active_chunk) {
        free_list_remove_chunk(ci);
        platform_chunk_unmap(chunks[ci].base, chunks[ci].mapped_size);
#if MMAP_ALLOC_DEBUG == 1
        dbg_file_allocated -= chunks[ci].mapped_size;
        dbg_print_stats("free_chunk");
#endif
        if (!chunks[ci].is_oversized) {
            reusable_offsets[reusable_count++] = chunks[ci].file_offset;
        }
        chunks[ci].base = NULL;
    } else {
        FreeBlock* fb = (FreeBlock*)ptr;
        fb->next = free_list_head;
        free_list_head = fb;
        /* Hint OS to page out interior pages to the backing file (best-effort).
           MADV_PAGEOUT (Linux 5.4+) actively evicts dirty MAP_SHARED pages to
           the file. Fall back to MADV_DONTNEED on older kernels.
           The outer size guard is intentionally removed: the math already
           produces an empty range for sub-page blocks, so the madvise is a
           no-op for small allocations without needing an explicit branch. */
        {
            uintptr_t start = ((uintptr_t)ptr + sizeof(FreeBlock) + MMAP_PAGE_SIZE - 1)
                              & ~((uintptr_t)(MMAP_PAGE_SIZE - 1));
            uintptr_t end = ((uintptr_t)ptr + (size_t)hdr->size)
                            & ~((uintptr_t)(MMAP_PAGE_SIZE - 1));
            if (end > start) {
#ifdef _WIN32
                DiscardVirtualMemory((void*)start, end - start);
#else
                if (madvise((void*)start, end - start, MADV_PAGEOUT) < 0)
                    madvise((void*)start, end - start, MADV_DONTNEED);
#endif
            }
        }
    }
}

void* realloc(void* ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) {
        new_size = MIN_ALLOC_SIZE;
    }

    if (is_bootstrap_ptr(ptr)) {
        void* new_ptr = malloc(new_size);
        if (!new_ptr) return NULL;
        size_t safe_copy = (size_t)(bootstrap_buf + MMAP_ALLOC_BOOTSTRAP_SIZE - (char*)ptr);
        if (safe_copy > new_size) safe_copy = new_size;
        memcpy(new_ptr, ptr, safe_copy);
        return new_ptr;
    }

    AllocHeader* hdr = get_header(ptr);
    size_t old_size = (size_t)hdr->size;

    void* new_ptr = malloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    free(ptr);
    return new_ptr;
}

void* calloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) return NULL;
    size_t total = count * size;
    void* p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}
