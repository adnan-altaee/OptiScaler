#pragma once

#include "SysUtils.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <shaders/Shader_Dx12Utils.h>
#include <shaders/Shader_Dx12.h>

#define DL_NUM_OF_HEAPS 2

// Turns linear view depth (distance along the view axis, in world units) into reversed-Z hardware depth for a
// projection with the given near and far planes (near -> 1, far -> 0). Frame generation (DLSS-G) expects hardware
// depth; some upscaler inputs hand over linear depth instead (Assassin's Creed Origins through its DLSS mod).
class DL_Dx12 : public Shader_Dx12
{
  private:
    FrameDescriptorHeap _frameHeaps[DL_NUM_OF_HEAPS];

    ID3D12Resource* _buffer = nullptr;
    D3D12_RESOURCE_STATES _bufferState = D3D12_RESOURCE_STATE_COMMON;

    uint32_t InNumThreadsX = 16;
    uint32_t InNumThreadsY = 16;

  public:
    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, D3D12_RESOURCE_STATES InState);
    void SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState);
    bool Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, ID3D12Resource* OutResource,
                  float nearPlane, float farPlane);

    ID3D12Resource* Buffer() { return _buffer; }

    DL_Dx12(std::string InName, ID3D12Device* InDevice);

    ~DL_Dx12();
};
