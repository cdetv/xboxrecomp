/**
 * HLSL pieces shared by the host effects that work from the depth buffer
 * (ao_d3d11.c; motion blur next): a C string to put in front of an
 * effect's own shader text.
 *
 *   hfx_vs     the full-screen triangle (SV_VertexID 0..2, no buffers)
 *   hfx_dist   a depth-buffer value d (0..1, D3D's z / w) to the distance
 *              in front of the camera, from its near / far planes -- the
 *              formula dof_d3d11.c's dist_of uses
 *   hfx_view   a pixel to its place in camera space (x right, y up, z the
 *              distance): px the pixel's centre in host pixels, rect the
 *              frame's rectangle (x0, y0, w, h, host pixels: the clip, so
 *              the render scale and any widened aspect are already in),
 *              inv_scale 1 / the projection's x and y scale (m00, m11)
 */
#ifndef HOSTFX_VIEW_H
#define HOSTFX_VIEW_H

#define HOSTFX_VIEW_HLSL \
"float4 hfx_vs(uint id : SV_VertexID) : SV_Position {\n" \
"    float2 t = float2((id << 1) & 2, id & 2);\n" \
"    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);\n" \
"}\n" \
"float hfx_dist(float d, float zn, float zf) {\n" \
"    return zn / max(1.0 - d * (zf - zn) / zf, 1e-7);\n" \
"}\n" \
"float3 hfx_view(float2 px, float d, float4 rect, float2 inv_scale,\n" \
"                float zn, float zf) {\n" \
"    float z = hfx_dist(d, zn, zf);\n" \
"    float2 ndc = (px - rect.xy) / rect.zw * float2(2, -2) + float2(-1, 1);\n" \
"    return float3(ndc * inv_scale * z, z);\n" \
"}\n"

#endif
