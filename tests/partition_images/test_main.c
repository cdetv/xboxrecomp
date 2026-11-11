/* Partition images hold only what is written (see README.md). */
#include "kernel.h"
#include <winioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }

static char s_game[MAX_PATH], s_save[MAX_PATH];
static int s_ok = 1;

#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_ok = 0; } } while (0)

static void image_path(int p, char *out)
{
    sprintf(out, "%s\\Partition%d.img", s_save, p);
}

static LONGLONG file_len(int p)
{
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    image_path(p, path);
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa))
        return -1;
    return ((LONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

/* An image as the old path init left it: full partition size. Sparse with
 * `data` bytes of 0x5A at the start, or (sparse == 0) fully allocated, the
 * way a copy of the folder leaves it. */
static void make_old_image(int p, BOOL sparse, DWORD data)
{
    char path[MAX_PATH];
    HANDLE h;
    DWORD ret;
    LARGE_INTEGER end;
    image_path(p, path);
    h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { CHECK(0, "make image %d", p); return; }
    if (sparse)
        DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &ret, NULL);
    if (data) {
        uint8_t *buf = malloc(data);
        memset(buf, 0x5A, data);
        WriteFile(h, buf, data, &ret, NULL);
        free(buf);
    }
    end.QuadPart = (LONGLONG)xbox_partition_image_bytes(p);
    SetFilePointerEx(h, end, NULL, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
}

static NTSTATUS open_device(const char *xbox, ULONG options, HANDLE *h)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    name.Length = (USHORT)strlen(xbox);
    name.MaximumLength = name.Length + 1;
    name.Buffer = (PCHAR)xbox;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0;
    return xbox_NtCreateFile(h, XBOX_GENERIC_READ | XBOX_GENERIC_WRITE | XBOX_SYNCHRONIZE,
                             &oa, &ios, NULL, 0, 0, XBOX_FILE_OPEN,
                             options | XBOX_FILE_SYNCHRONOUS_IO_NONALERT);
}

static NTSTATUS read_at(HANDLE h, LONGLONG off, uint8_t *buf, ULONG len, ULONG *got)
{
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER o;
    NTSTATUS st;
    o.QuadPart = off;
    memset(buf, 0xCC, len);
    memset(&ios, 0, sizeof(ios));
    st = xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, len, off >= 0 ? &o : NULL);
    *got = (ULONG)ios.Information;
    return st;
}

static int all(const uint8_t *buf, ULONG len, uint8_t v)
{
    ULONG i;
    for (i = 0; i < len; i++)
        if (buf[i] != v)
            return 0;
    return 1;
}

int main(void)
{
    char root[MAX_PATH], path[MAX_PATH];
    static uint8_t buf[8192];
    const ULONGLONG z = xbox_partition_image_bytes(5);
    HANDLE h;
    NTSTATUS st;
    ULONG got;
    int p;

    CHECK(z == 0x177000ull * 512, "Z: size %llu", (unsigned long long)z);
    CHECK(xbox_partition_image_bytes(0) == 0 && xbox_partition_image_bytes(6) == 0,
          "sizes outside 1-5");

    GetTempPathA(MAX_PATH, root);
    sprintf(s_game, "%sxr-part-game-%lu", root, GetCurrentProcessId());
    sprintf(s_save, "%sxr-part-save-%lu", root, GetCurrentProcessId());
    CreateDirectoryA(s_game, NULL);
    CreateDirectoryA(s_save, NULL);

    /* Images from before: 3 sparse with 4 KB written, 4 fully allocated (a
     * copy), 2 not full size (already sized by writes: left alone). */
    make_old_image(3, TRUE, 4096);
    make_old_image(4, FALSE, 0);
    image_path(2, path);
    {
        HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD w;
        memset(buf, 0x33, 1000);
        WriteFile(f, buf, 1000, &w, NULL);
        CloseHandle(f);
    }

    xbox_path_init(s_game, s_save);

    CHECK(file_len(1) == 0, "Partition1 new: %lld", file_len(1));
    CHECK(file_len(5) == 0, "Partition5 new: %lld", file_len(5));
    CHECK(file_len(2) == 1000, "Partition2 not full size kept: %lld", file_len(2));
    /* NTFS allocates sparse files in 64 KB blocks: the cut lands at the end of
     * the block holding the data. */
    CHECK(file_len(3) >= 4096 && file_len(3) <= 65536,
          "Partition3 cut after its data: %lld", file_len(3));
    CHECK(file_len(4) == 0, "Partition4 fully allocated emptied: %lld", file_len(4));

    /* Image numbers: only <save>\Partition1-5.img. */
    {
        WCHAR w[MAX_PATH];
        swprintf_s(w, MAX_PATH, L"%S\\partition5.IMG", s_save);
        CHECK(xbox_partition_image_number(w) == 5, "number of %S", w);
        swprintf_s(w, MAX_PATH, L"%S\\Partition0.img", s_save);
        CHECK(xbox_partition_image_number(w) == 0, "Partition0 is no sized image");
        swprintf_s(w, MAX_PATH, L"%S\\Partition5.img", s_game);
        CHECK(xbox_partition_image_number(w) == 0, "outside the save dir");
    }

    /* Partition 3 kept its data, then zeros past the image. */
    st = open_device("\\Device\\Harddisk0\\Partition3", 0, &h);
    CHECK(st == 0, "open Partition3: 0x%08X", (unsigned)st);
    if (st == 0) {
        st = read_at(h, 4096 - 512, buf, 1024, &got);
        CHECK(st == 0 && got == 1024 && all(buf, 512, 0x5A) && all(buf + 512, 512, 0),
              "read across the image's end: st 0x%08X got %lu", (unsigned)st, got);
        xbox_NtClose(h);
    }

    st = open_device("\\Device\\Harddisk0\\Partition5", 0, &h);
    CHECK(st == 0, "open Partition5: 0x%08X", (unsigned)st);
    if (st == 0) {
        XBOX_IO_STATUS_BLOCK ios;
        XBOX_FILE_STANDARD_INFORMATION si;
        LARGE_INTEGER o;
        ULONG w;

        CHECK(xbox_PartitionHandleBytes(h) == z, "handle size");

        /* Empty image: any read inside the partition is zeros. */
        st = read_at(h, 0x10000000, buf, 4096, &got);
        CHECK(st == 0 && got == 4096 && all(buf, 4096, 0),
              "read deep inside: st 0x%08X got %lu", (unsigned)st, got);
        /* Stops at the partition's end, then end of file. */
        st = read_at(h, (LONGLONG)z - 256, buf, 512, &got);
        CHECK(st == 0 && got == 256 && all(buf, 256, 0) && all(buf + 256, 256, 0xCC),
              "read across the partition end: st 0x%08X got %lu", (unsigned)st, got);
        st = read_at(h, (LONGLONG)z, buf, 512, &got);
        CHECK(st == STATUS_END_OF_FILE && got == 0,
              "read at the partition end: st 0x%08X got %lu", (unsigned)st, got);

        /* A write lands where it is aimed and grows the image to there. */
        memset(buf, 0xA5, 512);
        o.QuadPart = 0x10000;
        memset(&ios, 0, sizeof(ios));
        st = xbox_NtWriteFile(h, NULL, NULL, NULL, &ios, buf, 512, &o);
        CHECK(st == 0 && ios.Information == 512, "write: 0x%08X", (unsigned)st);
        st = read_at(h, 0x10000 - 512, buf, 1536, &got);
        CHECK(st == 0 && got == 1536 && all(buf, 512, 0) && all(buf + 512, 512, 0xA5) &&
              all(buf + 1024, 512, 0), "read back the write: st 0x%08X got %lu",
              (unsigned)st, got);

        /* A sequential read past the image moves the position on. */
        {
            XBOX_FILE_POSITION_INFORMATION pi;
            pi.CurrentByteOffset.QuadPart = 0x20000;
            xbox_NtSetInformationFile(h, &ios, &pi, sizeof(pi), XboxFilePositionInformation);
            st = read_at(h, -1, buf, 1024, &got);
            CHECK(st == 0 && got == 1024 && all(buf, 1024, 0), "sequential read: 0x%08X",
                  (unsigned)st);
            memset(&pi, 0, sizeof(pi));
            xbox_NtQueryInformationFile(h, &ios, &pi, sizeof(pi), XboxFilePositionInformation);
            CHECK(pi.CurrentByteOffset.QuadPart == 0x20000 + 1024, "position after: %lld",
                  (long long)pi.CurrentByteOffset.QuadPart);
        }

        memset(&si, 0, sizeof(si));
        st = xbox_NtQueryInformationFile(h, &ios, &si, sizeof(si), XboxFileStandardInformation);
        CHECK(st == 0 && (ULONGLONG)si.EndOfFile.QuadPart == z, "standard info end: %lld",
              (long long)si.EndOfFile.QuadPart);
        xbox_NtClose(h);
        CHECK(xbox_PartitionHandleBytes(h) == 0, "handle forgotten after close");
        CHECK(file_len(5) == 0x10000 + 512, "image after the write: %lld", file_len(5));
        (void)w;
    }

    /* The free-space open of a partition (as a directory) still works. */
    st = open_device("\\Device\\Harddisk0\\Partition1", XBOX_FILE_DIRECTORY_FILE, &h);
    CHECK(st == 0, "directory open of Partition1: 0x%08X", (unsigned)st);
    if (st == 0) {
        CHECK(xbox_PartitionHandleBytes(h) == 0, "directory handle is no image");
        xbox_NtClose(h);
    }

    /* A second start leaves the sized images as they are. */
    {
        LONGLONG len3 = file_len(3);
        xbox_path_init(s_game, s_save);
        CHECK(file_len(5) == 0x10000 + 512 && file_len(3) == len3, "second start");
    }

    for (p = 0; p <= 5; p++) {
        image_path(p, path);
        DeleteFileA(path);
    }
    {
        static const char *subs[] = { "TitleData", "UserData", "Cache", "SystemData" };
        for (p = 0; p < 4; p++) {
            sprintf(path, "%s\\%s", s_save, subs[p]);
            RemoveDirectoryA(path);
        }
    }
    RemoveDirectoryA(s_save);
    RemoveDirectoryA(s_game);

    puts(s_ok ? "partition_images: all passed" : "partition_images: FAILED");
    return s_ok ? 0 : 1;
}
