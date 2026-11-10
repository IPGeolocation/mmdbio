/* What differs between the systems mmdbio runs on: file mapping, positional
 * I/O and temporary files on Windows, process memory, the resident part of
 * a mapped file, and running this program again. */
#ifndef COMPAT_H
#define COMPAT_H

#include <time.h>

#include "common.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/types.h>
/* every open() here passes O_CLOEXEC: on Windows it also means binary */
#undef O_CLOEXEC
#define O_CLOEXEC (_O_BINARY | _O_NOINHERIT)
ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
int fsync(int fd);
struct tm *localtime_r(const time_t *t, struct tm *out);
struct tm *gmtime_r(const time_t *t, struct tm *out);
#define PATH_SEPARATORS "/\\"
#else
#define PATH_SEPARATORS "/"
#endif

/* inet_pton(AF_INET / AF_INET6) with glibc's rules (no leading zeros in
 * IPv4, at most four hex digits a group), the same on every system and
 * without the socket headers: 1 when src is an address, else 0. */
int pton4(const char *src, unsigned char dst[4]);
int pton6(const char *src, unsigned char dst[16]);

/* Remembers argv[0], for finding this program when nothing better does;
 * on Windows, also puts stdin and stdout in binary mode. */
void compat_init(int argc, char **argv);

/* $TMPDIR, else the system's temporary directory. */
const char *temp_dir(void);
/* A new file in dir, read-write, gone once closed (or at exit). */
int temp_file_fd(const char *dir, const char *tag);
/* tmpfile(), in temp_dir(). */
FILE *temp_stream(void);

/* The whole file mapped read-only. */
int map_file(const char *path, const uint8_t **map, size_t *size);
void unmap_file(const uint8_t *map, size_t size);

/* mkdir, 0755, fine when it exists. */
int make_dir(const char *path);
/* Every missing directory above path. */
void make_parent_dirs(const char *path);
/* rename(), replacing to even on Windows. */
int replace_file(const char *from, const char *to);

/* Bytes of [addr, addr + len) (a mapping) that are in memory. */
int mapped_resident_bytes(const void *addr, size_t len, uint64_t *out);
/* Peak resident memory in bytes, and the page faults so far. */
void process_usage(uint64_t *peak_rss, uint64_t *minflt, uint64_t *majflt);
/* Runs this program with args (NULL-terminated, without the program name)
 * and collects its standard output. Fails unless it exits with status 0. */
int run_self(const char *const *args, buf_t *out);

/* Lets the terminal on stream f show ANSI colours (Windows consoles need
 * asking); false when it can't. */
bool terminal_colors(FILE *f);

#endif
