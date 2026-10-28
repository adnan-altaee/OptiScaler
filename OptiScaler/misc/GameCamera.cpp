#include "pch.h"
#include "GameCamera.h"

#include <Config.h>
#include <State.h>

#include <d3d11.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>

// ReShade add-on API (v6.8.0, API version 20), events only
#include "../../external/reshade/include/reshade_events.hpp"

namespace
{
// PassParams registers used: c0 .. c41
constexpr int kRegisters = 42;
constexpr uint64_t kMinCameraBytes = kRegisters * 16;
constexpr uint64_t kMaxCameraBytes = 8192;

struct Capture
{
    uint64_t sequence = 0;
    float c[kRegisters][4] {};
};

using M4 = std::array<double, 16>; // [row * 4 + column], column-vector convention like the game's rows

constexpr int kRingSize = 32;
std::mutex ringMutex;
std::array<Capture, kRingSize> ring {};
int ringNext = 0;
uint64_t ringSequence = 0;

// Maps in flight on this thread (pointer returned by Map, read again at Unmap)
struct PendingMap
{
    uint64_t resource = 0;
    const void* data = nullptr;
};

thread_local PendingMap tlsPending[4] {};
thread_local int tlsPendingCount = 0;

// Selection state (render thread)
bool haveLast = false;
uint64_t lastSequence = 0;
double lastPosition[3] {};
double lastForward[3] {};
int signVotes[4] {};
int lockedSign = -1;
uint64_t selectCalls = 0;
uint64_t selectMatched = 0;
uint64_t selectNewest = 0;
uint64_t selectNone = 0;
uint64_t cutsFound = 0;
int checkFailures = 0;
bool disabledByCheck = false;
bool firstFrameLogged = false;
bool noMatchLogged = false;

bool Finite(float v) { return std::isfinite(v) && std::fabs(v) < 1.0e9f; }

bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

M4 LoadRows(const float (&c)[kRegisters][4], int base)
{
    M4 m {};
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            m[r * 4 + k] = c[base + r][k];

    return m;
}

M4 Mul(const M4& a, const M4& b)
{
    M4 m {};
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
        {
            double sum = 0.0;
            for (int i = 0; i < 4; ++i)
                sum += a[r * 4 + i] * b[i * 4 + k];

            m[r * 4 + k] = sum;
        }

    return m;
}

M4 Transpose(const M4& a)
{
    M4 m {};
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            m[r * 4 + k] = a[k * 4 + r];

    return m;
}

bool Inverse(const M4& a, M4& out)
{
    // Gauss-Jordan with partial pivoting
    double m[4][8] {};
    for (int r = 0; r < 4; ++r)
    {
        for (int k = 0; k < 4; ++k)
            m[r][k] = a[r * 4 + k];

        m[r][4 + r] = 1.0;
    }

    for (int col = 0; col < 4; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(m[r][col]) > std::fabs(m[pivot][col]))
                pivot = r;

        if (std::fabs(m[pivot][col]) < 1.0e-12)
            return false;

        if (pivot != col)
            for (int k = 0; k < 8; ++k)
                std::swap(m[pivot][k], m[col][k]);

        const double scale = 1.0 / m[col][col];
        for (int k = 0; k < 8; ++k)
            m[col][k] *= scale;

        for (int r = 0; r < 4; ++r)
        {
            if (r == col)
                continue;

            const double f = m[r][col];
            if (f == 0.0)
                continue;

            for (int k = 0; k < 8; ++k)
                m[r][k] -= f * m[col][k];
        }
    }

    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k)
            out[r * 4 + k] = m[r][4 + k];

    return true;
}

// Column-vector transform of a point, returns the homogeneous result
std::array<double, 4> Apply(const M4& m, double x, double y, double z, double w)
{
    std::array<double, 4> v {};
    for (int r = 0; r < 4; ++r)
        v[r] = m[r * 4 + 0] * x + m[r * 4 + 1] * y + m[r * 4 + 2] * z + m[r * 4 + 3] * w;

    return v;
}

void StoreStreamline(const M4& columnVectorMatrix, float (&out)[16])
{
    // Streamline uses row vectors: its matrix is the transpose of the game's
    const M4 t = Transpose(columnVectorMatrix);
    for (int i = 0; i < 16; ++i)
        out[i] = static_cast<float>(t[i]);
}

// A rotation-only 4x4 from the upper 3x3 of m
M4 RotationOf(const M4& m)
{
    M4 r {};
    for (int row = 0; row < 3; ++row)
        for (int k = 0; k < 3; ++k)
            r[row * 4 + k] = m[row * 4 + k];

    r[15] = 1.0;
    return r;
}

bool Orthonormal3x3(const float (&c)[kRegisters][4], int base)
{
    for (int r = 0; r < 3; ++r)
    {
        const double len = std::sqrt(double(c[base + r][0]) * c[base + r][0] + double(c[base + r][1]) * c[base + r][1] +
                                     double(c[base + r][2]) * c[base + r][2]);
        if (!Near(len, 1.0, 0.02))
            return false;
    }

    return true;
}

// The main camera's PassParams, not a shadow cascade, reflection probe or anything else using the same buffer size
bool IsMainCamera(const float (&c)[kRegisters][4])
{
    for (int r = 0; r < kRegisters; ++r)
        for (int k = 0; k < 4; ++k)
            if (!Finite(c[r][k]))
                return false;

    // Perspective projection with w = +-z
    const auto& p0 = c[20];
    const auto& p1 = c[21];
    const auto& p3 = c[23];

    if (p0[0] < 0.2f || p0[0] > 20.0f || p1[1] < 0.2f || p1[1] > 20.0f)
        return false;

    if (std::fabs(p0[1]) > 1.0e-3f || std::fabs(p1[0]) > 1.0e-3f || std::fabs(p0[3]) > 1.0e-3f ||
        std::fabs(p1[3]) > 1.0e-3f || std::fabs(p0[2]) > 0.02f || std::fabs(p1[2]) > 0.02f)
        return false;

    if (std::fabs(p3[0]) > 1.0e-4f || std::fabs(p3[1]) > 1.0e-4f || std::fabs(p3[3]) > 1.0e-4f ||
        !Near(std::fabs(p3[2]), 1.0, 1.0e-4))
        return false;

    if (c[22][3] == 0.0f)
        return false;

    // Rigid view transforms, current and previous
    if (!Orthonormal3x3(c, 4) || !Orthonormal3x3(c, 12) || !Orthonormal3x3(c, 16))
        return false;

    // worldToView rotation is the transpose of viewToWorld's
    for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k)
            if (!Near(c[12 + r][k], c[4 + k][r], 0.02))
                return false;

    // The eye is viewToWorld's translation
    const double scale = 0.05 + 1.0e-4 * (std::fabs(c[0][0]) + std::fabs(c[0][1]) + std::fabs(c[0][2]));
    if (!Near(c[0][0], c[4][3], scale) || !Near(c[0][1], c[5][3], scale) || !Near(c[0][2], c[6][3], scale))
        return false;

    return true;
}

void StoreCapture(const void* data)
{
    Capture capture {};
    memcpy(capture.c, data, sizeof(capture.c));

    if (!IsMainCamera(capture.c))
        return;

    std::lock_guard<std::mutex> lock(ringMutex);

    // Many passes of a frame carry the same camera: keep one copy
    const auto& newest = ring[(ringNext + kRingSize - 1) % kRingSize];
    if (newest.sequence != 0 && memcmp(newest.c[0], capture.c[0], sizeof(float) * 4) == 0 &&
        memcmp(newest.c[4], capture.c[4], sizeof(float) * 12) == 0 &&
        memcmp(newest.c[20], capture.c[20], sizeof(float) * 8) == 0)
    {
        return;
    }

    capture.sequence = ++ringSequence;
    ring[ringNext] = capture;
    ringNext = (ringNext + 1) % kRingSize;
}

bool IsCameraSizedConstantBuffer(reshade::api::device* device, reshade::api::resource resource)
{
    if (device == nullptr || resource.handle == 0 || device->get_api() != reshade::api::device_api::d3d11)
        return false;

    // On D3D11, ReShade resource handles are the native ID3D11Resource pointers (buffers here)
    D3D11_BUFFER_DESC desc {};
    reinterpret_cast<ID3D11Buffer*>(resource.handle)->GetDesc(&desc);

    return (desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0 && desc.ByteWidth >= kMinCameraBytes &&
           desc.ByteWidth <= kMaxCameraBytes;
}

void OnMapBuffer(reshade::api::device* device, reshade::api::resource resource, uint64_t, uint64_t,
                 reshade::api::map_access access, void** data)
{
    if (data == nullptr || *data == nullptr ||
        (access != reshade::api::map_access::write_discard && access != reshade::api::map_access::write_only))
    {
        return;
    }

    if (!IsCameraSizedConstantBuffer(device, resource))
        return;

    for (int i = 0; i < tlsPendingCount; ++i)
    {
        if (tlsPending[i].resource == resource.handle)
        {
            tlsPending[i].data = *data;
            return;
        }
    }

    if (tlsPendingCount < static_cast<int>(std::size(tlsPending)))
    {
        tlsPending[tlsPendingCount++] = { resource.handle, *data };
    }
    else
    {
        // Oldest out
        for (int i = 1; i < tlsPendingCount; ++i)
            tlsPending[i - 1] = tlsPending[i];

        tlsPending[tlsPendingCount - 1] = { resource.handle, *data };
    }
}

void OnUnmapBuffer(reshade::api::device*, reshade::api::resource resource)
{
    if (tlsPendingCount == 0)
        return;

    for (int i = 0; i < tlsPendingCount; ++i)
    {
        if (tlsPending[i].resource != resource.handle)
            continue;

        const void* data = tlsPending[i].data;

        for (int k = i + 1; k < tlsPendingCount; ++k)
            tlsPending[k - 1] = tlsPending[k];

        --tlsPendingCount;

        // Unmap event comes before the real Unmap, the pointer is still valid
        if (data != nullptr)
            StoreCapture(data);

        return;
    }
}

bool OnUpdateBuffer(reshade::api::device* device, const void* data, reshade::api::resource dest, uint64_t offset,
                    uint64_t size)
{
    if (data != nullptr && offset == 0 && size >= kMinCameraBytes && IsCameraSizedConstantBuffer(device, dest))
        StoreCapture(data);

    return false;
}

bool OnUpdateBufferCommand(reshade::api::command_list* cmdList, const void* data, reshade::api::resource dest,
                           uint64_t offset, uint64_t size)
{
    if (cmdList == nullptr)
        return false;

    return OnUpdateBuffer(cmdList->get_device(), data, dest, offset, size);
}

// Depth probe
struct DepthProbe
{
    ID3D11Texture2D* staging[3] {};
    bool pending[3] {};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint64_t calls = 0;
    int votes[5] {}; // 1 inverted, 0 standard, -1 inconclusive
    int voteCount = 0;
    int decided = -1;
    bool unsupportedLogged = false;
};

DepthProbe probe {};

void ReleaseProbe()
{
    for (auto& texture : probe.staging)
    {
        if (texture != nullptr)
        {
            texture->Release();
            texture = nullptr;
        }
    }

    for (auto& p : probe.pending)
        p = false;
}

void AddDepthVote(int vote)
{
    probe.votes[probe.voteCount % 5] = vote;
    probe.voteCount++;

    const int count = std::min(probe.voteCount, 5);
    int inverted = 0;
    int standard = 0;
    for (int i = 0; i < count; ++i)
    {
        inverted += probe.votes[i] == 1;
        standard += probe.votes[i] == 0;
    }

    int decided = probe.decided;
    if (inverted >= 3 && standard == 0)
        decided = 1;
    else if (standard >= 3 && inverted == 0)
        decided = 0;

    if (decided != probe.decided)
    {
        LOG_INFO("Game camera: the upscaler's depth texture is {} (near = {}), told to frame generation as such",
                 decided == 1 ? "inverted" : "standard", decided == 1 ? 1 : 0);
        probe.decided = decided;
    }
}
} // namespace

bool GameCamera::IsEnabled()
{
    return Config::Instance()->Dx11wDx12GameCamera.value_or(
        static_cast<bool>(State::Instance().gameQuirks & GameQuirk::AnvilPassParamsCamera));
}

void GameCamera::RegisterReShadeEvents(void* registerEventFunction)
{
    if (registerEventFunction == nullptr || !IsEnabled())
        return;

    using RegisterEventFn = void (*)(reshade::addon_event, void*);
    auto registerEvent = reinterpret_cast<RegisterEventFn>(registerEventFunction);

    registerEvent(reshade::addon_event::map_buffer_region, reinterpret_cast<void*>(&OnMapBuffer));
    registerEvent(reshade::addon_event::unmap_buffer_region, reinterpret_cast<void*>(&OnUnmapBuffer));
    registerEvent(reshade::addon_event::update_buffer_region, reinterpret_cast<void*>(&OnUpdateBuffer));
    registerEvent(reshade::addon_event::update_buffer_region_command, reinterpret_cast<void*>(&OnUpdateBufferCommand));

    LOG_INFO("Game camera: reading the game's PassParams constant buffer for frame generation");
}

bool GameCamera::GetFrame(float jitterX, float jitterY, uint32_t renderWidth, uint32_t renderHeight,
                          GameCameraFrame& out)
{
    if (!IsEnabled() || disabledByCheck || renderWidth == 0 || renderHeight == 0)
        return false;

    selectCalls++;

    std::array<Capture, kRingSize> snapshot;
    {
        std::lock_guard<std::mutex> lock(ringMutex);
        snapshot = ring;
    }

    const double renderAspect = double(renderWidth) / double(renderHeight);
    const Capture* chosen = nullptr;
    const Capture* newest = nullptr;
    int chosenSign = -1;

    // Jitter in the projection: ndc offset -P02 / -P12 (w = -z), in render pixels * width / 2
    static constexpr float kSigns[4][2] = { { 1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 } };
    const bool haveJitter = std::fabs(jitterX) > 1.0e-4f || std::fabs(jitterY) > 1.0e-4f;

    for (const auto& capture : snapshot)
    {
        if (capture.sequence == 0)
            continue;

        const double aspect = double(capture.c[21][1]) / double(capture.c[20][0]);
        if (std::fabs(aspect - renderAspect) > renderAspect * 0.02)
            continue;

        if (newest == nullptr || capture.sequence > newest->sequence)
            newest = &capture;

        if (!haveJitter)
            continue;

        const double px = double(capture.c[20][2]) * renderWidth * 0.5;
        const double py = double(capture.c[21][2]) * renderHeight * 0.5;

        for (int s = 0; s < 4; ++s)
        {
            if (lockedSign >= 0 && s != lockedSign)
                continue;

            if (Near(px * kSigns[s][0], jitterX, 0.02) && Near(py * kSigns[s][1], jitterY, 0.02) &&
                (chosen == nullptr || capture.sequence > chosen->sequence))
            {
                chosen = &capture;
                chosenSign = s;
            }
        }
    }

    if (chosen != nullptr)
    {
        selectMatched++;

        if (lockedSign < 0 && chosenSign >= 0 && ++signVotes[chosenSign] >= 30)
        {
            lockedSign = chosenSign;
            LOG_INFO("Game camera: the evaluate's jitter is the projection's jitter * ({}, {}) * size / 2",
                     kSigns[lockedSign][0], kSigns[lockedSign][1]);
        }
    }
    else if (newest != nullptr)
    {
        chosen = newest;
        selectNewest++;

        if (haveJitter && !noMatchLogged && selectCalls > 120 && selectMatched == 0)
        {
            noMatchLogged = true;
            LOG_WARN("Game camera: no capture matches the evaluate's jitter ({}, {}) at {}x{}; newest projection "
                     "jitter ({}, {}); using the newest camera",
                     jitterX, jitterY, renderWidth, renderHeight, newest->c[20][2], newest->c[21][2]);
        }
    }
    else
    {
        selectNone++;
        return false;
    }

    const auto& c = chosen->c;

    // Current projection without jitter
    M4 projection = LoadRows(c, 20);
    projection[0 * 4 + 2] = 0.0;
    projection[1 * 4 + 2] = 0.0;

    M4 projectionInverse {};
    if (!Inverse(projection, projectionInverse))
        return false;

    // Previous projection without jitter: viewNoTranslationProjPrev = Pprev * worldToViewPrev rotation
    const M4 worldToViewPrevRotation = RotationOf(LoadRows(c, 16));
    const M4 viewNoTranslationProjPrev = LoadRows(c, 36);
    M4 previousProjection = Mul(viewNoTranslationProjPrev, Transpose(worldToViewPrevRotation));
    previousProjection[0 * 4 + 2] = 0.0;
    previousProjection[1 * 4 + 2] = 0.0;

    // Current view -> world relative to the previous view translation (small numbers, no precision loss)
    const M4 viewToWorld = LoadRows(c, 4);
    M4 viewToPrevRelative = RotationOf(viewToWorld);
    viewToPrevRelative[0 * 4 + 3] = double(c[4][3]) - double(c[41][0]);
    viewToPrevRelative[1 * 4 + 3] = double(c[5][3]) - double(c[41][1]);
    viewToPrevRelative[2 * 4 + 3] = double(c[6][3]) - double(c[41][2]);

    // clip -> view -> world (relative) -> previous view (rotation) -> previous clip
    const M4 clipToPrevClip =
        Mul(Mul(Mul(previousProjection, worldToViewPrevRotation), viewToPrevRelative), projectionInverse);

    M4 prevClipToClip {};
    if (!Inverse(clipToPrevClip, prevClipToClip))
        return false;

    // Forward is -z for a right-handed view (w = -z), +z otherwise
    const double forwardSign = c[23][2] < 0.0f ? -1.0 : 1.0;

    double position[3] = { c[4][3], c[5][3], c[6][3] };
    double right[3] = { c[4][0], c[5][0], c[6][0] };
    double up[3] = { c[4][1], c[5][1], c[6][1] };
    double forward[3] = { forwardSign * c[4][2], forwardSign * c[5][2], forwardSign * c[6][2] };

    // Self check against the game's own matrices: a point 10 m ahead, projected by viewProj / viewProjPrevFrame
    {
        const double wx = position[0] + forward[0] * 10.0 + right[0] * 2.0 + up[0] * 1.0;
        const double wy = position[1] + forward[1] * 10.0 + right[1] * 2.0 + up[1] * 1.0;
        const double wz = position[2] + forward[2] * 10.0 + right[2] * 2.0 + up[2] * 1.0;

        const auto gameClip = Apply(LoadRows(c, 24), wx, wy, wz, 1.0);
        const auto gamePrevClip = Apply(LoadRows(c, 32), wx, wy, wz, 1.0);

        const auto view = Apply(LoadRows(c, 12), wx, wy, wz, 1.0);
        const auto clip = Apply(projection, view[0], view[1], view[2], view[3]);
        const auto prevClip = Apply(clipToPrevClip, clip[0], clip[1], clip[2], clip[3]);

        const double ndcX = clip[0] / clip[3];
        const double ndcY = clip[1] / clip[3];
        // The game's viewProj carries the jitter: ndc = unjittered + P02 * P32 (P32 = -1 for w = -z)
        const double gameX = gameClip[0] / gameClip[3] - double(c[20][2]) * c[23][2];
        const double gameY = gameClip[1] / gameClip[3] - double(c[21][2]) * c[23][2];

        const double prevX = prevClip[0] / prevClip[3];
        const double prevY = prevClip[1] / prevClip[3];
        const double gamePrevX = gamePrevClip[0] / gamePrevClip[3];
        const double gamePrevY = gamePrevClip[1] / gamePrevClip[3];

        const double currentError = std::max(std::fabs(ndcX - gameX), std::fabs(ndcY - gameY));
        const double previousError = std::max(std::fabs(prevX - gamePrevX), std::fabs(prevY - gamePrevY));

        // Previous: viewProjPrevFrame may carry the previous jitter (up to 1 pixel)
        const double previousTolerance = 3.0e-3 + 2.0 / double(renderHeight);

        if (!(currentError < 3.0e-3) || !(previousError < previousTolerance))
        {
            if (++checkFailures <= 5)
            {
                LOG_WARN("Game camera: check against the game's matrices failed (current {:.6f}, previous {:.6f}); "
                         "camera not used this frame",
                         currentError, previousError);
            }

            if (checkFailures >= 120 && selectMatched + selectNewest < static_cast<uint64_t>(checkFailures) * 2)
            {
                disabledByCheck = true;
                LOG_ERROR("Game camera: the checks keep failing, game camera disabled for this session");
            }

            return false;
        }
    }

    // Depth range from the projection
    const auto depthAt = [&](double z)
    {
        const auto p = Apply(projection, 0.0, 0.0, z, 1.0);
        return p[2] / p[3];
    };

    const double zNear = forwardSign * 1.0;
    const double zFar = forwardSign * 100.0;
    const bool reversed = depthAt(zFar) < depthAt(zNear);

    const auto solveDepth = [&](double d)
    {
        // (P22 z + P23) = d (P32 z + P33)
        const double denominator = projection[2 * 4 + 2] - d * projection[3 * 4 + 2];
        if (std::fabs(denominator) < 1.0e-12)
            return std::numeric_limits<double>::infinity();

        return std::fabs((d * projection[3 * 4 + 3] - projection[2 * 4 + 3]) / denominator);
    };

    double nearPlane = solveDepth(reversed ? 1.0 : 0.0);
    double farPlane = solveDepth(reversed ? 0.0 : 1.0);

    if (!std::isfinite(farPlane) || farPlane <= nearPlane || farPlane > 1.0e7)
        farPlane = 1.0e7;

    StoreStreamline(projection, out.viewToClip);
    StoreStreamline(projectionInverse, out.clipToView);
    StoreStreamline(clipToPrevClip, out.clipToPrevClip);
    StoreStreamline(prevClipToClip, out.prevClipToClip);

    const auto normalize = [](double (&v)[3])
    {
        const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 0.0)
            for (double& x : v)
                x /= len;
    };

    normalize(right);
    normalize(up);
    normalize(forward);

    for (int i = 0; i < 3; ++i)
    {
        out.position[i] = static_cast<float>(position[i]);
        out.right[i] = static_cast<float>(right[i]);
        out.up[i] = static_cast<float>(up[i]);
        out.forward[i] = static_cast<float>(forward[i]);
    }

    out.nearPlane = static_cast<float>(nearPlane);
    out.farPlane = static_cast<float>(farPlane);
    out.verticalFov = static_cast<float>(2.0 * std::atan(1.0 / double(c[21][1])));
    out.aspect = static_cast<float>(double(c[21][1]) / double(c[20][0]));
    out.depthReversed = reversed;

    // Camera cut: a jump no player movement produces in one frame (cutscene shot changes, teleports)
    out.cut = false;
    if (haveLast && chosen->sequence != lastSequence)
    {
        const double dx = position[0] - lastPosition[0];
        const double dy = position[1] - lastPosition[1];
        const double dz = position[2] - lastPosition[2];
        const double moved = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double cosine = forward[0] * lastForward[0] + forward[1] * lastForward[1] + forward[2] * lastForward[2];
        const double turned = std::acos(std::clamp(cosine, -1.0, 1.0)) * 57.29577951308232;

        if (moved > 2.0 || turned > 30.0)
        {
            out.cut = true;
            cutsFound++;
            LOG_DEBUG("Game camera: cut (moved {:.2f} m, turned {:.1f} deg)", moved, turned);
        }
    }

    haveLast = true;
    lastSequence = chosen->sequence;
    for (int i = 0; i < 3; ++i)
    {
        lastPosition[i] = position[i];
        lastForward[i] = forward[i];
    }

    if (!firstFrameLogged)
    {
        firstFrameLogged = true;
        LOG_INFO("Game camera: vertical FOV {:.2f} deg, aspect {:.4f}, near {:.4f}, far {:.1f}, depth {}, position "
                 "({:.2f}, {:.2f}, {:.2f})",
                 out.verticalFov * 57.29577951308232, out.aspect, out.nearPlane, out.farPlane,
                 reversed ? "reversed" : "standard", position[0], position[1], position[2]);
    }

    if (selectCalls % 3600 == 0)
    {
        LOG_INFO("Game camera: {} frames, {} matched by jitter, {} newest capture, {} without camera, {} cuts, {} "
                 "failed checks",
                 selectCalls, selectMatched, selectNewest, selectNone, cutsFound, checkFailures);
    }

    return true;
}

void GameCamera::ProbeDepth(ID3D11DeviceContext* context, ID3D11Resource* depth, uint32_t renderWidth,
                            uint32_t renderHeight)
{
    if (context == nullptr || depth == nullptr)
        return;

    probe.calls++;

    // Read back finished probes first (never waits)
    for (int slot = 0; slot < 3; ++slot)
    {
        if (!probe.pending[slot] || probe.staging[slot] == nullptr)
            continue;

        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (context->Map(probe.staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK)
            continue;

        const float* values = static_cast<const float*>(mapped.pData);
        int low = 0;
        int high = 0;
        for (int i = 0; i < 16; ++i)
        {
            const float v = values[i];
            if (!std::isfinite(v) || v < -0.01f || v > 1.01f)
                continue;

            low += v < 0.5f;
            high += v >= 0.5f;
        }

        context->Unmap(probe.staging[slot], 0);
        probe.pending[slot] = false;

        AddDepthVote(low >= 10 ? 1 : (high >= 10 ? 0 : -1));
    }

    if (probe.calls % 30 != 1)
        return;

    ID3D11Texture2D* depthTexture = nullptr;
    if (FAILED(depth->QueryInterface(IID_PPV_ARGS(&depthTexture))) || depthTexture == nullptr)
        return;

    D3D11_TEXTURE2D_DESC desc {};
    depthTexture->GetDesc(&desc);
    depthTexture->Release();

    // Partial copies of depth-stencil resources are not allowed, and only 32-bit float depth is read here
    const bool supported = (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) == 0 && desc.SampleDesc.Count == 1 &&
                           (desc.Format == DXGI_FORMAT_R32_FLOAT || desc.Format == DXGI_FORMAT_R32_TYPELESS);
    if (!supported)
    {
        if (!probe.unsupportedLogged)
        {
            probe.unsupportedLogged = true;
            LOG_INFO("Game camera: depth texture format {} (bind {:X}) not probed", (UINT) desc.Format,
                     desc.BindFlags);
        }

        return;
    }

    if (probe.format != desc.Format)
    {
        ReleaseProbe();
        probe.format = desc.Format;
    }

    const int slot = static_cast<int>((probe.calls / 30) % 3);
    if (probe.pending[slot])
        return;

    if (probe.staging[slot] == nullptr)
    {
        ID3D11Device* device = nullptr;
        context->GetDevice(&device);
        if (device == nullptr)
            return;

        D3D11_TEXTURE2D_DESC stagingDesc {};
        stagingDesc.Width = 16;
        stagingDesc.Height = 1;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = desc.Format;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        const auto result = device->CreateTexture2D(&stagingDesc, nullptr, &probe.staging[slot]);
        device->Release();

        if (FAILED(result))
            return;
    }

    const UINT width = std::min<UINT>(renderWidth != 0 ? renderWidth : desc.Width, desc.Width);
    const UINT height = std::min<UINT>(renderHeight != 0 ? renderHeight : desc.Height, desc.Height);

    for (UINT i = 0; i < 16; ++i)
    {
        const UINT x = std::min<UINT>(width - 1, (UINT) ((i % 4 + 0.5) * width / 4.0));
        const UINT y = std::min<UINT>(height - 1, (UINT) ((i / 4 + 0.5) * height / 4.0));
        const D3D11_BOX box = { x, y, 0, x + 1, y + 1, 1 };
        context->CopySubresourceRegion(probe.staging[slot], 0, i, 0, 0, depth, 0, &box);
    }

    probe.pending[slot] = true;
}

int GameCamera::DepthInverted() { return probe.decided; }
