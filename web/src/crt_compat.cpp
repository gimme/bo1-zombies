// MSVC CRT functions the engine uses that musl lacks (secure string functions, _itoa & co., 64-bit time), and the
// CRT file calls whose paths need Windows separators normalized (bo1_web_prelude.h routes fopen / rename / _mkdir
// here).
#include <windows.h>

#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "web_platform.h"

#undef fopen
#undef rename
#undef _mkdir

// ----- reading the install through a cache -----
// Every read() of a file under /game is a synchronous call to the WasmFS backend's thread (library_bo1.js), about a
// tenth of a millisecond even when the data is there. The engine reads its iwds through stdio in small pieces with a
// seek before each zip entry, so stdio streams on install files read through this cache instead: spans of the files
// kept in memory (the least recently used dropped), each filled with one large read from where the engine reads, of up
// to kSpanBytes: as much as the browser's store gives from there on, the entry the engine wants and what follows it
// that the map's list has too (a read gets fewer bytes where the store has no more just then).

namespace
{
const uint32_t kSpanBytes = 256 * 1024;
const size_t kCacheBudget = 128u << 20;

struct CachedSpan
{
    std::vector<uint8_t> data;
    uint64_t lastUse;
};

std::mutex s_cacheLock;
std::unordered_map<std::string, uint32_t> s_cacheFileIds;
std::unordered_map<uint32_t, std::map<int64_t, CachedSpan>> s_cacheSpans; // by file id, by where each starts
size_t s_cacheBytes;
uint64_t s_cacheClock;

struct CachedFile
{
    int fd;
    uint32_t id;
    int64_t size;
    int64_t pos;
    std::string path;
};

// copy what the cache has of [pos, pos + n) at the start of that range; 0 when no span has pos
size_t CacheCopy(CachedFile *f, char *buf, size_t n)
{
    auto file = s_cacheSpans.find(f->id);
    if (file == s_cacheSpans.end())
        return 0;
    auto it = file->second.upper_bound(f->pos);
    if (it == file->second.begin())
        return 0;
    --it;
    int64_t off = f->pos - it->first;
    if (off >= (int64_t)it->second.data.size())
        return 0;
    it->second.lastUse = ++s_cacheClock;
    size_t len = (std::min)(n, it->second.data.size() - (size_t)off);
    memcpy(buf, it->second.data.data() + off, len);
    return len;
}

ssize_t CachedRead(void *cookie, char *buf, size_t n)
{
    CachedFile *f = (CachedFile *)cookie;
    size_t done = 0;
    while (done < n && f->pos < f->size)
    {
        size_t got;
        {
            std::lock_guard<std::mutex> guard(s_cacheLock);
            got = CacheCopy(f, buf + done, n - done);
        }
        if (!got)
        {
            // fill a span from pos (outside the lock: other threads keep reading meanwhile), with one read: the store
            // gives what it has from there, the rest of the span past it being no concern of the engine's
            int64_t start = f->pos;
            std::vector<uint8_t> data((size_t)(std::min<int64_t>)(kSpanBytes, f->size - start));
            double began = Bo1_Milliseconds();
            ssize_t r = pread(f->fd, data.data(), data.size(), start);
            Bo1_NoteFileRead(f->path.c_str(), Bo1_Milliseconds() - began);
            if (r <= 0)
                break;
            size_t have = (size_t)r;
            data.resize(have);
            std::lock_guard<std::mutex> guard(s_cacheLock);
            auto &spans = s_cacheSpans[f->id];
            if (!spans.count(start))
            {
                while (s_cacheBytes + have > kCacheBudget && s_cacheBytes > 0)
                {
                    std::map<int64_t, CachedSpan> *oldestFile = nullptr;
                    std::map<int64_t, CachedSpan>::iterator oldest;
                    for (auto &[id, fileSpans] : s_cacheSpans)
                        for (auto it = fileSpans.begin(); it != fileSpans.end(); ++it)
                            if (!oldestFile || it->second.lastUse < oldest->second.lastUse)
                                oldestFile = &fileSpans, oldest = it;
                    if (!oldestFile)
                        break;
                    s_cacheBytes -= oldest->second.data.size();
                    oldestFile->erase(oldest);
                }
                s_cacheBytes += have;
                spans[start] = CachedSpan{std::move(data), ++s_cacheClock};
            }
            got = CacheCopy(f, buf + done, n - done);
            if (!got)
                break;
        }
        done += got;
        f->pos += got;
    }
    return (ssize_t)done;
}

int CachedSeek(void *cookie, off64_t *offset, int whence)
{
    CachedFile *f = (CachedFile *)cookie;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? f->pos : f->size;
    int64_t pos = base + *offset;
    if (pos < 0)
        return -1;
    f->pos = pos;
    *offset = pos;
    return 0;
}

int CachedClose(void *cookie)
{
    CachedFile *f = (CachedFile *)cookie;
    close(f->fd);
    delete f;
    return 0;
}

FILE *OpenCached(const std::string &resolved)
{
    int fd = open(resolved.c_str(), O_RDONLY);
    if (fd < 0)
        return nullptr;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        close(fd);
        return nullptr;
    }
    CachedFile *f = new CachedFile{fd, 0, (int64_t)st.st_size, 0, resolved};
    {
        std::lock_guard<std::mutex> guard(s_cacheLock);
        auto it = s_cacheFileIds.find(resolved);
        if (it == s_cacheFileIds.end())
            it = s_cacheFileIds.emplace(resolved, (uint32_t)s_cacheFileIds.size() + 1).first;
        f->id = it->second;
    }
    cookie_io_functions_t io = {CachedRead, nullptr, CachedSeek, CachedClose};
    FILE *file = fopencookie(f, "rb", io);
    if (!file)
    {
        CachedClose(f);
        return nullptr;
    }
    // Unbuffered: stdio would read up to a buffer (1 KB) ahead of what the engine asks, past the end of a zip entry
    // into the next, which the browser's store may not have. The cache serves the small reads as fast anyway. So a
    // game reads only what it asks for, and all of a map's reads are in the server's list of what it reads
    // (zombies-hub's server/files.go), which a browser downloads ahead and keeps.
    setvbuf(file, nullptr, _IONBF, 0);
    return file;
}
} // namespace

extern "C" FILE *bo1_fopen(const char *path, const char *mode)
{
    bool write = strpbrk(mode, "wa+") != nullptr;
    std::string resolved = Bo1_ResolvePath(path, write);
    if (write && (strchr(mode, 'a') || strchr(mode, '+')))
    {
        // appending to / updating a file that so far exists only in the install: start from its contents
        std::string src = Bo1_ResolvePath(path, false);
        if (src != resolved && access(resolved.c_str(), F_OK) != 0 && access(src.c_str(), F_OK) == 0)
            CopyFileA(src.c_str(), resolved.c_str(), TRUE);
    }
    FILE *f;
    if (!write && strchr(mode, 't'))
    {
        // MSVC text mode reads CRLF as LF (localization.txt: the language name would end in CR); musl has no
        // text mode, so the file is read and converted into a memory stream
        f = fopen(resolved.c_str(), "rb");
        if (f)
        {
            std::string text;
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                text.append(buf, n);
            fclose(f);
            std::string out;
            out.reserve(text.size());
            for (size_t i = 0; i < text.size(); ++i)
            {
                if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                    continue;
                out.push_back(text[i]);
            }
            f = fmemopen(nullptr, out.size() + 1, "w+");
            if (f)
            {
                fwrite(out.data(), 1, out.size(), f);
                rewind(f);
            }
        }
    }
    else if (!write && !strncmp(resolved.c_str(), BO1_WEB_GAME_ROOT "/", sizeof(BO1_WEB_GAME_ROOT)))
    {
        f = OpenCached(resolved);
    }
    else
    {
        f = fopen(resolved.c_str(), mode);
    }
    if (Bo1_LogFiles())
        Bo1_Log("fopen %s [%s] -> %s%s", path, mode, resolved.c_str(), f ? "" : " FAILED");
    return f;
}

extern "C" int bo1_rename(const char *from, const char *to)
{
    return rename(Bo1_ResolvePath(from, false).c_str(), Bo1_ResolvePath(to, true).c_str());
}

extern "C" int bo1_mkdir(const char *path) { return mkdir(Bo1_ResolvePath(path, true).c_str(), 0777); }

extern "C" errno_t fopen_s(FILE **pFile, const char *filename, const char *mode)
{
    *pFile = bo1_fopen(filename, mode);
    return *pFile ? 0 : errno;
}

extern "C" void *bo1_aligned_malloc(size_t size, size_t align)
{
    if (align < sizeof(void *))
        align = sizeof(void *);
    void *p = nullptr;
    return posix_memalign(&p, align, size ? size : 1) ? nullptr : p;
}

extern "C" void *_aligned_realloc(void *memblock, size_t size, size_t alignment)
{
    void *p = bo1_aligned_malloc(size, alignment);
    if (p && memblock)
    {
        size_t old = malloc_usable_size(memblock);
        memcpy(p, memblock, old < size ? old : size);
        free(memblock);
    }
    return p;
}

extern "C" size_t bo1_msize(void *p) { return p ? malloc_usable_size(p) : 0; }

extern "C" int bo1_memicmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; ++i)
    {
        int d = tolower(x[i]) - tolower(y[i]);
        if (d)
            return d;
    }
    return 0;
}

static char *Bo1_ToRadix(unsigned long long value, bool negative, char *str, int radix)
{
    char tmp[72];
    int n = 0;
    if (radix < 2 || radix > 36)
        radix = 10;
    do
    {
        int d = (int)(value % (unsigned)radix);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        value /= (unsigned)radix;
    } while (value);
    char *p = str;
    if (negative)
        *p++ = '-';
    while (n)
        *p++ = tmp[--n];
    *p = 0;
    return str;
}

extern "C" char *_itoa(int value, char *str, int radix)
{
    if (radix == 10 && value < 0)
        return Bo1_ToRadix((unsigned long long)(-(long long)value), true, str, radix);
    return Bo1_ToRadix((unsigned int)value, false, str, radix);
}

extern "C" char *_ltoa(long value, char *str, int radix) { return _itoa((int)value, str, radix); }
extern "C" char *_ultoa(unsigned long value, char *str, int radix) { return Bo1_ToRadix(value, false, str, radix); }

extern "C" char *_i64toa(long long value, char *str, int radix)
{
    if (radix == 10 && value < 0)
        return Bo1_ToRadix((unsigned long long)(-value), true, str, radix);
    return Bo1_ToRadix((unsigned long long)value, false, str, radix);
}

extern "C" char *_ui64toa(unsigned long long value, char *str, int radix) { return Bo1_ToRadix(value, false, str, radix); }

extern "C" errno_t _itoa_s(int value, char *buffer, size_t size, int radix)
{
    char tmp[40];
    _itoa(value, tmp, radix);
    if (strlen(tmp) + 1 > size)
        return ERANGE;
    strcpy(buffer, tmp);
    return 0;
}

extern "C" errno_t _ltoa_s(long value, char *buffer, size_t size, int radix) { return _itoa_s((int)value, buffer, size, radix); }

// MSVC's rand: a linear congruential generator per thread, 15 bits out
static thread_local unsigned int t_randSeed = 1;

extern "C" int bo1_rand(void)
{
    t_randSeed = t_randSeed * 214013u + 2531011u;
    return (int)((t_randSeed >> 16) & 0x7FFF);
}

extern "C" void bo1_srand(unsigned int seed) { t_randSeed = seed; }

// MSVC's qsort: the CRT's quicksort (median of three, a selection sort below 9 elements, an explicit stack), so equal
// elements end in the order they do on Windows. musl's qsort orders them differently, and the network's Huffman tree
// (Huff_BuildFromData) is sorted with qsort: with musl's a web client misread some bytes the server sent.
// Checked against the Windows CRT's on random arrays full of equal keys.
static void bo1_qsort_swap(char *a, char *b, size_t width)
{
    if (a != b)
    {
        while (width--)
        {
            char tmp = *a;
            *a++ = *b;
            *b++ = tmp;
        }
    }
}

static void bo1_qsort_shortsort(char *lo, char *hi, size_t width, int (*comp)(const void *, const void *))
{
    while (hi > lo)
    {
        char *max = lo;
        for (char *p = lo + width; p <= hi; p += width)
        {
            if (comp(p, max) > 0)
                max = p;
        }
        bo1_qsort_swap(max, hi, width);
        hi -= width;
    }
}

extern "C" void bo1_qsort(void *base, size_t num, size_t width, int (*comp)(const void *, const void *))
{
    enum { CUTOFF = 8, STKSIZ = 8 * sizeof(void *) - 2 };
    char *lostk[STKSIZ], *histk[STKSIZ];
    int stkptr = 0;

    if (num < 2 || !width)
        return;
    char *lo = (char *)base;
    char *hi = (char *)base + width * (num - 1);

recurse:
    size_t size = (hi - lo) / width + 1;
    if (size <= CUTOFF)
    {
        bo1_qsort_shortsort(lo, hi, width, comp);
    }
    else
    {
        char *mid = lo + (size / 2) * width;
        if (comp(lo, mid) > 0)
            bo1_qsort_swap(lo, mid, width);
        if (comp(lo, hi) > 0)
            bo1_qsort_swap(lo, hi, width);
        if (comp(mid, hi) > 0)
            bo1_qsort_swap(mid, hi, width);

        char *loguy = lo;
        char *higuy = hi;
        for (;;)
        {
            if (mid > loguy)
            {
                do
                    loguy += width;
                while (loguy < mid && comp(loguy, mid) <= 0);
            }
            if (mid <= loguy)
            {
                do
                    loguy += width;
                while (loguy <= hi && comp(loguy, mid) <= 0);
            }
            do
                higuy -= width;
            while (higuy > mid && comp(higuy, mid) > 0);
            if (higuy < loguy)
                break;
            bo1_qsort_swap(loguy, higuy, width);
            if (mid == higuy)
                mid = loguy;
        }

        higuy += width;
        if (mid < higuy)
        {
            do
                higuy -= width;
            while (higuy > mid && comp(higuy, mid) == 0);
        }
        if (mid >= higuy)
        {
            do
                higuy -= width;
            while (higuy > lo && comp(higuy, mid) == 0);
        }

        if (higuy - lo >= hi - loguy)
        {
            if (lo < higuy)
            {
                lostk[stkptr] = lo;
                histk[stkptr] = higuy;
                ++stkptr;
            }
            if (loguy < hi)
            {
                lo = loguy;
                goto recurse;
            }
        }
        else
        {
            if (loguy < hi)
            {
                lostk[stkptr] = loguy;
                histk[stkptr] = hi;
                ++stkptr;
            }
            if (lo < higuy)
            {
                hi = higuy;
                goto recurse;
            }
        }
    }

    --stkptr;
    if (stkptr >= 0)
    {
        lo = lostk[stkptr];
        hi = histk[stkptr];
        goto recurse;
    }
}

extern "C" char *_strlwr(char *s)
{
    for (char *p = s; *p; ++p)
        *p = (char)tolower((unsigned char)*p);
    return s;
}

extern "C" char *_strupr(char *s)
{
    for (char *p = s; *p; ++p)
        *p = (char)toupper((unsigned char)*p);
    return s;
}

extern "C" errno_t _strlwr_s(char *s, size_t)
{
    _strlwr(s);
    return 0;
}

extern "C" errno_t _strupr_s(char *s, size_t)
{
    _strupr(s);
    return 0;
}

extern "C" char *_strrev(char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; ++i)
    {
        char c = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = c;
    }
    return s;
}

extern "C" int _putenv(const char *envstring)
{
    const char *eq = strchr(envstring, '=');
    if (!eq)
        return -1;
    std::string name(envstring, eq - envstring);
    return eq[1] ? setenv(name.c_str(), eq + 1, 1) : unsetenv(name.c_str());
}

extern "C" int _vscprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    return n;
}

extern "C" int _scprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = _vscprintf(fmt, ap);
    va_end(ap);
    return n;
}

extern "C" long _filelength(int fd)
{
    struct stat st;
    return fstat(fd, &st) ? -1 : (long)st.st_size;
}

extern "C" void _splitpath(const char *path, char *drive, char *dir, char *fname, char *ext)
{
    std::string p = Bo1_NormalizePath(path);
    if (drive)
        drive[0] = 0;
    size_t slash = p.rfind('/');
    std::string d = slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
    std::string f = slash == std::string::npos ? p : p.substr(slash + 1);
    size_t dot = f.rfind('.');
    std::string e = dot == std::string::npos ? std::string() : f.substr(dot);
    if (dot != std::string::npos)
        f = f.substr(0, dot);
    if (dir)
        strcpy(dir, d.c_str());
    if (fname)
        strcpy(fname, f.c_str());
    if (ext)
        strcpy(ext, e.c_str());
}

extern "C" void _makepath(char *path, const char *drive, const char *dir, const char *fname, const char *ext)
{
    path[0] = 0;
    if (drive && *drive)
        strcat(path, drive);
    if (dir && *dir)
    {
        strcat(path, dir);
        char last = dir[strlen(dir) - 1];
        if (last != '/' && last != '\\')
            strcat(path, "/");
    }
    if (fname)
        strcat(path, fname);
    if (ext && *ext)
    {
        if (*ext != '.')
            strcat(path, ".");
        strcat(path, ext);
    }
}

extern "C" char *_fullpath(char *absPath, const char *relPath, size_t maxLength)
{
    if (!absPath)
    {
        maxLength = MAX_PATH;
        absPath = (char *)malloc(maxLength);
    }
    return GetFullPathNameA(relPath, (DWORD)maxLength, absPath, nullptr) ? absPath : nullptr;
}

extern "C" int _kbhit(void) { return 0; }
extern "C" int _getch(void) { return 0; }
extern "C" unsigned int _controlfp(unsigned int, unsigned int) { return 0; }
extern "C" unsigned int _control87(unsigned int, unsigned int) { return 0; }
extern "C" void _fpreset(void) {}

// ----- secure CRT -----
// On a too-small buffer MSVC's invalid-parameter handler would end the process; here the result is truncated and
// the error returned, which every engine caller tolerates.

extern "C" int vsprintf_s(char *buffer, size_t size, const char *format, va_list ap)
{
    if (!buffer || !size)
        return -1;
    int n = vsnprintf(buffer, size, format, ap);
    if (n < 0 || (size_t)n >= size)
    {
        buffer[size - 1] = 0;
        return -1;
    }
    return n;
}

extern "C" int sprintf_s(char *buffer, size_t size, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = vsprintf_s(buffer, size, format, ap);
    va_end(ap);
    return n;
}

extern "C" int _vsnprintf_s(char *buffer, size_t size, size_t count, const char *format, va_list ap)
{
    if (!buffer || !size)
        return -1;
    size_t limit = count == _TRUNCATE || count + 1 > size ? size : count + 1;
    int n = vsnprintf(buffer, limit, format, ap);
    if (n < 0 || (size_t)n >= limit)
    {
        buffer[limit - 1] = 0;
        return -1;
    }
    return n;
}

extern "C" int vsnprintf_s(char *buffer, size_t size, size_t count, const char *format, va_list ap)
{
    return _vsnprintf_s(buffer, size, count, format, ap);
}

extern "C" int _snprintf_s(char *buffer, size_t size, size_t count, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int n = _vsnprintf_s(buffer, size, count, format, ap);
    va_end(ap);
    return n;
}

extern "C" errno_t strcpy_s(char *dest, size_t size, const char *src)
{
    if (!dest || !size)
        return EINVAL;
    I_strncpyz_web(dest, src ? src : "", size);
    return src && strlen(src) >= size ? ERANGE : 0;
}

extern "C" errno_t strncpy_s(char *dest, size_t size, const char *src, size_t count)
{
    if (!dest || !size)
        return EINVAL;
    size_t n = strnlen(src ? src : "", count == _TRUNCATE ? size - 1 : count);
    if (n >= size)
        n = size - 1;
    memcpy(dest, src, n);
    dest[n] = 0;
    return 0;
}

extern "C" errno_t strcat_s(char *dest, size_t size, const char *src)
{
    size_t len = strnlen(dest, size);
    if (len >= size)
        return EINVAL;
    I_strncpyz_web(dest + len, src, size - len);
    return 0;
}

extern "C" errno_t strncat_s(char *dest, size_t size, const char *src, size_t count)
{
    size_t len = strnlen(dest, size);
    if (len >= size)
        return EINVAL;
    size_t n = strnlen(src, count == _TRUNCATE ? size : count);
    if (len + n + 1 > size)
        n = size - len - 1;
    memcpy(dest + len, src, n);
    dest[len + n] = 0;
    return 0;
}

extern "C" errno_t memcpy_s(void *dest, size_t destsz, const void *src, size_t count)
{
    if (count > destsz)
        return ERANGE;
    memcpy(dest, src, count);
    return 0;
}

extern "C" errno_t memmove_s(void *dest, size_t destsz, const void *src, size_t count)
{
    if (count > destsz)
        return ERANGE;
    memmove(dest, src, count);
    return 0;
}

extern "C" char *strtok_s(char *strToken, const char *strDelimit, char **context) { return strtok_r(strToken, strDelimit, context); }
extern "C" size_t strnlen_s(const char *str, size_t n) { return str ? strnlen(str, n) : 0; }

extern "C" int sscanf_s(const char *buffer, const char *format, ...)
{
    // the engine's sscanf_s calls read numbers only (no %s / %c, which would carry buffer sizes)
    va_list ap;
    va_start(ap, format);
    int n = vsscanf(buffer, format, ap);
    va_end(ap);
    return n;
}

extern "C" errno_t _dupenv_s(char **buffer, size_t *numberOfElements, const char *varname)
{
    const char *v = getenv(varname);
    *buffer = v ? strdup(v) : nullptr;
    if (numberOfElements)
        *numberOfElements = v ? strlen(v) + 1 : 0;
    return 0;
}

extern "C" errno_t getenv_s(size_t *pReturnValue, char *buffer, size_t size, const char *varname)
{
    const char *v = getenv(varname);
    *pReturnValue = v ? strlen(v) + 1 : 0;
    if (v && buffer && size)
        I_strncpyz_web(buffer, v, size);
    else if (buffer && size)
        buffer[0] = 0;
    return 0;
}

extern "C" errno_t _get_timezone(long *seconds)
{
    *seconds = 0;
    return 0;
}

// ----- 64-bit time (MSVC's time_t is 64-bit; so is Emscripten's) -----

extern "C" long long _time64(long long *t)
{
    long long now = (long long)time(nullptr);
    if (t)
        *t = now;
    return now;
}

extern "C" long long _mktime64(struct tm *t) { return (long long)mktime(t); }

extern "C" struct tm *_localtime64(const long long *t)
{
    time_t tt = (time_t)*t;
    return localtime(&tt);
}

extern "C" struct tm *_gmtime64(const long long *t)
{
    time_t tt = (time_t)*t;
    return gmtime(&tt);
}

extern "C" char *_ctime64(const long long *t)
{
    time_t tt = (time_t)*t;
    return ctime(&tt);
}

extern "C" errno_t localtime_s(struct tm *tmDest, const long long *sourceTime)
{
    time_t tt = (time_t)*sourceTime;
    return localtime_r(&tt, tmDest) ? 0 : EINVAL;
}

extern "C" errno_t _localtime64_s(struct tm *tmDest, const long long *sourceTime) { return localtime_s(tmDest, sourceTime); }

extern "C" errno_t _gmtime64_s(struct tm *tmDest, const long long *sourceTime)
{
    time_t tt = (time_t)*sourceTime;
    return gmtime_r(&tt, tmDest) ? 0 : EINVAL;
}

extern "C" errno_t ctime_s(char *buffer, size_t size, const long long *sourceTime)
{
    time_t tt = (time_t)*sourceTime;
    char tmp[32];
    ctime_r(&tt, tmp);
    I_strncpyz_web(buffer, tmp, size);
    return 0;
}

extern "C" errno_t _ctime64_s(char *buffer, size_t size, const long long *sourceTime) { return ctime_s(buffer, size, sourceTime); }

extern "C" errno_t asctime_s(char *buffer, size_t size, const struct tm *tmSource)
{
    char tmp[32];
    asctime_r(tmSource, tmp);
    I_strncpyz_web(buffer, tmp, size);
    return 0;
}
