#pragma once

#include <cstdint>
#include <string>

struct ID3D11DeviceContext;
struct ID3D11Resource;
struct GameCameraFrame;

// Saves frame generation's inputs for a few consecutive frames, to check them outside the game (DX11 games with
// frame generation fed by the upscaler).
//
// For each frame: the motion vectors and depth handed to the upscaler, and the final color copied to the frame
// generation swapchain at present, as raw texture rows (row pitch and DXGI format in capture.json), plus the
// constants frame generation got (motion vector scale, jitter, sizes, flags and the game camera when known).
// Written to <OptiScaler folder>\fg-capture\<time>\. Copying and writing stalls the game for a moment.
namespace FGCapture
{
struct InputInfo
{
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    float mvScaleX = 0.0f;
    float mvScaleY = 0.0f;
    float jitterX = 0.0f;
    float jitterY = 0.0f;
    bool lowResMV = false;
    bool jitteredMV = false;
    bool depthInverted = false;
    bool reset = false;
    const GameCameraFrame* camera = nullptr; // nullptr when the game camera is not known
};

// Starts a capture of `frames` consecutive frames after `delaySeconds` (time to close the menu and move)
void Request(int frames, float delaySeconds);

bool Busy();

// One line for the menu
std::string Status();

// At the upscaler evaluate: this frame's motion vectors and depth
void OnInputs(ID3D11DeviceContext* context, ID3D11Resource* motionVectors, ID3D11Resource* depth,
              const InputInfo& info);

// At present: this frame's final color, as it is handed to frame generation
void OnColor(ID3D11DeviceContext* context, ID3D11Resource* color);
} // namespace FGCapture
