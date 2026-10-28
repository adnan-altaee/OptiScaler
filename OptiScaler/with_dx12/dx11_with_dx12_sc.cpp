#include "pch.h"
#include "dx11_with_dx12_sc.h"

#include <with_dx12/with_dx12.h>
#include <with_dx12/dx11_with_dx12_sync.h>

#include <hooks/FG_Hooks.h>
#include <menu/menu_overlay_dx.h>

#include <hudfix/Hudfix_Dx11.h>
#include <resource_tracking/ResTrack_dx11.h>

#include <Util.h>
#include <Config.h>
#include <misc/IdentifyGpu.h>
#include <misc/HiddenWindow.h>
#include <shaders/Shader_Common.h>
#include <misc/GameCamera.h>

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <tlhelp32.h>

// ReShade add-on API (v6.8.0, API version 20). Only the API and event headers are used, not reshade.hpp,
// so ReShade's Dear ImGui function table header is never pulled into this translation unit.
#include "../../external/reshade/include/reshade_events.hpp"

// Matches ReShade 6.8.0 (reshade.hpp, which is not included here, defines the same value)
#ifndef RESHADE_API_VERSION
#define RESHADE_API_VERSION 20
#endif

#pragma intrinsic(_ReturnAddress)

static int scCount = 0;

// ReShade capture for the Dx11wDx12 FG swapchain.
//
// ReShade wraps the hidden DX11 swapchain this wrapper presents (it sees that CreateSwapChain call, not the
// game's) and renders its effects and overlay inside that swapchain's Present. The original order copied the
// back buffer to the DX12 FG swapchain before that Present, so ReShade's output never reached the screen.
// With Dx11wDx12CaptureAfterReShade, the hidden swapchain is presented first and the back buffer is copied
// from ReShade's reshade_present event ("called after ReShade has rendered its overlay"), right before the
// native Present. OptiScaler registers itself as an external ReShade add-on only to receive that event.
namespace ReShadeCapture
{
static Dx11wDx12SC* captureTarget = nullptr;
static DWORD captureThread = 0;
static int state = 0; // 0 = not tried, 1 = registered, -1 = unavailable

static HMODULE FindReShadeModule()
{
    HMODULE result = nullptr;
    auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return nullptr;

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            if (GetProcAddress(entry.hModule, "ReShadeRegisterAddon") != nullptr &&
                GetProcAddress(entry.hModule, "ReShadeRegisterEvent") != nullptr)
            {
                result = entry.hModule;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

static bool overlayOpen = false;
static UINT inputLogLines = 0;
static UINT inputLogFrame = 0;
static uint32_t lastMouseX = UINT32_MAX;
static uint32_t lastMouseY = UINT32_MAX;
static bool lastLeftButton = false;

// Logs what ReShade's input sees while its overlay is open (limited number of lines), to tell a mouse position
// problem (cursor not following) from a routing problem (no button presses arriving)
static void LogOverlayInput(reshade::api::effect_runtime* runtime)
{
    if (!overlayOpen || inputLogLines >= 40)
        return;

    inputLogFrame++;

    uint32_t mouseX = 0;
    uint32_t mouseY = 0;
    runtime->get_mouse_cursor_position(&mouseX, &mouseY);
    const bool leftButton = runtime->is_mouse_button_down(0);

    const bool buttonChanged = leftButton != lastLeftButton;
    const bool moved = mouseX != lastMouseX || mouseY != lastMouseY;

    // Button changes right away, cursor moves at most twice a second
    if (!buttonChanged && !(moved && inputLogFrame >= 30))
        return;

    inputLogFrame = 0;
    inputLogLines++;
    lastMouseX = mouseX;
    lastMouseY = mouseY;
    lastLeftButton = leftButton;

    POINT cursor = {};
    GetPhysicalCursorPos(&cursor); // GetCursorPos is hooked by ReShade while it blocks cursor warping

    uint32_t width = 0;
    uint32_t height = 0;
    runtime->get_screenshot_width_and_height(&width, &height);

    LOG_INFO("ReShade overlay input: cursor {},{} in a {}x{} frame, left button {} (screen cursor {},{})", mouseX,
             mouseY, width, height, leftButton ? "down" : "up", cursor.x, cursor.y);
}

static bool OnReShadeOpenOverlay(reshade::api::effect_runtime* runtime, bool open, reshade::api::input_source source)
{
    overlayOpen = open;
    LOG_INFO("ReShade overlay {} (source {})", open ? "opened" : "closed", (int) source);
    return false; // never block ReShade from opening or closing it
}

static void OnReShadePresent(reshade::api::effect_runtime* runtime)
{
    if (runtime != nullptr)
        LogOverlayInput(runtime);

    auto target = captureTarget;
    if (target == nullptr || runtime == nullptr || GetCurrentThreadId() != captureThread)
        return;

    const auto backBuffer = runtime->get_current_back_buffer();
    if (backBuffer.handle == 0)
        return;

    // On D3D11, ReShade resource handles are the native ID3D11Resource pointers
    target->CaptureReShadeBackBuffer(reinterpret_cast<ID3D11Resource*>(backBuffer.handle));
}

static bool EnsureRegistered()
{
    if (state != 0)
        return state > 0;

    state = -1;

    auto reshadeModule = FindReShadeModule();
    if (reshadeModule == nullptr)
    {
        LOG_INFO("ReShade not loaded, Dx11wDx12 capture keeps the original order");
        return false;
    }

    using RegisterAddonFn = bool (*)(void*, uint32_t);
    using RegisterEventFn = void (*)(reshade::addon_event, void*);

    auto registerAddon = reinterpret_cast<RegisterAddonFn>(GetProcAddress(reshadeModule, "ReShadeRegisterAddon"));
    auto registerEvent = reinterpret_cast<RegisterEventFn>(GetProcAddress(reshadeModule, "ReShadeRegisterEvent"));

    if (registerAddon == nullptr || registerEvent == nullptr || dllModule == nullptr)
        return false;

    if (!registerAddon(dllModule, RESHADE_API_VERSION))
    {
        LOG_WARN("ReShadeRegisterAddon failed (ReShade older than API version {}?)", RESHADE_API_VERSION);
        return false;
    }

    registerEvent(reshade::addon_event::reshade_present, reinterpret_cast<void*>(&OnReShadePresent));
    registerEvent(reshade::addon_event::reshade_open_overlay, reinterpret_cast<void*>(&OnReShadeOpenOverlay));

    // Frame generation camera from the game's constant buffers (misc/GameCamera.cpp)
    GameCamera::RegisterReShadeEvents(reinterpret_cast<void*>(registerEvent));

    state = 1;
    LOG_INFO("Registered with ReShade; Dx11wDx12 will capture frames after ReShade's effects and overlay");
    return true;
}
} // namespace ReShadeCapture

namespace
{
template <typename T> void SafeRelease(T*& value)
{
    if (value != nullptr)
    {
        value->Release();
        value = nullptr;
    }
}

void SafeCloseHandle(HANDLE& value)
{
    if (value != nullptr)
    {
        CloseHandle(value);
        value = nullptr;
    }
}

bool WaitForQueueIdle(ID3D12CommandQueue* queue, ID3D12Fence* fence, HANDLE fenceEvent, UINT64& fenceValue)
{
    if (queue == nullptr || fence == nullptr || fenceEvent == nullptr)
        return true;

    const UINT64 waitValue = ++fenceValue;
    auto result = queue->Signal(fence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("FG/present queue idle Signal failed: {:X}", (UINT) result);
        return false;
    }

    if (fence->GetCompletedValue() >= waitValue)
        return true;

    result = fence->SetEventOnCompletion(waitValue, fenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("FG/present queue idle SetEventOnCompletion failed. fence {}, completed {}, result {:X}", waitValue,
                  fence->GetCompletedValue(), (UINT) result);
        return false;
    }

    const auto waitResult = WaitForSingleObject(fenceEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("FG/present queue idle wait failed. fence {}, completed {}, waitResult {:X}", waitValue,
                  fence->GetCompletedValue(), waitResult);
        return false;
    }

    return true;
}

void TransitionResource(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource,
                        D3D12_RESOURCE_STATES beforeState, D3D12_RESOURCE_STATES afterState)
{
    if (commandList == nullptr || resource == nullptr || beforeState == afterState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;

    commandList->ResourceBarrier(1, &barrier);
}

UINT ResolveBufferCount(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferCount > 0)
        return desc.BufferCount;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)) && desc1.BufferCount > 0)
        return desc1.BufferCount;

    return 2;
}

DXGI_FORMAT ResolveBufferFormat(IDXGISwapChain* swapchain, IDXGISwapChain1* swapchain1)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    if (swapchain != nullptr && SUCCEEDED(swapchain->GetDesc(&desc)) && desc.BufferDesc.Format != DXGI_FORMAT_UNKNOWN)
        return desc.BufferDesc.Format;

    DXGI_SWAP_CHAIN_DESC1 desc1 = {};
    if (swapchain1 != nullptr && SUCCEEDED(swapchain1->GetDesc1(&desc1)))
        return desc1.Format;

    return DXGI_FORMAT_UNKNOWN;
}

bool IsSame(IDXGISwapChain* swapchain, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    if (swapchain == nullptr)
        return false;

    DXGI_SWAP_CHAIN_DESC desc {};
    if (FAILED(swapchain->GetDesc(&desc)))
        return false;

    const UINT resolvedBufferCount = bufferCount != 0 ? bufferCount : desc.BufferCount;
    const UINT resolvedWidth = width != 0 ? width : desc.BufferDesc.Width;
    const UINT resolvedHeight = height != 0 ? height : desc.BufferDesc.Height;
    const DXGI_FORMAT resolvedFormat = format != DXGI_FORMAT_UNKNOWN ? format : desc.BufferDesc.Format;

    return resolvedBufferCount == desc.BufferCount && resolvedWidth == desc.BufferDesc.Width &&
           resolvedHeight == desc.BufferDesc.Height && resolvedFormat == desc.BufferDesc.Format && flags == desc.Flags;
}
} // namespace

Dx11wDx12SC::Dx11wDx12SC(IDXGISwapChain* real, IDXGISwapChain4* fgSC, ID3D11Device* pDevice, HWND hWnd, UINT flags)
    : _real(real), _fgSwapChain(fgSC), _dx11Device(pDevice), _handle(hWnd), _refcount(1)
{
    _id = ++scCount;
    _lastFlags = flags;

    if (_real != nullptr)
    {
        _real->AddRef();
        _real->QueryInterface(IID_PPV_ARGS(&_real1));
        _real->QueryInterface(IID_PPV_ARGS(&_real2));
        _real->QueryInterface(IID_PPV_ARGS(&_real3));
        _real->QueryInterface(IID_PPV_ARGS(&_real4));
    }

    if (_fgSwapChain != nullptr)
        _fgSwapChain->AddRef();

    if (_dx11Device != nullptr)
    {
        _dx11Device->AddRef();
        auto device5Result = _dx11Device->QueryInterface(IID_PPV_ARGS(&_dx11Device5));
        if (FAILED(device5Result))
            LOG_WARN("ID3D11Device5 unavailable: {:X}", (UINT) device5Result);

        _dx11Device->GetImmediateContext(&_dx11Context);
        if (_dx11Context != nullptr)
        {
            auto context4Result = _dx11Context->QueryInterface(IID_PPV_ARGS(&_dx11Context4));
            if (FAILED(context4Result))
                LOG_WARN("ID3D11DeviceContext4 unavailable: {:X}", (UINT) context4Result);
        }
    }

    if (_dx11Device != nullptr && State::Instance().activeFgInput == FGInput::Upscaler &&
        !Config::Instance()->FGDisableHUDFix.value_or_default())
    {
        ResTrack_Dx11::HookDevice(_dx11Device);
    }

    if (WithDx12::PrepareD3D12ForD3D11(_dx11Device, D3D_FEATURE_LEVEL_11_0))
    {
        _dx12Device = WithDx12::GetD3D12Device();
        _dx12CommandQueue = WithDx12::GetD3D12CommandQueue();
    }
    else
    {
        LOG_ERROR("failed to resolve D3D12 device/queue");
    }

    State::Instance().swapchainInteropApi = SwapchainInteropApi::Dx11wDx12;

    _RefreshCachedSwapchainDesc();
    _UpdateHdr10State();

    LOG_INFO("Dx11wDx12SC {} created, real: {:X}, fg: {:X}, dx11: {:X}, dx12: {:X}, queue: {:X}", _id, (UINT64) _real,
             (UINT64) _fgSwapChain, (UINT64) _dx11Device, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
}

Dx11wDx12SC::~Dx11wDx12SC()
{
    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropObjects();
    _ReleaseHdr10Resources(true);

    SafeRelease(_real4);
    SafeRelease(_real3);
    SafeRelease(_real2);
    SafeRelease(_real1);
    SafeRelease(_fgSwapChain);
    SafeRelease(_real);
    SafeRelease(_dx11Context4);
    SafeRelease(_dx11Context);
    SafeRelease(_dx11Device5);
    SafeRelease(_dx11Device);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::QueryInterface(REFIID riid, void** ppvObject)
{
    LOG_TRACE("Caller: {}", Util::WhoIsTheCaller(_ReturnAddress()));

    if (ppvObject == nullptr)
        return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
        riid == __uuidof(IDXGISwapChain))
    {
        AddRef();
        *ppvObject = static_cast<IDXGISwapChain*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain1))
    {
        if (_real1 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain1*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain2))
    {
        if (_real2 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain2*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain3))
    {
        if (_real3 == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain3*>(this);
        return S_OK;
    }

    if (riid == __uuidof(IDXGISwapChain4))
    {
        if (_real4 == nullptr && _fgSwapChain == nullptr)
            return E_NOINTERFACE;

        AddRef();
        *ppvObject = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    if (riid == __uuidof(Dx11wDx12SC))
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::AddRef()
{
    auto count = InterlockedIncrement(&_refcount);
    LOG_TRACE("Count: {}, caller: {}", count, Util::WhoIsTheCaller(_ReturnAddress()));
    return count;
}

ULONG STDMETHODCALLTYPE Dx11wDx12SC::Release()
{
    ULONG ret = InterlockedDecrement(&_refcount);

    LOG_TRACE("Count: {}, caller: {}", ret, Util::WhoIsTheCaller(_ReturnAddress()));

    if (ret == 0)
    {
        if (State::Instance().currentSwapchain == this)
            State::Instance().currentSwapchain = nullptr;

        if (State::Instance().currentWrappedSwapchain == this)
            State::Instance().currentWrappedSwapchain = nullptr;

        if (State::Instance().currentRealSwapchain == _real)
            State::Instance().currentRealSwapchain = nullptr;

        FGHooks::ClearDx12InteropPresentSC(_fgSwapChain);

        if (State::Instance().currentFGSwapchain == _fgSwapChain)
            State::Instance().currentFGSwapchain = nullptr;

        if (State::Instance().currentD3D11Device == _dx11Device)
            State::Instance().currentD3D11Device = nullptr;

        State::Instance().swapchainInteropApi = SwapchainInteropApi::None;

        auto fg = State::Instance().currentFG;
        if (fg != nullptr && fg->Mutex.getOwner() != 1 && fg->SwapchainContext() != nullptr)
        {
            fg->Deactivate();
            fg->ReleaseSwapchain(_handle);
        }

        ResTrack_Dx11::OnDeviceReleased(_dx11Device);

        delete this;
    }

    return ret;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData)
{
    return _real != nullptr ? _real->SetPrivateData(Name, DataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown)
{
    return _real != nullptr ? _real->SetPrivateDataInterface(Name, pUnknown) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData)
{
    return _real != nullptr ? _real->GetPrivateData(Name, pDataSize, pData) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetParent(REFIID riid, void** ppParent)
{
    return _real != nullptr ? _real->GetParent(riid, ppParent) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDevice(REFIID riid, void** ppDevice)
{
    return _real != nullptr ? _real->GetDevice(riid, ppDevice) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present(UINT SyncInterval, UINT Flags)
{
    if (_real == nullptr || _fgSwapChain == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    if (_fg == nullptr)
        _fg = State::Instance().currentFG;

    if ((Flags & DXGI_PRESENT_TEST) != 0)
        return _real->Present(SyncInterval, Flags);

    const bool dx11HudfixPresent = Config::Instance()->FGHUDFix.value_or_default() &&
                                   State::Instance().activeFgInput == FGInput::Upscaler &&
                                   State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12;
    if (dx11HudfixPresent)
    {
        ResTrack_Dx11::ClearPossibleHudless();
        Hudfix_Dx11::PresentStart();
    }

    if (!_InitInteropObjects())
        return DXGI_ERROR_DEVICE_REMOVED;

    auto dx11Index = _GetDx11BackBufferIndexForPresent();

    if (!_RequestSharedBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    // ReShade capture: present the hidden DX11 swapchain first so ReShade renders its effects and overlay,
    // and take the frame from its reshade_present event (see ReShadeCapture above). Disabled for good if
    // ReShade never reports a frame for this swapchain within the first 120 presents.
    bool realPresented = false;
    _capturedFromReShade = false;

    if (_real != nullptr && !State::Instance().isRunningOnLinux &&
        Config::Instance()->Dx11wDx12CaptureAfterReShade.value_or_default() &&
        (_reshadeCaptureHits > 0 || _reshadeCaptureMisses < 120) && ReShadeCapture::EnsureRegistered())
    {
        // ReShade maps the mouse through the window of the swapchain it wraps (here the hidden window) and
        // scales it by back buffer size / that window's client size, so the hidden window must mirror the
        // game window's client area or every click lands far outside the overlay
        _SyncHiddenWindowToGameWindow();

        ReShadeCapture::captureThread = GetCurrentThreadId();
        ReShadeCapture::captureTarget = this;

        auto realPresentResult = _real->Present(0, Flags);

        ReShadeCapture::captureTarget = nullptr;
        realPresented = true;

        if (FAILED(realPresentResult))
            LOG_WARN("hidden real DX11 Present (ReShade capture) failed: {:X}", (UINT) realPresentResult);

        if (_capturedFromReShade)
        {
            if (_reshadeCaptureHits++ == 0)
                LOG_INFO("ReShade capture active on Dx11wDx12SC {:X}", (size_t) this);
        }
        else if (_reshadeCaptureHits == 0 && ++_reshadeCaptureMisses == 120)
        {
            LOG_WARN("ReShade never reported a frame for this swapchain; using the original copy order");
        }
    }

    if (!_capturedFromReShade && !_CopyDx11BackBufferToShared(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_WaitDx11ThenDx12())
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_CopyDx11SharedToDx12FGBackBuffer(dx11Index))
        return DXGI_ERROR_DEVICE_REMOVED;

    if (!_WaitForInteropCopyOnPresentQueue())
        return DXGI_ERROR_DEVICE_REMOVED;

    const bool fgHookedPresenter =
        State::Instance().currentFGSwapchain == _fgSwapChain && !FGHooks::IsDx12InteropPresentSC(_fgSwapChain);

    // The game-facing DX11 swapchain is never presented in this wrapper.
    // For a plain external DX12 presenter, draw Opti's overlay here.
    // For a real FG swapchain, FGHooks::FGPresent/LocalPresent owns overlay/present-side work;
    // drawing it here would double-enter the overlay path before the FG present hook.
    if (!fgHookedPresenter)
        MenuOverlayDx::Present(_fgSwapChain, SyncInterval, Flags, nullptr, _dx12CommandQueue, _handle, false);
    else
        LOG_TRACE("real FG presenter detected; skipping wrapper overlay path");

    LOG_DEBUG("frame {}, dx11Index {}, real3Index {}, fakeIndex {}, bufferCount {}", State::Instance().frameCount,
              dx11Index, _real3 != nullptr ? _real3->GetCurrentBackBufferIndex() : 0xFFFFFFFF, _currentFakeIndex,
              _bufferCount);

    // Some memes with Vk swapchains on Lunyx and overlays
    if (_real != nullptr && !State::Instance().isRunningOnLinux && !realPresented)
    {
        UINT realFlags = Flags;

        // Do not wait for it
        auto realPresentResult = _real->Present(0, realFlags);

        if (FAILED(realPresentResult))
            LOG_WARN("hidden real DX11 Present failed: {:X}", (UINT) realPresentResult);
    }

    auto result = _fgSwapChain->Present(SyncInterval, Flags);

    if (dx11HudfixPresent)
        Hudfix_Dx11::PresentEnd();

    if (SUCCEEDED(result))
        _AdvanceFakeBackBufferIndex();
    else
        LOG_ERROR("fg Present failed: {:X}", (UINT) result);

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface)
{
    return _real != nullptr ? _real->GetBuffer(Buffer, riid, ppSurface) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget)
{
    LOG_DEBUG("Dx11wDx12SC SetFullscreenState: {}, target: {:X}, caller: {}", Fullscreen, (size_t) pTarget,
              Util::WhoIsTheCaller(_ReturnAddress()));

    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetFullscreenState(Fullscreen, pTarget);

    return _real != nullptr ? _real->SetFullscreenState(Fullscreen, pTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenState(pFullscreen, ppTarget);

    return _real != nullptr ? _real->GetFullscreenState(pFullscreen, ppTarget) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc)
{
    if (_real == nullptr)
        return DXGI_ERROR_DEVICE_REMOVED;

    auto result = _real->GetDesc(pDesc);
    if (SUCCEEDED(result) && pDesc != nullptr)
        pDesc->OutputWindow = _handle;

    return result;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                                     UINT SwapChainFlags)
{
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) NewFormat, SwapChainFlags);

    // DLSS-G/XeFG cannot present FP16: the FG swapchain gets HDR10 buffers and the frames are converted
    const DXGI_FORMAT gameFormat = NewFormat != DXGI_FORMAT_UNKNOWN ? NewFormat : _bufferFormat;
    const DXGI_FORMAT fgFormat = NeedsHdr10Conversion(gameFormat) ? DXGI_FORMAT_R10G10B10A2_UNORM : NewFormat;

    const bool skipFgResize = IsSame(_fgSwapChain, BufferCount, Width, Height, fgFormat, SwapChainFlags);
    const bool synchronizeXeFGPresent = skipFgResize && State::Instance().activeFgOutput == FGOutput::XeFG &&
                                        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12;

    std::unique_lock<std::shared_mutex> presentResizeLock(Dx11wDx12Sync::PresentResizeMutex(), std::defer_lock);
    if (synchronizeXeFGPresent)
    {
        auto fg = State::Instance().currentFG;
        if (fg != nullptr && fg->FrameGenerationContext() != nullptr && fg->IsActive())
        {
            State::Instance().fgChanged = true;
            fg->UpdateTarget();
            fg->Deactivate();
        }

        LOG_DEBUG("Waiting for XeFG native presents before equivalent ResizeBuffers");
        presentResizeLock.lock();
        LOG_DEBUG("XeFG native present barrier acquired");
    }

    if (!_WaitForCopyQueueIdle())
        LOG_WARN("continuing ResizeBuffers after copy fence wait failure");

    if (synchronizeXeFGPresent && !WaitForQueueIdle(_dx12CommandQueue, _copyFence, _copyFenceEvent, _copyFenceValue))
    {
        LOG_WARN("continuing ResizeBuffers after FG/present queue wait failure");
    }

    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropBackBuffers();

    HRESULT realResult = _real != nullptr ? _real->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags)
                                          : DXGI_ERROR_DEVICE_REMOVED;

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
    {
        if (skipFgResize)
        {
            LOG_DEBUG("Skipping FG ResizeBuffers");
            fgResult = S_OK;
        }
        else
        {
            fgResult = _fgSwapChain->ResizeBuffers(BufferCount, Width, Height, fgFormat, SwapChainFlags);
        }
    }

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    if (SUCCEEDED(realResult) && SUCCEEDED(fgResult))
    {
        _RefreshCachedSwapchainDesc();
        _UpdateHdr10State();

        if (Config::Instance()->IsFGEnabled())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

        if (State::Instance().currentFeature == nullptr)
        {
            State::Instance().screenWidth = static_cast<float>(Width);
            State::Instance().screenHeight = static_cast<float>(Height);
            State::Instance().lastMipBias = 100.0f;
            State::Instance().lastMipBiasMax = -100.0f;
        }
    }

    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->ResizeTarget(pNewTargetParameters)
               : (_real != nullptr ? _real->ResizeTarget(pNewTargetParameters) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetContainingOutput(IDXGIOutput** ppOutput)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetContainingOutput(ppOutput)
               : (_real != nullptr ? _real->GetContainingOutput(ppOutput) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats)
{
    return _fgSwapChain != nullptr ? _fgSwapChain->GetFrameStatistics(pStats)
                                   : (_real != nullptr ? _real->GetFrameStatistics(pStats) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetLastPresentCount(UINT* pLastPresentCount)
{
    return _fgSwapChain != nullptr
               ? _fgSwapChain->GetLastPresentCount(pLastPresentCount)
               : (_real != nullptr ? _real->GetLastPresentCount(pLastPresentCount) : DXGI_ERROR_DEVICE_REMOVED);
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc)
{
    return _real1 != nullptr ? _real1->GetDesc1(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFullscreenDesc(pDesc);

    return _real1 != nullptr ? _real1->GetFullscreenDesc(pDesc) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetHwnd(HWND* pHwnd)
{
    if (pHwnd == nullptr)
        return DXGI_ERROR_INVALID_CALL;

    *pHwnd = _handle;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetCoreWindow(REFIID refiid, void** ppUnk)
{
    return _real1 != nullptr ? _real1->GetCoreWindow(refiid, ppUnk) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::Present1(UINT SyncInterval, UINT Flags,
                                                const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    UNREFERENCED_PARAMETER(pPresentParameters);
    return Present(SyncInterval, Flags);
}

BOOL STDMETHODCALLTYPE Dx11wDx12SC::IsTemporaryMonoSupported(void)
{
    return _real1 != nullptr ? _real1->IsTemporaryMonoSupported() : FALSE;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput)
{
    return _real1 != nullptr ? _real1->GetRestrictToOutput(ppRestrictToOutput) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetBackgroundColor(const DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->SetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetBackgroundColor(DXGI_RGBA* pColor)
{
    return _real1 != nullptr ? _real1->GetBackgroundColor(pColor) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetRotation(DXGI_MODE_ROTATION Rotation)
{
    return _real1 != nullptr ? _real1->SetRotation(Rotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetRotation(DXGI_MODE_ROTATION* pRotation)
{
    return _real1 != nullptr ? _real1->GetRotation(pRotation) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetSourceSize(UINT Width, UINT Height)
{
    return _real2 != nullptr ? _real2->SetSourceSize(Width, Height) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetSourceSize(UINT* pWidth, UINT* pHeight)
{
    return _real2 != nullptr ? _real2->GetSourceSize(pWidth, pHeight) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMaximumFrameLatency(UINT MaxLatency)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetMaximumFrameLatency(MaxLatency);

    return _real2 != nullptr ? _real2->SetMaximumFrameLatency(MaxLatency) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMaximumFrameLatency(UINT* pMaxLatency)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetMaximumFrameLatency(pMaxLatency);

    return _real2 != nullptr ? _real2->GetMaximumFrameLatency(pMaxLatency) : DXGI_ERROR_DEVICE_REMOVED;
}

HANDLE STDMETHODCALLTYPE Dx11wDx12SC::GetFrameLatencyWaitableObject(void)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->GetFrameLatencyWaitableObject();

    return _real2 != nullptr ? _real2->GetFrameLatencyWaitableObject() : nullptr;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2 != nullptr ? _real2->SetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix)
{
    return _real2 != nullptr ? _real2->GetMatrixTransform(pMatrix) : DXGI_ERROR_DEVICE_REMOVED;
}

UINT STDMETHODCALLTYPE Dx11wDx12SC::GetCurrentBackBufferIndex(void)
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _currentFakeIndex;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                              UINT* pColorSpaceSupport)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);

    return _real3 != nullptr ? _real3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport)
                             : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace)
{
    _gameColorSpace = ColorSpace;

    // The FG swapchain keeps the HDR10 color space of the converted frames
    if (_hdr10Active)
    {
        LOG_INFO("Game color space {} kept for its back buffer, FG swapchain stays HDR10", (UINT) ColorSpace);
        return S_OK;
    }

    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetColorSpace1(ColorSpace);

    return _real3 != nullptr ? _real3->SetColorSpace1(ColorSpace) : DXGI_ERROR_DEVICE_REMOVED;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                                      UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                                      IUnknown* const* ppPresentQueue)
{
    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1: count {}, size {}x{}, format {}, flags {:X}", BufferCount, Width, Height,
              (UINT) Format, SwapChainFlags);

    if (_real3 == nullptr)
        return ResizeBuffers(BufferCount, Width, Height, Format, SwapChainFlags);

    const DXGI_FORMAT gameFormat = Format != DXGI_FORMAT_UNKNOWN ? Format : _bufferFormat;
    const DXGI_FORMAT fgFormat = NeedsHdr10Conversion(gameFormat) ? DXGI_FORMAT_R10G10B10A2_UNORM : Format;

    const bool skipFgResize = IsSame(_fgSwapChain, BufferCount, Width, Height, fgFormat, SwapChainFlags);
    const bool synchronizeXeFGPresent = skipFgResize && State::Instance().activeFgOutput == FGOutput::XeFG &&
                                        State::Instance().swapchainInteropApi == SwapchainInteropApi::Dx11wDx12;

    std::unique_lock<std::shared_mutex> presentResizeLock(Dx11wDx12Sync::PresentResizeMutex(), std::defer_lock);
    if (synchronizeXeFGPresent)
    {
        auto fg = State::Instance().currentFG;
        if (fg != nullptr && fg->FrameGenerationContext() != nullptr && fg->IsActive())
        {
            State::Instance().fgChanged = true;
            fg->UpdateTarget();
            fg->Deactivate();
        }

        LOG_DEBUG("Waiting for XeFG native presents before equivalent ResizeBuffers1");
        presentResizeLock.lock();
        LOG_DEBUG("XeFG native present barrier acquired");
    }

    if (!_WaitForCopyQueueIdle())
        LOG_WARN("continuing ResizeBuffers1 after copy fence wait failure");

    if (synchronizeXeFGPresent && !WaitForQueueIdle(_dx12CommandQueue, _copyFence, _copyFenceEvent, _copyFenceValue))
    {
        LOG_WARN("continuing ResizeBuffers1 after FG/present queue wait failure");
    }

    MenuOverlayDx::CleanupRenderTarget(true, _handle);
    _ReleaseInteropBackBuffers();

    HRESULT realResult =
        _real3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask, ppPresentQueue);

    HRESULT fgResult = DXGI_ERROR_DEVICE_REMOVED;
    if (SUCCEEDED(realResult) && _fgSwapChain != nullptr)
    {
        // The game's ppPresentQueue is not valid for the DX12 FG swapchain. Use ResizeBuffers for phase 1.
        if (skipFgResize)
        {
            LOG_DEBUG("Skipping FG ResizeBuffers1");
            fgResult = S_OK;
        }
        else
        {
            fgResult = _fgSwapChain->ResizeBuffers(BufferCount, Width, Height, fgFormat, SwapChainFlags);
        }
    }

    LOG_DEBUG("Dx11wDx12SC ResizeBuffers1 results: real {:X}, fg {:X}", (UINT) realResult, (UINT) fgResult);

    if (SUCCEEDED(realResult) && SUCCEEDED(fgResult))
    {
        _RefreshCachedSwapchainDesc();
        _UpdateHdr10State();

        if (Config::Instance()->IsFGEnabled())
        {
            State::Instance().fgResetCapturedResources = true;
            State::Instance().fgOnlyUseCapturedResources = false;
            State::Instance().fgChanged = true;
        }

        State::Instance().scChanged = true;
        State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    }

    return FAILED(realResult) ? realResult : fgResult;
}

HRESULT STDMETHODCALLTYPE Dx11wDx12SC::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData)
{
    if (_fgSwapChain != nullptr)
        return _fgSwapChain->SetHDRMetaData(Type, Size, pMetaData);

    return _real4 != nullptr ? _real4->SetHDRMetaData(Type, Size, pMetaData) : DXGI_ERROR_DEVICE_REMOVED;
}

bool Dx11wDx12SC::_InitInteropObjects()
{
    if (_interopInitialized)
    {
        _RefreshCachedSwapchainDesc();

        if (_bufferCount == 0)
        {
            LOG_ERROR("InitInteropObjects resolved zero backbuffers");
            return false;
        }

        if (_sharedDx11BackBufferCopies.size() != _bufferCount)
            _sharedDx11BackBufferCopies.resize(_bufferCount, nullptr);

        if (_openedDx11BackBuffers.size() != _bufferCount)
            _openedDx11BackBuffers.resize(_bufferCount, nullptr);

        if (_sharedBackBufferHandles.size() != _bufferCount)
            _sharedBackBufferHandles.resize(_bufferCount, nullptr);

        if (_openedDx11BackBufferStates.size() != _bufferCount)
            _openedDx11BackBufferStates.resize(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

        return true;
    }

    if (_dx11Device == nullptr || _dx11Device5 == nullptr || _dx11Context4 == nullptr || _dx12Device == nullptr ||
        _dx12CommandQueue == nullptr)
    {
        LOG_ERROR("interop objects missing: dx11 {}, dx11-5 {}, ctx4 {}, dx12 {}, queue {}", (UINT64) _dx11Device,
                  (UINT64) _dx11Device5, (UINT64) _dx11Context4, (UINT64) _dx12Device, (UINT64) _dx12CommandQueue);
        return false;
    }

    HRESULT result = S_OK;

    const UINT copyAllocatorCount = std::max<UINT>(_bufferCount != 0 ? _bufferCount : 3, 3);

    if (_copyAllocators.size() != copyAllocatorCount)
    {
        for (auto& allocator : _copyAllocators)
            SafeRelease(allocator);

        _copyAllocators.assign(copyAllocatorCount, nullptr);
    }

    if (_copyAllocatorFenceValues.size() != copyAllocatorCount)
        _copyAllocatorFenceValues.assign(copyAllocatorCount, 0);

    if (_copyCommandLists.size() != copyAllocatorCount)
    {
        for (auto& commandList : _copyCommandLists)
            SafeRelease(commandList);

        _copyCommandLists.assign(copyAllocatorCount, nullptr);
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyAllocators[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_copyAllocators[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandAllocator[{}] failed: {:X}", i, (UINT) result);
            return false;
        }
    }

    for (UINT i = 0; i < copyAllocatorCount; ++i)
    {
        if (_copyCommandLists[i] != nullptr)
            continue;

        result = _dx12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _copyAllocators[i], nullptr,
                                                IID_PPV_ARGS(&_copyCommandLists[i]));
        if (FAILED(result))
        {
            LOG_ERROR("CreateCommandList failed: {:X}", (UINT) result);
            return false;
        }

        _copyCommandLists[i]->Close();
    }

    if (_copyFence == nullptr)
    {
        result = _dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_copyFence));
        if (FAILED(result))
        {
            LOG_ERROR("Create copy fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_copyFenceEvent == nullptr)
    {
        _copyFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (_copyFenceEvent == nullptr)
        {
            LOG_ERROR("CreateEvent for copy fence failed");
            return false;
        }
    }

    if (_dx11Fence == nullptr)
    {
        result = _dx11Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&_dx11Fence));
        if (FAILED(result))
        {
            LOG_ERROR("D3D11 CreateFence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_sharedFenceHandle == nullptr)
    {
        result = _dx11Fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &_sharedFenceHandle);
        if (FAILED(result))
        {
            LOG_ERROR("CreateSharedHandle for D3D11 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    if (_dx12SharedFence == nullptr)
    {
        result = _dx12Device->OpenSharedHandle(_sharedFenceHandle, IID_PPV_ARGS(&_dx12SharedFence));
        if (FAILED(result))
        {
            LOG_ERROR("OpenSharedHandle for D3D12 fence failed: {:X}", (UINT) result);
            return false;
        }
    }

    _RefreshCachedSwapchainDesc();

    if (_bufferCount == 0)
    {
        LOG_ERROR("InitInteropObjects resolved zero backbuffers");
        return false;
    }

    _sharedDx11BackBufferCopies.assign(_bufferCount, nullptr);
    _openedDx11BackBuffers.assign(_bufferCount, nullptr);
    _sharedBackBufferHandles.assign(_bufferCount, nullptr);
    _openedDx11BackBufferStates.assign(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

    _interopInitialized = true;
    return true;
}

bool Dx11wDx12SC::_RequestSharedBackBuffer(UINT index)
{
    if (_real == nullptr || _dx11Device == nullptr || _dx12Device == nullptr)
        return false;

    if (_bufferCount == 0)
        _RefreshCachedSwapchainDesc();

    if (index >= _bufferCount)
    {
        LOG_ERROR("backbuffer index {} out of range {}", index, _bufferCount);
        return false;
    }

    if (_sharedDx11BackBufferCopies.size() <= index)
        _sharedDx11BackBufferCopies.resize(_bufferCount, nullptr);

    if (_openedDx11BackBuffers.size() <= index)
        _openedDx11BackBuffers.resize(_bufferCount, nullptr);

    if (_sharedBackBufferHandles.size() <= index)
        _sharedBackBufferHandles.resize(_bufferCount, nullptr);

    if (_openedDx11BackBufferStates.size() <= index)
        _openedDx11BackBufferStates.resize(_bufferCount, D3D12_RESOURCE_STATE_COMMON);

    if (_openedDx11BackBuffers[_currentFakeIndex] != nullptr)
        return true;

    // Read Dx11 sc backbuffer
    ID3D11Texture2D* sourceTexture = nullptr;
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer({}) failed: {:X}", index, (UINT) result);
        return false;
    }

    // Create shared copy
    IDXGIResource1* dxgiResource = nullptr;
    if (_sharedBackBufferHandles[_currentFakeIndex] == nullptr)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        sourceTexture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

        // HDR10 conversion writes the converted frame into the shared copy with a compute shader
        if (_hdr10Active)
        {
            desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
        }

        result = _dx11Device->CreateTexture2D(&desc, nullptr, &_sharedDx11BackBufferCopies[_currentFakeIndex]);
        if (FAILED(result) || _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
        {
            LOG_ERROR("CreateTexture2D shadow buffer {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }

        result = _sharedDx11BackBufferCopies[_currentFakeIndex]->QueryInterface(IID_PPV_ARGS(&dxgiResource));
        if (FAILED(result) || dxgiResource == nullptr)
        {
            LOG_ERROR("shadow IDXGIResource1 query {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }

        result = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr,
                                                  &_sharedBackBufferHandles[_currentFakeIndex]);
        dxgiResource->Release();

        if (FAILED(result) || _sharedBackBufferHandles[_currentFakeIndex] == nullptr)
        {
            LOG_ERROR("shadow CreateSharedHandle {} failed: {:X}", _currentFakeIndex, (UINT) result);
            sourceTexture->Release();
            return false;
        }
    }

    // Dx12 handle to Dx11 shared texture
    result = _dx12Device->OpenSharedHandle(_sharedBackBufferHandles[_currentFakeIndex],
                                           IID_PPV_ARGS(&_openedDx11BackBuffers[_currentFakeIndex]));
    sourceTexture->Release();

    if (FAILED(result) || _openedDx11BackBuffers[_currentFakeIndex] == nullptr)
    {
        LOG_ERROR("OpenSharedHandle for backbuffer {} failed: {:X}", _currentFakeIndex, (UINT) result);
        return false;
    }

    _openedDx11BackBufferStates[_currentFakeIndex] = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

// ReShade only knows the hidden window (the output window of the swapchain it wraps), which matters for its overlay:
// - mouse coordinates are converted with ScreenToClient/GetClientRect of that window, so it has to cover the
//   game window's client area exactly
// - window messages are only routed to its overlay when they are for that window, one of its children, or
//   its parent (or a window that registered raw input), and the game window is none of those. As a hidden
//   child of the game window, the game window's mouse messages are routed to it.
// Window changes are requested asynchronously (posted to the window's own thread, see misc/HiddenWindow.h),
// so the presenting thread never waits on another thread's message queue.
void Dx11wDx12SC::_SyncHiddenWindowToGameWindow()
{
    if (_real == nullptr || _handle == nullptr || !IsWindow(_handle))
        return;

    // The hidden window is the hidden DX11 swapchain's own output window (this wrapper's GetDesc reports the
    // game window instead). misc/HiddenWindow.h keeps its handle in an 'inline static', i.e. one copy per
    // translation unit, so it is read from the swapchain rather than from that global.
    if (_hiddenWindow == nullptr)
    {
        DXGI_SWAP_CHAIN_DESC realDesc = {};
        if (FAILED(_real->GetDesc(&realDesc)) || realDesc.OutputWindow == nullptr || realDesc.OutputWindow == _handle)
            return;

        _hiddenWindow = realDesc.OutputWindow;
    }

    if (!IsWindow(_hiddenWindow))
        return;

    _hiddenWindowSyncFrame++;

    const DWORD ownerThread = GetWindowThreadProcessId(_hiddenWindow, nullptr);
    const bool ownedByThisThread = ownerThread == GetCurrentThreadId();
    const bool hasPumpThread = GetPropW(_hiddenWindow, kHiddenSwapchainWindowPumpProp) != nullptr;
    const bool isChild = GetAncestor(_hiddenWindow, GA_PARENT) == _handle;

    if (isChild && !_hiddenWindowAttachLogged)
    {
        _hiddenWindowAttachLogged = true;
        _mirroredClientRect = {};
        LOG_INFO("Hidden DX11 swapchain window {:X} is now a hidden child of the game window {:X}",
                 (size_t) _hiddenWindow, (size_t) _handle);
    }

    // A window owned by another thread is only changed when that thread pumps messages
    if (!isChild && Config::Instance()->Dx11wDx12ParentHiddenWindow.value_or_default() &&
        (ownedByThisThread || hasPumpThread) && _hiddenWindowAttachRequests < 3 &&
        (_hiddenWindowAttachRequests == 0 || _hiddenWindowSyncFrame - _hiddenWindowLastAttachFrame > 120))
    {
        _hiddenWindowAttachRequests++;
        _hiddenWindowLastAttachFrame = _hiddenWindowSyncFrame;

        if (ownedByThisThread)
            SendMessageW(_hiddenWindow, kHiddenSwapchainWindowMsgAttach, reinterpret_cast<WPARAM>(_handle), 0);
        else
            PostMessageW(_hiddenWindow, kHiddenSwapchainWindowMsgAttach, reinterpret_cast<WPARAM>(_handle), 0);

        LOG_INFO("Requested hidden DX11 swapchain window {:X} to become a child of the game window {:X} "
                 "(attempt {}, owner thread {}, present thread {}, own pump thread: {})",
                 (size_t) _hiddenWindow, (size_t) _handle, _hiddenWindowAttachRequests, ownerThread,
                 GetCurrentThreadId(), hasPumpThread);
        return;
    }

    RECT client = {};
    if (!GetClientRect(_handle, &client) || client.right <= 0 || client.bottom <= 0)
        return;

    RECT target = { 0, 0, client.right, client.bottom };
    if (!isChild)
    {
        POINT topLeft = { 0, 0 };
        if (!ClientToScreen(_handle, &topLeft))
            return;

        target = { topLeft.x, topLeft.y, topLeft.x + client.right, topLeft.y + client.bottom };
    }

    if (!EqualRect(&target, &_mirroredClientRect))
    {
        // Stays hidden (no SWP_SHOWWINDOW). Async unless this thread owns the window, so a window owned by
        // another thread never blocks the present.
        UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;
        if (!ownedByThisThread)
            flags |= SWP_ASYNCWINDOWPOS;

        if (SetWindowPos(_hiddenWindow, nullptr, target.left, target.top, client.right, client.bottom, flags))
        {
            _mirroredClientRect = target;
            _hiddenWindowVerifyFrame = _hiddenWindowSyncFrame + 30;
            _hiddenWindowVerifyLogged = false;
            LOG_INFO("Hidden DX11 swapchain window follows the game window: {}x{} at {},{} ({})", client.right,
                     client.bottom, target.left, target.top, isChild ? "child" : "screen");
        }
    }

    // Check that the request was actually carried out (it is not if the owning thread never pumps messages)
    if (!_hiddenWindowVerifyLogged && _hiddenWindowVerifyFrame != 0 && _hiddenWindowSyncFrame >= _hiddenWindowVerifyFrame)
    {
        _hiddenWindowVerifyLogged = true;

        RECT hiddenClient = {};
        GetClientRect(_hiddenWindow, &hiddenClient);

        POINT hiddenOrigin = { 0, 0 };
        POINT gameOrigin = { 0, 0 };
        ClientToScreen(_hiddenWindow, &hiddenOrigin);
        ClientToScreen(_handle, &gameOrigin);

        if (hiddenClient.right == client.right && hiddenClient.bottom == client.bottom &&
            hiddenOrigin.x == gameOrigin.x && hiddenOrigin.y == gameOrigin.y)
        {
            LOG_INFO("Hidden DX11 swapchain window matches the game window: {}x{} at {},{} (child: {})",
                     hiddenClient.right, hiddenClient.bottom, hiddenOrigin.x, hiddenOrigin.y, isChild);
        }
        else
        {
            LOG_WARN("Hidden DX11 swapchain window does not match the game window: {}x{} at {},{} vs {}x{} at {},{} "
                     "(child: {}, owner thread {}, present thread {}, own pump thread: {}); ReShade's overlay will "
                     "not take the mouse correctly",
                     hiddenClient.right, hiddenClient.bottom, hiddenOrigin.x, hiddenOrigin.y, client.right,
                     client.bottom, gameOrigin.x, gameOrigin.y, isChild, ownerThread, GetCurrentThreadId(),
                     hasPumpThread);
        }
    }
}

bool Dx11wDx12SC::CaptureReShadeBackBuffer(ID3D11Resource* backBuffer)
{
    if (backBuffer == nullptr || _dx11Context == nullptr)
        return false;

    if (_currentFakeIndex >= _sharedDx11BackBufferCopies.size() ||
        _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    if (!_CopyToShared(backBuffer))
        return false;

    _capturedFromReShade = true;
    return true;
}

bool Dx11wDx12SC::_CopyDx11BackBufferToShared(UINT index)
{
    if (_currentFakeIndex >= _sharedDx11BackBufferCopies.size() ||
        _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    if (_dx11Context == nullptr || _real == nullptr)
        return false;

    ID3D11Texture2D* sourceTexture = nullptr;
    auto result = _real->GetBuffer(index, IID_PPV_ARGS(&sourceTexture));
    if (FAILED(result) || sourceTexture == nullptr)
    {
        LOG_ERROR("GetBuffer for shadow copy {} failed: {:X}", index, (UINT) result);
        return false;
    }

    LOG_DEBUG("Copying DX11 backbuffer {} sourceTexture: {:X} to shadow copy {:X}", index, (size_t) sourceTexture,
              (size_t) _sharedDx11BackBufferCopies[_currentFakeIndex]);

    const bool copied = _CopyToShared(sourceTexture);
    sourceTexture->Release();
    return copied;
}

bool Dx11wDx12SC::_WaitDx11ThenDx12()
{
    if (_dx11Context4 == nullptr || _dx11Fence == nullptr || _dx12CommandQueue == nullptr ||
        _dx12SharedFence == nullptr)
        return false;

    const auto waitValue = _sharedFenceValue++;

    auto result = _dx11Context4->Signal(_dx11Fence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("DX11 Signal failed: {:X}", (UINT) result);
        return false;
    }

    _dx11Context4->Flush();

    // Important:
    // Do not block the FG/present queue on the D3D11 shared fence.
    // Isolate the cross-API wait on the interop copy queue.
    result = _dx12CommandQueue->Wait(_dx12SharedFence, waitValue);
    if (FAILED(result))
    {
        LOG_ERROR("interop copy queue Wait on D3D11 fence failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_WaitForCopyAllocator(UINT slot)
{
    if (_copyFence == nullptr || _copyFenceEvent == nullptr)
        return true;

    if (slot >= _copyAllocatorFenceValues.size())
    {
        LOG_ERROR("copy allocator slot {} out of range {}", slot, _copyAllocatorFenceValues.size());
        return false;
    }

    const auto fenceValue = _copyAllocatorFenceValues[slot];

    if (fenceValue == 0)
        return true;

    const auto completedValue = _copyFence->GetCompletedValue();
    if (completedValue >= fenceValue)
        return true;

    auto result = _copyFence->SetEventOnCompletion(fenceValue, _copyFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator fence SetEventOnCompletion failed. slot {}, fence {}, completed {}, result {:X}",
                  slot, fenceValue, completedValue, (UINT) result);
        return false;
    }

    const auto waitResult = WaitForSingleObject(_copyFenceEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("copy allocator fence wait failed. slot {}, fence {}, completed {}, waitResult {:X}", slot,
                  fenceValue, _copyFence->GetCompletedValue(), waitResult);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_CopyDx11SharedToDx12FGBackBuffer(UINT dx11Index)
{
    if (_copyAllocators.empty() || _copyCommandLists.empty() || _dx12CommandQueue == nullptr || _copyFence == nullptr ||
        _fgSwapChain == nullptr || _currentFakeIndex >= _openedDx11BackBuffers.size() ||
        _openedDx11BackBuffers[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    const UINT copySlot = _currentFakeIndex;
    auto allocator = _copyAllocators[copySlot];

    if (allocator == nullptr)
        return false;

    if (!_WaitForCopyAllocator(copySlot))
        return false;

    auto result = allocator->Reset();
    if (FAILED(result))
    {
        LOG_ERROR("copy allocator[{}] reset failed: {:X}", copySlot, (UINT) result);
        return false;
    }

    result = _copyCommandLists[copySlot]->Reset(allocator, nullptr);
    if (FAILED(result))
    {
        LOG_ERROR("copy command list reset failed: {:X}", (UINT) result);
        return false;
    }

    UINT fgIndex = _fgSwapChain->GetCurrentBackBufferIndex();
    LOG_DEBUG("dx11Index {}, fgIndex {}", dx11Index, fgIndex);

    ID3D12Resource* fgBackBuffer = nullptr;
    result = _fgSwapChain->GetBuffer(fgIndex, IID_PPV_ARGS(&fgBackBuffer));
    if (FAILED(result) || fgBackBuffer == nullptr)
    {
        LOG_ERROR("FG GetBuffer({}) failed: {:X}", fgIndex, (UINT) result);
        _copyCommandLists[copySlot]->Close();
        return false;
    }

    auto sourceBefore = _openedDx11BackBufferStates[copySlot];

    TransitionResource(_copyCommandLists[copySlot], _openedDx11BackBuffers[copySlot], sourceBefore,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_COPY_DEST);

    _copyCommandLists[copySlot]->CopyResource(fgBackBuffer, _openedDx11BackBuffers[copySlot]);

    TransitionResource(_copyCommandLists[copySlot], fgBackBuffer, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_PRESENT);
    TransitionResource(_copyCommandLists[copySlot], _openedDx11BackBuffers[copySlot], D3D12_RESOURCE_STATE_COPY_SOURCE,
                       sourceBefore);

    _openedDx11BackBufferStates[copySlot] = D3D12_RESOURCE_STATE_COMMON;

    fgBackBuffer->Release();

    result = _copyCommandLists[copySlot]->Close();
    if (FAILED(result))
    {
        LOG_ERROR("copy command list close failed: {:X}", (UINT) result);
        return false;
    }

    ID3D12CommandList* lists[] = { _copyCommandLists[copySlot] };
    _dx12CommandQueue->ExecuteCommandLists(1, lists);

    const auto signalValue = ++_copyFenceValue;

    result = _dx12CommandQueue->Signal(_copyFence, signalValue);
    if (FAILED(result))
    {
        LOG_ERROR("interop copy fence signal failed: {:X}", (UINT) result);
        return false;
    }

    _copyAllocatorFenceValues[copySlot] = signalValue;
    _lastInteropCopyFenceValue = signalValue;

    return true;
}

bool Dx11wDx12SC::_WaitForInteropCopyOnPresentQueue()
{
    if (_copyFence == nullptr)
        return false;

    if (_lastInteropCopyFenceValue == 0)
        return true;

    auto presentQueue = _fg != nullptr ? _fg->GetCommandQueue() : nullptr;

    if (presentQueue == nullptr || presentQueue == _dx12CommandQueue)
        return true;

    auto result = presentQueue->Wait(_copyFence, _lastInteropCopyFenceValue);
    if (FAILED(result))
    {
        LOG_ERROR("present queue Wait on interop copy fence failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

bool Dx11wDx12SC::_WaitForCopyQueueIdle()
{
    if (_copyFence == nullptr || _copyFenceEvent == nullptr)
        return true;

    UINT64 waitValue = _lastInteropCopyFenceValue;

    for (const auto fenceValue : _copyAllocatorFenceValues)
        waitValue = std::max(waitValue, fenceValue);

    if (waitValue == 0)
        return true;

    const auto completedValue = _copyFence->GetCompletedValue();
    if (completedValue >= waitValue)
        return true;

    auto result = _copyFence->SetEventOnCompletion(waitValue, _copyFenceEvent);
    if (FAILED(result))
    {
        LOG_ERROR("copy queue idle SetEventOnCompletion failed. fence {}, completed {}, result {:X}", waitValue,
                  completedValue, (UINT) result);
        return false;
    }

    const auto waitResult = WaitForSingleObject(_copyFenceEvent, 5000);
    if (waitResult != WAIT_OBJECT_0)
    {
        LOG_ERROR("copy queue idle wait failed. fence {}, completed {}, waitResult {:X}", waitValue,
                  _copyFence->GetCompletedValue(), waitResult);
        return false;
    }

    for (auto& fenceValue : _copyAllocatorFenceValues)
        fenceValue = 0;

    _lastInteropCopyFenceValue = 0;

    return true;
}

void Dx11wDx12SC::_ReleaseInteropBackBuffers()
{
    _interopInitialized = false;

    _ReleaseHdr10Resources(false);

    for (auto& resource : _openedDx11BackBuffers)
        SafeRelease(resource);

    for (auto& texture : _sharedDx11BackBufferCopies)
        SafeRelease(texture);

    for (auto& handle : _sharedBackBufferHandles)
        SafeCloseHandle(handle);

    _openedDx11BackBufferStates.clear();
    _openedDx11BackBuffers.clear();
    _sharedDx11BackBufferCopies.clear();
    _sharedBackBufferHandles.clear();
}

void Dx11wDx12SC::_ReleaseInteropObjects()
{
    _WaitForCopyQueueIdle();
    _ReleaseInteropBackBuffers();

    _lastInteropCopyFenceValue = 0;

    SafeRelease(_dx12SharedFence);
    SafeRelease(_dx11Fence);
    SafeCloseHandle(_dx11FenceEvent);
    SafeCloseHandle(_sharedFenceHandle);

    for (auto& cmdList : _copyCommandLists)
        SafeRelease(cmdList);

    _copyCommandLists.clear();

    for (auto& allocator : _copyAllocators)
        SafeRelease(allocator);

    _copyAllocators.clear();
    _copyAllocatorFenceValues.clear();

    SafeRelease(_copyFence);
    SafeCloseHandle(_copyFenceEvent);
    _copyFenceValue = 1;

    _interopInitialized = false;
}

void Dx11wDx12SC::_RefreshCachedSwapchainDesc()
{
    _bufferCount = ResolveBufferCount(_real, _real1);
    _bufferFormat = ResolveBufferFormat(_real, _real1);
    _currentFakeIndex = _bufferCount > 0 ? _currentFakeIndex : 0;
}

UINT Dx11wDx12SC::_GetDx11BackBufferIndexForPresent() const
{
    if (_real3 != nullptr)
        return _real3->GetCurrentBackBufferIndex();

    return _bufferCount > 0 ? _currentFakeIndex : 0;
}

void Dx11wDx12SC::_AdvanceFakeBackBufferIndex() { _currentFakeIndex = (_currentFakeIndex + 1) % _bufferCount; }

// ---------------------------------------------------------------------------------------------------------------
// HDR10 conversion
//
// DLSS-G "does NOT support FP16 pixel format and scRGB color space" (Streamline DLSS-G programming guide, 11.0) and
// XeFG has the same limit. A DX11 game that renders HDR into an FP16 back buffer (Assassin's Creed Origins: scRGB,
// switched on through NVAPI in exclusive fullscreen) would leave frame generation without a usable frame. With
// Dx11wDx12HDR10Conversion the FG swapchain gets R10G10B10A2 buffers with the PQ/BT.2020 color space, and every
// frame is converted on the D3D11 side while it is copied into the shared texture. The display only shows HDR10
// from a windowed (borderless) flip-model swapchain when Windows HDR is turned on.
// ---------------------------------------------------------------------------------------------------------------

namespace
{
const char* kHdr10ConvertShader = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<unorm float4> Destination : register(u0);

cbuffer Params : register(b0)
{
    uint Width;
    uint Height;
    uint InputIsPQ;
    float ScRgbNits;
};

float3 EncodePQ(float3 nits)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    float3 y = saturate(nits / 10000.0);
    float3 ym = pow(y, m1);
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= Width || id.y >= Height)
        return;

    float3 color = Source.Load(int3(id.xy, 0)).rgb;
    float3 pq;

    if (InputIsPQ != 0)
    {
        pq = saturate(color);
    }
    else
    {
        // scRGB: linear BT.709 primaries, 1.0 = 80 nits, may be negative outside the BT.709 gamut
        float3 bt2020 = float3(dot(float3(0.6274040, 0.3292820, 0.0433136), color),
                               dot(float3(0.0690970, 0.9195400, 0.0113612), color),
                               dot(float3(0.0163916, 0.0880132, 0.8955950), color));

        // Also turns NaN into 0
        bt2020 = max(bt2020, 0.0);
        pq = EncodePQ(bt2020 * ScRgbNits);
    }

    Destination[id.xy] = float4(pq, 1.0);
}
)";

struct Hdr10ConvertConstants
{
    UINT Width;
    UINT Height;
    UINT InputIsPQ;
    float ScRgbNits;
};

bool IsWindowsHdrOn(IDXGISwapChain* swapChain)
{
    if (swapChain == nullptr)
        return false;

    IDXGIOutput* output = nullptr;
    if (FAILED(swapChain->GetContainingOutput(&output)) || output == nullptr)
        return false;

    bool hdrOn = false;
    IDXGIOutput6* output6 = nullptr;
    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) && output6 != nullptr)
    {
        DXGI_OUTPUT_DESC1 desc = {};
        if (SUCCEEDED(output6->GetDesc1(&desc)))
            hdrOn = desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

        output6->Release();
    }

    output->Release();
    return hdrOn;
}

void SetOutputColorSpaceState(DXGI_COLOR_SPACE_TYPE colorSpace)
{
    OutputColorSpace info {};
    info.dxgiColorSpace = colorSpace;
    info.range = ColorRange::Full;
    info.model = ColorModel::RGB;
    info.valid = true;

    switch (colorSpace)
    {
    case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
        info.transfer = ColorTransfer::PQ;
        info.primaries = ColorPrimaries::Rec2020;
        break;

    case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
        info.transfer = ColorTransfer::Linear;
        info.primaries = ColorPrimaries::Rec709;
        break;

    default:
        info.transfer = ColorTransfer::SRGB;
        info.primaries = ColorPrimaries::Rec709;
        break;
    }

    State::Instance().outputColorSpace = info;
}
} // namespace

bool Dx11wDx12SC::NeedsHdr10Conversion(DXGI_FORMAT gameFormat)
{
    if (gameFormat != DXGI_FORMAT_R16G16B16A16_FLOAT || !Config::Instance()->Dx11wDx12HDR10Conversion.value_or_default())
        return false;

    const auto fgOutput = State::Instance().activeFgOutput;
    return fgOutput == FGOutput::DLSSG || fgOutput == FGOutput::XeFG;
}

void Dx11wDx12SC::_UpdateHdr10State()
{
    if (_fgSwapChain == nullptr)
        return;

    DXGI_SWAP_CHAIN_DESC fgDesc = {};
    if (FAILED(_fgSwapChain->GetDesc(&fgDesc)))
        return;

    const bool active =
        _bufferFormat == DXGI_FORMAT_R16G16B16A16_FLOAT && fgDesc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;

    if (active)
    {
        const bool windowsHdr = IsWindowsHdrOn(_fgSwapChain);

        UINT support = 0;
        const auto supportResult = _fgSwapChain->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &support);
        const auto colorSpaceResult = _fgSwapChain->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);

        _fgColorSpaceOverridden = true;
        SetOutputColorSpaceState(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        State::Instance().hdrOutputActive = windowsHdr;

        if (!_hdr10Active)
        {
            LOG_INFO("HDR10 output on: the FP16 back buffer ({}) is converted to R10G10B10A2 PQ/BT.2020 for frame "
                     "generation; {}x{}, color space support {:X} ({:X}), SetColorSpace1 {:X}",
                     Config::Instance()->Dx11wDx12HDR10InputIsPQ.value_or_default() ? "PQ" : "scRGB",
                     fgDesc.BufferDesc.Width, fgDesc.BufferDesc.Height, support, (UINT) supportResult,
                     (UINT) colorSpaceResult);
        }

        if (!windowsHdr)
            LOG_WARN("Windows HDR is off for this display: an HDR10 frame generation swapchain needs it on (Win+Alt+B), "
                     "or the picture will look washed out");
    }
    else if (_fgColorSpaceOverridden)
    {
        const auto colorSpaceResult = _fgSwapChain->SetColorSpace1(_gameColorSpace);
        _fgColorSpaceOverridden = false;
        SetOutputColorSpaceState(_gameColorSpace);
        State::Instance().hdrOutputActive = false;

        LOG_INFO("HDR10 output off: FG swapchain format {}, color space {} restored ({:X})",
                 (UINT) fgDesc.BufferDesc.Format, (UINT) _gameColorSpace, (UINT) colorSpaceResult);
    }

    _hdr10Active = active;
}

void Dx11wDx12SC::_ReleaseHdr10Resources(bool releaseShader)
{
    for (auto& uav : _hdrSharedUavs)
        SafeRelease(uav);

    _hdrSharedUavs.clear();

    SafeRelease(_hdrSourceSrv);
    SafeRelease(_hdrSource);

    if (releaseShader)
    {
        SafeRelease(_hdrShader);
        SafeRelease(_hdrConstants);
    }
}

bool Dx11wDx12SC::_CopyToShared(ID3D11Resource* source)
{
    if (source == nullptr || _dx11Context == nullptr || _currentFakeIndex >= _sharedDx11BackBufferCopies.size() ||
        _sharedDx11BackBufferCopies[_currentFakeIndex] == nullptr)
    {
        return false;
    }

    if (!_hdr10Active)
    {
        _dx11Context->CopyResource(_sharedDx11BackBufferCopies[_currentFakeIndex], source);
        return true;
    }

    if (!_ConvertToHdr10(source) && !_hdr10ErrorLogged)
    {
        _hdr10ErrorLogged = true;
        LOG_ERROR("HDR10 conversion failed, frame generation gets no new frames");
    }

    // A format mismatch cannot be copied; never report a device loss for it
    return true;
}

bool Dx11wDx12SC::_ConvertToHdr10(ID3D11Resource* source)
{
    if (_dx11Device == nullptr || _hdrShaderFailed)
        return false;

    if (_hdrShader == nullptr)
    {
        auto blob = CompileShader(kHdr10ConvertShader, "CSMain", "cs_5_0");
        if (blob == nullptr)
        {
            _hdrShaderFailed = true;
            return false;
        }

        auto result = _dx11Device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                       &_hdrShader);
        blob->Release();

        if (FAILED(result))
        {
            LOG_ERROR("HDR10 CreateComputeShader failed: {:X}", (UINT) result);
            _hdrShaderFailed = true;
            return false;
        }
    }

    if (_hdrConstants == nullptr)
    {
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(Hdr10ConvertConstants);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        auto result = _dx11Device->CreateBuffer(&cbDesc, nullptr, &_hdrConstants);
        if (FAILED(result))
        {
            LOG_ERROR("HDR10 constant buffer failed: {:X}", (UINT) result);
            return false;
        }
    }

    ID3D11Texture2D* sourceTexture = nullptr;
    if (FAILED(source->QueryInterface(IID_PPV_ARGS(&sourceTexture))) || sourceTexture == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceTexture->GetDesc(&sourceDesc);
    sourceTexture->Release();

    // Swapchain buffers usually cannot be read by shaders, so take a readable FP16 copy first
    if (_hdrSource != nullptr)
    {
        D3D11_TEXTURE2D_DESC currentDesc = {};
        _hdrSource->GetDesc(&currentDesc);

        if (currentDesc.Width != sourceDesc.Width || currentDesc.Height != sourceDesc.Height ||
            currentDesc.Format != sourceDesc.Format)
        {
            SafeRelease(_hdrSourceSrv);
            SafeRelease(_hdrSource);
        }
    }

    if (_hdrSource == nullptr)
    {
        D3D11_TEXTURE2D_DESC desc = sourceDesc;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        auto result = _dx11Device->CreateTexture2D(&desc, nullptr, &_hdrSource);
        if (FAILED(result))
        {
            LOG_ERROR("HDR10 source copy texture failed: {:X}", (UINT) result);
            return false;
        }

        result = _dx11Device->CreateShaderResourceView(_hdrSource, nullptr, &_hdrSourceSrv);
        if (FAILED(result))
        {
            LOG_ERROR("HDR10 source SRV failed: {:X}", (UINT) result);
            SafeRelease(_hdrSource);
            return false;
        }
    }

    if (_hdrSharedUavs.size() < _sharedDx11BackBufferCopies.size())
        _hdrSharedUavs.resize(_sharedDx11BackBufferCopies.size(), nullptr);

    auto& uav = _hdrSharedUavs[_currentFakeIndex];
    if (uav == nullptr)
    {
        auto result =
            _dx11Device->CreateUnorderedAccessView(_sharedDx11BackBufferCopies[_currentFakeIndex], nullptr, &uav);
        if (FAILED(result))
        {
            LOG_ERROR("HDR10 shared copy UAV failed: {:X}", (UINT) result);
            return false;
        }
    }

    D3D11_TEXTURE2D_DESC targetDesc = {};
    _sharedDx11BackBufferCopies[_currentFakeIndex]->GetDesc(&targetDesc);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    auto result = _dx11Context->Map(_hdrConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(result))
    {
        LOG_ERROR("HDR10 constant buffer Map failed: {:X}", (UINT) result);
        return false;
    }

    Hdr10ConvertConstants constants {};
    constants.Width = std::min(targetDesc.Width, sourceDesc.Width);
    constants.Height = std::min(targetDesc.Height, sourceDesc.Height);
    constants.InputIsPQ = Config::Instance()->Dx11wDx12HDR10InputIsPQ.value_or_default() ? 1 : 0;
    constants.ScRgbNits = 80.0f;
    memcpy(mapped.pData, &constants, sizeof(constants));
    _dx11Context->Unmap(_hdrConstants, 0);

    _dx11Context->CopyResource(_hdrSource, source);

    // The compute stage belongs to the game (and ReShade) between frames: save and restore what is touched
    ID3D11ComputeShader* oldShader = nullptr;
    ID3D11ShaderResourceView* oldSrv = nullptr;
    ID3D11UnorderedAccessView* oldUav = nullptr;
    ID3D11Buffer* oldConstants = nullptr;

    _dx11Context->CSGetShader(&oldShader, nullptr, nullptr);
    _dx11Context->CSGetShaderResources(0, 1, &oldSrv);
    _dx11Context->CSGetUnorderedAccessViews(0, 1, &oldUav);
    _dx11Context->CSGetConstantBuffers(0, 1, &oldConstants);

    _dx11Context->CSSetShader(_hdrShader, nullptr, 0);
    _dx11Context->CSSetConstantBuffers(0, 1, &_hdrConstants);
    _dx11Context->CSSetShaderResources(0, 1, &_hdrSourceSrv);
    _dx11Context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

    _dx11Context->Dispatch((constants.Width + 7) / 8, (constants.Height + 7) / 8, 1);

    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    _dx11Context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    _dx11Context->CSSetShaderResources(0, 1, &nullSrv);

    _dx11Context->CSSetShader(oldShader, nullptr, 0);
    _dx11Context->CSSetShaderResources(0, 1, &oldSrv);
    _dx11Context->CSSetUnorderedAccessViews(0, 1, &oldUav, nullptr);
    _dx11Context->CSSetConstantBuffers(0, 1, &oldConstants);

    SafeRelease(oldShader);
    SafeRelease(oldSrv);
    SafeRelease(oldUav);
    SafeRelease(oldConstants);

    return true;
}
