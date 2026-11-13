// Shared D3D11 motion-estimation + frame-extrapolation engine (used by the OpenXR layer and the OpenVR hook).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "shaders.h"

namespace fsflow {

using Microsoft::WRL::ComPtr;

struct Tex {
    ComPtr<ID3D11Texture2D> t;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

// Per-eye motion state.
struct View {
    uint32_t lw = 0, lh = 0, cw = 0, ch = 0;
    Tex L[2], Lc, C0, C1, MVc, MV, MVf;
    int curL = 0;
    bool havePrev = false, flowValid = false;
    float prevR[3][3]{};
    float prevFov[4]{};
    uint64_t prevSpace = 0;
};

// Source description: a Texture2DArray SRV + the eye's rectangle inside it.
struct Source {
    ID3D11ShaderResourceView* srv = nullptr;
    uint32_t texW = 0, texH = 0;
    int32_t x = 0, y = 0;
    uint32_t w = 0, h = 0;
    uint32_t slice = 0;
};

struct alignas(16) CBData {
    float rect[4], inv[4], fov1[4], fov0[4], m0[4], m1[4], m2[4], p[4], g[4];
};

// Map a (possibly typeless) DXGI format to a typed format usable for SRV/RTV.
inline DXGI_FORMAT TypedFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default: return f;
    }
}

inline DXGI_FORMAT TypelessFormat(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24G8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32G8X24_TYPELESS;
        default: return f;
    }
}

inline void QuatToMat(float x, float y, float z, float w, float m[3][3]) {
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
}

class Engine {
public:
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;

    bool Init(ID3D11Device* d, ID3D11DeviceContext* c, std::string& err) {
        dev = d;
        ctx = c;
        struct { const char* e; ComPtr<ID3D11ComputeShader>* cs; } list[] = {
            {"CSLuma", &csLuma_}, {"CSComp", &csComp_}, {"CSDown", &csDown_},
            {"CSCoarse", &csCoarse_}, {"CSFine", &csFine_}, {"CSFilter", &csFilter_}};
        for (auto& l : list) {
            ComPtr<ID3DBlob> b;
            if (!Compile(l.e, "cs_5_0", b, err) ||
                FAILED(dev->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, l.cs->GetAddressOf()))) {
                if (err.empty()) err = "compute shader creation failed";
                return false;
            }
        }
        ComPtr<ID3DBlob> vb, pb;
        if (!Compile("VSFull", "vs_5_0", vb, err) || !Compile("PSWarp", "ps_5_0", pb, err) ||
            FAILED(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &vs_)) ||
            FAILED(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &ps_))) {
            if (err.empty()) err = "graphics shader creation failed";
            return false;
        }
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(CBData);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_)) || FAILED(dev->CreateSamplerState(&sd, &lin_))) {
            err = "buffer/sampler creation failed";
            return false;
        }
        return true;
    }

    // Analyze a new frame for one eye. R = eye orientation (row-major 3x3, eye->world),
    // fov = tanLeft, tanRight, tanUp, tanDown. space identifies the reference space.
    void Analyze(View& v, const Source& src, const float R[3][3], const float fov[4], uint64_t space, bool motion) {
        if (!src.srv || src.w == 0 || src.h == 0) { v.flowValid = v.havePrev = false; return; }
        uint32_t lw = (src.w + 3) / 4, lh = (src.h + 3) / 4, cw = (lw + 3) / 4, ch = (lh + 3) / 4;
        if (lw != v.lw || lh != v.lh) {
            v.lw = lw; v.lh = lh; v.cw = cw; v.ch = ch;
            bool ok = MakeTex(v.L[0], lw, lh, DXGI_FORMAT_R16_FLOAT) && MakeTex(v.L[1], lw, lh, DXGI_FORMAT_R16_FLOAT) &&
                      MakeTex(v.Lc, lw, lh, DXGI_FORMAT_R16_FLOAT) && MakeTex(v.C0, cw, ch, DXGI_FORMAT_R16_FLOAT) &&
                      MakeTex(v.C1, cw, ch, DXGI_FORMAT_R16_FLOAT) && MakeTex(v.MVc, cw, ch, DXGI_FORMAT_R16G16_FLOAT) &&
                      MakeTex(v.MV, cw, ch, DXGI_FORMAT_R16G16_FLOAT) && MakeTex(v.MVf, cw, ch, DXGI_FORMAT_R16G16_FLOAT);
            v.havePrev = false;
            if (!ok) { v.lw = v.lh = 0; v.flowValid = false; return; }
        }
        CBData d;
        FillCB(d, v, src);
        float* rows[3] = {d.m0, d.m1, d.m2};
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++)
                rows[i][j] = v.prevR[0][i] * R[0][j] + v.prevR[1][i] * R[1][j] + v.prevR[2][i] * R[2][j];
        memcpy(d.fov1, fov, sizeof(float) * 4);
        memcpy(d.fov0, v.prevFov, sizeof(float) * 4);
        UpdateCB(d);

        Tex& Lcur = v.L[v.curL];
        Tex& Lprev = v.L[1 - v.curL];
        Dispatch(csLuma_.Get(), src.srv, nullptr, nullptr, nullptr, Lcur.uav.Get(), nullptr, lw, lh);
        bool canFlow = motion && v.havePrev && v.prevSpace == space;
        if (canFlow) {
            Dispatch(csComp_.Get(), nullptr, Lprev.srv.Get(), nullptr, nullptr, v.Lc.uav.Get(), nullptr, lw, lh);
            Dispatch(csDown_.Get(), nullptr, Lcur.srv.Get(), nullptr, nullptr, v.C1.uav.Get(), nullptr, cw, ch);
            Dispatch(csDown_.Get(), nullptr, v.Lc.srv.Get(), nullptr, nullptr, v.C0.uav.Get(), nullptr, cw, ch);
            Dispatch(csCoarse_.Get(), nullptr, v.C0.srv.Get(), v.C1.srv.Get(), nullptr, nullptr, v.MVc.uav.Get(), cw, ch);
            Dispatch(csFine_.Get(), nullptr, v.Lc.srv.Get(), Lcur.srv.Get(), v.MVc.srv.Get(), nullptr, v.MV.uav.Get(), cw, ch);
            Dispatch(csFilter_.Get(), nullptr, nullptr, nullptr, v.MV.srv.Get(), nullptr, v.MVf.uav.Get(), cw, ch);
        }
        v.flowValid = canFlow;
        v.curL = 1 - v.curL;
        memcpy(v.prevR, R, sizeof(float) * 9);
        memcpy(v.prevFov, fov, sizeof(float) * 4);
        v.prevSpace = space;
        v.havePrev = true;
    }

    // Render the source eye rect, extrapolated by s frames-of-motion, into rtv (size src.w x src.h).
    // Optional rotational reprojection: Rsrc/Rout = eye->world rotation of the source frame and of the
    // output, fovSrc/fovOut = tanLeft, tanRight, tanUp, tanDown.
    void Warp(View& v, const Source& src, ID3D11RenderTargetView* rtv, float s, bool tint,
              const float (*Rsrc)[3] = nullptr, const float (*Rout)[3] = nullptr, const float* fovSrc = nullptr,
              const float* fovOut = nullptr) {
        CBData d;
        FillCB(d, v, src);
        d.p[0] = s;
        d.inv[3] = tint ? 1.0f : 0.0f;
        if (Rsrc && Rout && fovSrc && fovOut) {
            float* rows[3] = {d.m0, d.m1, d.m2};
            for (int i = 0; i < 3; i++)  // M = Rsrc^T * Rout: output-eye direction -> source-eye direction
                for (int j = 0; j < 3; j++)
                    rows[i][j] = Rsrc[0][i] * Rout[0][j] + Rsrc[1][i] * Rout[1][j] + Rsrc[2][i] * Rout[2][j];
            memcpy(d.fov1, fovOut, sizeof(float) * 4);
            memcpy(d.fov0, fovSrc, sizeof(float) * 4);
            d.g[2] = 1.0f;
        }
        UpdateCB(d);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(nullptr, 0);
        ctx->RSSetState(nullptr);
        D3D11_VIEWPORT vp{0, 0, float(src.w), float(src.h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_.Get(), nullptr, 0);
        ctx->PSSetShader(ps_.Get(), nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->PSSetSamplers(0, 1, lin_.GetAddressOf());
        ID3D11ShaderResourceView* srvs[4] = {src.srv, nullptr, nullptr, v.MVf.srv.Get()};
        ctx->PSSetShaderResources(0, 4, srvs);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nsrv[4] = {};
        ctx->PSSetShaderResources(0, 4, nsrv);
        ID3D11RenderTargetView* nrtv = nullptr;
        ctx->OMSetRenderTargets(1, &nrtv, nullptr);
    }

private:
    ComPtr<ID3D11ComputeShader> csLuma_, csComp_, csDown_, csCoarse_, csFine_, csFilter_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11SamplerState> lin_;

    static bool Compile(const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string& err) {
        ComPtr<ID3DBlob> e;
        HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "framesmooth.hlsl", nullptr, nullptr, entry,
                                target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &e);
        if (FAILED(hr)) {
            err = std::string("shader ") + entry + ": " + (e ? (const char*)e->GetBufferPointer() : "?");
            return false;
        }
        return true;
    }

    bool MakeTex(Tex& t, uint32_t w, uint32_t h, DXGI_FORMAT f) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = std::max(1u, w);
        d.Height = std::max(1u, h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = f;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        t = Tex{};
        return SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &t.t)) &&
               SUCCEEDED(dev->CreateShaderResourceView(t.t.Get(), nullptr, &t.srv)) &&
               SUCCEEDED(dev->CreateUnorderedAccessView(t.t.Get(), nullptr, &t.uav));
    }

    static void FillCB(CBData& d, const View& v, const Source& src) {
        memset(&d, 0, sizeof(d));
        d.rect[0] = float(src.x);
        d.rect[1] = float(src.y);
        d.rect[2] = float(src.w);
        d.rect[3] = float(src.h);
        d.inv[0] = 1.0f / float(src.texW);
        d.inv[1] = 1.0f / float(src.texH);
        d.inv[2] = float(src.slice);
        d.p[1] = float(v.lw);
        d.p[2] = float(v.lh);
        d.g[0] = float(v.cw);
        d.g[1] = float(v.ch);
    }

    void UpdateCB(const CBData& d) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &d, sizeof(d));
            ctx->Unmap(cb_.Get(), 0);
        }
    }

    void Dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1,
                  ID3D11ShaderResourceView* t2, ID3D11ShaderResourceView* t3, ID3D11UnorderedAccessView* uF,
                  ID3D11UnorderedAccessView* uV, uint32_t w, uint32_t h) {
        ID3D11ShaderResourceView* srvs[4] = {t0, t1, t2, t3};
        ID3D11UnorderedAccessView* uavs[2] = {uF, uV};
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
        ctx->CSSetSamplers(0, 1, lin_.GetAddressOf());
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
        ID3D11ShaderResourceView* nsrv[4] = {};
        ID3D11UnorderedAccessView* nuav[2] = {};
        ctx->CSSetShaderResources(0, 4, nsrv);
        ctx->CSSetUnorderedAccessViews(0, 2, nuav, nullptr);
    }
};

// Frame pacing state shared by both front-ends (all times in display slots / seconds).
struct Pacer {
    int divisor = 1;
    double lastSwitch = 0;
    float missEma = 0, earlyEma = 0;
    // returns true if divisor changed
    bool Update(int rateMode, double now) {
        int want = divisor;
        if (rateMode == 1) want = 2;        // always half
        else if (rateMode == 2) want = 1;   // native
        else if (now - lastSwitch > 2.0) {  // auto
            if (divisor == 1 && missEma > 0.10f) want = 2;
            else if (divisor == 2 && earlyEma > 0.85f) want = 1;
        }
        if (want == divisor) return false;
        divisor = want;
        lastSwitch = now;
        missEma = earlyEma = 0;
        return true;
    }
    void Expected(bool hit) { missEma = missEma * 0.95f + (hit ? 0.0f : 0.05f); }
    void Early(bool ready) { earlyEma = earlyEma * 0.97f + (ready ? 0.03f : 0.0f); }
};

inline double NowSec() {
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * inv;
}

}  // namespace fsflow
