#include "kernel.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }

static char s_game[MAX_PATH], s_save[MAX_PATH];
static int s_ok = 1;

/* Translate `xbox` and compare with <base>\<rest>, ignoring case. */
static void expect(const char *xbox, const char *base, const char *rest)
{
    WCHAR got[MAX_PATH], want[MAX_PATH];
    if (!xbox_translate_path(xbox, got, MAX_PATH)) {
        fprintf(stderr, "FAIL %s: not translated\n", xbox);
        s_ok = 0;
        return;
    }
    swprintf_s(want, MAX_PATH, L"%S\\%S", base, rest);
    if (_wcsicmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s\n  got  %S\n  want %S\n", xbox, got, want);
        s_ok = 0;
    }
}

static void link_drive(const char *link, const char *target)
{
    XBOX_ANSI_STRING l, t;
    l.Buffer = (PCHAR)link;   l.Length = (USHORT)strlen(link);   l.MaximumLength = l.Length;
    t.Buffer = (PCHAR)target; t.Length = (USHORT)strlen(target); t.MaximumLength = t.Length;
    if (xbox_IoCreateSymbolicLink(&l, &t) != 0) {
        fprintf(stderr, "FAIL IoCreateSymbolicLink %s\n", link);
        s_ok = 0;
    }
}

int main(void)
{
    static const char *made[] = {
        "Partition0.img", "Partition1.img", "Partition2.img", "Partition3.img",
        "Partition4.img", "Partition5.img", "TitleData", "UserData", "Cache",
        "SystemData",
    };
    char root[MAX_PATH], p[MAX_PATH];
    int i;

    GetTempPathA(MAX_PATH, root);
    sprintf_s(s_game, MAX_PATH, "%sxbr-path-rules-%lu-game", root, GetCurrentProcessId());
    sprintf_s(s_save, MAX_PATH, "%sxbr-path-rules-%lu-save", root, GetCurrentProcessId());
    CreateDirectoryA(s_game, NULL);
    xbox_path_init(s_game, s_save);

    expect("\\Device\\Harddisk0\\Partition1\\TDATA\\4D530051\\settings.dat",
           s_save, "TitleData\\4D530051\\settings.dat");
    expect("\\Device\\Harddisk0\\Partition1\\UDATA\\4D530051\\slot0\\save.bin",
           s_save, "UserData\\4D530051\\slot0\\save.bin");
    expect("\\Device\\Harddisk0\\Partition1\\tdata", s_save, "TitleData");
    expect("\\Device\\Harddisk0\\Partition1\\UDATA\\", s_save, "UserData");

    /* XAPI's mount, then a write through the drive letter. */
    link_drive("\\??\\T:", "\\Device\\Harddisk0\\Partition1\\TDATA\\4D530051");
    link_drive("\\??\\U:", "\\Device\\Harddisk0\\Partition1\\UDATA\\4D530051");
    expect("T:\\options.dat", s_save, "TitleData\\4D530051\\options.dat");
    expect("U:\\slot1\\save.bin", s_save, "UserData\\4D530051\\slot1\\save.bin");

    /* XMountUtilityDrive's slot files: console state, not disc content. */
    expect("\\Device\\Harddisk0\\partition1\\CACHE\\LocalCache00.bin",
           s_save, "HddCache\\LocalCache00.bin");
    expect("\\Device\\Harddisk0\\Partition1\\CACHE\\", s_save, "HddCache");

    /* Unchanged: the rest of Partition1, a near miss, and the disc. */
    expect("\\Device\\Harddisk0\\Partition1\\media\\level.xpr", s_game, "media\\level.xpr");
    expect("\\Device\\Harddisk0\\Partition1\\TDATAX\\a.bin", s_game, "TDATAX\\a.bin");
    expect("D:\\default.xbe", s_game, "default.xbe");

    for (i = 0; i < (int)(sizeof(made) / sizeof(made[0])); i++) {
        sprintf_s(p, MAX_PATH, "%s\\%s", s_save, made[i]);
        if (!DeleteFileA(p))
            RemoveDirectoryA(p);
    }
    RemoveDirectoryA(s_save);
    RemoveDirectoryA(s_game);

    if (!s_ok)
        return 1;
    puts("PASS: Partition1 TDATA/UDATA/CACHE go to the save dir, the rest of Partition1 to the game dir");
    return 0;
}
