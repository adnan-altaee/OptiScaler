#include "pch.h"
#include "GameHudless.h"

#include <Config.h>
#include <State.h>

#include <misc/DeferredRelease.h>

#include <atomic>
#include <d3d12.h>
#include <mutex>
#include <vector>

// ReShade add-on API (v6.8.0, API version 20), events only
#include "../../external/reshade/include/reshade_events.hpp"

namespace
{
// UICompositingParameters_texture0 (the HUD, premultiplied alpha) and _texture1 (the finished scene)
constexpr UINT kHudSlot = 86;
constexpr UINT kSceneSlot = 87;

// How often the share of frames with a HUD-less image is logged
constexpr uint64_t kStatsInterval = 1200;

template <typename T> void SafeRelease(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

// Formats that can be copied into each other with CopyResource
DXGI_FORMAT TypelessFamily(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;

    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return DXGI_FORMAT_R10G10B10A2_TYPELESS;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;

    default:
        return format;
    }
}

// Capture texture and its RTV, created on the game's device from the composite's render target
std::mutex g_mutex;
ID3D11Texture2D* g_texture = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
D3D11_TEXTURE2D_DESC g_textureDesc = {};
ID3D11Device* g_textureDevice = nullptr; // identity only, not referenced

// Set by the Dx11wDx12 swapchain every present
std::atomic<bool> g_active { false };
std::atomic<UINT> g_targetWidth { 0 };
std::atomic<UINT> g_targetHeight { 0 };
std::atomic<UINT> g_targetFormat { DXGI_FORMAT_UNKNOWN };

// Composite draws replayed so far. A present takes the capture when this moved since the previous present, which
// also holds when the game records a frame's deferred command lists before it presents the previous frame.
std::atomic<uint64_t> g_replays { 0 };
std::atomic<bool> g_failed { false };

// Only used under g_mutex. The first composite found locks its pixel shader: later draws with another shader
// (the game's DirectDisplay quads drawn onto the back buffer right after the composite, with the scene still bound
// at t87) are never taken for it. The lock is dropped after kUnlockPresents presents without the locked shader
// (HDR on/off can switch the composite to another shader).
constexpr uint64_t kUnlockPresents = 240;
void* g_lockedShader = nullptr; // identity only, not referenced
uint64_t g_presentsWithoutReplay = 0;
bool g_firstReplayLogged = false;

// D3D12 side: HUD-less image per frame generation back buffer
struct Dx12Entry
{
    ID3D12Resource* backBuffer = nullptr; // identity only, not referenced
    ID3D12Resource* hudless = nullptr;    // referenced
    uint64_t serial = 0;
};

std::mutex g_dx12Mutex;
std::vector<Dx12Entry> g_dx12Entries;
uint64_t g_dx12Serial = 0;

// Only used by the presenting thread
uint64_t g_replaysAtLastTake = 0;
uint64_t g_presents = 0;
uint64_t g_presentsWithCapture = 0;
bool g_formatMismatchLogged = false;

void ReleaseLocked()
{
    SafeRelease(g_rtv);
    SafeRelease(g_texture);
    g_textureDesc = {};
    g_textureDevice = nullptr;
}

// The 2D texture behind a view, when it is width x height and single sampled (AddRef'd)
ID3D11Texture2D* TextureOf(ID3D11View* view, UINT width, UINT height)
{
    if (view == nullptr)
        return nullptr;

    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);

    if (resource == nullptr)
        return nullptr;

    ID3D11Texture2D* texture = nullptr;
    resource->QueryInterface(IID_PPV_ARGS(&texture));
    resource->Release();

    if (texture == nullptr)
        return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);

    if (desc.Width != width || desc.Height != height || desc.SampleDesc.Count != 1)
    {
        texture->Release();
        return nullptr;
    }

    return texture;
}

bool EnsureTextureLocked(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& targetDesc, DXGI_FORMAT viewFormat)
{
    if (g_texture != nullptr && g_textureDevice == device && g_textureDesc.Width == targetDesc.Width &&
        g_textureDesc.Height == targetDesc.Height && g_textureDesc.Format == viewFormat)
    {
        return true;
    }

    ReleaseLocked();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = targetDesc.Width;
    desc.Height = targetDesc.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = viewFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    auto result = device->CreateTexture2D(&desc, nullptr, &g_texture);
    if (FAILED(result) || g_texture == nullptr)
    {
        LOG_ERROR("HUD-less texture {}x{} format {} failed: {:X}; HUD-less capture off", desc.Width, desc.Height,
                  (UINT) desc.Format, (UINT) result);
        ReleaseLocked();
        g_failed = true;
        return false;
    }

    result = device->CreateRenderTargetView(g_texture, nullptr, &g_rtv);
    if (FAILED(result) || g_rtv == nullptr)
    {
        LOG_ERROR("HUD-less RTV failed: {:X}; HUD-less capture off", (UINT) result);
        ReleaseLocked();
        g_failed = true;
        return false;
    }

    g_textureDesc = desc;
    g_textureDevice = device;

    LOG_INFO("HUD-less texture {}x{} format {}", desc.Width, desc.Height, (UINT) desc.Format);
    return true;
}

// Draws the composite once more into the capture texture with the HUD slot unbound, then puts back what the game
// had bound so its own draw (which runs right after this event) is untouched
void Replay(ID3D11DeviceContext* context, ID3D11RenderTargetView* gameRtv, ID3D11ShaderResourceView* hudSrv,
            const D3D11_TEXTURE2D_DESC& targetDesc, DXGI_FORMAT viewFormat, ID3D11PixelShader* shader,
            uint32_t vertexCount, uint32_t firstVertex)
{
    ID3D11Device* device = nullptr;
    context->GetDevice(&device);

    if (device == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_mutex);

    if (g_lockedShader != nullptr && shader != g_lockedShader)
    {
        device->Release();
        return;
    }

    const bool ready = EnsureTextureLocked(device, targetDesc, viewFormat);
    device->Release();

    if (!ready)
        return;

    ID3D11BlendState* blendState = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0;
    context->OMGetBlendState(&blendState, blendFactor, &sampleMask);

    ID3D11ShaderResourceView* noHud = nullptr;

    // UAVs bound next to the render target (none in Origins) are kept
    context->OMSetRenderTargetsAndUnorderedAccessViews(1, &g_rtv, nullptr, 1, D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
                                                       nullptr, nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->PSSetShaderResources(kHudSlot, 1, &noHud);

    context->Draw(vertexCount, firstVertex);

    context->PSSetShaderResources(kHudSlot, 1, &hudSrv);
    context->OMSetBlendState(blendState, blendFactor, sampleMask);
    context->OMSetRenderTargetsAndUnorderedAccessViews(1, &gameRtv, nullptr, 1, D3D11_KEEP_UNORDERED_ACCESS_VIEWS,
                                                       nullptr, nullptr);

    SafeRelease(blendState);

    if (g_lockedShader == nullptr)
    {
        g_lockedShader = shader;

        const auto type = context->GetType();
        LOG_INFO("HUD-less: game UI composite {} ({}x{}, format {}, {} vertices, pixel shader {:X}, {} context); "
                 "replaying it without the HUD for frame generation",
                 g_firstReplayLogged ? "found again" : "found", targetDesc.Width, targetDesc.Height, (UINT) viewFormat,
                 vertexCount, (size_t) shader, type == D3D11_DEVICE_CONTEXT_DEFERRED ? "deferred" : "immediate");

        g_firstReplayLogged = true;
    }

    g_replays.fetch_add(1);
}

bool OnDraw(reshade::api::command_list* cmdList, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
            uint32_t firstInstance)
{
    // The composite is a single full-screen triangle or quad
    if (vertexCount < 3 || vertexCount > 6 || instanceCount != 1 || firstInstance != 0 || cmdList == nullptr ||
        !g_active.load(std::memory_order_relaxed) || g_failed.load(std::memory_order_relaxed))
    {
        return false;
    }

    auto device = cmdList->get_device();
    if (device == nullptr || device->get_api() != reshade::api::device_api::d3d11)
        return false;

    // On D3D11 this is the game's own (unwrapped) ID3D11DeviceContext, immediate or deferred
    auto context = reinterpret_cast<ID3D11DeviceContext*>(cmdList->get_native());
    if (context == nullptr)
        return false;

    const UINT width = g_targetWidth.load(std::memory_order_relaxed);
    const UINT height = g_targetHeight.load(std::memory_order_relaxed);

    ID3D11RenderTargetView* rtvs[2] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(2, rtvs, &dsv);

    ID3D11Texture2D* target = nullptr;
    ID3D11ShaderResourceView* srvs[2] = {};
    ID3D11Texture2D* hudTexture = nullptr;
    ID3D11Texture2D* sceneTexture = nullptr;
    ID3D11PixelShader* shader = nullptr;

    do
    {
        // One render target, no depth: the back buffer (or a texture of its size) and nothing else
        if (rtvs[0] == nullptr || rtvs[1] != nullptr || dsv != nullptr)
            break;

        target = TextureOf(rtvs[0], width, height);
        if (target == nullptr)
            break;

        // The composite builds its quad from SV_VertexID (vs b144198d) and has no input layout; the game's
        // DirectDisplay quads drawn onto the back buffer after it read a vertex buffer
        ID3D11InputLayout* inputLayout = nullptr;
        context->IAGetInputLayout(&inputLayout);

        if (inputLayout != nullptr)
        {
            inputLayout->Release();
            break;
        }

        // The HUD and the scene, both full-size textures and neither of them the render target
        context->PSGetShaderResources(kHudSlot, 2, srvs);

        hudTexture = TextureOf(srvs[0], width, height);
        sceneTexture = TextureOf(srvs[1], width, height);

        if (hudTexture == nullptr || sceneTexture == nullptr || hudTexture == sceneTexture || hudTexture == target ||
            sceneTexture == target)
        {
            break;
        }

        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvs[0]->GetDesc(&rtvDesc);

        if (rtvDesc.ViewDimension != D3D11_RTV_DIMENSION_TEXTURE2D || rtvDesc.Texture2D.MipSlice != 0)
            break;

        D3D11_TEXTURE2D_DESC targetDesc = {};
        target->GetDesc(&targetDesc);

        context->PSGetShader(&shader, nullptr, nullptr);
        if (shader == nullptr)
            break;

        Replay(context, rtvs[0], srvs[0], targetDesc, rtvDesc.Format, shader, vertexCount, firstVertex);
    } while (false);

    SafeRelease(shader);
    SafeRelease(sceneTexture);
    SafeRelease(hudTexture);
    SafeRelease(srvs[1]);
    SafeRelease(srvs[0]);
    SafeRelease(target);
    SafeRelease(dsv);
    SafeRelease(rtvs[1]);
    SafeRelease(rtvs[0]);

    // Never skip the game's draw
    return false;
}
} // namespace

bool GameHudless::IsEnabled()
{
    return Config::Instance()->Dx11wDx12HudlessFromUI.value_or(
        static_cast<bool>(State::Instance().gameQuirks & GameQuirk::AnvilUICompositeHudless));
}

void GameHudless::RegisterReShadeEvents(void* registerEventFunction)
{
    if (registerEventFunction == nullptr)
        return;

    if (!IsEnabled())
    {
        LOG_INFO("HUD-less capture from the game's UI composite is off");
        return;
    }

    using RegisterEventFn = void (*)(reshade::addon_event, void*);
    auto registerEvent = reinterpret_cast<RegisterEventFn>(registerEventFunction);

    registerEvent(reshade::addon_event::draw, reinterpret_cast<void*>(&OnDraw));
    LOG_INFO("HUD-less capture from the game's UI composite registered with ReShade");
}

void GameHudless::SetTarget(bool active, UINT width, UINT height, DXGI_FORMAT format)
{
    g_targetWidth.store(width, std::memory_order_relaxed);
    g_targetHeight.store(height, std::memory_order_relaxed);
    g_targetFormat.store(format, std::memory_order_relaxed);
    g_active.store(active && width > 0 && height > 0, std::memory_order_relaxed);
}

ID3D11Texture2D* GameHudless::TakeCapture()
{
    const auto replays = g_replays.load();
    const bool fresh = replays != g_replaysAtLastTake;
    g_replaysAtLastTake = replays;

    ID3D11Texture2D* result = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (fresh)
        {
            g_presentsWithoutReplay = 0;
        }
        else if (g_lockedShader != nullptr && ++g_presentsWithoutReplay >= kUnlockPresents)
        {
            LOG_INFO("HUD-less: UI composite not seen for {} frames, looking for it again", kUnlockPresents);
            g_lockedShader = nullptr;
            g_presentsWithoutReplay = 0;
        }
    }

    if (fresh)
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        const auto targetFormat = static_cast<DXGI_FORMAT>(g_targetFormat.load(std::memory_order_relaxed));

        if (g_texture != nullptr && g_textureDesc.Width == g_targetWidth.load(std::memory_order_relaxed) &&
            g_textureDesc.Height == g_targetHeight.load(std::memory_order_relaxed))
        {
            // The FG side copies (or converts) it next to the back buffer copy, so it has to be the same format
            if (TypelessFamily(g_textureDesc.Format) == TypelessFamily(targetFormat))
            {
                result = g_texture;
                result->AddRef();
            }
            else if (!g_formatMismatchLogged)
            {
                g_formatMismatchLogged = true;
                LOG_WARN("HUD-less: the UI composite draws format {} but the back buffer is format {}; not used",
                         (UINT) g_textureDesc.Format, (UINT) targetFormat);
            }
        }
    }

    g_presents++;
    if (result != nullptr)
        g_presentsWithCapture++;

    if (g_presents % kStatsInterval == 0)
    {
        LOG_INFO("HUD-less: {} of the last {} frames from the game's UI composite (the rest used the frame itself)",
                 g_presentsWithCapture, kStatsInterval);
        g_presentsWithCapture = 0;
    }

    return result;
}

void GameHudless::PublishDx12(ID3D12Resource* fgBackBuffer, ID3D12Resource* hudless)
{
    if (fgBackBuffer == nullptr || hudless == nullptr)
        return;

    std::lock_guard<std::mutex> lock(g_dx12Mutex);

    for (auto& entry : g_dx12Entries)
    {
        if (entry.backBuffer != fgBackBuffer)
            continue;

        if (entry.hudless != hudless)
        {
            DeferredRelease::Release(entry.hudless);
            hudless->AddRef();
            entry.hudless = hudless;
        }

        entry.serial = ++g_dx12Serial;
        return;
    }

    hudless->AddRef();
    g_dx12Entries.push_back({ fgBackBuffer, hudless, ++g_dx12Serial });
}

void GameHudless::UnpublishDx12(ID3D12Resource* fgBackBuffer)
{
    std::lock_guard<std::mutex> lock(g_dx12Mutex);

    for (auto it = g_dx12Entries.begin(); it != g_dx12Entries.end(); ++it)
    {
        if (it->backBuffer != fgBackBuffer)
            continue;

        DeferredRelease::Release(it->hudless);
        g_dx12Entries.erase(it);
        return;
    }
}

void GameHudless::ClearDx12()
{
    std::lock_guard<std::mutex> lock(g_dx12Mutex);

    for (auto& entry : g_dx12Entries)
        DeferredRelease::Release(entry.hudless);

    g_dx12Entries.clear();
}

ID3D12Resource* GameHudless::FindDx12(ID3D12Resource* backBuffer, uint64_t* serial)
{
    if (backBuffer == nullptr)
        return nullptr;

    std::lock_guard<std::mutex> lock(g_dx12Mutex);

    for (auto& entry : g_dx12Entries)
    {
        if (entry.backBuffer != backBuffer)
            continue;

        if (serial != nullptr)
            *serial = entry.serial;

        entry.hudless->AddRef();
        return entry.hudless;
    }

    return nullptr;
}

void GameHudless::Release()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ReleaseLocked();
    g_lockedShader = nullptr;
    g_presentsWithoutReplay = 0;
}
