/**
 * The filter chain's scale stage (postfx_d3d11.c) against a CPU reference,
 * on WARP: a frame bigger than the target is shrunk by area in linear
 * light, a smaller one stretched bilinearly (clamped to its own edge), or
 * by FSR 1 (checked loosely: close to bilinear, nothing from beside the
 * frame). The frame sits in the corner of a bigger texture whose rest is
 * magenta, as a guest surface padded to its depth buffer's size is, so
 * anything read from beside it shows.
 *
 *   postfx-scale-test [--hw]   (--hw: the real GPU instead of WARP)
 */
#define COBJMACROS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <d3d11.h>

#include "postfx_d3d11.h"

int xbox_SetUpscaler(int mode);
int xbox_SetSharpen(float percent);

#define TEX_W 1600
#define TEX_H 1200
#define OUT_W 640
#define OUT_H 480

static ID3D11Device *s_dev;
static ID3D11DeviceContext *s_ctx;
static int s_fail;

/* The frame's pixel x,y (BGRA bytes): smooth gradients plus a hard-edged
 * checker, so both averaging and edges show. */
static void frame_px(int x, int y, int fw, int fh, unsigned char *p)
{
    int check = ((x / 7) + (y / 5)) & 1;
    p[2] = (unsigned char)(255 * x / (fw - 1));            /* R */
    p[1] = (unsigned char)(255 * y / (fh - 1));            /* G */
    p[0] = (unsigned char)(check ? 230 : 20);              /* B */
    p[3] = 255;
}

static ID3D11Texture2D *make_src(int fw, int fh)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Texture2D *t = NULL;
    unsigned char *px = malloc((size_t)TEX_W * TEX_H * 4);
    int x, y;

    for (y = 0; y < TEX_H; y++)
        for (x = 0; x < TEX_W; x++) {
            unsigned char *p = px + ((size_t)y * TEX_W + x) * 4;
            if (x < fw && y < fh) {
                frame_px(x, y, fw, fh, p);
            } else {
                p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255;   /* magenta */
            }
        }
    memset(&td, 0, sizeof td);
    td.Width = TEX_W;
    td.Height = TEX_H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    sd.pSysMem = px;
    sd.SysMemPitch = TEX_W * 4;
    sd.SysMemSlicePitch = 0;
    ID3D11Device_CreateTexture2D(s_dev, &td, &sd, &t);
    free(px);
    return t;
}

/* The filter chain from the frame (fw x fh at 0,0 of a magenta texture)
 * to an OUT_W x OUT_H target; out gets its BGRA bytes. */
static int run(int fw, int fh, unsigned char *out)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *src = make_src(fw, fh), *dst = NULL, *rb = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    int y, ok;

    memset(&td, 0, sizeof td);
    td.Width = OUT_W;
    td.Height = OUT_H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &dst);
    ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)dst, NULL,
                                        &rtv);
    ok = postfx_draw(s_dev, s_ctx, src, 0.0f, 0.0f, (float)fw, (float)fh,
                     rtv, 0.0f, 0.0f, (float)OUT_W, (float)OUT_H);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &rb);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)rb,
                                     (ID3D11Resource *)dst);
    ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)rb, 0, D3D11_MAP_READ, 0,
                            &m);
    for (y = 0; y < OUT_H; y++)
        memcpy(out + (size_t)y * OUT_W * 4,
               (unsigned char *)m.pData + (size_t)y * m.RowPitch, OUT_W * 4);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)rb, 0);
    ID3D11Texture2D_Release(rb);
    ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(dst);
    ID3D11Texture2D_Release(src);
    return ok;
}

static double lin(double v)
{
    v /= 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double enc(double v)
{
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return 255.0 * (v <= 0.0031308 ? v * 12.92
                                   : 1.055 * pow(v, 1.0 / 2.4) - 0.055);
}

/* Channel c of the expected pixel x,y: area average (shrink) or bilinear. */
static double expect(int fw, int fh, int x, int y, int c, int shrink)
{
    unsigned char p[4];
    double sx = (double)fw / OUT_W, sy = (double)fh / OUT_H, s = 0.0;
    int i, j;

    if (shrink) {
        double a = x * sx, b = a + sx, e = y * sy, f = e + sy;
        for (j = (int)floor(e); j < (int)ceil(f); j++)
            for (i = (int)floor(a); i < (int)ceil(b); i++) {
                double w = (fmin(b, i + 1) - fmax(a, i))
                         * (fmin(f, j + 1) - fmax(e, j));
                frame_px(i, j, fw, fh, p);
                s += lin(p[c]) * w;
            }
        return enc(s / (sx * sy));
    } else {
        double u = (x + 0.5) * sx - 0.5, v = (y + 0.5) * sy - 0.5, fu, fv;
        int i0, j0, i1, j1;
        unsigned char q[4][4];
        u = fmin(fmax(u, 0.0), fw - 1.0);
        v = fmin(fmax(v, 0.0), fh - 1.0);
        i0 = (int)floor(u); j0 = (int)floor(v);
        i1 = i0 + 1 < fw ? i0 + 1 : i0;
        j1 = j0 + 1 < fh ? j0 + 1 : j0;
        fu = u - i0; fv = v - j0;
        frame_px(i0, j0, fw, fh, q[0]);
        frame_px(i1, j0, fw, fh, q[1]);
        frame_px(i0, j1, fw, fh, q[2]);
        frame_px(i1, j1, fw, fh, q[3]);
        return (q[0][c] * (1 - fu) + q[1][c] * fu) * (1 - fv)
             + (q[2][c] * (1 - fu) + q[3][c] * fu) * fv;
    }
}

/* Largest difference to the reference over the colour channels, and how
 * many magenta-tinted pixels (bleed from beside the frame) there are. */
static void check(const char *name, int fw, int fh, int shrink, int tol)
{
    unsigned char *out = malloc((size_t)OUT_W * OUT_H * 4);
    double worst = 0.0, sum = 0.0, edge = 0.0;
    int x, y, c;

    if (!run(fw, fh, out)) {
        printf("FAIL %s: postfx_draw returned 0\n", name);
        s_fail = 1;
        free(out);
        return;
    }
    for (y = 0; y < OUT_H; y++)
        for (x = 0; x < OUT_W; x++)
            for (c = 0; c < 3; c++) {
                double d = fabs(expect(fw, fh, x, y, c, shrink)
                                - out[((size_t)y * OUT_W + x) * 4 + c]);
                sum += d;
                if (d > worst) worst = d;
                /* Red along the right edge and green along the bottom are
                 * smooth ramps near 255; magenta (no green) or the
                 * texture's edge would pull them away. */
                if ((x == OUT_W - 1 && c == 2) || (y == OUT_H - 1 && c == 1))
                    if (d > edge) edge = d;
            }
    sum /= (double)OUT_W * OUT_H * 3;
    printf("%s %s: %dx%d -> %dx%d, max diff %.1f, mean %.3f, edge %.1f"
           " (pixel 0,0: %u %u %u)\n",
           worst <= tol && edge <= 4.0 ? "ok  " : "FAIL", name, fw, fh,
           OUT_W, OUT_H, worst, sum, edge, out[2], out[1], out[0]);
    if (worst > tol || edge > 4.0)
        s_fail = 1;
    free(out);
}

int main(int argc, char **argv)
{
    int hw = argc > 1 && !strcmp(argv[1], "--hw");
    HRESULT hr = D3D11CreateDevice(NULL, hw ? D3D_DRIVER_TYPE_HARDWARE
                                            : D3D_DRIVER_TYPE_WARP, NULL, 0,
                                   NULL, 0, D3D11_SDK_VERSION, &s_dev, NULL,
                                   &s_ctx);
    if (FAILED(hr)) {
        printf("FAIL: D3D11CreateDevice 0x%08lX\n", (unsigned long)hr);
        return 1;
    }
    check("shrink 200 %", 1280, 960, 1, 1);
    check("shrink 150 %", 960, 720, 1, 1);
    check("shrink 125 %", 800, 600, 1, 1);
    check("bilinear 50 %", 320, 240, 0, 1);
    check("bilinear 75 %", 480, 360, 0, 1);
    xbox_SetUpscaler(1);
    /* FSR: sharper than bilinear, so only loosely like it; magenta from
     * beside the frame would push the blue and red far off at the edge. */
    check("fsr 50 %", 320, 240, 0, 140);
    check("fsr 67 %", 429, 322, 0, 140);
    xbox_SetSharpen(90.0f);
    check("fsr+rcas 50 %", 320, 240, 0, 160);
    puts(s_fail ? "FAILED" : "all passed");
    return s_fail;
}
