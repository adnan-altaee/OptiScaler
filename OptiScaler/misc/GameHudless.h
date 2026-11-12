#pragma once

#include <d3d11.h>
#include <cstdint>

struct ID3D12Resource;

// HUD-less image for frame generation in AnvilNext games (Assassin's Creed Origins, Dx11withDx12 with ReShade).
//
// The game renders the finished scene and its HUD into two separate textures and combines them onto the back
// buffer in one full-screen draw ("UICompositingParameters", ps 0e86ea53 in SDR):
//     back buffer = (scene * (1 - hud.a) + hud) ^ Brightness      hud at ps t86, scene at ps t87
// That draw is replayed once more, into a texture of our own, with t86 unbound (D3D11 reads 0 from an unbound
// slot). The result is exactly what the back buffer would hold without the HUD, brightness included. It works the
// same when the draw is recorded on a deferred context, where OptiScaler's HUDFix can't capture.
//
// Frame generation (DLSS-G, or a custom DLSSG engine such as ANR FG) then interpolates the HUD-less image and lays
// the real frame's HUD (and ReShade's overlay) back on top instead of warping it with the scene.
namespace GameHudless
{
// Config Dx11wDx12HudlessFromUI, by default on for games with the AnvilUICompositeHudless quirk
bool IsEnabled();

// Registers ReShade's draw event (OptiScaler is registered as a ReShade add-on by the Dx11wDx12 swapchain)
void RegisterReShadeEvents(void* registerEventFunction);

// Called by the Dx11wDx12 swapchain once per present, before it reads the capture: the back buffer the
// composite has to match, and whether frame generation is running (the draw hook does nothing otherwise)
void SetTarget(bool active, UINT width, UINT height, DXGI_FORMAT format);

// The HUD-less image of the frame being presented (AddRef'd), or nullptr when the game's composite draw was not
// seen since the previous present (menus without it, loading screens, a format the FG side can't take)
ID3D11Texture2D* TakeCapture();

// Releases the capture texture (swapchain resize/destroy). The next composite recreates it.
void Release();

// The D3D12 side of the HUD-less image of the frame being presented (the Dx11wDx12 swapchain's shared copy, in
// D3D12_RESOURCE_STATE_COMMON), or nullptr. Set by the swapchain every present, cleared before it is released.
// Streamline's DLSS-G does not always pass the tagged HUD-less color on to the nvngx_dlssg it drives, so the NGX
// replacement path (framegen/nvngx/Nvngx_FG.cpp) hands this one to custom engines itself when it is missing.
void SetDx12Hudless(ID3D12Resource* resource);
ID3D12Resource* Dx12Hudless();
} // namespace GameHudless
