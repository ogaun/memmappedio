#include <string>
#include <vector>
#include <windows.h>

static double Count2MSec = 0;
static __int64 TimeOrigin = 0;
static double now()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    __int64 x = li.QuadPart;
    if (!TimeOrigin)
    {
        TimeOrigin = li.QuadPart;
        LARGE_INTEGER li;
        if (QueryPerformanceFrequency(&li) && li.u.LowPart)
            Count2MSec = (double)1000.0 / li.u.LowPart;
        else
            Count2MSec = 1;
    }
    return (x-TimeOrigin) * Count2MSec;
}

static void get_cpu_user_time_ms(double& usr, double& sys)
{
    FILETIME a, b, c, d;
    if (GetProcessTimes(GetCurrentProcess(), &a, &b, &c, &d) != 0) {
        usr = 1e-6* (double)(d.dwLowDateTime | ((uint64_t)d.dwHighDateTime << 32)) * 100;
        sys = 1e-6* (double)(c.dwLowDateTime | ((uint64_t)c.dwHighDateTime << 32)) * 100;
    }
    else 
        usr = 0, sys = 0;
}

class BenchSection
{
    double start, start_usr, start_sys;
    const std::string str, pfx;
public:
    BenchSection(const char* const str, const char * pfx) : pfx(pfx), str(str), start(now())
    {
        get_cpu_user_time_ms(start_usr, start_sys);
    }

    ~BenchSection() {
        double elapsed = m();
        double usr, sys;
        get_cpu_user_time_ms(usr, sys);
        usr -= start_usr;
        sys -= start_sys;
        printf("%s%-20s\t%10.1f ms\t%10.1f ms CPU\t%10.1f ms USR\t%10.1f ms SYS\n", pfx.c_str(), str.c_str(), elapsed, usr + sys, usr, sys);
    }
    double m() const {
        return now() - start;
    }
};

class Mapping
{
    HANDLE hFile = INVALID_HANDLE_VALUE;
    HANDLE hMapFile = INVALID_HANDLE_VALUE;
    char* buf = nullptr;
    size_t sz = 0;
public:
    Mapping(HANDLE hFile) : hFile(hFile)
    {}

    ~Mapping()
    {
        close();
    }

    int map(size_t sz)
    {
        this->sz = sz;
        hMapFile = CreateFileMapping(
            hFile,
            NULL,                    // default security
            PAGE_READWRITE,          // read/write access
            sz >> 32,
            sz & 0xffffffff,
            NULL /*szName*/);       // name of mapping object

        if (hMapFile == NULL || hMapFile == INVALID_HANDLE_VALUE)
            return printf("Could not create file mapping object (%d).\n", GetLastError()), -1;

        buf = (char*)MapViewOfFile(hMapFile, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, sz);
        if (buf == NULL)
            return printf("Could not create file view object (%d).\n", GetLastError()), -2;
        return 0;
    }

    bool flush_view_of_file() const
    {
        BOOL r = ::FlushViewOfFile(buf, sz);
        return r;
    }

    bool flush_file_buffers() const
    {
        BOOL r = ::FlushFileBuffers(hFile);
        return r;
    }

    long long file_size() const
    {
        if (hFile == INVALID_HANDLE_VALUE)
            return -1;

        LARGE_INTEGER li;
        if (!GetFileSizeEx(hFile, &li))
            return -1;
        return li.QuadPart;
    }

    long long set_end_of_file(long long x)
    {
        if (hFile == INVALID_HANDLE_VALUE)
            return -1;
        LARGE_INTEGER li; li.QuadPart = x;
        if (!SetFilePointerEx(hFile, li, nullptr, FILE_BEGIN))
            return printf("SetFilePointerEx() failed (0x%llx) %u", x, GetLastError()), -1;
        if (!SetEndOfFile(hFile))
            return printf("SetEndOfFile() failed (0x%llx) %u", x, GetLastError()), -2;
        return x;
    }

    char* addr() {
        return buf;
    }

    void close()
    {
        if (hMapFile != INVALID_HANDLE_VALUE)
        {
            if (buf)
                UnmapViewOfFile(buf);
            buf = nullptr;
            CloseHandle(hMapFile);
            hMapFile = INVALID_HANDLE_VALUE;
        }
    }
};

static void message(const char* msg)
{
    printf("== %s ==\n", msg);
}

bool CheckPage(const void * buf, size_t pgsz, size_t pgnum)
{
    const size_t* arr = (const size_t*)buf;
    const size_t num = pgsz / sizeof(size_t);
    for (size_t i = 0; i < num; ++i)
        if (arr[i] != pgnum + i)
            return false;
    return true;
}

bool ReadPage(HANDLE fh, long long position, size_t sz, void* dest)
{
    OVERLAPPED overlapped;
    memset(&overlapped, 0, sizeof(OVERLAPPED));
    overlapped.OffsetHigh = DWORD(position >> 32);
    overlapped.Offset = DWORD(position & (DWORD)~0);
    DWORD processed = 0;
    if (!ReadFile(fh, (char*)dest, (DWORD) sz, &processed, &overlapped))
        return printf("ReadPage() failed (0x%llx) %u\n", position, GetLastError()), false;
    return true;
}

void InitPage(void * dest, size_t pgsz, size_t pgnum)
{
    size_t *arr = (size_t *)dest;
    const size_t num = pgsz / sizeof(size_t);
    for (size_t i = 0; i < num; ++i)
        arr[i] = pgnum + i;
}

static int test(int argc, const char** argv)
{
    const char* files[] = { "C:/temp/mmap.test", "D:/temp/mmap.test" };
    const size_t pgsz = 32768;
    for (const char* filename : files)
    {
        struct {
            DWORD flags;
            const char* flags_image;
        } CreateFileMode [] = {
            { 0, "NORMAL" },
            { FILE_FLAG_WRITE_THROUGH, "WRITE_THROUGH" },
            { FILE_FLAG_WRITE_THROUGH | FILE_FLAG_NO_BUFFERING, "WRITE_THROUGH|NO_BUFFERING" },
        };

        for (const auto& flags : CreateFileMode)
        {
            for (uint64_t filesz : { 1ull << 30, 1ull << 32, 1ull << 33 })
            {
                std::vector<char> RegularBuffer(filesz);
                char prefix[4096];
                snprintf(prefix, sizeof(prefix), "%s\t%-26s\t%u GB\t", filename, flags.flags_image, (unsigned)(filesz >> 30));

                if (filesz % pgsz)
                    return -4;
                HANDLE hFile = INVALID_HANDLE_VALUE;
                {
                    hFile = CreateFileA(filename,
                        GENERIC_WRITE | GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, CREATE_ALWAYS, flags.flags, NULL);
                    if (!hFile || hFile == INVALID_HANDLE_VALUE)
                        return printf("could not open '%s' in RW\n", filename), -10;
                }

                std::vector<char> PageBuffer_(pgsz + 4095);

                char* PageBuffer = reinterpret_cast<char*>(reinterpret_cast<intptr_t>(PageBuffer_.data() + 4095) & ~4095ull);

                Mapping TheMapping(hFile);
                size_t npg = filesz / pgsz;
                if (int error = TheMapping.map(pgsz * npg))
                    return error;

                std::vector<size_t> pages;

                auto test = [&]() -> int
                {
                    {
                        BenchSection b("Writing in memory", prefix);
                        for (size_t pg : pages)
                            InitPage(TheMapping.addr() + pg * pgsz, pgsz, pg);
                    }
                    for (int i = 0; i < 1; ++i)
                    {
                        BenchSection b("Writing in memory 2", prefix);
                        for (size_t pg : pages)
                            InitPage(TheMapping.addr() + pg * pgsz, pgsz, pg);
                    }
                    for (int i = 0; i < 2; ++i)
                    {
                        BenchSection b("Writing in buf memory", prefix);
                        for (size_t pg : pages)
                            InitPage(RegularBuffer.data() + pg * pgsz, pgsz, pg);
                    }
                    for (int i = 0; i < 2; ++i)
                    {
                        BenchSection b("FlushViewOfFile", prefix);
                        TheMapping.flush_view_of_file();
                    }
                    //TheMapping.close();
                    for (int i = 0; i < 2; ++i)
                    {
                        BenchSection b("FlushFileBuffers", prefix);
                        TheMapping.flush_file_buffers();
                    }
                    //if (int error = TheMapping.map(pgsz * npg))
                    //    return error;
                    for (int i = 0; i < 2; ++i)
                    {
                        BenchSection b("Reading back buf", prefix);
                        for (size_t pg : pages)
                        {
                            memcpy(PageBuffer, RegularBuffer.data() + pg * pgsz, pgsz);
                            if (!CheckPage(PageBuffer, pgsz, pg))
                            {
                                printf("Read from file != expected\n");
                                return -4;
                            }
                        }
                    }
                    for (int i = 0; i < 2; ++i)
                    {
                        BenchSection b("Reading back", prefix);
                        for (size_t pg : pages)
                        {
                            if (!ReadPage(hFile, pg * pgsz, pgsz, PageBuffer))
                                return -3;
                            if (!CheckPage(PageBuffer, pgsz, pg))
                            {
                                printf("Read from file != expected\n");
                                return -4;
                            }
                        }
                    }
                    for (int i = 0; i < 2; ++i)
                    {
                        BenchSection b("Checking buf direct", prefix);
                        for (size_t pg : pages)
                        {
                            if (!CheckPage(RegularBuffer.data() + pg * pgsz, pgsz, pg))
                            {
                                printf("Read from file != expected\n");
                                return -4;
                            }
                        }
                    }
                    for (int i = 0; i < 2; ++i)
                    {
                        BenchSection b("Checking mmap direct", prefix);
                        for (size_t pg : pages)
                        {
                            if (!CheckPage(TheMapping.addr() + pg * pgsz, pgsz, pg))
                            {
                                printf("Read from file != expected\n");
                                return -4;
                            }
                        }
                    }
                    return 0;
                };

                for (size_t pg = 0; pg < npg; ++pg)
                    pages.push_back(pg);

                if (int r = test())
                    return r;

                TheMapping.close();
                CloseHandle(hFile);
                _unlink(filename);
            }
        }
    }
    return 0;
}

int main(int argc, const char** argv)
{
    int r = test(argc, argv);
    return r;
}