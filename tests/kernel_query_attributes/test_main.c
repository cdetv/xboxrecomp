#include "kernel.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }

static int s_ok = 1;

/* Query `xbox` through NtQueryFullAttributesFile and compare the status. */
static void expect(const char *xbox, NTSTATUS want)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_FILE_NETWORK_OPEN_INFORMATION info;
    NTSTATUS got;

    name.Buffer = (PCHAR)xbox;
    name.Length = (USHORT)strlen(xbox);
    name.MaximumLength = name.Length + 1;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0;
    got = xbox_NtQueryFullAttributesFile(&oa, &info);
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %08lX, want %08lX\n", xbox,
                (unsigned long)got, (unsigned long)want);
        s_ok = 0;
    }
}

int main(void)
{
    static const char *images[] = {
        "Partition0.img", "Partition1.img", "Partition2.img",
        "Partition3.img", "Partition4.img", "Partition5.img",
    };
    static const char *dirs[] = { "TitleData", "UserData", "Cache", "SystemData" };
    char root[MAX_PATH], game[MAX_PATH], save[MAX_PATH], p[MAX_PATH];
    HANDLE h;
    int i;

    GetTempPathA(MAX_PATH, root);
    sprintf_s(game, MAX_PATH, "%sxbr-query-attrs-%lu-game", root, GetCurrentProcessId());
    sprintf_s(save, MAX_PATH, "%sxbr-query-attrs-%lu-save", root, GetCurrentProcessId());
    CreateDirectoryA(game, NULL);
    sprintf_s(p, MAX_PATH, "%s\\present.bin", game);
    h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 10;
    CloseHandle(h);
    xbox_path_init(game, save);

    /* Existing file and existing directory. */
    expect("D:\\present.bin", STATUS_SUCCESS);
    expect("Z:\\", STATUS_SUCCESS);
    /* Missing leaf in an existing directory: error 2 to the title. */
    expect("D:\\missing.bin", STATUS_OBJECT_NAME_NOT_FOUND);
    expect("Z:\\texture", STATUS_OBJECT_NAME_NOT_FOUND);
    /* Missing parent directory: error 3, so a recursive mkdir creates the
     * parent first. Conker probes Z:\texture\frontend\tips on an empty cache. */
    expect("D:\\nodir\\leaf.bin", STATUS_OBJECT_PATH_NOT_FOUND);
    expect("Z:\\texture\\frontend\\tips", STATUS_OBJECT_PATH_NOT_FOUND);

    DeleteFileA(p);
    RemoveDirectoryA(game);
    for (i = 0; i < (int)(sizeof(images) / sizeof(images[0])); i++) {
        sprintf_s(p, MAX_PATH, "%s\\%s", save, images[i]);
        DeleteFileA(p);
    }
    for (i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++) {
        sprintf_s(p, MAX_PATH, "%s\\%s", save, dirs[i]);
        RemoveDirectoryA(p);
    }
    RemoveDirectoryA(save);

    if (!s_ok) return 1;
    puts("PASS: NtQueryFullAttributesFile keeps missing leaf (NAME) and missing parent (PATH) apart");
    return 0;
}
