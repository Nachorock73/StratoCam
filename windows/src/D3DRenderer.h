#pragma once

#include "Common.h"
#include "H264Decoder.h"
#include "ViewTransform.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

constexpr UINT WM_APP_RENDER_FRAME = WM_APP + 40;

class D3DRenderer {
public:
    using RenderedCallback = std::function<void(const FrameTiming&)>;
    using StatusCallback = std::function<void(const std::string&)>;

    D3DRenderer(RenderedCallback renderedCallback, StatusCallback statusCallback);
    ~D3DRenderer();
    bool Initialize(HWND window);
    ID3D11Device* Device() const { return device_; }
    const std::string& AdapterName() const { return adapterName_; }
    bool EnsureCpuI420(DecodedFrame& frame);
    void Resize(int width, int height, bool minimized = false);
    void SetDestinationRect(int left, int top, int width, int height);
    void SetMinimized(bool minimized);
    void SetResizeInProgress(bool resizing);
    void Submit(std::shared_ptr<DecodedFrame> frame);
    void RenderLatest();
    void SetViewTransform(int rotationDegrees, PreviewScaleMode scaleMode);
    void SetFocusIndicator(float displayU, float displayV, int state);
    void ClearFocusIndicator();
    float FocusIndicatorU() const { return focusU_; }
    float FocusIndicatorV() const { return focusV_; }
    uint64_t DroppedDecodedFrames() const { return droppedDecodedFrames_.load(); }
    void ResetStatistics() {
        droppedDecodedFrames_.store(0);
        submittedFrames_.store(0);
        supersededFrames_.store(0);
        presentBusyFrames_.store(0);
        renderedFrames_ = 0;
    }

private:
    bool CreateRenderTarget();
    bool CreateShaders();
    bool EnsureTextures(int width, int height);
    bool EnsureNv12Texture(int width, int height);
    void RequestLatestRender();
    void ReleaseDevice();

    HWND window_{};
    RenderedCallback renderedCallback_;
    StatusCallback statusCallback_;
    std::mutex latestMutex_;
    std::shared_ptr<DecodedFrame> latestFrame_;
    bool renderMessagePending_{};
    std::atomic<uint64_t> droppedDecodedFrames_{};
    std::atomic<uint64_t> submittedFrames_{};
    std::atomic<uint64_t> supersededFrames_{};
    std::atomic<uint64_t> presentBusyFrames_{};
    uint64_t renderedFrames_{};
    std::string adapterName_;

    ID3D11Device* device_{};
    ID3D11DeviceContext* context_{};
    IDXGISwapChain* swapChain_{};
    ID3D11RenderTargetView* renderTarget_{};
    ID3D11VertexShader* vertexShader_{};
    ID3D11PixelShader* pixelShader_{};
    ID3D11PixelShader* pixelShaderNv12_{};
    ID3D11SamplerState* sampler_{};
    ID3D11RasterizerState* rasterizerState_{};
    ID3D11Buffer* transformBuffer_{};
    ID3D11Texture2D* frameTextures_[3]{};
    ID3D11ShaderResourceView* frameViews_[3]{};
    ID3D11Texture2D* nv12Texture_{};
    ID3D11ShaderResourceView* nv12Views_[2]{};
    ID3D11Texture2D* nv12Readback_{};
    int textureWidth_{};
    int textureHeight_{};
    int nv12Width_{};
    int nv12Height_{};
    int bufferWidth_{};
    int bufferHeight_{};
    RECT destinationRect_{};
    uint64_t geometryRevision_{};
    uint64_t renderedGeometryRevision_{};
    std::atomic<bool> minimized_{};
    std::atomic<bool> resizeInProgress_{};
    std::atomic<int> rotationDegrees_{};
    std::atomic<PreviewScaleMode> scaleMode_{PreviewScaleMode::Fit};
    float focusU_{0.5f};
    float focusV_{0.5f};
    int focusState_{};
};
