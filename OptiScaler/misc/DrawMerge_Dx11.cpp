#include "pch.h"
#include "DrawMerge_Dx11.h"

#include <Config.h>
#include <State.h>
#include <proxies/Ntdll_Proxy.h>
#include <proxies/KernelBase_Proxy.h>

#include <detours/detours.h>

#include <d3d11.h>
#include <d3d11_4.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace
{
// ReShade 6.x: QueryInterface with this IID on one of its proxy objects returns the object it wraps
// (ReShade source/com_utils.hpp, IID_UnwrappedObject)
constexpr GUID IID_ReShadeUnwrappedObject = {
    0x7f2c9a11, 0x3b4e, 0x4d6a, { 0x81, 0x2f, 0x5e, 0x9c, 0xd3, 0x7a, 0x1b, 0x42 }
};

// NVAPI (nvapi.h, R615): interface ids for nvapi_QueryInterface and the functions used here
using NvStatus = int;
constexpr NvStatus kNvOk = 0;
constexpr NvStatus kNvDeviceNotRegistered = -202;

using PFN_nvapi_QueryInterface = void*(__cdecl*) (unsigned int);
using PFN_NvAPI_Initialize = NvStatus(__cdecl*)();
using PFN_NvAPI_D3D_RegisterDevice = NvStatus(__cdecl*)(IUnknown* device);
using PFN_NvAPI_D3D11_MultiDraw = NvStatus(__cdecl*)(ID3D11DeviceContext* context, uint32_t drawCount,
                                                     ID3D11Buffer* argsBuffer, uint32_t alignedByteOffsetForArgs,
                                                     uint32_t alignedByteStrideForArgs);

constexpr unsigned int kNvIdInitialize = 0x0150E828;
constexpr unsigned int kNvIdRegisterDevice = 0x8C02C4D0;
constexpr unsigned int kNvIdMultiDrawIndexedInstancedIndirect = 0x59E890F9;
constexpr unsigned int kNvIdMultiDrawInstancedIndirect = 0xD4E26BBF;
constexpr unsigned int kNvIdFakenvapiMarker = 0x21382138;

PFN_NvAPI_D3D_RegisterDevice g_nvRegisterDevice = nullptr;
PFN_NvAPI_D3D11_MultiDraw g_nvMultiDrawIndexed = nullptr;
PFN_NvAPI_D3D11_MultiDraw g_nvMultiDraw = nullptr;

// The args of a DrawIndexedInstancedIndirect are 5 UINTs, of a DrawInstancedIndirect 4 (NVAPI requires a stride of
// at least that much). Any constant stride is exact: each draw of a multi-draw reads offset + i * stride.
constexpr uint32_t kMinStrideIndexed = 5 * sizeof(uint32_t);
constexpr uint32_t kMinStride = 4 * sizeof(uint32_t);
constexpr uint32_t kMaxStride = 4096;
constexpr uint32_t kMaxRun = 1u << 20;

// 0: functions of the immediate context's vtable, 1: those of a deferred context where they differ
constexpr int kVariants = 2;

// Runs counted by their draw count: 1, 2-3, 4-7, 8-15, 16-63, 64 and more
constexpr int kRunBuckets = 6;

inline int RunBucket(uint32_t draws)
{
    if (draws <= 1)
        return 0;
    if (draws <= 3)
        return 1;
    if (draws <= 7)
        return 2;
    if (draws <= 15)
        return 3;
    if (draws <= 63)
        return 4;
    return 5;
}

// Counter written by one thread at a time and read approximately by the statistics: plain load and store,
// no locked instruction
using Counter = std::atomic<uint64_t>;

inline void Add(Counter& counter, uint64_t value = 1)
{
    counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
}

// Per context state. A D3D11 context is used by one thread at a time (the game may hand it over between
// threads, with its own synchronisation), so the pending run needs no lock.
struct alignas(64) Slot
{
    ID3D11Buffer* buffer = nullptr; // args buffer of the pending run, referenced while pending
    uint32_t offset = 0;
    uint32_t stride = 0;
    uint32_t count = 0; // draws held back, 0 = nothing pending
    bool indexed = false;

    bool uavBound = false;   // graphics UAVs bound (OMSetRenderTargetsAndUnorderedAccessViews)
    bool predicated = false; // SetPredication with a predicate
    uint8_t variant = 0;

    Counter seen { 0 };     // eligible indirect draws while merging is on
    Counter calls { 0 };    // calls issued for them
    Counter mdiCalls { 0 }; // NVAPI multi-draws
    Counter mdiDraws { 0 }; // draws inside them
    Counter blocked { 0 };  // draws passed straight through because of UAVs or a predicate
    Counter runDraws[kRunBuckets] {};
};

// Context pointer -> slot, open addressing, insert-only. Lookups are lock-free.
constexpr size_t kTableSize = 1024;
std::atomic<void*> g_keys[kTableSize] {};
Slot* g_slots[kTableSize] {};
std::mutex g_tableMutex;
size_t g_tableUsed = 0;
bool g_tableFullLogged = false;

thread_local void* t_cacheContext = nullptr;
thread_local Slot* t_cacheSlot = nullptr;
thread_local int t_inFlush = 0;

std::atomic<bool> g_installAttempted { false };
std::atomic<bool> g_installed { false };
std::atomic<bool> g_active { false };
std::atomic<bool> g_mdiUsable { false };
std::atomic<uint64_t> g_nvapiErrors { 0 };
bool g_reshadeLayer = false;
bool g_allowUavs = false;

// Vtable of the hooked layer's immediate context at install, to notice if it is ever replaced
void** g_immediateVtable = nullptr;
bool g_vtableChangeLogged = false;

inline size_t HashContext(const void* context)
{
    uint64_t x = reinterpret_cast<uintptr_t>(context);
    x ^= x >> 29;
    x *= 0x9E3779B97F4A7C15ull;
    return static_cast<size_t>(x >> 54); // top 10 bits, kTableSize = 1024
}

Slot* FindSlot(const void* context)
{
    size_t i = HashContext(context);

    for (size_t n = 0; n < kTableSize; ++n)
    {
        const void* key = g_keys[i].load(std::memory_order_acquire);

        if (key == context)
            return g_slots[i];

        if (key == nullptr)
            return nullptr;

        i = (i + 1) & (kTableSize - 1);
    }

    return nullptr;
}

Slot* GetOrCreateSlot(ID3D11DeviceContext* context, int variant)
{
    if (t_cacheContext == context)
        return t_cacheSlot;

    Slot* slot = FindSlot(context);

    if (slot == nullptr)
    {
        std::lock_guard<std::mutex> lock(g_tableMutex);

        slot = FindSlot(context);

        if (slot == nullptr)
        {
            if (g_tableUsed >= kTableSize * 3 / 4)
            {
                if (!g_tableFullLogged)
                {
                    g_tableFullLogged = true;
                    LOG_WARN("Draw merge: context table full ({} contexts), new contexts are not merged", g_tableUsed);
                }

                return nullptr;
            }

            size_t i = HashContext(context);
            while (g_keys[i].load(std::memory_order_relaxed) != nullptr)
                i = (i + 1) & (kTableSize - 1);

            slot = new Slot();
            slot->variant = static_cast<uint8_t>(variant);
            g_slots[i] = slot;
            g_keys[i].store(context, std::memory_order_release);
            ++g_tableUsed;
        }
    }

    t_cacheContext = context;
    t_cacheSlot = slot;
    return slot;
}

// Slot of a context, only if it already has one (contexts that never merged or tracked anything have none)
inline Slot* FindSlotCached(ID3D11DeviceContext* context)
{
    if (t_cacheContext == context)
        return t_cacheSlot;

    Slot* slot = FindSlot(context);

    if (slot != nullptr)
    {
        t_cacheContext = context;
        t_cacheSlot = slot;
    }

    return slot;
}

// ---------------------------------------------------------------------------------------------------------------
// Originals of the methods with their own handling

using PFN_DrawIndirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using PFN_SetPredication = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Predicate*, BOOL);
using PFN_OMSetRenderTargets = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
                                                        ID3D11DepthStencilView*);
using PFN_OMSetRenderTargetsAndUAVs = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT,
                                                               ID3D11RenderTargetView* const*, ID3D11DepthStencilView*,
                                                               UINT, UINT, ID3D11UnorderedAccessView* const*,
                                                               const UINT*);
using PFN_ExecuteCommandList = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using PFN_ClearState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using PFN_FinishCommandList = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL, ID3D11CommandList**);
using PFN_SwapDeviceContextState = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3DDeviceContextState*,
                                                            ID3DDeviceContextState**);

PFN_DrawIndirect o_DrawIndexedInstancedIndirect[kVariants] {};
PFN_DrawIndirect o_DrawInstancedIndirect[kVariants] {};
PFN_SetPredication o_SetPredication[kVariants] {};
PFN_OMSetRenderTargets o_OMSetRenderTargets[kVariants] {};
PFN_OMSetRenderTargetsAndUAVs o_OMSetRenderTargetsAndUnorderedAccessViews[kVariants] {};
PFN_ExecuteCommandList o_ExecuteCommandList[kVariants] {};
PFN_ClearState o_ClearState[kVariants] {};
PFN_FinishCommandList o_FinishCommandList[kVariants] {};
PFN_SwapDeviceContextState o_SwapDeviceContextState[kVariants] {};

inline PFN_DrawIndirect DrawOriginal(bool indexed, int variant)
{
    auto fn = indexed ? o_DrawIndexedInstancedIndirect[variant] : o_DrawInstancedIndirect[variant];

    if (fn == nullptr)
        fn = indexed ? o_DrawIndexedInstancedIndirect[0] : o_DrawInstancedIndirect[0];

    return fn;
}

void NoteNvapiError(NvStatus status)
{
    const auto errors = g_nvapiErrors.fetch_add(1) + 1;

    if (errors <= 4)
        LOG_WARN("Draw merge: NVAPI multi-draw failed with status {}", status);

    if (errors >= 32 && g_mdiUsable.exchange(false))
        LOG_ERROR("Draw merge: NVAPI multi-draw keeps failing, merging turned off");
}

void RegisterDeviceOf(ID3D11DeviceContext* nativeContext)
{
    ID3D11Device* device = nullptr;
    nativeContext->GetDevice(&device);

    if (device != nullptr)
    {
        const auto status = g_nvRegisterDevice(device);
        LOG_INFO("Draw merge: NvAPI_D3D_RegisterDevice for {:X}: {}", (size_t) device, status);
        device->Release();
    }
}

// Issues the pending run of a slot: one NVAPI multi-draw, or the original draw for a single one
void FlushSlot(Slot* slot, ID3D11DeviceContext* context)
{
    const uint32_t count = slot->count;
    ID3D11Buffer* buffer = slot->buffer;
    const uint32_t offset = slot->offset;
    const uint32_t stride = slot->stride;
    const bool indexed = slot->indexed;

    slot->count = 0;
    slot->buffer = nullptr;

    if (count == 0 || buffer == nullptr)
        return;

    ++t_inFlush;

    const auto original = DrawOriginal(indexed, slot->variant);
    bool issued = false;

    if (count > 1 && g_mdiUsable.load(std::memory_order_relaxed))
    {
        auto multiDraw = indexed ? g_nvMultiDrawIndexed : g_nvMultiDraw;
        ID3D11DeviceContext* nativeContext = context;
        bool unwrapped = false;

        if (g_reshadeLayer)
        {
            nativeContext = nullptr;
            unwrapped = SUCCEEDED(context->QueryInterface(IID_ReShadeUnwrappedObject,
                                                          reinterpret_cast<void**>(&nativeContext))) &&
                        nativeContext != nullptr;
        }

        if (multiDraw != nullptr && nativeContext != nullptr)
        {
            auto status = multiDraw(nativeContext, count, buffer, offset, stride);

            if (status == kNvDeviceNotRegistered)
            {
                RegisterDeviceOf(nativeContext);
                status = multiDraw(nativeContext, count, buffer, offset, stride);
            }

            if (status == kNvOk)
                issued = true;
            else
                NoteNvapiError(status);
        }

        if (unwrapped)
            nativeContext->Release();
    }

    if (issued)
    {
        Add(slot->calls);
        Add(slot->mdiCalls);
        Add(slot->mdiDraws, count);
    }
    else
    {
        for (uint32_t i = 0; i < count; ++i)
            original(context, buffer, offset + i * stride);

        Add(slot->calls, count);
    }

    Add(slot->runDraws[RunBucket(count)], count);

    --t_inFlush;

    buffer->Release();
}

// Every hooked method except the merged draws starts here: a run held back on this context is issued first
void FlushBeforeCall(ID3D11DeviceContext* context)
{
    if (t_inFlush != 0)
        return;

    Slot* slot = FindSlotCached(context);

    if (slot != nullptr && slot->count != 0)
        FlushSlot(slot, context);
}

// ---------------------------------------------------------------------------------------------------------------
// Hooks

// Generic: flush, then the original method. One instance per vtable index and variant.
template <int I, int V, typename T> struct Thunk;

template <int I, int V, typename C, typename R, typename... A> struct Thunk<I, V, R (C::*)(A...)>
{
    using Fn = R(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, A...);

    static inline Fn original = nullptr;

    static R STDMETHODCALLTYPE Hook(ID3D11DeviceContext* self, A... args)
    {
        FlushBeforeCall(self);
        return original(self, args...);
    }
};

template <int V, bool Indexed>
void STDMETHODCALLTYPE hkDrawIndirect(ID3D11DeviceContext* context, ID3D11Buffer* args, UINT offset)
{
    const auto original = Indexed ? o_DrawIndexedInstancedIndirect[V] : o_DrawInstancedIndirect[V];

    if (t_inFlush != 0)
    {
        original(context, args, offset);
        return;
    }

    if (!g_active.load(std::memory_order_relaxed) || args == nullptr)
    {
        FlushBeforeCall(context);
        original(context, args, offset);
        return;
    }

    Slot* slot = GetOrCreateSlot(context, V);

    if (slot == nullptr)
    {
        original(context, args, offset);
        return;
    }

    Add(slot->seen);

    if ((slot->uavBound && !g_allowUavs) || slot->predicated)
    {
        if (slot->count != 0)
            FlushSlot(slot, context);

        Add(slot->blocked);
        Add(slot->calls);
        Add(slot->runDraws[0]);
        original(context, args, offset);
        return;
    }

    if (slot->count != 0)
    {
        if (slot->indexed == Indexed && slot->buffer == args)
        {
            if (slot->count == 1)
            {
                constexpr uint32_t minStride = Indexed ? kMinStrideIndexed : kMinStride;

                if (offset > slot->offset)
                {
                    const uint32_t stride = offset - slot->offset;

                    if (stride >= minStride && stride <= kMaxStride && (stride & 3u) == 0)
                    {
                        slot->stride = stride;
                        slot->count = 2;
                        return;
                    }
                }
            }
            else if (slot->count < kMaxRun &&
                     static_cast<uint64_t>(slot->offset) + static_cast<uint64_t>(slot->count) * slot->stride == offset)
            {
                slot->count++;
                return;
            }
        }

        FlushSlot(slot, context);
    }

    // Start a new run with this draw, held back until the next call on this context
    args->AddRef();
    slot->buffer = args;
    slot->offset = offset;
    slot->stride = 0;
    slot->count = 1;
    slot->indexed = Indexed;
}

template <int V>
void STDMETHODCALLTYPE hkSetPredication(ID3D11DeviceContext* context, ID3D11Predicate* predicate, BOOL value)
{
    FlushBeforeCall(context);
    o_SetPredication[V](context, predicate, value);

    if (predicate != nullptr)
    {
        if (auto slot = GetOrCreateSlot(context, V))
            slot->predicated = true;
    }
    else if (auto slot = FindSlotCached(context))
    {
        slot->predicated = false;
    }
}

template <int V>
void STDMETHODCALLTYPE hkOMSetRenderTargets(ID3D11DeviceContext* context, UINT numViews,
                                            ID3D11RenderTargetView* const* renderTargets,
                                            ID3D11DepthStencilView* depthStencil)
{
    FlushBeforeCall(context);
    o_OMSetRenderTargets[V](context, numViews, renderTargets, depthStencil);

    // OMSetRenderTargets unbinds every UAV of the output merger
    if (auto slot = FindSlotCached(context))
        slot->uavBound = false;
}

template <int V>
void STDMETHODCALLTYPE hkOMSetRenderTargetsAndUnorderedAccessViews(
    ID3D11DeviceContext* context, UINT numRTVs, ID3D11RenderTargetView* const* renderTargets,
    ID3D11DepthStencilView* depthStencil, UINT uavStartSlot, UINT numUAVs,
    ID3D11UnorderedAccessView* const* unorderedAccessViews, const UINT* uavInitialCounts)
{
    FlushBeforeCall(context);
    o_OMSetRenderTargetsAndUnorderedAccessViews[V](context, numRTVs, renderTargets, depthStencil, uavStartSlot, numUAVs,
                                                   unorderedAccessViews, uavInitialCounts);

    if (numUAVs == D3D11_KEEP_UNORDERED_ACCESS_VIEWS)
        return;

    // The UAVs given replace the whole set, slots outside [start, start + count) are unbound
    bool anyUav = false;

    if (unorderedAccessViews != nullptr)
    {
        for (UINT i = 0; i < numUAVs && i < D3D11_1_UAV_SLOT_COUNT; ++i)
        {
            if (unorderedAccessViews[i] != nullptr)
            {
                anyUav = true;
                break;
            }
        }
    }

    if (anyUav)
    {
        if (auto slot = GetOrCreateSlot(context, V))
            slot->uavBound = true;
    }
    else if (auto slot = FindSlotCached(context))
    {
        slot->uavBound = false;
    }
}

template <int V>
void STDMETHODCALLTYPE hkExecuteCommandList(ID3D11DeviceContext* context, ID3D11CommandList* commandList,
                                            BOOL restoreContextState)
{
    FlushBeforeCall(context);
    o_ExecuteCommandList[V](context, commandList, restoreContextState);

    // Without restore, the context's state is cleared after the command list ran
    if (!restoreContextState)
    {
        if (auto slot = FindSlotCached(context))
        {
            slot->uavBound = false;
            slot->predicated = false;
        }
    }
}

template <int V> void STDMETHODCALLTYPE hkClearState(ID3D11DeviceContext* context)
{
    FlushBeforeCall(context);
    o_ClearState[V](context);

    if (auto slot = FindSlotCached(context))
    {
        slot->uavBound = false;
        slot->predicated = false;
    }
}

template <int V>
HRESULT STDMETHODCALLTYPE hkFinishCommandList(ID3D11DeviceContext* context, BOOL restoreDeferredContextState,
                                              ID3D11CommandList** commandList)
{
    FlushBeforeCall(context);
    const auto result = o_FinishCommandList[V](context, restoreDeferredContextState, commandList);

    // Without restore, the deferred context starts the next command list from the default state
    if (!restoreDeferredContextState)
    {
        if (auto slot = FindSlotCached(context))
        {
            slot->uavBound = false;
            slot->predicated = false;
        }
    }

    return result;
}

template <int V>
void STDMETHODCALLTYPE hkSwapDeviceContextState(ID3D11DeviceContext* context, ID3DDeviceContextState* state,
                                                ID3DDeviceContextState** previousState)
{
    FlushBeforeCall(context);
    o_SwapDeviceContextState[V](context, state, previousState);

    // The swapped-in state is unknown: don't merge until UAVs and predication are set again
    if (auto slot = GetOrCreateSlot(context, V))
    {
        slot->uavBound = true;
        slot->predicated = true;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Hook table: vtable index (d3d11*.idl order, after IUnknown and ID3D11DeviceChild), name, whether the method has
// its own handling, whether a failed hook disables merging. Getters (Get*, IsAnnotationEnabled) record nothing and
// are not hooked.

struct HookEntry
{
    int index;
    const char* name;
    bool special;
    bool critical;
    // A special that this context type never uses validly (FinishCommandList on the immediate context,
    // SwapDeviceContextState on deferred contexts): if its function is a stub shared with other methods, it is
    // left to the generic hook of those instead of failing
    int unusedOnVariant;
    void** original[kVariants];
    void* detour[kVariants];
};

#define DM_SPECIAL(I, NAME, HOOK, ORIGINAL, UNUSED_ON)                                                                 \
    HookEntry                                                                                                          \
    {                                                                                                                  \
        I, #NAME, true, true, UNUSED_ON,                                                                               \
            { reinterpret_cast<void**>(&ORIGINAL[0]), reinterpret_cast<void**>(&ORIGINAL[1]) },                        \
        {                                                                                                              \
            reinterpret_cast<void*>(&HOOK<0>), reinterpret_cast<void*>(&HOOK<1>)                                       \
        }                                                                                                              \
    }

#define DM_DRAW(I, NAME, INDEXED, ORIGINAL)                                                                            \
    HookEntry                                                                                                          \
    {                                                                                                                  \
        I, #NAME, true, true, -1, { reinterpret_cast<void**>(&ORIGINAL[0]), reinterpret_cast<void**>(&ORIGINAL[1]) },  \
        {                                                                                                              \
            reinterpret_cast<void*>(&hkDrawIndirect<0, INDEXED>), reinterpret_cast<void*>(&hkDrawIndirect<1, INDEXED>) \
        }                                                                                                              \
    }

#define DM_GENERIC(I, IFACE, NAME, CRITICAL)                                                                           \
    HookEntry                                                                                                          \
    {                                                                                                                  \
        I, #NAME, false, CRITICAL, -1,                                                                                 \
            { reinterpret_cast<void**>(&Thunk<I, 0, decltype(&IFACE::NAME)>::original),                                \
              reinterpret_cast<void**>(&Thunk<I, 1, decltype(&IFACE::NAME)>::original) },                              \
        {                                                                                                              \
            reinterpret_cast<void*>(&Thunk<I, 0, decltype(&IFACE::NAME)>::Hook),                                       \
                reinterpret_cast<void*>(&Thunk<I, 1, decltype(&IFACE::NAME)>::Hook)                                    \
        }                                                                                                              \
    }

const HookEntry kHooks[] = {
    // With their own handling first, so they own their functions
    DM_DRAW(39, DrawIndexedInstancedIndirect, true, o_DrawIndexedInstancedIndirect),
    DM_DRAW(40, DrawInstancedIndirect, false, o_DrawInstancedIndirect),
    DM_SPECIAL(30, SetPredication, hkSetPredication, o_SetPredication, -1),
    DM_SPECIAL(33, OMSetRenderTargets, hkOMSetRenderTargets, o_OMSetRenderTargets, -1),
    DM_SPECIAL(34, OMSetRenderTargetsAndUnorderedAccessViews, hkOMSetRenderTargetsAndUnorderedAccessViews,
               o_OMSetRenderTargetsAndUnorderedAccessViews, -1),
    DM_SPECIAL(58, ExecuteCommandList, hkExecuteCommandList, o_ExecuteCommandList, -1),
    DM_SPECIAL(110, ClearState, hkClearState, o_ClearState, -1),
    DM_SPECIAL(114, FinishCommandList, hkFinishCommandList, o_FinishCommandList, 0),
    DM_SPECIAL(131, SwapDeviceContextState, hkSwapDeviceContextState, o_SwapDeviceContextState, 1),

    // ID3D11DeviceContext
    DM_GENERIC(7, ID3D11DeviceContext, VSSetConstantBuffers, true),
    DM_GENERIC(8, ID3D11DeviceContext, PSSetShaderResources, true),
    DM_GENERIC(9, ID3D11DeviceContext, PSSetShader, true),
    DM_GENERIC(10, ID3D11DeviceContext, PSSetSamplers, true),
    DM_GENERIC(11, ID3D11DeviceContext, VSSetShader, true),
    DM_GENERIC(12, ID3D11DeviceContext, DrawIndexed, true),
    DM_GENERIC(13, ID3D11DeviceContext, Draw, true),
    DM_GENERIC(14, ID3D11DeviceContext, Map, true),
    DM_GENERIC(15, ID3D11DeviceContext, Unmap, true),
    DM_GENERIC(16, ID3D11DeviceContext, PSSetConstantBuffers, true),
    DM_GENERIC(17, ID3D11DeviceContext, IASetInputLayout, true),
    DM_GENERIC(18, ID3D11DeviceContext, IASetVertexBuffers, true),
    DM_GENERIC(19, ID3D11DeviceContext, IASetIndexBuffer, true),
    DM_GENERIC(20, ID3D11DeviceContext, DrawIndexedInstanced, true),
    DM_GENERIC(21, ID3D11DeviceContext, DrawInstanced, true),
    DM_GENERIC(22, ID3D11DeviceContext, GSSetConstantBuffers, true),
    DM_GENERIC(23, ID3D11DeviceContext, GSSetShader, true),
    DM_GENERIC(24, ID3D11DeviceContext, IASetPrimitiveTopology, true),
    DM_GENERIC(25, ID3D11DeviceContext, VSSetShaderResources, true),
    DM_GENERIC(26, ID3D11DeviceContext, VSSetSamplers, true),
    DM_GENERIC(27, ID3D11DeviceContext, Begin, true),
    DM_GENERIC(28, ID3D11DeviceContext, End, true),
    DM_GENERIC(31, ID3D11DeviceContext, GSSetShaderResources, true),
    DM_GENERIC(32, ID3D11DeviceContext, GSSetSamplers, true),
    DM_GENERIC(35, ID3D11DeviceContext, OMSetBlendState, true),
    DM_GENERIC(36, ID3D11DeviceContext, OMSetDepthStencilState, true),
    DM_GENERIC(37, ID3D11DeviceContext, SOSetTargets, true),
    DM_GENERIC(38, ID3D11DeviceContext, DrawAuto, true),
    DM_GENERIC(41, ID3D11DeviceContext, Dispatch, true),
    DM_GENERIC(42, ID3D11DeviceContext, DispatchIndirect, true),
    DM_GENERIC(43, ID3D11DeviceContext, RSSetState, true),
    DM_GENERIC(44, ID3D11DeviceContext, RSSetViewports, true),
    DM_GENERIC(45, ID3D11DeviceContext, RSSetScissorRects, true),
    DM_GENERIC(46, ID3D11DeviceContext, CopySubresourceRegion, true),
    DM_GENERIC(47, ID3D11DeviceContext, CopyResource, true),
    DM_GENERIC(48, ID3D11DeviceContext, UpdateSubresource, true),
    DM_GENERIC(49, ID3D11DeviceContext, CopyStructureCount, true),
    DM_GENERIC(50, ID3D11DeviceContext, ClearRenderTargetView, true),
    DM_GENERIC(51, ID3D11DeviceContext, ClearUnorderedAccessViewUint, true),
    DM_GENERIC(52, ID3D11DeviceContext, ClearUnorderedAccessViewFloat, true),
    DM_GENERIC(53, ID3D11DeviceContext, ClearDepthStencilView, true),
    DM_GENERIC(54, ID3D11DeviceContext, GenerateMips, true),
    DM_GENERIC(55, ID3D11DeviceContext, SetResourceMinLOD, true),
    DM_GENERIC(57, ID3D11DeviceContext, ResolveSubresource, true),
    DM_GENERIC(59, ID3D11DeviceContext, HSSetShaderResources, true),
    DM_GENERIC(60, ID3D11DeviceContext, HSSetShader, true),
    DM_GENERIC(61, ID3D11DeviceContext, HSSetSamplers, true),
    DM_GENERIC(62, ID3D11DeviceContext, HSSetConstantBuffers, true),
    DM_GENERIC(63, ID3D11DeviceContext, DSSetShaderResources, true),
    DM_GENERIC(64, ID3D11DeviceContext, DSSetShader, true),
    DM_GENERIC(65, ID3D11DeviceContext, DSSetSamplers, true),
    DM_GENERIC(66, ID3D11DeviceContext, DSSetConstantBuffers, true),
    DM_GENERIC(67, ID3D11DeviceContext, CSSetShaderResources, true),
    DM_GENERIC(68, ID3D11DeviceContext, CSSetUnorderedAccessViews, true),
    DM_GENERIC(69, ID3D11DeviceContext, CSSetShader, true),
    DM_GENERIC(70, ID3D11DeviceContext, CSSetSamplers, true),
    DM_GENERIC(71, ID3D11DeviceContext, CSSetConstantBuffers, true),
    DM_GENERIC(111, ID3D11DeviceContext, Flush, true),

    // ID3D11DeviceContext1
    DM_GENERIC(115, ID3D11DeviceContext1, CopySubresourceRegion1, true),
    DM_GENERIC(116, ID3D11DeviceContext1, UpdateSubresource1, true),
    DM_GENERIC(117, ID3D11DeviceContext1, DiscardResource, true),
    DM_GENERIC(118, ID3D11DeviceContext1, DiscardView, true),
    DM_GENERIC(119, ID3D11DeviceContext1, VSSetConstantBuffers1, true),
    DM_GENERIC(120, ID3D11DeviceContext1, HSSetConstantBuffers1, true),
    DM_GENERIC(121, ID3D11DeviceContext1, DSSetConstantBuffers1, true),
    DM_GENERIC(122, ID3D11DeviceContext1, GSSetConstantBuffers1, true),
    DM_GENERIC(123, ID3D11DeviceContext1, PSSetConstantBuffers1, true),
    DM_GENERIC(124, ID3D11DeviceContext1, CSSetConstantBuffers1, true),
    DM_GENERIC(132, ID3D11DeviceContext1, ClearView, true),
    DM_GENERIC(133, ID3D11DeviceContext1, DiscardView1, true),

    // ID3D11DeviceContext2
    DM_GENERIC(134, ID3D11DeviceContext2, UpdateTileMappings, true),
    DM_GENERIC(135, ID3D11DeviceContext2, CopyTileMappings, true),
    DM_GENERIC(136, ID3D11DeviceContext2, CopyTiles, true),
    DM_GENERIC(137, ID3D11DeviceContext2, UpdateTiles, true),
    DM_GENERIC(138, ID3D11DeviceContext2, ResizeTilePool, true),
    DM_GENERIC(139, ID3D11DeviceContext2, TiledResourceBarrier, true),
    // Debug markers: their order relative to merged draws does not change rendering
    DM_GENERIC(141, ID3D11DeviceContext2, SetMarkerInt, false),
    DM_GENERIC(142, ID3D11DeviceContext2, BeginEventInt, false),
    DM_GENERIC(143, ID3D11DeviceContext2, EndEvent, false),

    // ID3D11DeviceContext3
    DM_GENERIC(144, ID3D11DeviceContext3, Flush1, false),
    DM_GENERIC(145, ID3D11DeviceContext3, SetHardwareProtectionState, false),

    // ID3D11DeviceContext4
    DM_GENERIC(147, ID3D11DeviceContext4, Signal, true),
    DM_GENERIC(148, ID3D11DeviceContext4, Wait, true),
};

#undef DM_SPECIAL
#undef DM_DRAW
#undef DM_GENERIC

struct InstalledHook
{
    void** original;
    void* detour;
};

std::vector<InstalledHook> g_installedHooks;

void DetachAll()
{
    for (auto it = g_installedHooks.rbegin(); it != g_installedHooks.rend(); ++it)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(it->original, it->detour);
        DetourTransactionCommit();
    }

    g_installedHooks.clear();
}

// A function that only returns (a no-op or "not supported" stub, e.g. on deferred contexts) records nothing, so a
// failed hook on it is harmless. Detours can't patch such stubs when there is no padding after them.
bool IsReturnOnlyStub(const void* address)
{
    const auto* code = static_cast<const uint8_t*>(address);

    if (code[0] == 0xC3 || code[0] == 0xC2) // ret, ret imm16
        return true;

    if ((code[0] == 0x33 || code[0] == 0x31) && code[1] == 0xC0 && code[2] == 0xC3) // xor eax, eax; ret
        return true;

    if (code[0] == 0xB8 && code[5] == 0xC3) // mov eax, imm32; ret
        return true;

    return false;
}

std::string ModuleOf(const void* address)
{
    HMODULE module = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module) ||
        module == nullptr)
    {
        return "?";
    }

    char path[MAX_PATH] = {};
    GetModuleFileNameA(module, path, MAX_PATH);

    std::string name = path;
    const auto slash = name.find_last_of("\\/");
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

// Number of vtable entries of a context, from the highest interface it implements on the same object.
// 0 if a newer interface is a separate pointer (its methods would bypass the hooks of the main vtable).
size_t VtableLength(ID3D11DeviceContext* context)
{
    const struct
    {
        const IID& iid;
        size_t length;
    } levels[] = {
        { __uuidof(ID3D11DeviceContext4), 149 },
        { __uuidof(ID3D11DeviceContext3), 147 },
        { __uuidof(ID3D11DeviceContext2), 144 },
        { __uuidof(ID3D11DeviceContext1), 134 },
    };

    size_t length = 0;

    for (const auto& level : levels)
    {
        IUnknown* other = nullptr;

        if (FAILED(context->QueryInterface(level.iid, reinterpret_cast<void**>(&other))) || other == nullptr)
            continue;

        const bool sameObject = static_cast<void*>(other) == static_cast<void*>(context);
        other->Release();

        if (!sameObject)
        {
            LOG_ERROR("Draw merge: the context's interface with {} methods is a separate pointer, not merging",
                      level.length);
            return 0;
        }

        if (length == 0)
            length = level.length;
    }

    return length != 0 ? length : 115;
}

// Hooks one vtable. hooked: function address -> vtable index, across both variants.
bool HookVtable(void** vtable, int variant, size_t length, std::vector<std::pair<void*, int>>& hooked, int& hookedCount,
                int& sharedCount)
{
    auto findHooked = [&](void* address) -> const std::pair<void*, int>*
    {
        for (const auto& entry : hooked)
        {
            if (entry.first == address)
                return &entry;
        }

        return nullptr;
    };

    // A method with its own handling must not share its function with any other hooked method of this vtable
    std::vector<const HookEntry*> skipped;

    for (const auto& special : kHooks)
    {
        if (!special.special || static_cast<size_t>(special.index) >= length)
            continue;

        for (const auto& other : kHooks)
        {
            if (&other == &special || static_cast<size_t>(other.index) >= length)
                continue;

            if (vtable[special.index] == vtable[other.index])
            {
                if (special.unusedOnVariant == variant)
                {
                    LOG_INFO("Draw merge: {} shares a stub with {} (variant {}), left to the generic hook",
                             special.name, other.name, variant);
                    skipped.push_back(&special);
                    break;
                }

                LOG_ERROR("Draw merge: {} and {} share one function (variant {}), not merging", special.name,
                          other.name, variant);
                return false;
            }
        }
    }

    // Functions to hook in this vtable; a function shared by several methods is hooked once
    struct Candidate
    {
        const HookEntry* entry;
        void* address;
        bool excluded;
    };

    std::vector<Candidate> candidates;

    for (const auto& entry : kHooks)
    {
        if (static_cast<size_t>(entry.index) >= length)
            continue;

        if (std::find(skipped.begin(), skipped.end(), &entry) != skipped.end())
            continue;

        void* address = vtable[entry.index];

        if (address == nullptr)
            continue;

        if (const auto existing = findHooked(address))
        {
            if (existing->second == entry.index)
            {
                // Same function as the immediate context's: its hook already handles this variant too
                if (entry.special)
                    *entry.original[variant] = *entry.original[0];

                ++sharedCount;
                continue;
            }

            if (entry.special)
            {
                LOG_ERROR("Draw merge: {} (variant {}) shares its function with another method, not merging",
                          entry.name, variant);
                return false;
            }

            ++sharedCount;
            continue;
        }

        candidates.push_back({ &entry, address, false });
        hooked.push_back({ address, entry.index });
    }

    // All functions of the vtable in one Detours transaction, so the code is patched in one go. A function Detours
    // refuses is left out and the transaction retried, if that is harmless.
    for (int attempt = 0; attempt < 16; ++attempt)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        Candidate* failed = nullptr;

        for (auto& candidate : candidates)
        {
            if (candidate.excluded)
                continue;

            *candidate.entry->original[variant] = candidate.address;

            if (DetourAttach(candidate.entry->original[variant], candidate.entry->detour[variant]) != NO_ERROR)
            {
                failed = &candidate;
                break;
            }
        }

        if (failed != nullptr)
        {
            DetourTransactionAbort();

            for (auto& candidate : candidates)
                *candidate.entry->original[variant] = nullptr;

            const auto entry = failed->entry;

            if (entry->critical && !(IsReturnOnlyStub(failed->address) && !entry->special))
            {
                LOG_ERROR("Draw merge: hooking {} (variant {}) at {:X} failed, not merging", entry->name, variant,
                          (size_t) failed->address);
                return false;
            }

            LOG_WARN("Draw merge: hooking {} (variant {}) failed, continuing without it", entry->name, variant);
            failed->excluded = true;
            continue;
        }

        const auto commitResult = DetourTransactionCommit();

        if (commitResult != NO_ERROR)
        {
            for (auto& candidate : candidates)
                *candidate.entry->original[variant] = nullptr;

            LOG_ERROR("Draw merge: Detours commit failed ({}) for variant {}, not merging", commitResult, variant);
            return false;
        }

        for (const auto& candidate : candidates)
        {
            if (candidate.excluded)
                continue;

            g_installedHooks.push_back({ candidate.entry->original[variant], candidate.entry->detour[variant] });
            ++hookedCount;
        }

        return true;
    }

    LOG_ERROR("Draw merge: too many functions refused by Detours (variant {}), not merging", variant);
    return false;
}

bool InitNvApi(ID3D11Device* nativeDevice)
{
    auto nvapi = NtdllProxy::LoadLibraryExW_Ldr(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);

    if (nvapi == nullptr)
    {
        LOG_INFO("Draw merge: nvapi64.dll not found, not merging");
        return false;
    }

    auto queryInterface =
        reinterpret_cast<PFN_nvapi_QueryInterface>(KernelBaseProxy::GetProcAddress_()(nvapi, "nvapi_QueryInterface"));

    if (queryInterface == nullptr)
    {
        LOG_WARN("Draw merge: nvapi_QueryInterface not found, not merging");
        return false;
    }

    if (queryInterface(kNvIdFakenvapiMarker) != nullptr)
    {
        LOG_INFO("Draw merge: system nvapi64.dll is fakenvapi, not merging");
        return false;
    }

    auto initialize = reinterpret_cast<PFN_NvAPI_Initialize>(queryInterface(kNvIdInitialize));
    g_nvRegisterDevice = reinterpret_cast<PFN_NvAPI_D3D_RegisterDevice>(queryInterface(kNvIdRegisterDevice));
    g_nvMultiDrawIndexed =
        reinterpret_cast<PFN_NvAPI_D3D11_MultiDraw>(queryInterface(kNvIdMultiDrawIndexedInstancedIndirect));
    g_nvMultiDraw = reinterpret_cast<PFN_NvAPI_D3D11_MultiDraw>(queryInterface(kNvIdMultiDrawInstancedIndirect));

    if (initialize == nullptr || g_nvRegisterDevice == nullptr || g_nvMultiDrawIndexed == nullptr)
    {
        LOG_WARN("Draw merge: NVAPI multi-draw functions not available (init {:X}, register {:X}, indexed {:X})",
                 (size_t) initialize, (size_t) g_nvRegisterDevice, (size_t) g_nvMultiDrawIndexed);
        return false;
    }

    auto status = initialize();

    if (status != kNvOk)
    {
        LOG_WARN("Draw merge: NvAPI_Initialize failed with {}, not merging", status);
        return false;
    }

    status = g_nvRegisterDevice(nativeDevice);
    LOG_INFO("Draw merge: NvAPI_D3D_RegisterDevice for {:X}: {}", (size_t) nativeDevice, status);

    if (status != kNvOk)
        return false;

    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Statistics

struct Totals
{
    uint64_t seen = 0;
    uint64_t calls = 0;
    uint64_t mdiCalls = 0;
    uint64_t mdiDraws = 0;
    uint64_t blocked = 0;
    uint64_t runDraws[kRunBuckets] = {};
};

Totals SumSlots()
{
    Totals totals;

    for (size_t i = 0; i < kTableSize; ++i)
    {
        if (g_keys[i].load(std::memory_order_acquire) == nullptr)
            continue;

        const Slot* slot = g_slots[i];
        totals.seen += slot->seen.load(std::memory_order_relaxed);
        totals.calls += slot->calls.load(std::memory_order_relaxed);
        totals.mdiCalls += slot->mdiCalls.load(std::memory_order_relaxed);
        totals.mdiDraws += slot->mdiDraws.load(std::memory_order_relaxed);
        totals.blocked += slot->blocked.load(std::memory_order_relaxed);

        for (int b = 0; b < kRunBuckets; ++b)
            totals.runDraws[b] += slot->runDraws[b].load(std::memory_order_relaxed);
    }

    return totals;
}

std::mutex g_statsMutex;
DrawMergeDx11::Stats g_lastStats;
Totals g_previousTotals;
uint64_t g_intervalFrames = 0;
std::chrono::steady_clock::time_point g_intervalStart {};
int g_intervalsSinceLog = 0;
bool g_firstIntervalLogged = false;

void UpdateStats()
{
    ++g_intervalFrames;

    const auto now = std::chrono::steady_clock::now();

    if (g_intervalStart == std::chrono::steady_clock::time_point {})
    {
        g_intervalStart = now;
        g_previousTotals = SumSlots();
        g_intervalFrames = 0;
        return;
    }

    if (now - g_intervalStart < std::chrono::seconds(1) || g_intervalFrames == 0)
        return;

    const Totals totals = SumSlots();
    const double frames = static_cast<double>(g_intervalFrames);

    Totals delta;
    delta.seen = totals.seen - g_previousTotals.seen;
    delta.calls = totals.calls - g_previousTotals.calls;
    delta.mdiCalls = totals.mdiCalls - g_previousTotals.mdiCalls;
    delta.mdiDraws = totals.mdiDraws - g_previousTotals.mdiDraws;
    delta.blocked = totals.blocked - g_previousTotals.blocked;

    uint64_t runTotal = 0;
    for (int b = 0; b < kRunBuckets; ++b)
    {
        delta.runDraws[b] = totals.runDraws[b] - g_previousTotals.runDraws[b];
        runTotal += delta.runDraws[b];
    }

    DrawMergeDx11::Stats stats;
    stats.active = g_active.load(std::memory_order_relaxed);
    stats.reshadeLayer = g_reshadeLayer;
    stats.drawsPerFrame = delta.seen / frames;
    stats.callsPerFrame = delta.calls / frames;
    stats.mergedShare = delta.seen > 0 ? static_cast<double>(delta.mdiDraws) / static_cast<double>(delta.seen) : 0.0;
    stats.averageRun = delta.mdiCalls > 0 ? static_cast<double>(delta.mdiDraws) / delta.mdiCalls : 0.0;
    stats.blockedPerFrame = delta.blocked / frames;
    stats.nvapiErrors = g_nvapiErrors.load(std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        g_lastStats = stats;
    }

    const int logInterval = Config::Instance()->DrawMergeStatsInterval.value_or_default();

    if (logInterval > 0 && (++g_intervalsSinceLog >= logInterval || !g_firstIntervalLogged) && delta.seen > 0)
    {
        g_intervalsSinceLog = 0;
        g_firstIntervalLogged = true;

        auto share = [&](int bucket)
        { return runTotal > 0 ? 100.0 * static_cast<double>(delta.runDraws[bucket]) / runTotal : 0.0; };

        LOG_INFO("Draw merge: {:.0f} indirect draws/frame -> {:.0f} calls ({:.1f}% in multi-draws, average run "
                 "{:.1f}), {:.0f} not merged because of UAVs/predicate; draws by run length 1: {:.0f}%, 2-3: "
                 "{:.0f}%, 4-7: {:.0f}%, 8-15: {:.0f}%, 16-63: {:.0f}%, 64+: {:.0f}%; NVAPI errors {}; {} fps",
                 stats.drawsPerFrame, stats.callsPerFrame, 100.0 * stats.mergedShare, stats.averageRun,
                 stats.blockedPerFrame, share(0), share(1), share(2), share(3), share(4), share(5), stats.nvapiErrors,
                 static_cast<int>(frames / std::chrono::duration<double>(now - g_intervalStart).count() + 0.5));
    }

    g_previousTotals = totals;
    g_intervalFrames = 0;
    g_intervalStart = now;
}
} // namespace

bool DrawMergeDx11::IsEnabledByConfig()
{
    return Config::Instance()->DrawMergeEnabled.value_or(
        static_cast<bool>(State::Instance().gameQuirks & GameQuirk::MergeIndirectDraws));
}

bool DrawMergeDx11::IsInstalled() { return g_installed.load(); }

void DrawMergeDx11::Install(ID3D11Device* device)
{
    if (device == nullptr || !IsEnabledByConfig())
        return;

    bool expected = false;
    if (!g_installAttempted.compare_exchange_strong(expected, true))
        return;

    // Behind ReShade the game's device is ReShade's proxy; NVAPI needs the real device
    ID3D11Device* nativeDevice = nullptr;
    const bool behindReShade =
        SUCCEEDED(device->QueryInterface(IID_ReShadeUnwrappedObject, reinterpret_cast<void**>(&nativeDevice))) &&
        nativeDevice != nullptr;

    if (!behindReShade)
    {
        nativeDevice = device;
        nativeDevice->AddRef();
    }

    g_reshadeLayer = behindReShade && Config::Instance()->DrawMergeAtReShadeLayer.value_or_default();
    g_allowUavs = Config::Instance()->DrawMergeAllowUAVs.value_or_default();

    LOG_INFO("Draw merge: device {:X}, native {:X}, behind ReShade: {}, hook layer: {}, merge with UAVs: {}",
             (size_t) device, (size_t) nativeDevice, behindReShade, g_reshadeLayer ? "ReShade proxy" : "D3D11 runtime",
             g_allowUavs);

    if (!InitNvApi(nativeDevice))
    {
        nativeDevice->Release();
        return;
    }

    ID3D11Device* layerDevice = g_reshadeLayer ? device : nativeDevice;

    ID3D11DeviceContext* immediate = nullptr;
    layerDevice->GetImmediateContext(&immediate);

    ID3D11DeviceContext* deferred = nullptr;
    const auto deferredResult = layerDevice->CreateDeferredContext(0, &deferred);

    if (immediate == nullptr)
    {
        LOG_ERROR("Draw merge: no immediate context, not merging");

        if (deferred != nullptr)
            deferred->Release();

        nativeDevice->Release();
        return;
    }

    if (FAILED(deferredResult) || deferred == nullptr)
        LOG_WARN("Draw merge: CreateDeferredContext failed ({:X}), hooking the immediate context's functions only",
                 (UINT) deferredResult);

    const size_t length = VtableLength(immediate);

    if (length == 0)
    {
        immediate->Release();

        if (deferred != nullptr)
            deferred->Release();

        nativeDevice->Release();
        return;
    }

    void** immediateVtable = *reinterpret_cast<void***>(immediate);
    void** deferredVtable = deferred != nullptr ? *reinterpret_cast<void***>(deferred) : nullptr;

    LOG_INFO("Draw merge: vtable length {}, immediate DrawIndexedInstancedIndirect {:X} in {}, deferred {:X} in {}",
             length, (size_t) immediateVtable[39], ModuleOf(immediateVtable[39]),
             deferredVtable != nullptr ? (size_t) deferredVtable[39] : 0,
             deferredVtable != nullptr ? ModuleOf(deferredVtable[39]) : "-");

    std::vector<std::pair<void*, int>> hooked;
    int hookedCount = 0;
    int sharedCount = 0;

    bool ok = HookVtable(immediateVtable, 0, length, hooked, hookedCount, sharedCount);

    if (ok && deferredVtable != nullptr && deferredVtable != immediateVtable)
        ok = HookVtable(deferredVtable, 1, length, hooked, hookedCount, sharedCount);

    // Specials of variant 1 where the deferred context has no separate vtable
    for (const auto& entry : kHooks)
    {
        if (entry.special && *entry.original[1] == nullptr)
            *entry.original[1] = *entry.original[0];
    }

    if (!ok)
    {
        DetachAll();
        LOG_ERROR("Draw merge: hooks removed, not merging");
    }
    else
    {
        g_immediateVtable = immediateVtable;
        g_mdiUsable.store(true);
        g_active.store(true);
        g_installed.store(true);

        LOG_INFO("Draw merge: installed, {} functions hooked, {} shared between methods or contexts", hookedCount,
                 sharedCount);
    }

    immediate->Release();

    if (deferred != nullptr)
        deferred->Release();

    nativeDevice->Release();
}

void DrawMergeDx11::OnPresent(ID3D11DeviceContext* immediateContext)
{
    if (!g_installed.load(std::memory_order_relaxed))
        return;

    g_active.store(IsEnabledByConfig() && g_mdiUsable.load(std::memory_order_relaxed), std::memory_order_relaxed);

    if (immediateContext != nullptr)
    {
        // The hooks sit on the native runtime unless the ReShade layer was chosen
        ID3D11DeviceContext* layerContext = immediateContext;
        bool unwrapped = false;

        if (!g_reshadeLayer)
        {
            ID3D11DeviceContext* nativeContext = nullptr;

            if (SUCCEEDED(immediateContext->QueryInterface(IID_ReShadeUnwrappedObject,
                                                           reinterpret_cast<void**>(&nativeContext))) &&
                nativeContext != nullptr)
            {
                layerContext = nativeContext;
                unwrapped = true;
            }
        }

        // A run still held back on the immediate context belongs to this frame
        FlushBeforeCall(layerContext);

        // If something replaced the context's vtable, the hooks no longer see its calls
        if (*reinterpret_cast<void***>(layerContext) != g_immediateVtable && !g_vtableChangeLogged)
        {
            g_vtableChangeLogged = true;
            LOG_WARN("Draw merge: the immediate context's vtable changed since install ({:X} -> {:X}), "
                     "its calls may no longer be merged",
                     (size_t) g_immediateVtable, (size_t) * reinterpret_cast<void***>(layerContext));
        }

        if (unwrapped)
            layerContext->Release();
    }

    UpdateStats();
}

DrawMergeDx11::Stats DrawMergeDx11::GetStats()
{
    std::lock_guard<std::mutex> lock(g_statsMutex);
    return g_lastStats;
}
