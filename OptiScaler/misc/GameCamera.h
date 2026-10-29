#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Resource;

// The game's real camera for frame generation, read from its per-pass constant buffer.
//
// Assassin's Creed Origins (AnvilNext) writes a 2048-byte "PassParams_cbuffer" for its passes. Layout from the game's
// shader reflection (origins-gfx NOTES.md, verified numerically there), matrices stored as rows of a column-vector
// transform (out[i] = dot(c[base + i], float4(v, 1))):
//   c0 eyePosition, c1 eyePositionPrevFrame, c4-7 viewToWorld, c8-11 viewToWorldPrevFrame, c12-15 worldToView,
//   c16-19 worldToViewPrevFrame, c20-23 projMatrix (TAA jitter in column z), c24-27 viewProj,
//   c28-31 viewNoTranslationProj, c32-35 viewProjPrevFrame, c36-39 viewNoTranslationProjPrevFrame,
//   c40 viewTranslation, c41 viewTranslationPrevFrame.
// Right-handed view looking down -z, reversed-Z with an (almost) infinite far plane, near 0.1, world units in metres.
//
// The buffer is captured on the CPU through ReShade's map/unmap/update events (OptiScaler registers as a ReShade
// add-on, see with_dx12/dx11_with_dx12_sc.cpp). Every capture that is not the main camera (shadow cascades,
// reflection probes, ...) fails the checks in IsMainCamera. At the upscaler evaluate, the capture whose projection
// carries the evaluate's jitter is this frame's camera.
struct GameCameraFrame
{
    // Streamline convention: row-major, row vectors (v * M), no jitter
    float viewToClip[16] {};
    float clipToView[16] {};
    float clipToPrevClip[16] {};
    float prevClipToClip[16] {};

    float position[3] {};
    float up[3] {};
    float right[3] {};
    float forward[3] {};

    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    float verticalFov = 0.0f; // radians
    float aspect = 0.0f;      // width / height

    bool depthReversed = false; // the projection maps near to 1
    bool cut = false;           // camera cut since the previously used frame
};

namespace GameCamera
{
// Config Dx11wDx12GameCamera, by default on for games with the AnvilPassParamsCamera quirk
bool IsEnabled();

// Registers the buffer events with ReShade (ReShadeRegisterEvent), once OptiScaler is registered as an add-on
void RegisterReShadeEvents(void* registerEventFunction);

// At the upscaler evaluate: the captured camera whose projection jitter matches this frame's (render pixels)
bool GetFrame(float jitterX, float jitterY, uint32_t renderWidth, uint32_t renderHeight, GameCameraFrame& out);

// Depth convention of the depth texture handed to the upscaler, from a few sampled texels (never waits on the GPU)
void ProbeDepth(ID3D11DeviceContext* context, ID3D11Resource* depth, uint32_t renderWidth, uint32_t renderHeight);

// -1 not known yet, 0 standard (near = 0), 1 inverted (near = 1), 2 linear view depth in world units (see below)
int DepthInverted();

// The depth texture holds linear view depth (metres), not hardware depth: Assassin's Creed Origins' DLSS mod hands
// that to the upscaler. Frame generation needs hardware depth, so it gets a converted copy (shaders/depth_linear).
bool DepthIsLinear();
} // namespace GameCamera
