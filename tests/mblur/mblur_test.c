/**
 * The host motion blur (mblur_d3d11.c) on WARP against the same maths done
 * on the CPU in doubles. A 320x180 frame sits at (16, 12) in a bigger
 * surface whose rest is magenta (as a guest frame inside its padded
 * surface); its depth buffer holds one distance (or the sky); guest pixels
 * are half the host's (the longest smear is given in those).
 *
 *   motion view  a pure turn, a pure move, the sky under a move, the cap,
 *                the sharp middle: each pixel's colour within 2 levels of the
 *                CPU's motion, and its direction (a camera that turned right
 *                sees the picture move left: green)
 *   blur         vertical black / white stripes, 8 pixels each: a turn that
 *                moves the middle 16 pixels across greys them out (an even
 *                mix in linear light, ~186); a tilt that moves them up and
 *                down leaves them as they were; a motion under half a pixel
 *                leaves every pixel exactly as it was
 *   always       alpha and everything outside the rectangle untouched
 *
 *   mblur-test [--hw] [--dump <dir>]   (--hw: the real GPU instead of WARP;
 *                                       --dump: each result as a BMP)
 */
#define COBJMACROS
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>

#include "mblur_d3d11.h"
#include "nv2a_pb_state.h"

#define TEX_W 352
#define TEX_H 208
#define X0 16
#define Y0 12
#define W 320
#define H 180
#define GUEST_H 90.0
#define ZN 10.0
#define ZF 150000.0
#define SX 1.0
#define SY 1.7777778
#define ALPHA 77
#define PI 3.14159265358979

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static const char *s_dump;
static int s_fail;
static unsigned char s_in[TEX_W * TEX_H * 4], s_out[TEX_W * TEX_H * 4];

static int in_rect(int x, int y)
{
    return x >= X0 && x < X0 + W && y >= Y0 && y < Y0 + H;
}

/* BGRA: stripes inside the frame, magenta beside it. */
static void make_input(void)
{
    int x, y;

    for (y = 0; y < TEX_H; y++)
        for (x = 0; x < TEX_W; x++) {
            unsigned char *p = s_in + ((size_t)y * TEX_W + x) * 4;
            if (in_rect(x, y)) {
                unsigned char v = (((x - X0) / 8) & 1) ? 255 : 0;
                p[0] = p[1] = p[2] = v;
                p[3] = ALPHA;
            } else {
                p[0] = 255;
                p[1] = 0;
                p[2] = 255;
                p[3] = 200;
            }
        }
}

/* A distance as the depth buffer's 24 bits (D3D's z / w); 0 = the sky. */
static uint32_t depth24(double dist)
{
    double d;

    if (dist <= 0.0)
        return 0xFFFFFFu;
    d = (1.0 - ZN / dist) * ZF / (ZF - ZN);
    return (uint32_t)floor(d * 16777215.0 + 0.5);
}

static double dist_of(uint32_t d24)
{
    double d = d24 / 16777215.0;
    return ZN / (1.0 - d * (ZF - ZN) / ZF);
}

static void dump(const char *name)
{
    char path[MAX_PATH];
    unsigned char hdr[54];
    FILE *f;
    int x, y, row = (TEX_W * 3 + 3) & ~3;

    if (!s_dump)
        return;
    snprintf(path, sizeof path, "%s\\mblur_%s.bmp", s_dump, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B';
    hdr[1] = 'M';
    *(uint32_t *)(hdr + 2) = 54u + (uint32_t)(row * TEX_H);
    *(uint32_t *)(hdr + 10) = 54;
    *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = TEX_W;
    *(int32_t *)(hdr + 22) = TEX_H;
    *(uint16_t *)(hdr + 26) = 1;
    *(uint16_t *)(hdr + 28) = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = TEX_H - 1; y >= 0; y--) {
        unsigned char line[TEX_W * 3 + 4];
        memset(line, 0, sizeof line);
        for (x = 0; x < TEX_W; x++)
            memcpy(line + x * 3, s_out + ((size_t)y * TEX_W + x) * 4, 3);
        fwrite(line, 1, (size_t)row, f);
    }
    fclose(f);
}

/* The frame and a depth buffer of d24 everywhere, blurred; s_out the
 * result. */
static int run(const MblurParams *p, uint32_t d24)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *tex = NULL, *ztex = NULL, *rb = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11ShaderResourceView *zsrv = NULL;
    uint32_t *z = malloc((size_t)TEX_W * TEX_H * 4);
    int ok, i, y;

    for (i = 0; i < TEX_W * TEX_H; i++)
        z[i] = d24;
    memset(&td, 0, sizeof td);
    td.Width = TEX_W;
    td.Height = TEX_H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = s_in;
    sd.SysMemPitch = TEX_W * 4;
    sd.SysMemSlicePitch = 0;
    ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &tex);
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)tex, NULL,
                                        &rtv);
    td.Format = DXGI_FORMAT_R24G8_TYPELESS;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    sd.pSysMem = z;
    ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &ztex);
    memset(&sv, 0, sizeof sv);
    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (ztex)
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)ztex,
                                              &sv, &zsrv);
    if (!tex || !rtv || !zsrv) {
        printf("FAIL: test textures could not be made\n");
        exit(1);
    }
    ok = mblur_draw(s_dev, s_ctx, tex, rtv, zsrv, X0, Y0, W, H,
                    (float)GUEST_H, p);
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &rb);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)rb,
                                     (ID3D11Resource *)tex);
    ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)rb, 0, D3D11_MAP_READ, 0,
                            &m);
    for (y = 0; y < TEX_H; y++)
        memcpy(s_out + (size_t)y * TEX_W * 4,
               (unsigned char *)m.pData + (size_t)y * m.RowPitch, TEX_W * 4);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)rb, 0);
    ID3D11Texture2D_Release(rb);
    ID3D11ShaderResourceView_Release(zsrv);
    ID3D11Texture2D_Release(ztex);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(tex);
    free(z);
    return ok;
}

static double smooth(double e0, double e1, double x)
{
    double t = (x - e0) / (e1 - e0);
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    return t * t * (3.0 - 2.0 * t);
}

/* The motion of pixel x, y (host pixels, now minus then) as the pass works
 * it out, in doubles and with the rotation as a matrix. */
static void cpu_motion(const MblurParams *p, uint32_t d24, int x, int y,
                       double *mx, double *my)
{
    double q[4], n, r[3][3], v[3], l[3], px = x + 0.5, py = y + 0.5;
    double dist = dist_of(d24), nx, ny, lx, ly, ax, ay, len, maxh, rr;
    int sky = d24 == 0xFFFFFFu, i;

    for (i = 0; i < 4; i++)
        q[i] = p->q[i];
    n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (i = 0; i < 4; i++)
        q[i] /= n;
    r[0][0] = 1 - 2 * (q[1] * q[1] + q[2] * q[2]);
    r[0][1] = 2 * (q[0] * q[1] - q[2] * q[3]);
    r[0][2] = 2 * (q[0] * q[2] + q[1] * q[3]);
    r[1][0] = 2 * (q[0] * q[1] + q[2] * q[3]);
    r[1][1] = 1 - 2 * (q[0] * q[0] + q[2] * q[2]);
    r[1][2] = 2 * (q[1] * q[2] - q[0] * q[3]);
    r[2][0] = 2 * (q[0] * q[2] - q[1] * q[3]);
    r[2][1] = 2 * (q[1] * q[2] + q[0] * q[3]);
    r[2][2] = 1 - 2 * (q[0] * q[0] + q[1] * q[1]);
    nx = (px - X0) / W * 2.0 - 1.0;
    ny = -((py - Y0) / H * 2.0 - 1.0);
    v[0] = nx * dist / SX;
    v[1] = ny * dist / SY;
    v[2] = dist;
    for (i = 0; i < 3; i++)
        l[i] = r[i][0] * v[0] + r[i][1] * v[1] + r[i][2] * v[2]
               + (sky ? 0.0 : p->t[i]);
    if (l[2] < ZN)
        l[2] = ZN;
    lx = l[0] * SX / l[2];
    ly = l[1] * SY / l[2];
    ax = (px - (X0 + (lx * 0.5 + 0.5) * W)) * p->amount;
    ay = (py - (Y0 + (-ly * 0.5 + 0.5) * H)) * p->amount;
    maxh = p->max_blur * H / GUEST_H;
    len = sqrt(ax * ax + ay * ay);
    if (len > maxh) {
        ax *= maxh / len;
        ay *= maxh / len;
    }
    if (p->centre > 0.0f) {
        rr = hypot(px - X0 - W / 2.0, py - Y0 - H / 2.0)
             / hypot(W / 2.0, H / 2.0);
        rr = smooth(p->centre, p->centre * 2.0, rr);
        ax *= rr;
        ay *= rr;
    }
    *mx = ax;
    *my = ay;
}

static unsigned char level(double v)
{
    v = v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
    return (unsigned char)floor(v * 255.0 + 0.5);
}

/* Alpha and everything beside the frame as they were. */
static void check_kept(const char *name)
{
    int x, y, bad = 0;

    for (y = 0; y < TEX_H; y++)
        for (x = 0; x < TEX_W; x++) {
            size_t o = ((size_t)y * TEX_W + x) * 4;
            if (in_rect(x, y) ? s_out[o + 3] != ALPHA
                              : memcmp(s_out + o, s_in + o, 4) != 0)
                bad++;
        }
    if (bad) {
        printf("FAIL: %s: %d pixels changed beside the frame or in alpha\n",
               name, bad);
        s_fail = 1;
    }
}

/* The motion view against cpu_motion, every pixel. */
static void check_view(const char *name, MblurParams p, uint32_t d24)
{
    int x, y, worst = 0, wx = 0, wy = 0;
    double maxh = p.max_blur * H / GUEST_H;

    p.flags = NV_HOST_MBLUR_VIEW;
    if (!run(&p, d24)) {
        printf("FAIL: %s: not drawn\n", name);
        s_fail = 1;
        return;
    }
    for (y = Y0; y < Y0 + H; y++)
        for (x = X0; x < X0 + W; x++) {
            double mx, my;
            unsigned char e[3];
            const unsigned char *g = s_out + ((size_t)y * TEX_W + x) * 4;
            int c;
            cpu_motion(&p, d24, x, y, &mx, &my);
            e[0] = level(fabs(my) / maxh);          /* B */
            e[1] = level(-mx / maxh);               /* G */
            e[2] = level(mx / maxh);                /* R */
            for (c = 0; c < 3; c++)
                if (abs((int)g[c] - (int)e[c]) > worst) {
                    worst = abs((int)g[c] - (int)e[c]);
                    wx = x;
                    wy = y;
                }
        }
    {
        const unsigned char *m = s_out
            + ((size_t)(Y0 + H / 2) * TEX_W + X0 + W / 2) * 4;
        printf("%s %s: worst %d levels off (at %d,%d); middle R %u G %u"
               " B %u\n", worst <= 2 ? "ok  " : "FAIL", name, worst, wx, wy,
               m[2], m[1], m[0]);
    }
    if (worst > 2)
        s_fail = 1;
    check_kept(name);
    dump(name);
}

static MblurParams turn(double deg_x, double deg_y, float amount)
{
    MblurParams p;
    double ax = deg_x * PI / 180.0, ay = deg_y * PI / 180.0;

    memset(&p, 0, sizeof p);
    /* About x, then about y (each a half angle in the quaternion). */
    p.q[0] = (float)(sin(ax / 2) * cos(ay / 2));
    p.q[1] = (float)(cos(ax / 2) * sin(ay / 2));
    p.q[2] = (float)(-sin(ax / 2) * sin(ay / 2));
    p.q[3] = (float)(cos(ax / 2) * cos(ay / 2));
    p.zn = (float)ZN;
    p.zf = (float)ZF;
    p.sx = (float)SX;
    p.sy = (float)SY;
    p.amount = amount;
    p.max_blur = 40.0f;                 /* 80 host pixels */
    return p;
}

int main(int argc, char **argv)
{
    int hw = 0, i, x, y;
    uint32_t near500 = depth24(500.0);
    MblurParams p;
    HRESULT hr;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hw"))
            hw = 1;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc)
            s_dump = argv[++i];
    }
    hr = D3D11CreateDevice(NULL, hw ? D3D_DRIVER_TYPE_HARDWARE
                                    : D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                           D3D11_SDK_VERSION, &s_dev, NULL, &s_ctx);
    if (FAILED(hr)) {
        printf("FAIL: D3D11CreateDevice 0x%08lX\n", (unsigned long)hr);
        return 1;
    }
    make_input();

    /* Turned 2 degrees right since the last frame: the middle's point was
     * right of the middle then, so it moved left (green), ~45 pixels. */
    p = turn(0.0, 2.0, 8.0f);
    check_view("view turn", p, near500);
    {
        const unsigned char *m = s_out
            + ((size_t)(Y0 + H / 2) * TEX_W + X0 + W / 2) * 4;
        if (!(m[1] > 100 && m[2] == 0)) {
            printf("FAIL: view turn: a right turn should move the middle left"
                   " (green)\n");
            s_fail = 1;
        }
    }
    /* Moved 5 units right at 500 away: everything moved left, the same
     * everywhere (1.6 pixels x 20). */
    p = turn(0.0, 0.0, 20.0f);
    p.t[0] = 5.0f;
    check_view("view move", p, near500);
    /* The sky moves with turns only. */
    p.t[0] = 50.0f;
    p.t[1] = 30.0f;
    p.t[2] = -20.0f;
    check_view("view sky moved", p, 0xFFFFFFu);
    for (y = Y0, i = 0; y < Y0 + H; y++)
        for (x = X0; x < X0 + W; x++) {
            const unsigned char *g = s_out + ((size_t)y * TEX_W + x) * 4;
            i += g[0] || g[1] || g[2];
        }
    if (i) {
        printf("FAIL: view sky moved: %d pixels show motion\n", i);
        s_fail = 1;
    }
    /* A quick turn and a tilt: capped at the longest smear everywhere. */
    p = turn(5.0, 30.0, 8.0f);
    check_view("view cap", p, near500);
    /* The same with the middle kept sharp (0.3 of the half diagonal). */
    p.centre = 0.3f;
    check_view("view middle", p, near500);

    /* Blur: the middle moved 16 pixels across, one stripe pair. */
    p = turn(0.0, 2.0, (float)(16.0 / (SX * tan(2.0 * PI / 180.0) * W / 2)));
    if (!run(&p, near500)) {
        printf("FAIL: blur across: not drawn\n");
        s_fail = 1;
    } else {
        int lo = 255, hi = 0, sum = 0, n = 0;
        for (y = Y0 + H / 2 - 20; y < Y0 + H / 2 + 20; y++)
            for (x = X0 + W / 2 - 40; x < X0 + W / 2 + 40; x++) {
                const unsigned char *g = s_out + ((size_t)y * TEX_W + x) * 4;
                lo = g[1] < lo ? g[1] : lo;
                hi = g[1] > hi ? g[1] : hi;
                sum += g[1];
                n++;
                if (g[0] != g[1] || g[2] != g[1])
                    lo = -1000;          /* not grey */
            }
        i = lo >= 150 && hi <= 215 && sum >= 178 * n && sum <= 194 * n;
        printf("%s blur across: middle %d..%d, mean %.1f (even mix ~186)\n",
               i ? "ok  " : "FAIL", lo, hi, (double)sum / n);
        if (!i)
            s_fail = 1;
        check_kept("blur across");
        dump("blur_across");
    }
    /* Tilted 1.5 degrees: the middle moved ~13 pixels up or down, along
     * the stripes; away from their edges nothing changes. */
    p = turn(1.5, 0.0, 3.0f);
    if (!run(&p, near500)) {
        printf("FAIL: blur along: not drawn\n");
        s_fail = 1;
    } else {
        int worst = 0;
        for (y = Y0 + H * 3 / 10; y < Y0 + H * 7 / 10; y++)
            for (x = X0 + W * 3 / 10; x < X0 + W * 7 / 10; x++) {
                size_t o = ((size_t)y * TEX_W + x) * 4;
                int k = (x - X0) % 8;
                if (k >= 2 && k <= 5 && abs(s_out[o] - s_in[o]) > worst)
                    worst = abs(s_out[o] - s_in[o]);
            }
        printf("%s blur along: worst %d levels off the stripes\n",
               worst <= 6 ? "ok  " : "FAIL", worst);
        if (worst > 6)
            s_fail = 1;
        check_kept("blur along");
        dump("blur_along");
    }
    /* Under half a pixel: every pixel exactly as it was. */
    p = turn(0.0, 0.01, 1.0f);
    if (!run(&p, near500) || memcmp(s_out, s_in, sizeof s_in)) {
        printf("FAIL: tiny motion: the frame changed\n");
        s_fail = 1;
    } else {
        printf("ok   tiny motion: the frame as it was\n");
    }

    printf(s_fail ? "FAILED\n" : "PASSED\n");
    return s_fail;
}
