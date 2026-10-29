#include "pch.h"
#include "DL_Dx12.h"

#include <State.h>

namespace
{
struct alignas(256) DLConstants
{
    float NearPlane;
    float FarPlane;
};

// Compiled at runtime (no precompiled variant)
const char* dlShaderCode = R"(
cbuffer Params : register(b0)
{
    float NearPlane;
    float FarPlane;
};

Texture2D<float> SourceTexture : register(t0);
RWTexture2D<float> DestinationTexture : register(u0);

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint width, height;
    SourceTexture.GetDimensions(width, height);
    if (id.x >= width || id.y >= height)
        return;

    // Linear view depth -> reversed-Z hardware depth of a projection with these planes
    float z = clamp(SourceTexture.Load(int3(id.xy, 0)), NearPlane, FarPlane);
    DestinationTexture[id.xy] = saturate(NearPlane * (FarPlane - z) / (z * (FarPlane - NearPlane)));
}
)";
} // namespace

bool DL_Dx12::CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, D3D12_RESOURCE_STATES InState)
{
    auto resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    auto result = Shader_Dx12::CreateBufferResource(InDevice, InSource, InState, &_buffer, resourceFlags, 0, 0,
                                                    DXGI_FORMAT_R32_FLOAT);

    if (result)
    {
        _buffer->SetName(L"Hardware_Depth_Buffer");
        _bufferState = InState;
    }

    return result;
}

void DL_Dx12::SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState)
{
    return Shader_Dx12::SetBufferState(InCommandList, InState, _buffer, &_bufferState);
}

bool DL_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource,
                       float nearPlane, float farPlane)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || InResource == nullptr || OutResource == nullptr ||
        !(nearPlane > 0.0f) || !(farPlane > nearPlane))
    {
        return false;
    }

    _counter++;
    _counter = _counter % DL_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = _frameHeaps[_counter];

    CreateShaderResourceView(_device, InResource, currentHeap.GetSrvCPU(0));
    CreateUnorderedAccessView(_device, OutResource, currentHeap.GetUavCPU(0), 0);

    DLConstants constants {};
    constants.NearPlane = nearPlane;
    constants.FarPlane = farPlane;

    if (!CreateConstantsBuffer(_device, _constantBuffer, constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);

    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    const auto desc = InResource->GetDesc();
    InCmdList->Dispatch(static_cast<UINT>((desc.Width + InNumThreadsX - 1) / InNumThreadsX),
                        (desc.Height + InNumThreadsY - 1) / InNumThreadsY, 1);

    return true;
}

DL_Dx12::DL_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    if (!SetupRootSignature(InDevice, 1, 1, 1))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DLConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto result =
        InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&_constantBuffer));

    if (result != S_OK)
    {
        LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
        return;
    }

    ID3DBlob* blob = CompileShader(dlShaderCode, "CSMain", "cs_5_0");
    if (blob == nullptr)
    {
        LOG_ERROR("[{0}] Shader compilation failed", _name);
        return;
    }

    const bool created = CreateComputeShader(InDevice, _rootSignature, &_pipelineState, blob, {});
    blob->Release();

    if (!created)
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, DL_NUM_OF_HEAPS);
}

DL_Dx12::~DL_Dx12()
{
    if (State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < DL_NUM_OF_HEAPS; i++)
        _frameHeaps[i].ReleaseHeaps();

    DeferredRelease::Release(_buffer);
}
