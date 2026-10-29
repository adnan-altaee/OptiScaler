#include "pch.h"
#include "Upscaler_Inputs_Dx11wDx12.h"
#include "Upscaler_Inputs.h"

#include "MathUtils.h"

#include <with_dx12/with_dx12.h>
#include <upscalers/IFeature_Dx11wDx12.h>
#include <hudfix/Hudfix_Dx11.h>
#include <resource_tracking/ResTrack_dx11.h>
#include "shaders/depth_scale/DS_Dx12.h"
#include <misc/GameCamera.h>
#include <misc/FGCapture.h>

using namespace OptiMath;
static DS_Dx12* DepthScaleDx11wDx12 = nullptr;

// Used when the upscaler is native DX11 and doesn't have its own D3D12 shared resources
static Dx11WithDx12::D3D11_UPSCALER_RESOURCE_CACHE_C FgResourceCache = {};

static bool PrepareFgResourceCache(NVSDK_NGX_Parameter* parameters, UINT64 frameKey)
{
    if (parameters == nullptr)
        return false;

    const auto mask = Dx11WithDx12::ResourceMask::Mv | Dx11WithDx12::ResourceMask::Depth;
    const auto dontUseNtShared = Config::Instance()->DontUseNTShared.value_or_default();

    // Next slot, the previous ones can still be in use by FG
    const auto result = Dx11WithDx12::PrepareUpscalerResources(
        FgResourceCache, parameters, mask, FgResourceCache.FrameIndex + 1, frameKey, dontUseNtShared, false, true);

    if (!result.Success)
    {
        LOG_ERROR("Dx11wDx12 FG input cache preparation failed");
        return false;
    }

    return true;
}

static bool ReusePreparedUpscalerCacheForFg(const Dx11WithDx12::D3D11_UPSCALER_RESOURCE_CACHE_C& cache)
{
    const auto mask = Dx11WithDx12::ResourceMask::Mv | Dx11WithDx12::ResourceMask::Depth;

    if (!Dx11WithDx12::HasPreparedUpscalerResources(cache, mask))
    {
        LOG_WARN("Dx11wDx12 FG input cache miss");
        return false;
    }

    return true;
}

void UpscalerInputsDx11wDx12::Init(ID3D11Device* dx11Device, ID3D11DeviceContext* dx11Context, ID3D12Device* dx12Device,
                                   ID3D12CommandQueue* dx12CommandQueue)
{
    if (State::Instance().activeFgInput != FGInput::Upscaler)
        return;

    if (dx12Device != nullptr && dx12CommandQueue != nullptr && !WithDx12::IsInited())
        WithDx12::SetD3D12Objects(dx12Device, dx12CommandQueue, D3D12_COMMAND_LIST_TYPE_DIRECT);

    if (!WithDx12::IsInited() && !WithDx12::PrepareD3D12ForD3D11(dx11Device, D3D_FEATURE_LEVEL_11_0))
    {
        LOG_ERROR("UpscalerInputsDx11wDx12::Init failed to resolve D3D12 device/queue");
        return;
    }

    _dx12Device = WithDx12::GetD3D12Device();
    _dx12CommandQueue = WithDx12::GetD3D12CommandQueue();

    Dx11WithDx12::Init(dx11Device, dx11Context);
}

void UpscalerInputsDx11wDx12::Reset()
{
    ResTrack_Dx11::ClearPossibleHudless();
    Hudfix_Dx11::ResetCounters();
}

void UpscalerInputsDx11wDx12::Shutdown()
{
    Reset();
    Dx11WithDx12::ResetUpscalerResourceCache(FgResourceCache, true);
}

void UpscalerInputsDx11wDx12::UpscaleStart(NVSDK_NGX_Parameter* InParameters, IFeature_Dx11* feature)
{
    Hudfix_Dx11::SetSkipStatus(true);

    if (InParameters == nullptr || feature == nullptr)
    {
        Hudfix_Dx11::SetSkipStatus(false);
        return;
    }

    float mvScaleX = 0.0f;
    float mvScaleY = 0.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;

    // FSR Camera values
    auto camera = UpscalerInputs::GetCameraValues(*InParameters, feature);

    if (State::Instance().activeFgInput != FGInput::Upscaler || _dx12Device == nullptr)
        return;

    const Dx11WithDx12::D3D11_UPSCALER_RESOURCE_CACHE_C* cache = &FgResourceCache;

    if (auto withDx12Feature = dynamic_cast<IFeature_Dx11wDx12*>(feature); withDx12Feature != nullptr)
    {
        cache = &withDx12Feature->GetResourceCache();

        if (!ReusePreparedUpscalerCacheForFg(*cache))
            return;
    }
    else if (!PrepareFgResourceCache(InParameters, Dx11WithDx12::NextUpscalerFrameId()))
    {
        LOG_ERROR("Dx11wDx12 FG input cache preparation failed");
        return;
    }

    auto fg = State::Instance().currentFG;

    if (fg == nullptr)
        return;

    auto fgConstants = UpscalerInputs::GetFGConstants(feature);

    // The upscaler's DepthInverted flag can be wrong for frame generation (a mod feeding DLSS may not set it):
    // decide from the depth texels themselves
    const bool gameCamera = GameCamera::IsEnabled();
    if (Config::Instance()->Dx11wDx12DetectDepthInverted.value_or(gameCamera))
    {
        ID3D11Resource* depth = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_Depth, &depth) != NVSDK_NGX_Result_Success)
            InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**) &depth);

        GameCamera::ProbeDepth(Dx11WithDx12::GetD3D11DeviceContext(), depth, feature->RenderWidth(),
                               feature->RenderHeight());

        if (const int inverted = GameCamera::DepthInverted(); inverted == 1)
            fgConstants.flags |= FG_Flags::InvertedDepth;
        else if (inverted == 0)
            fgConstants.flags.reset(FG_Flags::InvertedDepth);
    }

    fg->EvaluateState(_dx12Device, fgConstants);

    // Not needed while FG or the option is off, its destructor defers the buffer release
    if (DepthScaleDx11wDx12 != nullptr &&
        (!Config::Instance()->IsFGEnabled() || !Config::Instance()->FGEnableDepthScale.value_or_default()))
    {
        delete DepthScaleDx11wDx12;
        DepthScaleDx11wDx12 = nullptr;
    }

    int reset = 0;
    InParameters->Get(NVSDK_NGX_Parameter_Reset, &reset);

    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);
    InParameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &mvScaleY);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &jitterX);
    InParameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &jitterY);

    fg->StartNewFrame();

    auto aspectRatio = (float) feature->DisplayWidth() / (float) feature->DisplayHeight();
    fg->SetCameraValues(camera.cameraNear, camera.cameraFar, camera.cameraVFov, aspectRatio, camera.meterFactor);

    // The game's real camera instead of the placeholder values above, when it can be read (misc/GameCamera.cpp)
    GameCameraFrame gameFrame {};
    bool haveGameFrame = false;
    if (gameCamera)
    {
        haveGameFrame =
            GameCamera::GetFrame(jitterX, jitterY, feature->RenderWidth(), feature->RenderHeight(), gameFrame);
        if (haveGameFrame)
        {
            float nearValue = gameFrame.nearPlane;
            float farValue = gameFrame.farPlane;

            // Same order as UpscalerInputs::GetCameraValues: swapped for inverted depth (FFX convention)
            if (fgConstants.flags[FG_Flags::InvertedDepth])
                std::swap(nearValue, farValue);

            fg->SetCameraValues(nearValue, farValue, gameFrame.verticalFov, gameFrame.aspect, camera.meterFactor);
            fg->SetCameraData(gameFrame.position, gameFrame.up, gameFrame.right, gameFrame.forward);
            fg->SetCameraMatrices(gameFrame.viewToClip, gameFrame.clipToView, gameFrame.clipToPrevClip,
                                  gameFrame.prevClipToClip);

            if (gameFrame.cut)
                reset = 1;
        }
        else
        {
            float zero[3] = {};
            fg->SetCameraData(zero, zero, zero, zero);
        }
    }

    fg->SetFrameTimeDelta(State::Instance().lastFGFrameTime);
    fg->SetMVScale(mvScaleX, mvScaleY);
    fg->SetJitter(jitterX, jitterY);
    fg->SetReset(reset);
    fg->SetInterpolationRect(feature->DisplayWidth(), feature->DisplayHeight());

    // Menu "Capture FG inputs": this frame's motion vectors, depth and constants (misc/FGCapture.cpp)
    if (FGCapture::Busy())
    {
        ID3D11Resource* mv = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, &mv) != NVSDK_NGX_Result_Success)
            InParameters->Get(NVSDK_NGX_Parameter_MotionVectors, (void**) &mv);

        ID3D11Resource* depth = nullptr;
        if (InParameters->Get(NVSDK_NGX_Parameter_Depth, &depth) != NVSDK_NGX_Result_Success)
            InParameters->Get(NVSDK_NGX_Parameter_Depth, (void**) &depth);

        FGCapture::InputInfo info {};
        info.renderWidth = feature->RenderWidth();
        info.renderHeight = feature->RenderHeight();
        info.displayWidth = feature->DisplayWidth();
        info.displayHeight = feature->DisplayHeight();
        info.mvScaleX = mvScaleX;
        info.mvScaleY = mvScaleY;
        info.jitterX = jitterX;
        info.jitterY = jitterY;
        info.lowResMV = feature->LowResMV();
        info.jitteredMV = feature->JitteredMV();
        info.depthInverted = fgConstants.flags[FG_Flags::InvertedDepth];
        info.reset = reset != 0;
        info.camera = haveGameFrame ? &gameFrame : nullptr;

        FGCapture::OnInputs(Dx11WithDx12::GetD3D11DeviceContext(), mv, depth, info);
    }

    Hudfix_Dx11::UpscaleStart();

    if (State::Instance().isShuttingDown || !fg->IsActive() || !Config::Instance()->IsFGEnabled() ||
        State::Instance().currentSwapchain == nullptr)
    {
        return;
    }

    if (fg->Mutex.getOwner() == 2)
    {
        LOG_TRACE("Waiting for present!");
        fg->Mutex.lock(4);
        fg->Mutex.unlockThis(4);
    }

    const auto frameIndex = fg->GetIndex();

    LOG_DEBUG("(FG Dx11wDx12) using cached inputs for fgUpscaledImage[{}], frame: {}", frameIndex, fg->FrameCount());

    ID3D12Resource* paramVelocity = cache->CurrentMv().Dx12Resource;
    ID3D12Resource* paramDepth = cache->CurrentDepth().Dx12Resource;

    auto cmdList = fg->GetUICommandList();

    if (paramVelocity != nullptr)
    {
        Dx12Resource setResource {};
        setResource.type = FG_ResourceType::Velocity;
        setResource.cmdList = cmdList;
        setResource.resource = paramVelocity;
        setResource.state = (D3D12_RESOURCE_STATES) Config::Instance()->MVResourceBarrier.value_or(
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        setResource.validity = FG_ResourceValidity::ValidNow;

        if (feature->LowResMV())
        {
            setResource.width = feature->RenderWidth();
            setResource.height = feature->RenderHeight();
        }
        else
        {
            setResource.width = feature->TargetWidth();
            setResource.height = feature->TargetHeight();
        }

        fg->SetResource(&setResource);
    }

    if (paramDepth != nullptr)
    {
        auto done = false;

        if (Config::Instance()->FGEnableDepthScale.value_or_default())
        {
            if (DepthScaleDx11wDx12 == nullptr)
                DepthScaleDx11wDx12 = new DS_Dx12("Depth Scale Dx11wDx12", _dx12Device);

            if (DepthScaleDx11wDx12->CreateBufferResource(_dx12Device, paramDepth, 0, 0,
                                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
                DepthScaleDx11wDx12->Buffer() != nullptr)
            {
                DepthScaleDx11wDx12->SetBufferState(cmdList, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                if (DepthScaleDx11wDx12->Dispatch(cmdList, paramDepth, DepthScaleDx11wDx12->Buffer()))
                {
                    Dx12Resource setResource {};
                    setResource.type = FG_ResourceType::Depth;
                    setResource.cmdList = cmdList;
                    setResource.resource = DepthScaleDx11wDx12->Buffer();
                    setResource.width = feature->RenderWidth();
                    setResource.height = feature->RenderHeight();
                    setResource.state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    setResource.validity = FG_ResourceValidity::UntilPresent;

                    fg->SetResource(&setResource);
                    done = true;
                }
            }
        }

        if (!done)
        {
            Dx12Resource setResource {};
            setResource.type = FG_ResourceType::Depth;
            setResource.cmdList = cmdList;
            setResource.resource = paramDepth;
            setResource.width = feature->RenderWidth();
            setResource.height = feature->RenderHeight();
            setResource.state = (D3D12_RESOURCE_STATES) Config::Instance()->DepthResourceBarrier.value_or(
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            setResource.validity = FG_ResourceValidity::ValidNow;

            fg->SetResource(&setResource);
        }
    }

    LOG_DEBUG("(FG Dx11wDx12) D3D11 input preparation done, frame: {}", fg->FrameCount());
}

void UpscalerInputsDx11wDx12::UpscaleEnd(NVSDK_NGX_Parameter* InParameters, IFeature_Dx11* feature)
{
    Hudfix_Dx11::SetSkipStatus(false);

    if (InParameters == nullptr || feature == nullptr)
        return;

    auto fg = State::Instance().currentFG;

    if (fg == nullptr || State::Instance().activeFgInput != FGInput::Upscaler || _dx12Device == nullptr)
        return;

    if (fg->IsActive() && Config::Instance()->IsFGEnabled() && State::Instance().currentSwapchain != nullptr)
    {
        if (Config::Instance()->FGHUDFix.value_or_default())
        {
            Hudfix_Dx11::UpscaleEnd(feature->FrameCount(), State::Instance().lastFGFrameTime);

            ID3D11Resource* output = nullptr;
            if (InParameters->Get(NVSDK_NGX_Parameter_Output, &output) != NVSDK_NGX_Result_Success)
                InParameters->Get(NVSDK_NGX_Parameter_Output, (void**) &output);

            if (output != nullptr)
            {
                ID3D11Texture2D* outputTexture = nullptr;
                if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&outputTexture))) && outputTexture != nullptr)
                {
                    Dx11ResourceInfo info = {};
                    if (Hudfix_Dx11::FillResourceInfo(outputTexture, Dx11ResourceType::UAV, Dx11CaptureInfo::Upscaler,
                                                      &info))
                    {
                        Hudfix_Dx11::CheckForHudless(Dx11WithDx12::GetD3D11DeviceContext(), &info, true);
                    }

                    outputTexture->Release();
                }
            }
        }
        else
        {
            LOG_DEBUG("(FG Dx11wDx12) running, frame: {}", feature->FrameCount());
        }
    }
}
