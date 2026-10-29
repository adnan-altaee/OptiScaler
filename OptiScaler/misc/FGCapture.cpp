#include "pch.h"
#include "FGCapture.h"

#include <Config.h>
#include <misc/GameCamera.h>

#include <d3d11.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>

namespace
{
enum class Stage
{
    Idle,
    Waiting,
    Capturing
};

std::mutex captureMutex;
Stage stage = Stage::Idle;
int framesWanted = 0;
int frameIndex = 0;
bool haveInputs = false; // inputs of the current frame saved, its color not yet
std::chrono::steady_clock::time_point startAt;
std::filesystem::path folder;
std::string frameEntries; // JSON objects of the finished frames
std::string currentEntry; // JSON members of the frame in progress
std::string status = "Idle";

std::string JsonMatrix(const float* m)
{
    std::string out = "[";
    for (int i = 0; i < 16; ++i)
        out += std::format("{}{:.9g}", i == 0 ? "" : ", ", m[i]);
    return out + "]";
}

std::string JsonVector(const float* v) { return std::format("[{:.9g}, {:.9g}, {:.9g}]", v[0], v[1], v[2]); }

// Copies mip 0 of a 2D texture to the CPU and writes its rows as they are (row pitch included). On success `entry`
// gets a JSON object describing the file.
bool SaveTexture(ID3D11DeviceContext* context, ID3D11Resource* resource, const std::wstring& name, std::string& entry)
{
    if (context == nullptr || resource == nullptr)
        return false;

    ID3D11Texture2D* texture = nullptr;
    if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))) || texture == nullptr)
        return false;

    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);

    ID3D11Device* device = nullptr;
    texture->GetDevice(&device);

    bool saved = false;
    ID3D11Texture2D* staging = nullptr;

    if (device != nullptr && desc.SampleDesc.Count == 1)
    {
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;

        if (SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, &staging)) && staging != nullptr)
        {
            context->CopySubresourceRegion(staging, 0, 0, 0, 0, texture, 0, nullptr);

            D3D11_MAPPED_SUBRESOURCE mapped = {};
            if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData != nullptr)
            {
                const auto path = folder / name;
                if (FILE* file = _wfopen(path.c_str(), L"wb"); file != nullptr)
                {
                    saved = std::fwrite(mapped.pData, mapped.RowPitch, desc.Height, file) == desc.Height;
                    std::fclose(file);
                }

                if (saved)
                {
                    entry = std::format("{{\"file\": \"{}\", \"width\": {}, \"height\": {}, \"format\": {}, "
                                        "\"rowPitch\": {}}}",
                                        wstring_to_string(name), desc.Width, desc.Height, (UINT) desc.Format,
                                        mapped.RowPitch);
                }

                context->Unmap(staging, 0);
            }
        }
    }

    if (!saved)
        LOG_WARN("FG capture: could not save {} (format {}, {} samples)", wstring_to_string(name), (UINT) desc.Format,
                 desc.SampleDesc.Count);

    if (staging != nullptr)
        staging->Release();
    if (device != nullptr)
        device->Release();
    texture->Release();
    return saved;
}

void Finish()
{
    const auto path = folder / L"capture.json";
    if (FILE* file = _wfopen(path.c_str(), L"wb"); file != nullptr)
    {
        const auto json = std::format("{{\n\"frames\": [\n{}\n]\n}}\n", frameEntries);
        std::fwrite(json.data(), 1, json.size(), file);
        std::fclose(file);
    }

    status = std::format("Saved {} frames to {}", frameIndex, wstring_to_string(folder.wstring()));
    LOG_INFO("FG capture: {}", status);
    stage = Stage::Idle;
}
} // namespace

void FGCapture::Request(int frames, float delaySeconds)
{
    std::lock_guard<std::mutex> lock(captureMutex);
    if (stage != Stage::Idle)
        return;

    framesWanted = std::clamp(frames, 2, 8);
    startAt = std::chrono::steady_clock::now() +
              std::chrono::milliseconds(static_cast<int>(std::max(delaySeconds, 0.0f) * 1000.0f));
    stage = Stage::Waiting;
    status = "Waiting";
}

bool FGCapture::Busy()
{
    std::lock_guard<std::mutex> lock(captureMutex);
    return stage != Stage::Idle;
}

std::string FGCapture::Status()
{
    std::lock_guard<std::mutex> lock(captureMutex);
    if (stage == Stage::Waiting)
    {
        const auto left = std::chrono::duration<float>(startAt - std::chrono::steady_clock::now()).count();
        return std::format("Starts in {:.0f} s", std::max(left, 0.0f));
    }

    if (stage == Stage::Capturing)
        return std::format("Capturing frame {} of {}", frameIndex + 1, framesWanted);

    return status;
}

void FGCapture::OnInputs(ID3D11DeviceContext* context, ID3D11Resource* motionVectors, ID3D11Resource* depth,
                         const InputInfo& info)
{
    std::lock_guard<std::mutex> lock(captureMutex);

    if (stage == Stage::Waiting && std::chrono::steady_clock::now() >= startAt)
    {
        std::error_code ec;
        auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        tm local = {};
        localtime_s(&local, &time);
        wchar_t stamp[32] = {};
        wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &local);

        folder = std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"fg-capture" / stamp;
        std::filesystem::create_directories(folder, ec);
        if (ec)
        {
            status = "Could not create " + wstring_to_string(folder.wstring());
            stage = Stage::Idle;
            return;
        }

        frameIndex = 0;
        haveInputs = false;
        frameEntries.clear();
        stage = Stage::Capturing;
        LOG_INFO("FG capture: {} frames to {}", framesWanted, wstring_to_string(folder.wstring()));
    }

    // One set of inputs per frame (the color at present closes it)
    if (stage != Stage::Capturing || haveInputs)
        return;

    std::string mvEntry = "null";
    std::string depthEntry = "null";
    SaveTexture(context, motionVectors, std::format(L"frame{}_mv.raw", frameIndex), mvEntry);
    SaveTexture(context, depth, std::format(L"frame{}_depth.raw", frameIndex), depthEntry);

    currentEntry =
        std::format("\"index\": {}, \"mv\": {}, \"depth\": {}, \"renderSize\": [{}, {}], "
                    "\"displaySize\": [{}, {}], \"mvScale\": [{:.9g}, {:.9g}], \"jitter\": [{:.9g}, {:.9g}], "
                    "\"lowResMV\": {}, \"jitteredMV\": {}, \"depthInverted\": {}, \"reset\": {}",
                    frameIndex, mvEntry, depthEntry, info.renderWidth, info.renderHeight, info.displayWidth,
                    info.displayHeight, info.mvScaleX, info.mvScaleY, info.jitterX, info.jitterY, info.lowResMV,
                    info.jitteredMV, info.depthInverted, info.reset);

    if (info.camera != nullptr)
    {
        const auto& c = *info.camera;
        currentEntry += std::format(
            ", \"camera\": {{\"viewToClip\": {}, \"clipToView\": {}, \"clipToPrevClip\": {}, \"prevClipToClip\": {}, "
            "\"position\": {}, \"up\": {}, \"right\": {}, \"forward\": {}, \"near\": {:.9g}, \"far\": {:.9g}, "
            "\"verticalFov\": {:.9g}, \"aspect\": {:.9g}, \"depthReversed\": {}, \"cut\": {}}}",
            JsonMatrix(c.viewToClip), JsonMatrix(c.clipToView), JsonMatrix(c.clipToPrevClip),
            JsonMatrix(c.prevClipToClip), JsonVector(c.position), JsonVector(c.up), JsonVector(c.right),
            JsonVector(c.forward), c.nearPlane, c.farPlane, c.verticalFov, c.aspect, c.depthReversed, c.cut);
    }

    haveInputs = true;
}

void FGCapture::OnColor(ID3D11DeviceContext* context, ID3D11Resource* color)
{
    std::lock_guard<std::mutex> lock(captureMutex);
    if (stage != Stage::Capturing || !haveInputs)
        return;

    std::string colorEntry = "null";
    SaveTexture(context, color, std::format(L"frame{}_color.raw", frameIndex), colorEntry);

    frameEntries += std::format("{}{{{}, \"color\": {}}}", frameIndex == 0 ? "" : ",\n", currentEntry, colorEntry);
    haveInputs = false;

    if (++frameIndex >= framesWanted)
        Finish();
}
