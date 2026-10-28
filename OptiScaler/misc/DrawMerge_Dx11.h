#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

// Merges a DX11 game's consecutive indirect draws into NVAPI multi-draw calls.
//
// Assassin's Creed Origins (AnvilNext) is GPU-driven: it issues 45,000-60,000 DrawIndexedInstancedIndirect calls a
// frame in towns (up to 130,000), one per culled batch, on deferred contexts, and never uses the multi-draw
// extensions of NVAPI or AGS. Every call pays the D3D11 runtime and driver (and, above them, ReShade and its
// add-ons), and frame time rises with the draw count. DXVK showed the fix in 2019: it merges consecutive indirect
// draws into one multi-draw when nothing happens in between, they read the same argument buffer and the offset steps
// by a fixed stride (AC Odyssey, up to 20% faster when CPU-bound).
//
// Here the same rule runs on D3D11 with NvAPI_D3D11_MultiDrawIndexedInstancedIndirect, which NVIDIA documents for
// immediate and deferred contexts once NvAPI_D3D_RegisterDevice was called:
//  - Every ID3D11DeviceContext method that records something is detoured. An eligible indirect draw is held back;
//    the next draw extends the run while it continues it; any other call first issues the run (one NVAPI multi-draw,
//    or the plain draw when the run is a single draw) and then proceeds. Runs never cross FinishCommandList,
//    ExecuteCommandList, a state change, a query, a map or a copy.
//  - Not merged while graphics UAVs are bound (D3D11 orders UAV writes between draws, NVIDIA does not document
//    whether a multi-draw does) unless Config DrawMergeAllowUAVs, or while a predicate is set.
//  - Layer: by default the native D3D11 runtime functions (below ReShade), so ReShade and every add-on still see each
//    draw and only the runtime/driver cost is saved. With DrawMergeAtReShadeLayer the hooks sit on ReShade's proxy
//    context instead: merged runs then also skip ReShade's event dispatch and every add-on callback, but add-ons no
//    longer see the draws inside a merged run.
namespace DrawMergeDx11
{
// Config DrawMergeEnabled, by default on for games with the MergeIndirectDraws quirk
bool IsEnabledByConfig();

// At D3D11 device creation: registers the device with NVAPI before the game creates its deferred contexts
void OnDeviceCreated(ID3D11Device* device);

// Installs the hooks once, for the game's device (ReShade's proxy device is unwrapped when present)
void Install(ID3D11Device* device);

bool IsInstalled();

// Call at the start of the game's present, on the thread that uses the immediate context (the game's, possibly
// ReShade's proxy): issues a run still held back there, applies the runtime on/off setting and logs statistics
void OnPresent(ID3D11DeviceContext* immediateContext);

struct Stats
{
    int mode = 0;        // Config DrawMergeMode in use (0 off, 1 hold back, 2/3 NVAPI multi-draw, 4 runs past ReShade)
    bool active = false; // merging currently on
    bool reshadeLayer = false;     // hooks on ReShade's proxy context instead of the native runtime
    double drawsPerFrame = 0.0;    // eligible indirect draws seen
    double callsPerFrame = 0.0;    // calls issued for them (multi-draws + single draws)
    double mergedShare = 0.0;      // share of draws that went out in a multi-draw
    double averageRun = 0.0;       // draws per multi-draw
    double blockedPerFrame = 0.0;  // draws not merged because UAVs or a predicate were bound
    double bypassedPerFrame = 0.0; // draws of runs sent past ReShade (mode 4)
    uint64_t nvapiErrors = 0;
    uint64_t listFailures = 0; // FinishCommandList failures seen
    uint64_t mdiImmediate = 0; // multi-draws issued so far on the immediate context
    uint64_t mdiDeferred = 0;  // and on deferred contexts
};

// Statistics of the last completed interval (about one second)
Stats GetStats();
} // namespace DrawMergeDx11
