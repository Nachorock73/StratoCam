#include "D3DRenderer.h"
#include "PipelineDiagnostics.h"
#include "PreviewGeometry.h"

#include <d3d10.h>

namespace {
constexpr char kVertexShader[] = R"(
struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Output main(uint id : SV_VertexID) {
    Output output;
    float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    output.uv = uv;
    return output;
})";

constexpr char kPixelShader[] = R"(
cbuffer ViewTransform : register(b0) { float4 transform; float4 focus; float4 viewInfo; };
Texture2D yTexture : register(t0);
Texture2D uTexture : register(t1);
Texture2D vTexture : register(t2);
SamplerState frameSampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 sampleUv = (uv - 0.5) * transform.yz + 0.5;
    if (transform.x < 45.0) sampleUv = sampleUv;
    else if (transform.x < 135.0) sampleUv = float2(sampleUv.y, 1.0 - sampleUv.x);
    else if (transform.x < 225.0) sampleUv = 1.0 - sampleUv;
    else sampleUv = float2(1.0 - sampleUv.y, sampleUv.x);
    float y = 1.16438356 * (yTexture.Sample(frameSampler, sampleUv).r - 0.0627451);
    float u = uTexture.Sample(frameSampler, sampleUv).r - 0.5;
    float v = vTexture.Sample(frameSampler, sampleUv).r - 0.5;
    float4 color = float4(y + 1.59602678 * v, y - 0.39176229 * u - 0.81296764 * v, y + 2.01723214 * u, 1.0);
    if (focus.z > 0.5) {
        float2 delta = abs((uv - focus.xy) * viewInfo.xy);
        float edge = max(delta.x, delta.y);
        bool square = abs(edge - 20.0) < 1.6 && min(delta.x, delta.y) < 20.0;
        bool crosshair = (delta.x < 1.2 && delta.y < 6.0) || (delta.y < 1.2 && delta.x < 6.0);
        if (square || crosshair) {
            float3 indicator = focus.z < 1.5 ? float3(1.0, 0.70, 0.20) :
                               focus.z < 2.5 ? float3(0.25, 0.90, 0.52) : float3(1.0, 0.30, 0.28);
            color = float4(indicator, 1.0);
        }
    }
    return color;
})";

constexpr char kNv12PixelShader[] = R"(
cbuffer ViewTransform : register(b0) { float4 transform; float4 focus; float4 viewInfo; };
Texture2D yTexture : register(t0);
Texture2D uvTexture : register(t1);
SamplerState frameSampler : register(s0);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 sampleUv = (uv - 0.5) * transform.yz + 0.5;
    if (transform.x < 45.0) sampleUv = sampleUv;
    else if (transform.x < 135.0) sampleUv = float2(sampleUv.y, 1.0 - sampleUv.x);
    else if (transform.x < 225.0) sampleUv = 1.0 - sampleUv;
    else sampleUv = float2(1.0 - sampleUv.y, sampleUv.x);
    float y = 1.16438356 * (yTexture.Sample(frameSampler, sampleUv).r - 0.0627451);
    float2 chroma = uvTexture.Sample(frameSampler, sampleUv).rg - 0.5;
    float4 color = float4(y + 1.59602678 * chroma.y,
                         y - 0.39176229 * chroma.x - 0.81296764 * chroma.y,
                         y + 2.01723214 * chroma.x, 1.0);
    if (focus.z > 0.5) {
        float2 delta = abs((uv - focus.xy) * viewInfo.xy);
        float edge = max(delta.x, delta.y);
        bool square = abs(edge - 20.0) < 1.6 && min(delta.x, delta.y) < 20.0;
        bool crosshair = (delta.x < 1.2 && delta.y < 6.0) || (delta.y < 1.2 && delta.x < 6.0);
        if (square || crosshair) {
            float3 indicator = focus.z < 1.5 ? float3(1.0, 0.70, 0.20) :
                               focus.z < 2.5 ? float3(0.25, 0.90, 0.52) : float3(1.0, 0.30, 0.28);
            color = float4(indicator, 1.0);
        }
    }
    return color;
})";
}

D3DRenderer::D3DRenderer(RenderedCallback renderedCallback, StatusCallback statusCallback)
    : renderedCallback_(std::move(renderedCallback)), statusCallback_(std::move(statusCallback)) {}

D3DRenderer::~D3DRenderer() { ReleaseDevice(); }

bool D3DRenderer::Initialize(HWND window) {
    window_ = window;
    RECT bounds{};
    GetClientRect(window_, &bounds);
    DXGI_SWAP_CHAIN_DESC swap{};
    swap.BufferDesc.Width = std::max<LONG>(1, bounds.right);
    swap.BufferDesc.Height = std::max<LONG>(1, bounds.bottom);
    swap.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swap.SampleDesc.Count = 1;
    swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap.BufferCount = 2;
    swap.OutputWindow = window_;
    swap.Windowed = TRUE;
    swap.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    D3D_FEATURE_LEVEL selected{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION, &swap, &swapChain_, &device_, &selected, &context_);
    if (FAILED(result)) {
        statusCallback_("D3D11 hardware device creation failed");
        return false;
    }
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(adapter->GetDesc(&description))) adapterName_ = WideToUtf8(description.Description);
    }
    SafeRelease(adapter);
    SafeRelease(dxgiDevice);
    ID3D10Multithread* multithread = nullptr;
    if (SUCCEEDED(device_->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void**>(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
        multithread->Release();
    }
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.ScissorEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rasterizer, &rasterizerState_))) return false;
    if (!CreateRenderTarget() || !CreateShaders()) return false;
    bufferWidth_ = std::max<LONG>(1, bounds.right);
    bufferHeight_ = std::max<LONG>(1, bounds.bottom);
    destinationRect_ = {0, 0, bufferWidth_, bufferHeight_};
    PipelineDiagnostics::Log("renderer", "D3D11 adapter=" + adapterName_);
    statusCallback_("Direct3D 11 renderer ready · " + adapterName_);
    return true;
}

bool D3DRenderer::CreateRenderTarget() {
    SafeRelease(renderTarget_);
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT result = swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    D3D11_TEXTURE2D_DESC description{};
    if (SUCCEEDED(result)) {
        backBuffer->GetDesc(&description);
        result = device_->CreateRenderTargetView(backBuffer, nullptr, &renderTarget_);
    }
    SafeRelease(backBuffer);
    if (SUCCEEDED(result)) {
        PipelineDiagnostics::Log("resize", "render target recreated size=" +
            std::to_string(description.Width) + "x" + std::to_string(description.Height));
    }
    return SUCCEEDED(result);
}

bool D3DRenderer::CreateShaders() {
    ID3DBlob* vertexBlob = nullptr;
    ID3DBlob* pixelBlob = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT result = D3DCompile(kVertexShader, sizeof(kVertexShader), nullptr, nullptr, nullptr, "main", "vs_4_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertexBlob, &errors);
    if (FAILED(result)) {
        statusCallback_(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "Vertex shader compilation failed");
        SafeRelease(errors);
        return false;
    }
    SafeRelease(errors);
    result = D3DCompile(kPixelShader, sizeof(kPixelShader), nullptr, nullptr, nullptr, "main", "ps_4_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixelBlob, &errors);
    if (FAILED(result)) {
        statusCallback_(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "Pixel shader compilation failed");
        SafeRelease(errors);
        SafeRelease(vertexBlob);
        return false;
    }
    ID3DBlob* nv12Blob = nullptr;
    if (SUCCEEDED(result)) result = D3DCompile(kNv12PixelShader, sizeof(kNv12PixelShader), nullptr, nullptr, nullptr,
                                               "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                               &nv12Blob, &errors);
    result = SUCCEEDED(result)
        ? device_->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &vertexShader_)
        : result;
    if (SUCCEEDED(result)) result = device_->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, &pixelShader_);
    if (SUCCEEDED(result)) result = device_->CreatePixelShader(nv12Blob->GetBufferPointer(), nv12Blob->GetBufferSize(), nullptr, &pixelShaderNv12_);
    SafeRelease(vertexBlob);
    SafeRelease(pixelBlob);
    SafeRelease(nv12Blob);
    SafeRelease(errors);
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (SUCCEEDED(result)) result = device_->CreateSamplerState(&sampler, &sampler_);
    D3D11_BUFFER_DESC constants{};
    constants.ByteWidth = 48;
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (SUCCEEDED(result)) result = device_->CreateBuffer(&constants, nullptr, &transformBuffer_);
    return SUCCEEDED(result);
}

void D3DRenderer::SetViewTransform(int rotationDegrees, PreviewScaleMode scaleMode) {
    rotationDegrees_.store(((rotationDegrees % 360) + 360) % 360);
    scaleMode_.store(scaleMode);
}

void D3DRenderer::SetFocusIndicator(float displayU, float displayV, int state) {
    focusU_ = std::clamp(displayU, 0.0f, 1.0f);
    focusV_ = std::clamp(displayV, 0.0f, 1.0f);
    focusState_ = state;
}

void D3DRenderer::ClearFocusIndicator() { focusState_ = 0; }

bool D3DRenderer::EnsureTextures(int width, int height) {
    if (frameTextures_[0] && width == textureWidth_ && height == textureHeight_) return true;
    for (int plane = 0; plane < 3; ++plane) {
        SafeRelease(frameViews_[plane]);
        SafeRelease(frameTextures_[plane]);
    }
    HRESULT result = S_OK;
    for (int plane = 0; plane < 3 && SUCCEEDED(result); ++plane) {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = plane == 0 ? width : (width + 1) / 2;
        texture.Height = plane == 0 ? height : (height + 1) / 2;
        texture.MipLevels = 1;
        texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R8_UNORM;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DYNAMIC;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texture.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        result = device_->CreateTexture2D(&texture, nullptr, &frameTextures_[plane]);
        if (SUCCEEDED(result)) result = device_->CreateShaderResourceView(frameTextures_[plane], nullptr, &frameViews_[plane]);
    }
    if (FAILED(result)) return false;
    textureWidth_ = width;
    textureHeight_ = height;
    return true;
}

bool D3DRenderer::EnsureNv12Texture(int width, int height) {
    if (nv12Texture_ && width == nv12Width_ && height == nv12Height_) return true;
    for (auto*& view : nv12Views_) SafeRelease(view);
    SafeRelease(nv12Texture_);
    SafeRelease(nv12Readback_);
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = width;
    texture.Height = height;
    texture.MipLevels = 1;
    texture.ArraySize = 1;
    texture.Format = DXGI_FORMAT_NV12;
    texture.SampleDesc.Count = 1;
    texture.Usage = D3D11_USAGE_DEFAULT;
    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    HRESULT result = device_->CreateTexture2D(&texture, nullptr, &nv12Texture_);
    D3D11_SHADER_RESOURCE_VIEW_DESC yView{};
    yView.Format = DXGI_FORMAT_R8_UNORM;
    yView.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    yView.Texture2D.MipLevels = 1;
    if (SUCCEEDED(result)) result = device_->CreateShaderResourceView(nv12Texture_, &yView, &nv12Views_[0]);
    D3D11_SHADER_RESOURCE_VIEW_DESC uvView = yView;
    uvView.Format = DXGI_FORMAT_R8G8_UNORM;
    if (SUCCEEDED(result)) result = device_->CreateShaderResourceView(nv12Texture_, &uvView, &nv12Views_[1]);
    if (FAILED(result)) {
        PipelineDiagnostics::Log("renderer", "NV12 shader texture creation failed HRESULT=" +
            std::to_string(static_cast<long>(result)));
        return false;
    }
    texture.Usage = D3D11_USAGE_STAGING;
    texture.BindFlags = 0;
    texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&texture, nullptr, &nv12Readback_))) return false;
    nv12Width_ = width;
    nv12Height_ = height;
    return true;
}

bool D3DRenderer::EnsureCpuI420(DecodedFrame& frame) {
    if (frame.storage == DecodedFrameStorage::I420) return true;
    if (!frame.texture || !EnsureNv12Texture(frame.width, frame.height)) return false;
    context_->CopySubresourceRegion(nv12Readback_, 0, 0, 0, 0, frame.texture, frame.textureSubresource, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(nv12Readback_, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    frame.yStride = frame.width;
    frame.uvStride = (frame.width + 1) / 2;
    const int chromaHeight = (frame.height + 1) / 2;
    frame.y.resize(static_cast<size_t>(frame.yStride) * frame.height);
    frame.u.resize(static_cast<size_t>(frame.uvStride) * chromaHeight);
    frame.v.resize(static_cast<size_t>(frame.uvStride) * chromaHeight);
    const auto* source = static_cast<const uint8_t*>(mapped.pData);
    for (int row = 0; row < frame.height; ++row) {
        std::memcpy(frame.y.data() + static_cast<size_t>(row) * frame.yStride,
                    source + static_cast<size_t>(row) * mapped.RowPitch, frame.width);
    }
    const auto* uv = source + static_cast<size_t>(mapped.RowPitch) * frame.height;
    for (int row = 0; row < chromaHeight; ++row) {
        const auto* sourceRow = uv + static_cast<size_t>(row) * mapped.RowPitch;
        auto* u = frame.u.data() + static_cast<size_t>(row) * frame.uvStride;
        auto* v = frame.v.data() + static_cast<size_t>(row) * frame.uvStride;
        for (int column = 0; column < frame.uvStride; ++column) {
            u[column] = sourceRow[column * 2];
            v[column] = sourceRow[column * 2 + 1];
        }
    }
    context_->Unmap(nv12Readback_, 0);
    return true;
}

void D3DRenderer::Submit(std::shared_ptr<DecodedFrame> frame) {
    const uint64_t submitted = ++submittedFrames_;
    frame->timing.d3dSubmitNs = QpcNanoseconds();
    bool post = false;
    {
        std::lock_guard lock(latestMutex_);
        if (latestFrame_) {
            ++droppedDecodedFrames_;
            const uint64_t superseded = ++supersededFrames_;
            if (superseded <= 3 || superseded % 30 == 0)
                PipelineDiagnostics::Log("renderer", "DROP decoded frame: superseded by latest frame; total=" +
                    std::to_string(superseded));
        }
        frame->timing.decodedFrameReplacementCount = supersededFrames_.load();
        latestFrame_ = std::move(frame);
        if (!renderMessagePending_) {
            renderMessagePending_ = true;
            post = true;
        }
    }
    if (submitted <= 3 || submitted % 30 == 0)
        PipelineDiagnostics::Log("renderer", "decoded-frame slot submitted=" + std::to_string(submitted));
    if (post && window_) PostMessageW(window_, WM_APP_RENDER_FRAME, 0, 0);
}

void D3DRenderer::RequestLatestRender() {
    bool post = false;
    {
        std::lock_guard lock(latestMutex_);
        if (latestFrame_ && !renderMessagePending_ && !minimized_.load() && !resizeInProgress_.load()) {
            renderMessagePending_ = true;
            post = true;
        }
    }
    if (post && window_) PostMessageW(window_, WM_APP_RENDER_FRAME, 0, 0);
}

void D3DRenderer::SetDestinationRect(int left, int top, int width, int height) {
    destinationRect_ = {left, top, left + std::max(0, width), top + std::max(0, height)};
    ++geometryRevision_;
    RECT client{};
    GetClientRect(window_, &client);
    PipelineDiagnostics::Log("resize", "destination revision=" + std::to_string(geometryRevision_) +
        " rect=" + std::to_string(destinationRect_.left) + "," + std::to_string(destinationRect_.top) +
        "," + std::to_string(destinationRect_.right) + "," + std::to_string(destinationRect_.bottom) +
        " previewClient=" + std::to_string(client.right) + "x" + std::to_string(client.bottom));
    RequestLatestRender();
}

void D3DRenderer::SetMinimized(bool minimized) {
    minimized_.store(minimized);
    if (!minimized) RequestLatestRender();
}

void D3DRenderer::SetResizeInProgress(bool resizing) {
    const bool previous = resizeInProgress_.exchange(resizing);
    if (previous == resizing) return;
    PipelineDiagnostics::Log("resize", resizing ? "presentation suspended for window resize"
                                                 : "presentation resumed after window resize");
    if (!resizing) RequestLatestRender();
}

void D3DRenderer::RenderLatest() {
    RECT client{};
    GetClientRect(window_, &client);
    RECT destination{
        std::clamp(destinationRect_.left, 0L, client.right),
        std::clamp(destinationRect_.top, 0L, client.bottom),
        std::clamp(destinationRect_.right, 0L, client.right),
        std::clamp(destinationRect_.bottom, 0L, client.bottom)
    };
    if (minimized_.load() || resizeInProgress_.load() || !renderTarget_ || destination.right <= destination.left ||
        destination.bottom <= destination.top || bufferWidth_ <= 0 || bufferHeight_ <= 0) {
        std::lock_guard lock(latestMutex_);
        renderMessagePending_ = false;
        return;
    }
    std::shared_ptr<DecodedFrame> frame;
    {
        std::lock_guard lock(latestMutex_);
        frame.swap(latestFrame_);
        renderMessagePending_ = false;
    }
    if (!frame) { PipelineDiagnostics::Log("renderer", "DROP render: decoded-frame slot empty"); return; }
    if (!device_) { PipelineDiagnostics::Log("renderer", "DROP render: D3D device unavailable"); return; }
    const bool gpuNv12 = frame->storage == DecodedFrameStorage::D3D11Nv12;
    if (gpuNv12) {
        if (!frame->texture || !EnsureNv12Texture(frame->width, frame->height)) {
            PipelineDiagnostics::Log("renderer", "DROP render: NV12 texture unavailable"); return;
        }
        context_->CopySubresourceRegion(nv12Texture_, 0, 0, 0, 0, frame->texture, frame->textureSubresource, nullptr);
    } else {
        if (!EnsureTextures(frame->width, frame->height)) { PipelineDiagnostics::Log("renderer", "DROP render: texture creation failed"); return; }
        const uint8_t* sources[] = {frame->y.data(), frame->u.data(), frame->v.data()};
        const int strides[] = {frame->yStride, frame->uvStride, frame->uvStride};
        const int heights[] = {frame->height, (frame->height + 1) / 2, (frame->height + 1) / 2};
        for (int plane = 0; plane < 3; ++plane) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context_->Map(frameTextures_[plane], 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                PipelineDiagnostics::Log("renderer", "DROP render: texture map failed plane=" + std::to_string(plane)); return;
            }
            for (int row = 0; row < heights[plane]; ++row) {
                std::memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
                            sources[plane] + static_cast<size_t>(row) * strides[plane], static_cast<size_t>(strides[plane]));
            }
            context_->Unmap(frameTextures_[plane], 0);
        }
    }
    const float viewWidth = static_cast<float>(destination.right - destination.left);
    const float viewHeight = static_cast<float>(destination.bottom - destination.top);
    const int rotation = rotationDegrees_.load();
    const PreviewScaleMode scaleMode = scaleMode_.load();
    const auto geometry = CalculatePreviewDisplayGeometry(
        viewWidth, viewHeight, frame->width, frame->height, rotation, scaleMode);
    if (!geometry.valid) return;
    D3D11_VIEWPORT viewport{
        static_cast<float>(destination.left + geometry.imageLeft),
        static_cast<float>(destination.top + geometry.imageTop),
        static_cast<float>(geometry.imageWidth),
        static_cast<float>(geometry.imageHeight), 0.0f, 1.0f};
    float constants[12] = {
        static_cast<float>(rotation), static_cast<float>(geometry.sampleScaleX),
        static_cast<float>(geometry.sampleScaleY), 0.0f,
        focusU_, focusV_, static_cast<float>(focusState_), 0.0f,
        viewport.Width, viewport.Height, 0.0f, 0.0f
    };
    const D3D11_RECT scissor{destination.left, destination.top, destination.right, destination.bottom};
    if (renderedGeometryRevision_ != geometryRevision_) {
        renderedGeometryRevision_ = geometryRevision_;
        PipelineDiagnostics::Log("resize", "render revision=" + std::to_string(geometryRevision_) +
            " client=" + std::to_string(client.right) + "x" + std::to_string(client.bottom) +
            " buffer=" + std::to_string(bufferWidth_) + "x" + std::to_string(bufferHeight_) +
            " viewport=" + std::to_string(viewport.TopLeftX) + "," + std::to_string(viewport.TopLeftY) +
            "," + std::to_string(viewport.Width) + "," + std::to_string(viewport.Height) +
            " scissor=" + std::to_string(scissor.left) + "," + std::to_string(scissor.top) +
            "," + std::to_string(scissor.right) + "," + std::to_string(scissor.bottom));
    }
    const float clear[] = {0.015f, 0.02f, 0.03f, 1.0f};
    context_->OMSetRenderTargets(1, &renderTarget_, nullptr);
    context_->ClearRenderTargetView(renderTarget_, clear);
    context_->RSSetState(rasterizerState_);
    context_->RSSetViewports(1, &viewport);
    context_->RSSetScissorRects(1, &scissor);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_, nullptr, 0);
    context_->PSSetShader(gpuNv12 ? pixelShaderNv12_ : pixelShader_, nullptr, 0);
    context_->PSSetShaderResources(0, gpuNv12 ? 2 : 3, gpuNv12 ? nv12Views_ : frameViews_);
    context_->PSSetSamplers(0, 1, &sampler_);
    context_->UpdateSubresource(transformBuffer_, 0, nullptr, constants, 0, 0);
    context_->PSSetConstantBuffers(0, 1, &transformBuffer_);
    context_->Draw(3, 0);
    const HRESULT presented = swapChain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (SUCCEEDED(presented)) {
        ++renderedFrames_;
        if (renderedFrames_ <= 3 || renderedFrames_ % 30 == 0)
            PipelineDiagnostics::Log("renderer", "D3D present submitted=" + std::to_string(renderedFrames_));
        frame->timing.renderSubmitNs = QpcNanoseconds();
        frame->timing.processingLatencyMs =
            (frame->timing.renderSubmitNs - frame->timing.windowsReceiveNs) / 1'000'000.0;
        frame->timing.liveFrameAgeMs = frame->timing.knownPreDecoderMs > 0
            ? frame->timing.knownPreDecoderMs + frame->timing.processingLatencyMs
            : 0.0;
        renderedCallback_(frame->timing);
    } else if (presented == DXGI_ERROR_WAS_STILL_DRAWING) {
        ++droppedDecodedFrames_;
        const uint64_t busy = ++presentBusyFrames_;
        if (busy <= 3 || busy % 30 == 0)
            PipelineDiagnostics::Log("renderer", "DROP render: DXGI still drawing; total=" + std::to_string(busy));
    } else {
        PipelineDiagnostics::Log("renderer", "DROP render: Present HRESULT=" + std::to_string(static_cast<long>(presented)));
    }
}

void D3DRenderer::Resize(int width, int height, bool minimized) {
    if (!swapChain_ || !window_) return;
    RECT actual{};
    GetClientRect(window_, &actual);
    PipelineDiagnostics::Log("resize", "Resize request=" + std::to_string(width) + "x" +
        std::to_string(height) + " actual=" + std::to_string(actual.right) + "x" +
        std::to_string(actual.bottom) + " previous=" + std::to_string(bufferWidth_) + "x" +
        std::to_string(bufferHeight_) + " minimized=" + (minimized ? "true" : "false"));
    minimized_.store(minimized || width <= 0 || height <= 0);
    if (minimized_.load()) return;
    if (width == bufferWidth_ && height == bufferHeight_ && renderTarget_) {
        RequestLatestRender();
        return;
    }
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* nullViews[3]{};
    context_->PSSetShaderResources(0, 3, nullViews);
    SafeRelease(renderTarget_);
    context_->Flush();
    const HRESULT resized = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(resized) || !CreateRenderTarget()) {
        bufferWidth_ = 0;
        bufferHeight_ = 0;
        PipelineDiagnostics::Log("renderer", "ResizeBuffers failed HRESULT=" +
            std::to_string(static_cast<long>(resized)) + " requested=" +
            std::to_string(width) + "x" + std::to_string(height));
        statusCallback_("Direct3D preview resize failed; waiting for the next layout");
        return;
    }
    bufferWidth_ = width;
    bufferHeight_ = height;
    ++geometryRevision_;
    PipelineDiagnostics::Log("resize", "ResizeBuffers succeeded revision=" +
        std::to_string(geometryRevision_) + " buffer=" + std::to_string(bufferWidth_) + "x" +
        std::to_string(bufferHeight_));
    RequestLatestRender();
}

void D3DRenderer::ReleaseDevice() {
    for (int plane = 0; plane < 3; ++plane) {
        SafeRelease(frameViews_[plane]);
        SafeRelease(frameTextures_[plane]);
    }
    for (auto*& view : nv12Views_) SafeRelease(view);
    SafeRelease(nv12Readback_);
    SafeRelease(nv12Texture_);
    SafeRelease(rasterizerState_);
    SafeRelease(sampler_);
    SafeRelease(transformBuffer_);
    SafeRelease(pixelShader_);
    SafeRelease(pixelShaderNv12_);
    SafeRelease(vertexShader_);
    SafeRelease(renderTarget_);
    SafeRelease(swapChain_);
    SafeRelease(context_);
    SafeRelease(device_);
}
