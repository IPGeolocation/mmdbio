#include "compat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <psapi.h>
#else
#include <spawn.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/sysctl.h>
#endif
extern char **environ;
#endif

static const char *argv0 = "mmdbio";

void compat_init(int argc, char **argv)
{
	if (argc > 0 && argv[0] && *argv[0])
		argv0 = argv[0];
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
}

const char *temp_dir(void)
{
	const char *t = getenv("TMPDIR");
	if (t && *t)
		return t;
#ifdef _WIN32
	static char dir[MAX_PATH + 1];
	DWORD n = GetTempPathA(sizeof dir, dir);
	if (n > 0 && n < sizeof dir) {
		while (n > 1 && (dir[n - 1] == '\\' || dir[n - 1] == '/'))
			dir[--n] = 0;
		return dir;
	}
	return ".";
#else
	return "/tmp";
#endif
}

int pton4(const char *src, unsigned char dst[4])
{
	unsigned char tmp[4] = {0}, *tp = tmp;
	int saw_digit = 0, octets = 0, ch;
	while ((ch = (unsigned char)*src++) != 0) {
		if (ch >= '0' && ch <= '9') {
			unsigned nw = *tp * 10u + (unsigned)(ch - '0');
			if (saw_digit && *tp == 0)
				return 0;
			if (nw > 255)
				return 0;
			*tp = (unsigned char)nw;
			if (!saw_digit) {
				if (++octets > 4)
					return 0;
				saw_digit = 1;
			}
		} else if (ch == '.' && saw_digit) {
			if (octets == 4)
				return 0;
			*++tp = 0;
			saw_digit = 0;
		} else {
			return 0;
		}
	}
	if (octets < 4)
		return 0;
	memcpy(dst, tmp, 4);
	return 1;
}

int pton6(const char *src, unsigned char dst[16])
{
	static const char xdigits[] = "0123456789abcdef";
	unsigned char tmp[16], *tp = tmp, *endp = tmp + 16, *colonp = NULL;
	memset(tmp, 0, sizeof tmp);
	if (*src == ':' && *++src != ':')
		return 0;
	const char *curtok = src;
	int saw_xdigit = 0, digits = 0, ch;
	unsigned val = 0;
	while ((ch = (unsigned char)*src++) != 0) {
		if (ch >= 'A' && ch <= 'F')
			ch += 'a' - 'A';
		const char *pch = strchr(xdigits, ch);
		if (pch) {
			val = val << 4 | (unsigned)(pch - xdigits);
			if (++digits > 4)
				return 0;
			saw_xdigit = 1;
			continue;
		}
		if (ch == ':') {
			curtok = src;
			if (!saw_xdigit) {
				if (colonp)
					return 0;
				colonp = tp;
				continue;
			}
			if (*src == 0 || tp + 2 > endp)
				return 0;
			*tp++ = (unsigned char)(val >> 8);
			*tp++ = (unsigned char)val;
			saw_xdigit = digits = 0;
			val = 0;
			continue;
		}
		if (ch == '.' && tp + 4 <= endp && pton4(curtok, tp) > 0) {
			tp += 4;
			saw_xdigit = 0;
			break;
		}
		return 0;
	}
	if (saw_xdigit) {
		if (tp + 2 > endp)
			return 0;
		*tp++ = (unsigned char)(val >> 8);
		*tp++ = (unsigned char)val;
	}
	if (colonp) {
		if (tp == endp)
			return 0;
		int n = (int)(tp - colonp);
		for (int i = 1; i <= n; i++) {
			endp[-i] = colonp[n - i];
			colonp[n - i] = 0;
		}
		tp = endp;
	}
	if (tp != endp)
		return 0;
	memcpy(dst, tmp, 16);
	return 1;
}

void make_parent_dirs(const char *path)
{
	char *p = strdup(path);
	char *c = p + 1;
#ifdef _WIN32
	if (p[0] && p[1] == ':')
		c = p + 2; /* C: */
#endif
	for (; *c; c++) {
		if (!strchr(PATH_SEPARATORS, *c))
			continue;
		char was = *c;
		*c = 0;
		make_dir(p);
		*c = was;
	}
	free(p);
}

#ifdef _WIN32

/* ---- Windows ---- */

static int win_errno(void)
{
	switch (GetLastError()) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
		return ENOENT;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
		return EACCES;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		return EEXIST;
	case ERROR_DISK_FULL:
	case ERROR_HANDLE_DISK_FULL:
		return ENOSPC;
	default:
		return EIO;
	}
}

ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	OVERLAPPED ov;
	memset(&ov, 0, sizeof ov);
	ov.Offset = (DWORD)((uint64_t)off & 0xffffffffu);
	ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
	DWORD got = 0;
	if (n > 0x40000000u)
		n = 0x40000000u;
	if (!ReadFile(h, buf, (DWORD)n, &got, &ov)) {
		if (GetLastError() == ERROR_HANDLE_EOF)
			return 0;
		errno = win_errno();
		return -1;
	}
	return (ssize_t)got;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	OVERLAPPED ov;
	memset(&ov, 0, sizeof ov);
	ov.Offset = (DWORD)((uint64_t)off & 0xffffffffu);
	ov.OffsetHigh = (DWORD)((uint64_t)off >> 32);
	DWORD put = 0;
	if (n > 0x40000000u)
		n = 0x40000000u;
	if (!WriteFile(h, buf, (DWORD)n, &put, &ov)) {
		errno = win_errno();
		return -1;
	}
	return (ssize_t)put;
}

int fsync(int fd) { return _commit(fd); }


struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) == 0 ? out : NULL; }

struct tm *gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) == 0 ? out : NULL; }

int temp_file_fd(const char *dir, const char *tag)
{
	static unsigned counter;
	for (int attempt = 0; attempt < 100; attempt++) {
		char path[MAX_PATH * 2];
		snprintf(path, sizeof path, "%s\\mmdbio-%s-%lu-%u-%u.tmp", dir, tag, (unsigned long)GetCurrentProcessId(),
			 (unsigned)GetTickCount(), counter++);
		HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				       NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
		if (h == INVALID_HANDLE_VALUE) {
			if (GetLastError() == ERROR_FILE_EXISTS)
				continue;
			err_set("create a temporary file in %s: %s (set --tmp-dir)", dir, go_strerror(win_errno()));
			return -1;
		}
		int fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
		if (fd < 0) {
			CloseHandle(h);
			err_set("create a temporary file in %s", dir);
		}
		return fd;
	}
	err_set("create a temporary file in %s: too many attempts", dir);
	return -1;
}

FILE *temp_stream(void)
{
	int fd = temp_file_fd(temp_dir(), "tmp");
	if (fd < 0)
		return NULL;
	FILE *f = _fdopen(fd, "w+b");
	if (!f)
		_close(fd);
	return f;
}

int map_file(const char *path, const uint8_t **map, size_t *size)
{
	HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
			       FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		err_set("open %s: %s", path, go_strerror(win_errno()));
		return -1;
	}
	LARGE_INTEGER sz;
	if (!GetFileSizeEx(h, &sz)) {
		err_set("stat %s: %s", path, go_strerror(win_errno()));
		CloseHandle(h);
		return -1;
	}
	if (sz.QuadPart == 0) {
		err_set("file is empty");
		CloseHandle(h);
		return -1;
	}
	if ((uint64_t)sz.QuadPart > (uint64_t)SIZE_MAX) {
		err_set("%s is too big to map in a 32-bit build", path);
		CloseHandle(h);
		return -1;
	}
	HANDLE m = CreateFileMappingA(h, NULL, PAGE_READONLY, 0, 0, NULL);
	CloseHandle(h);
	if (!m) {
		err_set("mmap %s: %s", path, go_strerror(win_errno()));
		return -1;
	}
	void *v = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
	CloseHandle(m);
	if (!v) {
		err_set("mmap %s: %s", path, go_strerror(win_errno()));
		return -1;
	}
	*map = v;
	*size = (size_t)sz.QuadPart;
	return 0;
}

void unmap_file(const uint8_t *map, size_t size)
{
	(void)size;
	if (map)
		UnmapViewOfFile(map);
}

int make_dir(const char *path) { return _mkdir(path) == 0 || errno == EEXIST ? 0 : -1; }

int replace_file(const char *from, const char *to)
{
	if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
		return 0;
	errno = win_errno();
	return -1;
}

int mapped_resident_bytes(const void *addr, size_t len, uint64_t *out)
{
	(void)addr;
	(void)len;
	*out = 0;
	err_set("measuring a mapping's resident pages is not supported on Windows");
	return -1;
}

void process_usage(uint64_t *peak_rss, uint64_t *minflt, uint64_t *majflt)
{
	PROCESS_MEMORY_COUNTERS pmc;
	memset(&pmc, 0, sizeof pmc);
	GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
	*peak_rss = (uint64_t)pmc.PeakWorkingSetSize;
	*minflt = (uint64_t)pmc.PageFaultCount;
	*majflt = 0;
}

/* one argument for CreateProcess's command line, quoted as the C runtime
 * splits it */
static void quote_arg(buf_t *cmd, const char *a)
{
	if (cmd->len)
		buf_push(cmd, ' ');
	if (*a && !strpbrk(a, " \t\"")) {
		buf_append(cmd, a, strlen(a));
		return;
	}
	buf_push(cmd, '"');
	for (const char *p = a;; p++) {
		size_t slashes = 0;
		while (*p == '\\') {
			slashes++;
			p++;
		}
		if (!*p) {
			for (size_t i = 0; i < slashes * 2; i++)
				buf_push(cmd, '\\');
			break;
		}
		if (*p == '"') {
			for (size_t i = 0; i < slashes * 2 + 1; i++)
				buf_push(cmd, '\\');
		} else {
			for (size_t i = 0; i < slashes; i++)
				buf_push(cmd, '\\');
		}
		buf_push(cmd, (uint8_t)*p);
	}
	buf_push(cmd, '"');
}

int run_self(const char *const *args, buf_t *out)
{
	char exe[MAX_PATH * 2];
	DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
	if (n == 0 || n >= sizeof exe) {
		err_set("can't find this program's path");
		return -1;
	}
	buf_t cmd = {0};
	quote_arg(&cmd, exe);
	for (size_t i = 0; args[i]; i++)
		quote_arg(&cmd, args[i]);
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	HANDLE rd, wr;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		err_set("pipe: %s", go_strerror(win_errno()));
		buf_free(&cmd);
		return -1;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFOA si;
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = wr;
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	PROCESS_INFORMATION pi;
	BOOL ok = CreateProcessA(exe, (char *)buf_cstr(&cmd), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
	CloseHandle(wr);
	buf_free(&cmd);
	if (!ok) {
		err_set("run %s: %s", exe, go_strerror(win_errno()));
		CloseHandle(rd);
		return -1;
	}
	char chunk[4096];
	DWORD got;
	while (ReadFile(rd, chunk, sizeof chunk, &got, NULL) && got > 0)
		buf_append(out, chunk, got);
	CloseHandle(rd);
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (code != 0) {
		err_set("%s %s failed", exe, args[0]);
		return -1;
	}
	return 0;
}

bool terminal_colors(FILE *f)
{
	if (!_isatty(_fileno(f)))
		return false;
	HANDLE h = GetStdHandle(f == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
	DWORD mode;
	if (!GetConsoleMode(h, &mode))
		return false;
	if (mode & 0x0004) /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */
		return true;
	return SetConsoleMode(h, mode | 0x0004) != 0;
}

#else

/* ---- POSIX ---- */

int temp_file_fd(const char *dir, const char *tag)
{
	buf_t p = {0};
	buf_printf(&p, "%s/mmdbio-%s-XXXXXX", dir, tag);
	char *path = (char *)buf_cstr(&p);
	int fd = mkstemp(path);
	if (fd < 0) {
		err_set("create a temporary file in %s: %s (set --tmp-dir)", dir, go_strerror(errno));
		buf_free(&p);
		return -1;
	}
	unlink(path);
	buf_free(&p);
	return fd;
}

FILE *temp_stream(void) { return tmpfile(); }

int map_file(const char *path, const uint8_t **map, size_t *size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		err_set("open %s: %s", path, go_strerror(errno));
		return -1;
	}
	struct stat st;
	if (fstat(fd, &st) < 0) {
		err_set("stat %s: %s", path, go_strerror(errno));
		close(fd);
		return -1;
	}
	if (st.st_size == 0) {
		err_set("file is empty");
		close(fd);
		return -1;
	}
	if ((uint64_t)st.st_size > (uint64_t)SIZE_MAX) {
		err_set("%s is too big to map in a 32-bit build", path);
		close(fd);
		return -1;
	}
	void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) {
		err_set("mmap %s: %s", path, go_strerror(errno));
		return -1;
	}
	*map = m;
	*size = (size_t)st.st_size;
	return 0;
}

void unmap_file(const uint8_t *map, size_t size)
{
	if (map)
		munmap((void *)map, size);
}

int make_dir(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1; }

int replace_file(const char *from, const char *to) { return rename(from, to); }

#ifdef __linux__
/* The mapping's Rss in /proc/self/smaps: the pages this process touched,
 * where mincore would count every cached page of the file. */
static int smaps_rss(const void *addr, uint64_t *out)
{
	FILE *f = fopen("/proc/self/smaps", "r");
	if (!f)
		return -1;
	char line[512];
	bool in = false;
	int rc = -1;
	while (fgets(line, sizeof line, f)) {
		unsigned long long lo, hi;
		char c;
		if (sscanf(line, "%llx-%llx %c", &lo, &hi, &c) == 3 && strchr(line, '-') < strchr(line, ' ')) {
			in = lo == (unsigned long long)(uintptr_t)addr;
			continue;
		}
		unsigned long long kb;
		if (in && sscanf(line, "Rss: %llu kB", &kb) == 1) {
			*out = (uint64_t)kb * 1024;
			rc = 0;
			break;
		}
	}
	fclose(f);
	return rc;
}
#endif

int mapped_resident_bytes(const void *addr, size_t len, uint64_t *out)
{
#ifdef __linux__
	if (smaps_rss(addr, out) == 0)
		return 0;
#endif
#if defined(__OpenBSD__)
	(void)addr;
	(void)len;
	*out = 0;
	err_set("mincore is not available on this system");
	return -1;
#else
	long page = sysconf(_SC_PAGESIZE);
	if (page <= 0)
		page = 4096;
	size_t pages = (len + (size_t)page - 1) / (size_t)page;
	unsigned char *vec = xmalloc(pages + 1);
	if (mincore((void *)addr, len, (void *)vec) < 0) {
		err_set("mincore: %s", go_strerror(errno));
		free(vec);
		return -1;
	}
	uint64_t n = 0;
	for (size_t i = 0; i < pages; i++)
		n += vec[i] & 1;
	free(vec);
	*out = n * (uint64_t)page;
	return 0;
#endif
}

void process_usage(uint64_t *peak_rss, uint64_t *minflt, uint64_t *majflt)
{
	struct rusage ru;
	memset(&ru, 0, sizeof ru);
	getrusage(RUSAGE_SELF, &ru);
	*minflt = (uint64_t)ru.ru_minflt;
	*majflt = (uint64_t)ru.ru_majflt;
#ifdef __linux__
	/* ru_maxrss survives exec on Linux (a spawned child would report its
	 * parent's peak); VmHWM belongs to this program's memory alone */
	FILE *f = fopen("/proc/self/status", "r");
	if (f) {
		char line[256];
		unsigned long long kb;
		bool got = false;
		while (!got && fgets(line, sizeof line, f))
			got = sscanf(line, "VmHWM: %llu kB", &kb) == 1;
		fclose(f);
		if (got) {
			*peak_rss = (uint64_t)kb * 1024;
			return;
		}
	}
#endif
#ifdef __APPLE__
	*peak_rss = (uint64_t)ru.ru_maxrss; /* bytes */
#else
	*peak_rss = (uint64_t)ru.ru_maxrss * 1024; /* kilobytes */
#endif
}

/* this program's path: the system's answer, else argv[0] (looked up in
 * $PATH when it has no slash) */
static void self_path(buf_t *out)
{
	out->len = 0;
#if defined(__linux__)
	char p[PATH_MAX];
	ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
	if (n > 0) {
		buf_append(out, p, (size_t)n);
		buf_cstr(out);
		return;
	}
#elif defined(__APPLE__)
	char p[PATH_MAX];
	uint32_t size = sizeof p;
	if (_NSGetExecutablePath(p, &size) == 0) {
		buf_append(out, p, strlen(p));
		buf_cstr(out);
		return;
	}
#elif defined(__FreeBSD__) || defined(__DragonFly__)
	int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
	char p[PATH_MAX];
	size_t size = sizeof p;
	if (sysctl(mib, 4, p, &size, NULL, 0) == 0) {
		buf_append(out, p, strlen(p));
		buf_cstr(out);
		return;
	}
#elif defined(__NetBSD__)
	char p[PATH_MAX];
	ssize_t n = readlink("/proc/curproc/exe", p, sizeof p - 1);
	if (n > 0) {
		buf_append(out, p, (size_t)n);
		buf_cstr(out);
		return;
	}
#endif
	if (strchr(argv0, '/')) {
		buf_append(out, argv0, strlen(argv0));
		buf_cstr(out);
		return;
	}
	const char *path = getenv("PATH");
	for (const char *d = path ? path : ""; *d;) {
		const char *colon = strchr(d, ':');
		size_t n = colon ? (size_t)(colon - d) : strlen(d);
		out->len = 0;
		buf_append(out, n ? d : ".", n ? n : 1);
		buf_printf(out, "/%s", argv0);
		if (access(buf_cstr(out), X_OK) == 0)
			return;
		if (!colon)
			break;
		d = colon + 1;
	}
	out->len = 0;
	buf_append(out, argv0, strlen(argv0));
	buf_cstr(out);
}

int run_self(const char *const *args, buf_t *out)
{
	buf_t exe = {0};
	self_path(&exe);
	size_t n = 0;
	while (args[n])
		n++;
	char **argv = xcalloc(n + 2, sizeof *argv);
	argv[0] = (char *)buf_cstr(&exe);
	for (size_t i = 0; i < n; i++)
		argv[i + 1] = (char *)args[i];
	int fds[2];
	int rc = -1;
	if (pipe(fds) < 0) {
		err_set("pipe: %s", go_strerror(errno));
		goto done;
	}
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
	posix_spawn_file_actions_addclose(&fa, fds[0]);
	posix_spawn_file_actions_addclose(&fa, fds[1]);
	pid_t pid;
	int e = posix_spawn(&pid, argv[0], &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (e != 0) {
		err_set("run %s: %s", argv[0], go_strerror(e));
		close(fds[0]);
		goto done;
	}
	uint8_t chunk[4096];
	for (;;) {
		ssize_t got = read(fds[0], chunk, sizeof chunk);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			break;
		buf_append(out, chunk, (size_t)got);
	}
	close(fds[0]);
	int status;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		err_set("%s %s failed", argv[0], args[0]);
		goto done;
	}
	rc = 0;
done:
	free(argv);
	buf_free(&exe);
	return rc;
}

bool terminal_colors(FILE *f) { return isatty(fileno(f)); }

#endif
