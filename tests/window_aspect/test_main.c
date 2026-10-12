/* xbox_FitAspect: the GPU view inside the frame window keeps the image's
 * aspect ratio at any window shape (fb_present.c). Before, the swap chain
 * stretched 640x480 to the whole client area. */
#include <stdio.h>
#include <stdlib.h>

void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }
extern void xbox_FitAspect(int cw, int ch, int w, int h,
                           int *x, int *y, int *fw, int *fh);

static int fails;

static void check(int cw, int ch, int ex, int ey, int ew, int eh, const char *what)
{
    int x, y, w, h;
    xbox_FitAspect(cw, ch, 640, 480, &x, &y, &w, &h);
    int ok = x == ex && y == ey && w == ew && h == eh;
    printf("%s: %s %dx%d -> %d,%d %dx%d\n", ok ? "PASS" : "FAIL", what, cw, ch, x, y, w, h);
    fails += !ok;
}

int main(void)
{
    check(640, 480, 0, 0, 640, 480, "same shape fills");
    check(1284, 521, 295, 0, 694, 521, "wide: bars left/right");
    check(544, 862, 0, 227, 544, 408, "tall: bars top/bottom");
    check(1280, 960, 0, 0, 1280, 960, "bigger, same shape, fills");
    check(0, 0, 0, 0, 0, 0, "minimised");
    return fails ? 1 : 0;
}
