// HLSL for motion estimation + frame extrapolation. Compiled at runtime with D3DCompile.
//
// Pipeline per eye, per new game frame:
//   CSLuma   : color (view rect) -> luma at 1/4 res (L)
//   CSComp   : previous L re-projected into the current eye orientation (removes head rotation)
//   CSDown   : L -> 1/16 res (C) for both frames
//   CSCoarse : full search on C (+-6 px = +-96 full-res px), one vector per 16x16 block
//   CSFine   : candidate + refine search on L, one vector per 16x16 block
//   CSFilter : 3x3 vector median
// Per synthesized frame:
//   PSWarp   : out(x) = src(x - s*mv), fixed-point iterated (backward warp)
#pragma once

static const char kShaderSource[] = R"HLSL(
cbuffer CB : register(b0) {
    float4 gRect;   // source view rect x, y, w, h (pixels)
    float4 gInv;    // 1/texW, 1/texH, arraySlice, debugTint
    float4 gFov1;   // tanLeft, tanRight, tanUp, tanDown (current frame)
    float4 gFov0;   // same for previous frame
    float4 gM0; float4 gM1; float4 gM2;  // rows of R0^T * R1
    float4 gP;      // s (extrapolation factor), lw, lh, 0
    float4 gG;      // cw, ch, 0, 0
};
SamplerState gLin : register(s0);
Texture2DArray<float4> gSrc : register(t0);
Texture2D<float>  gIn0 : register(t1);
Texture2D<float>  gIn1 : register(t2);
Texture2D<float2> gMvIn : register(t3);
RWTexture2D<float>  gOutF : register(u0);
RWTexture2D<float2> gOutV : register(u1);

[numthreads(8, 8, 1)]
void CSLuma(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)gP.y || id.y >= (uint)gP.z) return;
    float2 base = gRect.xy + float2(id.xy) * 4.0;
    float2 lo = gRect.xy + 1.0, hi = gRect.xy + gRect.zw - 1.0;
    float l = 0;
    [unroll] for (int j = 0; j < 2; j++)
    [unroll] for (int i = 0; i < 2; i++) {
        float2 p = clamp(base + float2(1 + 2 * i, 1 + 2 * j), lo, hi);
        float3 c = gSrc.SampleLevel(gLin, float3(p * gInv.xy, gInv.z), 0).rgb;
        l += dot(saturate(c), float3(0.299, 0.587, 0.114));
    }
    gOutF[id.xy] = sqrt(l * 0.25);
}

[numthreads(8, 8, 1)]
void CSComp(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)gP.y || id.y >= (uint)gP.z) return;
    float2 uv = (float2(id.xy) + 0.5) / gP.yz;
    float3 d = float3(lerp(gFov1.x, gFov1.y, uv.x), lerp(gFov1.z, gFov1.w, uv.y), -1.0);
    float3 d0 = float3(dot(gM0.xyz, d), dot(gM1.xyz, d), dot(gM2.xyz, d));
    float r = -1.0;
    if (d0.z < -1e-3) {
        float2 t = d0.xy / -d0.z;
        float2 uv0 = float2((t.x - gFov0.x) / (gFov0.y - gFov0.x), (t.y - gFov0.z) / (gFov0.w - gFov0.z));
        if (all(uv0 >= 0.0) && all(uv0 <= 1.0)) r = gIn0.SampleLevel(gLin, uv0, 0);
    }
    gOutF[id.xy] = r;
}

[numthreads(8, 8, 1)]
void CSDown(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)gG.x || id.y >= (uint)gG.y) return;
    int2 b = int2(id.xy) * 4;
    int2 mx = int2(gP.yz) - 1;
    float s = 0;
    bool bad = false;
    [unroll] for (int j = 0; j < 4; j++)
    [unroll] for (int i = 0; i < 4; i++) {
        float v = gIn0.Load(int3(min(b + int2(i, j), mx), 0));
        bad = bad || (v < 0);
        s += v;
    }
    gOutF[id.xy] = bad ? -1.0 : s / 16.0;
}

float Fetch(Texture2D<float> t, int2 p, int2 size) {
    if (any(p < 0) || any(p >= size)) return -1.0;
    return t.Load(int3(p, 0));
}
float Diff(float a, float b) { return (a < 0 || b < 0) ? 0.2 : abs(a - b); }

#define CR 6
[numthreads(8, 8, 1)]
void CSCoarse(uint3 id : SV_DispatchThreadID) {
    int2 size = int2(gG.xy);
    int2 p = int2(id.xy);
    if (any(p >= size)) return;
    float c[25];
    [unroll] for (int k = 0; k < 25; k++) c[k] = Fetch(gIn1, p + int2(k % 5 - 2, k / 5 - 2), size);
    float best = 1e9;
    float2 bv = 0;
    [loop] for (int vy = -CR; vy <= CR; vy++)
    [loop] for (int vx = -CR; vx <= CR; vx++) {
        float cost = 0;
        [unroll] for (int k = 0; k < 25; k++)
            cost += Diff(c[k], Fetch(gIn0, p + int2(k % 5 - 2, k / 5 - 2) - int2(vx, vy), size));
        cost = cost / 25.0 + 0.003 * length(float2(vx, vy));
        if (cost < best) { best = cost; bv = float2(vx, vy); }
    }
    gOutV[id.xy] = bv;
}

float BlockCost(int2 o, int2 v, int2 size) {
    float cost = 0;
    [unroll] for (int j = 0; j < 8; j++)
    [unroll] for (int i = 0; i < 8; i++) {
        int2 q = o + int2(i, j);
        cost += Diff(Fetch(gIn1, q, size), Fetch(gIn0, q - v, size));
    }
    return cost / 64.0 + 0.0015 * length(float2(v));
}

[numthreads(8, 8, 1)]
void CSFine(uint3 id : SV_DispatchThreadID) {
    int2 gsize = int2(gG.xy);
    int2 g = int2(id.xy);
    if (any(g >= gsize)) return;
    int2 lsize = int2(gP.yz);
    int2 o = g * 4 - 2;
    float best = BlockCost(o, int2(0, 0), lsize) - 0.002;  // slight preference for "static"
    int2 bv = 0;
    [loop] for (int k = 0; k < 9; k++) {
        int2 n = clamp(g + int2(k % 3 - 1, k / 3 - 1), 0, gsize - 1);
        int2 v = int2(round(gMvIn.Load(int3(n, 0)) * 4.0));
        float c = BlockCost(o, v, lsize);
        if (c < best) { best = c; bv = v; }
    }
    int2 c0 = bv;
    [loop] for (int j = -2; j <= 2; j++)
    [loop] for (int i = -2; i <= 2; i++) {
        if (i == 0 && j == 0) continue;
        int2 v = c0 + int2(i, j);
        float c = BlockCost(o, v, lsize);
        if (c < best) { best = c; bv = v; }
    }
    gOutV[g] = float2(bv);
}

float Median9(float a[9]) {
    [unroll] for (int i = 0; i < 5; i++)
    [unroll] for (int j = i + 1; j < 9; j++) {
        float lo = min(a[i], a[j]);
        float hi = max(a[i], a[j]);
        a[i] = lo; a[j] = hi;
    }
    return a[4];
}

[numthreads(8, 8, 1)]
void CSFilter(uint3 id : SV_DispatchThreadID) {
    int2 gsize = int2(gG.xy);
    int2 g = int2(id.xy);
    if (any(g >= gsize)) return;
    float xs[9], ys[9];
    [unroll] for (int k = 0; k < 9; k++) {
        int2 n = clamp(g + int2(k % 3 - 1, k / 3 - 1), 0, gsize - 1);
        float2 v = gMvIn.Load(int3(n, 0));
        xs[k] = v.x; ys[k] = v.y;
    }
    gOutV[g] = float2(Median9(xs), Median9(ys));
}

struct VOut { float4 pos : SV_Position; };
VOut VSFull(uint id : SV_VertexID) {
    VOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float2 Mv(float2 px) { return gMvIn.SampleLevel(gLin, px / (gG.xy * 16.0), 0) * 4.0; }

float4 PSWarp(VOut i) : SV_Target {
    float2 x = i.pos.xy;
    float s = gP.x;
    // Optional rotational reprojection (gG.z = 1): output pixel -> view direction in the output eye
    // (fov1) -> rotate into the source frame's eye (M) -> source pixel (fov0).
    float2 p0 = x;
    if (gG.z > 0.5) {
        float2 uv = x / gRect.zw;
        float3 d = float3(lerp(gFov1.x, gFov1.y, uv.x), lerp(gFov1.z, gFov1.w, uv.y), -1.0);
        float3 d0 = float3(dot(gM0.xyz, d), dot(gM1.xyz, d), dot(gM2.xyz, d));
        d0.z = min(d0.z, -1e-3);
        float2 t = d0.xy / -d0.z;
        float2 uv0 = float2((t.x - gFov0.x) / (gFov0.y - gFov0.x), (t.y - gFov0.z) / (gFov0.w - gFov0.z));
        p0 = uv0 * gRect.zw;
    }
    float2 p = p0 - s * Mv(p0);
    p = p0 - s * Mv(p);
    p = clamp(p, 0.5, gRect.zw - 0.5);
    float4 c = gSrc.SampleLevel(gLin, float3((gRect.xy + p) * gInv.xy, gInv.z), 0);
    if (gInv.w > 0.5) c.rgb = lerp(c.rgb, float3(0.0, 1.0, 0.0), 0.15);
    return c;
}
)HLSL";
