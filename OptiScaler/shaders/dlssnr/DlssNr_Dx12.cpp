#include "pch.h"

#include <set>

#include <dlssnr/DlssNr.h>


#include <dlssnr/DlssNr_Capture.h>
#include <dlssnr/DlssNr_DriverGate.h>
#include <dlssnr/DlssNr_Proxy.h>
#include <dlssnr/DlssNr_ExposureScan.h>

#include "DlssNr_Dx12.h"

#include <Config.h>
#include <State.h>
#include <Util.h>

#include <proxies/NVNGX_Proxy.h>
#include <hooks/D3D12_Hooks.h>
#include <gpu_time/GpuTime_Dx12.h>

#include <mutex>
#include <algorithm>
#include <cstring>
#include "DlssNr_ResidualPair.h"
#include "DlssNr_DenoiseFirst_Dx12.h"
#include "DlssNr_DetailReuse_Dx12.h"
#include "precompile/DlssNr_Shader.h"
#include "precompile/dlssnr_residual_Shader.h"
#include "../output_scaling/OS_Dx12.h"

namespace
{
// NGX result codes, by name.
//
// A user's log recently read "init 0x-452FFFFF", which is an int formatted as hex and is
// undiagnosable by anyone. It was 0xBAD00001, FeatureNotSupported -- a complete answer, printed as
// noise. Names cost nothing and turn a bug report into a diagnosis.
const char* NgxResultName(unsigned int r)
{
    switch (r)
    {
    case 0x1: return "Success";
    case 0xBAD00001: return "FAIL_FeatureNotSupported";
    case 0xBAD00002: return "FAIL_PlatformError";
    case 0xBAD00003: return "FAIL_FeatureAlreadyExists";
    case 0xBAD00004: return "FAIL_FeatureNotFound";
    case 0xBAD00005: return "FAIL_InvalidParameter";
    case 0xBAD00006: return "FAIL_ScratchBufferTooSmall";
    case 0xBAD00007: return "FAIL_NotInitialized";
    case 0xBAD00008: return "FAIL_UnsupportedInputFormat";
    case 0xBAD00009: return "FAIL_RWFlagMissing";
    case 0xBAD0000A: return "FAIL_MissingInput";
    case 0xBAD0000B: return "FAIL_UnableToInitializeFeature";
    case 0xBAD0000C: return "FAIL_OutOfDate";
    case 0xBAD0000D: return "FAIL_OutOfGPUMemory";
    case 0xBAD0000E: return "FAIL_UnsupportedFormat";
    case 0xBAD0000F: return "FAIL_UnableToWriteToAppDataPath";
    case 0xBAD00010: return "FAIL_UnsupportedParameter";
    case 0xBAD00011: return "FAIL_Denied";
    case 0xBAD00012: return "FAIL_NotImplemented";
    default: return "unknown";
    }
}

// Does the driver's own nvngx.dll dispatch Neural Rendering?
//
// The trick is that correct parameters are not needed to find out, because the KIND of failure is
// the answer. A dispatcher that has never heard of feature 18 rejects it before looking at anything:
//
//   FeatureNotFound / FeatureNotSupported / NotImplemented -- the driver does not route it, and the
//       forwarder is necessary rather than merely tolerated.
//   MissingInput / InvalidParameter / UnsupportedParameter -- the driver DOES route it. It reached
//       the feature, which then complained about the arguments. That is the win: it means the whole
//       forwarder, and the per-game copy of the model, can go.
//   Success -- better still, though not expected from an empty parameter block.
//
// Once per session, and only when asked for.
void ProbeProxyDispatch(ID3D12GraphicsCommandList* cmdList)
{
    static bool done = false;

    if (done)
        return;

    done = true;

    if (!NVNGXProxy::IsDx12Inited())
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx is not initialised here, nothing to ask");
        return;
    }

    const auto allocate = NVNGXProxy::D3D12_AllocateParameters();
    const auto destroy = NVNGXProxy::D3D12_DestroyParameters();
    const auto create = NVNGXProxy::D3D12_CreateFeature();
    const auto release = NVNGXProxy::D3D12_ReleaseFeature();

    if (allocate == nullptr || create == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: the driver's nvngx does not export what the probe needs");
        return;
    }

    NVSDK_NGX_Parameter* params = nullptr;

    if (allocate(&params) != NVSDK_NGX_Result_Success || params == nullptr)
    {
        LOG_INFO("DLSS-NR proxy probe: could not allocate a parameter block");
        return;
    }

    // Feature 18, and a feature that certainly does not exist, asked the same way.
    //
    // A single result cannot answer this. "UnableToInitializeFeature" for 18 looks like the
    // dispatcher having found the feature and failed to start it on an empty parameter block -- but
    // it might equally be what this dispatcher says about anything it cannot set up. The control
    // settles it: if a nonsense id comes back differently, the difference is knowledge of feature
    // 18. If both come back the same, the first result meant nothing.
    NVSDK_NGX_Handle* handle = nullptr;
    const auto result = (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 18, params, &handle);

    if (handle != nullptr && release != nullptr)
        release(handle);

    NVSDK_NGX_Handle* controlHandle = nullptr;
    const auto control =
        (unsigned int) create(cmdList, (NVSDK_NGX_Feature) 200, params, &controlHandle);

    if (controlHandle != nullptr && release != nullptr)
        release(controlHandle);

    LOG_INFO("DLSS-NR proxy probe: feature 18 -> 0x{:X} ({}), control feature 200 -> 0x{:X} ({})",
             result, NgxResultName(result), control, NgxResultName(control));

    const bool rejectedOutright =
        result == 0xBAD00004 || result == 0xBAD00001 || result == 0xBAD00012;

    if (result == control)
        LOG_INFO("DLSS-NR proxy probe: both answers identical, so this says nothing about feature 18 "
                 "-- the driver treats it exactly as it treats a feature that does not exist");
    else if (rejectedOutright)
        LOG_INFO("DLSS-NR proxy probe: feature 18 is rejected outright -- the driver does not route "
                 "it and the forwarder is required");
    else
        LOG_INFO("DLSS-NR proxy probe: feature 18 answers differently from a nonexistent one, so the "
                 "driver knows it -- the forwarder and the per-game model copy could both go");

    if (destroy != nullptr)
        destroy(params);
}

// Everything the model is reached through. The snippet refuses callers whose module path does not
// contain "nvngx.dll", so the calls are made from a small library named for exactly that reason and
// shipped beside OptiScaler; see nvngx.dll_dlssnr.dll.
using PFN_NrCreate = void*(__cdecl*) (const wchar_t*, const wchar_t*, ID3D12Device*,
                                      ID3D12GraphicsCommandList*, void*, unsigned int, unsigned int, int,
                                      float, int, float, float, float, int, int);
using PFN_NrEvaluate = int(__cdecl*) (ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*,
                                      ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned int,
                                      unsigned int, unsigned int, unsigned int, int, int, float, int,
                                      float, float, float, int, float, float);
using PFN_NrRelease = void(__cdecl*) (void*);
using PFN_NrSetExtras = void(__cdecl*) (void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                                        unsigned int, unsigned int, unsigned int, unsigned int);
using PFN_NrSetFloatSlot = void(__cdecl*) (int);
using PFN_NrProbeFloat = void(__cdecl*) (void*, const char*, float, int);

// One per back buffer, so an allocator is never reset while its frame is still in flight.

struct NrState
{
    HMODULE forwarder = nullptr;
    PFN_NrCreate create = nullptr;
    PFN_NrEvaluate evaluate = nullptr;
    PFN_NrRelease release = nullptr;
    PFN_NrSetExtras setExtras = nullptr;
    PFN_NrSetFloatSlot setFloatSlot = nullptr;
    PFN_NrProbeFloat probeFloat = nullptr;
    bool floatSlotKnown = false;

    // The scaling-ratio probe, resolved alongside the other forwarder entry points.
    int (*queryRatio)(const wchar_t*, void*, unsigned int, float*) = nullptr;
    const int* lastRatioStage = nullptr;
    int* lastInit = nullptr;
    int* lastCreate = nullptr;

    NVSDK_NGX_Parameter* capabilityParams = nullptr;
    void* feature = nullptr;

    // A feature per extra pass, each with its own temporal history.
    //
    // One feature run three times in a frame is told three frames passed with nothing moving between
    // them, so its history fights every pass after the first -- which is what "loses detail on later
    // passes" was. Separate features each see one frame per frame, which is the contract they were
    // built for.
    //
    // It is also the only reading that fits the one clue we have about how this is done elsewhere:
    // that implementation's memory grows with the pass count, and reusing a single feature cannot do
    // that. A feature apiece can, because each carries its own history.
    //
    // Indexed by pass, so [0] is unused and the first extra pass is [1]. Wasting one pointer keeps
    // every index here equal to the pass number it belongs to.
    void* passFeature[10] = {};
    bool passNeedsReset[10] = {};
    bool passCreateFailed[10] = {};
    bool passPendingSubmission[10] = {};
    unsigned long long passCreateEpoch[10] = {};

    bool featurePendingSubmission = false;
    unsigned long long featureCreateEpoch = 0;
    bool beforeUpscale = false;
    bool rayReconstruction = false;

    // The model cannot read and write one resource, so the frame is staged through these.
    ID3D12Resource* colorCopy = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Resource* passScratch = nullptr;
    bool passScratchFailed = false;

    // ResidualAcrossRR v2 surfaces and pairing state.
    ID3D12Resource* residualEdited = nullptr;
    ID3D12Resource* residualHistory[2] = {};
    ID3D12Resource* residualComposed = nullptr;
    unsigned int residualHistoryIndex = 0;
    bool residualHistoryPrimed = false;
    bool residualStoreValid = false;
    bool residualModeActive = false;
    bool residualHistoryRejection = false;
    float residualHistoryPreExposure = 1.0f;
    unsigned int residualOutputWidth = 0;
    unsigned int residualOutputHeight = 0;
    DXGI_FORMAT residualOutputFormat = DXGI_FORMAT_UNKNOWN;
    DlssNr::ResidualEvalPair residualPair {};

    // The frame as the upscaler wrote it. The resolve adds the model's edit to this rather than
    // reconstructing it by inverting the tone curve, which is what turned every light in the frame into
    // a string of coloured cells.
    ID3D12Resource* hdrCopy = nullptr;

    // The frame shrunk for the model, when it is working below full resolution.
    ID3D12Resource* colorSmall = nullptr;
    ID3D12Resource* depthSmall = nullptr;
    ID3D12Resource* motionSmall = nullptr;
    bool loggedGuideMatch = false;
    uint32_t loggedMotionReference = 0, loggedMotionTexture = 0, loggedMotionWork = 0;

    // Supersampling (working scale > 1): the Output Scaling upsampler used to enlarge the proxy to the
    // model's larger-than-native working size with a real filter instead of the box minifier. Created
    // lazily on the first super-native frame, released in Shutdown; sizes from the resources each call,
    // so a resolution change needs no rebuild.
    OS_Dx12* superUp = nullptr;

    // Supersampling down-leg: the native-sized buffer the Nx model answer is averaged into, and the
    // downscaler that does it. With superUp this lands the super-native answer at native for a 1:1
    // composite (no aliased minify). nrScaler is the filter both were built with, so a changed
    // DlssNrScalingDownscaler rebuilds them.
    ID3D12Resource* outputNative = nullptr;
    OS_Dx12* superDown = nullptr;
    Scaler nrScaler = Scaler::Count;

    // Frame hold (design/frame-hold.md): a persistent copy of the output taken on hold-on and restored
    // over the live output before the encode reads it while held, so a setting change re-renders the
    // same frame. heldWhitePoint is the snapshot used while held -- measurement is suspended.
    ID3D12Resource* heldColor = nullptr;
    bool heldActive = false;
    unsigned int heldWidth = 0;
    unsigned int heldHeight = 0;
    DXGI_FORMAT heldFormat = DXGI_FORMAT_UNKNOWN;
    float heldWhitePoint = 1.0f;

    unsigned int workWidth = 0;
    unsigned int workHeight = 0;

    // The white point meter.
    //
    // A 64x64 grid of tile luminances, copied to a readback buffer and looked at a few frames later.
    // Four buffers deep rather than one: the copy is recorded into the game's own command list and
    // there is no fence here to wait on, so the only thing making a read safe is that the frame it
    // came from is long retired. Three frames of distance is what the meter this replaces used.
    //
    // A stale read costs a slightly wrong float that the average below absorbs. A read of a buffer
    // still being written would cost the same, which is why the value is smoothed rather than used
    // raw.
    ID3D12Resource* meter = nullptr;
    ID3D12Resource* meterReadback[4] = {};

    // The calibration grid: what scale the game's buffer is on, measured from the untouched copy.
    // Its own surface and ring rather than sharing the meter's, because the two run at different
    // sizes -- the meter fetches one texel and this reads the whole frame.
    ID3D12Resource* calib = nullptr;
    ID3D12Resource* calibReadback[4] = {};
    unsigned long long calibFrames = 0;

    // The last few answers, so the menu can say how settled the number is. A suggestion taken during
    // a fade or a loading screen is worth less than one taken while standing still, and the spread
    // across recent frames is what tells them apart.
    static constexpr unsigned int kCalibHistory = 32;
    float calibHistory[kCalibHistory] = {};
    unsigned int calibCount = 0;
    float calibSuggestion = 0.0f;
    float calibSteadiness = 0.0f;
    bool calibUsable = false;
    const char* calibWhy = "measuring...";
    bool calibPassthrough = false;

    // Whether the frame that filled each readback slot actually had an exposure texture bound.
    //
    // The meter writes tile 0 from whatever sits in the exposure slot, and DispatchPass substitutes
    // the source picture when nothing is bound -- so without this the "exposure" read back is the red
    // channel of the frame's top-left pixel. In Cyberpunk, which supplies no exposure texture, that
    // pixel is scene content: it moved by up to 272x between consecutive frames and drove the white
    // point from 0.18 to 74. That is the whole frame flashing in luminance.
    //
    // The grid is read three frames after it is written, so the flag has to travel with the slot
    // rather than being asked of the current frame.
    bool meterExposureValid[4] = {};
    unsigned int meterSlot = 0;
    unsigned long long meterFrames = 0;

    // Whether the setting was on last frame, so the off->on edge can be caught.
    //
    // Deliberately the SETTING and not `wantExposure`: the texture itself comes and goes between
    // frames and holding the last good value across those gaps is the whole point of the field below.
    // Only the user turning the option back on means "anything held is from an unknown time ago".
    bool exposureSettingWasOn = false;

    // The game's exposure, as last read back, and the pre-exposure that goes with it. Held rather
    // than defaulted: the texture comes and goes between frames and a fallback to 1.0 on the gaps
    // would be a flicker source.
    float gameExposure = 0.0f;
    float gamePreExposure = 1.0f;

    // What the game OFFERS, as opposed to what has been read. Recorded from the parameter block every
    // frame whether or not the setting is on, and deliberately so: the menu has to be able to answer
    // "would this do anything here?" before the user turns it on, and reading a pointer for null costs
    // nothing. Whether it was ever offered is kept separately from whether it was offered this frame,
    // because games drop it on transitions -- GTA V dropped it three times in one session -- and one
    // absent frame is not the same answer as never.
    bool exposureOfferedNow = false;
    bool exposureEverOffered = false;
    unsigned long long exposureFrames = 0;

    // Cloned unconditionally when running at present, and only for typeless formats otherwise.
    ID3D12Resource* depthClone = nullptr;
    ID3D12Resource* motionClone = nullptr;

    // The constant-depth probe's surface. Separate from depthClone on purpose: it is defined by
    // never having been written, and sharing a surface with a mode that writes would destroy that.
    ID3D12Resource* depthConstant = nullptr;

    unsigned int width = 0;
    unsigned int height = 0;
    bool reset = true;

    // Dimensions of the guides as the upscaler handed them over, kept for the present path, which runs
    // long after that call has returned.
    unsigned int guideWidth = 0;
    unsigned int guideHeight = 0;

    // How the game encodes its guides, as the game itself reports it. Captured with the guides, since
    // the finished-frame path runs long after the upscaler's call has returned.
    bool guideDepthInverted = false;
    float guideMvScaleX = 1.0f;
    float guideMvScaleY = 1.0f;

    // The values the live feature was created with, and when a difference from them was first seen.
    unsigned int builtPreset = 0;
    float builtIntensity = 0.0f;
    unsigned int builtStyle = 0;
    float builtLocalStructure = 0.0f;
    float builtLocalTone = 0.0f;
    float builtSkinStructure = 0.0f;
    bool builtAutoMask = false;
    unsigned int builtPassPreset[10] = {};
    unsigned int builtPassStyle[10] = {};
    unsigned long long settledAt = 0;

    // Once something fails there is no recovering it mid-session, and retrying every frame turns a
    // failure into a crash. It stays off and says why.
    bool failed = false;
    const char* reason = "";
};

NrState g_nr;
std::unique_ptr<DlssNr_Dx12> g_compose;
std::unique_ptr<DlssNr_DenoiseFirst_Dx12> g_denoiseFirst;
std::unique_ptr<DlssNr_DetailReuse_Dx12> g_detailReuse;

// What the pass costs on the GPU, for the breakdown in the overlay.
std::unique_ptr<GpuTime_Dx12> g_gpuTime;

// A second timer, around the model's evaluate and nothing else.
//
// The first one brackets the whole pass, which is the number the menu shows and the right one for
// "what does this feature cost". It is the wrong number for deciding what to optimise: the 4.10 ms at
// full model resolution and 2.24 ms at half were both whole-pass, and both included this pass's own
// encode and resolve at DISPLAY resolution plus the guide copies, none of which move when the model's
// resolution does. Fitting a fixed term to those two points therefore attributes our own unchanging
// work to NGX overhead.
//
// Splitting them says how much of the pass is the model and how much is ours -- and ours is the half
// we can actually do something about.
std::unique_ptr<GpuTime_Dx12> g_ngxTime;
std::optional<double> g_lastNgxTime;
std::optional<double> g_lastGpuTime;

// Writes matched before/after frames on request, so comparisons stop depending on video.
capture::FrameCapture g_capture;

// One capture happens on its own each session, so there is always a fresh sample without anyone having
// to remember to ask. Started after the scene has had a moment to settle: the first frames after a
// feature is built carry its reset, and are not representative of anything.
constexpr unsigned long long kAutoCaptureAfterFrames = 180;
bool g_autoCaptureDone = false;

// Cleared once per run, so a session's captures are its own and nothing accumulates across launches.
void ClearCaptureDirectory()
{
    static bool cleared = false;

    if (cleared)
        return;

    cleared = true;

    std::error_code ec;
    const auto dir = Util::DllPath().remove_filename() / "dlssnr-capture";

    if (std::filesystem::exists(dir, ec))
    {
        std::filesystem::remove_all(dir, ec);

        if (ec)
            LOG_WARN("DLSS-NR could not clear {}: {}", dir.string(), ec.message());
    }
}

unsigned long long g_frames = 0;

// A capture requested from outside the game: when the render path has no fence of its own, the write
// waits until this frame count, by which point the GPU is certainly past the copies.
unsigned long long g_captureWriteAtFrame = 0;

// Dropping a file named dlssnr-capture.trigger beside OptiScaler requests a capture, so a session can
// be asked for one from outside the game -- no alt-tab, no menu. Checked once a second, effectively.
void CheckCaptureTrigger()
{
    if ((g_frames % 60) != 0)
        return;

    std::error_code ec;
    const auto trigger = Util::DllPath().remove_filename() / "dlssnr-capture.trigger";

    if (std::filesystem::exists(trigger, ec))
    {
        std::filesystem::remove(trigger, ec);
        DlssNr::RequestCapture(capture::kMaxFrames);
        LOG_INFO("DLSS-NR capture requested by trigger file");
    }
}

// The encoded mean is aimed here. Mid-grey rather than anything brighter: the model has to see both the
// shadow detail it might lift and the highlights it must not blow out.
constexpr float kTargetEncodedMean = 0.45f;

// How fast the derived value follows the scene. Readings arrive a few times a second, and an exposure
// that lunges at every cut is worse than one that arrives a moment late.
constexpr float kWhitePointBlend = 0.25f;

// Recomputes the white point from a measured mean. Inverting the encode for the white point that puts
// that mean at the target gives wp = mean * (1 - t^g) / t^g.
float WhitePointForMean(float meanLuma)
{
    const float encoded = powf(kTargetEncodedMean, 2.2f);
    const float ratio = encoded / (1.0f - encoded);
    const float wp = meanLuma / ratio;
    // A black frame between scenes would otherwise drive this to zero and divide the next frame by it.
    return wp < 0.01f ? 0.01f : (wp > 10000.0f ? 10000.0f : wp);
}

std::filesystem::path g_dllDir;

// Loads the forwarder that owns the calls into the snippet.
bool EnsureForwarder()
{
    if (g_nr.forwarder != nullptr)
        return g_nr.create != nullptr;

    if (g_dllDir.empty())
        g_dllDir = Util::DllPath().remove_filename();

    // Beside OptiScaler first, then beside the executable: someone dropping this into a game folder may
    // reasonably put it in either place.
    auto found = Util::FindFilePath(g_dllDir, "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
        found = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx.dll_dlssnr.dll");

    if (!found.has_value())
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll not found beside OptiScaler ({}) or the game executable",
                  g_dllDir.string());
        g_nr.reason = "nvngx.dll_dlssnr.dll is missing";
        return false;
    }

    // FindFilePath hands back the file itself, not the directory holding it.
    const auto path = found.value();
    g_nr.forwarder = LoadLibraryW(path.wstring().c_str());

    if (g_nr.forwarder == nullptr)
    {
        LOG_ERROR("nvngx.dll_dlssnr.dll found at {} but would not load, error {}", path.string(),
                  GetLastError());
        g_nr.reason = "nvngx.dll_dlssnr.dll would not load";
        return false;
    }

    g_nr.queryRatio = (int (*)(const wchar_t*, void*, unsigned int, float*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_query_scaling_ratio");
    g_nr.lastRatioStage = (const int*) GetProcAddress(g_nr.forwarder, "dlssnr_last_ratio_stage");

    g_nr.create = (PFN_NrCreate) GetProcAddress(g_nr.forwarder, "dlssnr_call_create");
    g_nr.evaluate = (PFN_NrEvaluate) GetProcAddress(g_nr.forwarder, "dlssnr_call_evaluate");
    g_nr.release = (PFN_NrRelease) GetProcAddress(g_nr.forwarder, "dlssnr_call_release");
    // Optional: an older forwarder simply lacks it, and the model runs as before.
    g_nr.setExtras = (PFN_NrSetExtras) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_extras");
    g_nr.setFloatSlot = (PFN_NrSetFloatSlot) GetProcAddress(g_nr.forwarder, "dlssnr_call_set_float_slot");
    g_nr.probeFloat = (PFN_NrProbeFloat) GetProcAddress(g_nr.forwarder, "dlssnr_call_probe_float");
    g_nr.lastInit = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_init");
    g_nr.lastCreate = (int*) GetProcAddress(g_nr.forwarder, "dlssnr_call_last_create");

    if (g_nr.create == nullptr || g_nr.evaluate == nullptr)
    {
        g_nr.reason = "the forwarder is missing its exports";
        return false;
    }

    LOG_INFO("DLSS-NR forwarder loaded from {}", path.string());
    return true;
}

// The model needs the driver core's own capability block: it carries the snippet and preset callbacks a
// feature expects at create time, which a freshly allocated block does not have.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params);
void ReportScalingRatios();

bool EnsureCapabilityParams(ID3D12Device* device)
{
    if (g_nr.capabilityParams != nullptr)
        return true;

    if (!NVNGXProxy::IsDx12Inited() && !NVNGXProxy::InitDx12(device))
    {
        g_nr.reason = "the NGX core would not initialise";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters() == nullptr)
    {
        g_nr.reason = "the NGX core has no capability parameters";
        return false;
    }

    if (NVNGXProxy::D3D12_GetCapabilityParameters()(&g_nr.capabilityParams) != NVSDK_NGX_Result_Success ||
        g_nr.capabilityParams == nullptr)
    {
        g_nr.capabilityParams = nullptr;
        g_nr.reason = "the NGX core refused its capability parameters";
        return false;
    }

    // Before anything is written to it, work out where this block keeps floats.
    DiscoverFloatSlot(g_nr.capabilityParams);

    // Ask the model what scaling ratio it wants, once, for every quality level it might accept.
    //
    // Read-only and answered before any feature exists. The point is to find out whether NVIDIA's own
    // performance mode for this model is reachable: the snippet has ComputeScalingRatioCommon and the
    // kernel table has _ds, _upsample and _upsample_tilesync variants of every fused Swin block, which
    // together suggest the model can run its interior below display resolution natively -- rather than
    // being handed a picture we shrank ourselves, which costs an extra resample of the edit on the way
    // back and quantises the Swin grid to a lattice we chose rather than the one it was trained on.
    ReportScalingRatios();
    return true;
}

// What the model says it wants to run at, per quality level. Logged once, used for nothing yet.
//
// Answered by the snippet's own callback rather than chosen by us. If it answers, NVIDIA ships a
// performance mode for Neural Rendering and the resolution slider is a worse hand-rolled version of
// it. If it does not, the slider is all there is and that is worth knowing too.
void ReportScalingRatios()
{
    if (g_nr.queryRatio == nullptr || g_nr.capabilityParams == nullptr)
        return;

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    static const char* kNames[] = { "MaxPerf",         "Balanced",    "MaxQuality",
                                    "UltraPerformance", "UltraQuality", "DLAA" };

    char line[512] = {};
    size_t used = 0;
    bool any = false;

    for (unsigned int q = 0; q < 6; ++q)
    {
        float ratio = -1.0f;
        const int rc = g_nr.queryRatio(snippet->wstring().c_str(), g_nr.capabilityParams, q, &ratio);
        int written = 0;

        if (rc == 1)
        {
            any = true;
            written = snprintf(line + used, sizeof(line) - used, "%s=%.4f ", kNames[q], ratio);
        }
        else if (rc == -1)
        {
            written = snprintf(line + used, sizeof(line) - used, "%s=refused ", kNames[q]);
        }

        if (written > 0)
            used += (size_t) written;
    }

    if (any)
        LOG_INFO("DLSS-NR the model's own scaling ratios: {}", line);
    else
        LOG_INFO("DLSS-NR scaling ratio callback not published by this snippet (stage {})",
                 g_nr.lastRatioStage != nullptr ? *g_nr.lastRatioStage : -1);
}

// Works out which vtable slot this parameter block keeps floats in, by writing a known value through
// each candidate and asking for it back through the header's typed getter. Only a slot that returns the
// value it was given is accepted.
//
// Slot 1 is where the public header declares the float overload, so it is tried first and wins wherever
// that assumption holds. It does not hold for the driver's own block: every float written there reads
// back as FAIL_UnsupportedParameter while every uint lands, which is why intensity, local structure,
// local tone and skin structure never did anything.
void DiscoverFloatSlot(NVSDK_NGX_Parameter* params)
{
    if (g_nr.floatSlotKnown || params == nullptr || g_nr.probeFloat == nullptr ||
        g_nr.setFloatSlot == nullptr)
        return;

    g_nr.floatSlotKnown = true;

    static const char* kProbeKey = "DLSSNR.OptiScalerFloatProbe";
    static const int kCandidates[] = { 1, 2, 5, 6, 7, 4, 3, 0 };
    const float expected = 0.375f; // exact in binary, so the round trip is exact or it is wrong

    for (int slot : kCandidates)
    {
        float readBack = 0.0f;
        g_nr.probeFloat(params, kProbeKey, expected, slot);

        if (params->Get(kProbeKey, &readBack) == NVSDK_NGX_Result_Success && readBack == expected)
        {
            g_nr.setFloatSlot(slot);
            LOG_INFO("DLSS-NR float parameters go through vtable slot {}", slot);
            return;
        }
    }

    LOG_ERROR("DLSS-NR could not find the float setter: intensity, local structure, local tone and skin "
              "structure will have no effect. The uint parameters still apply.");
}

// Switching inject points changes the surface format underneath the scratch set: the finished frame
// works in the swapchain's format, the pre-frame-generation path in the upscaler's. A stale set either
// clamps linear HDR into an 8-bit texture -- wrong brightness until something forces a rebuild -- or
// hands CopyResource mismatched formats, which fails silently and makes the whole pass appear to do
// nothing. So the set is torn down whenever the format it was built for is not the format needed now.
// Retired model features and surfaces are parked and freed a comfortable number of evaluates later.
// Releasing them immediately was the device hang: with frame generation the GPU runs several frames
// behind, this work rides the game's own queue that no module fence covers, and an NGX feature or
// scratch texture freed under in-flight work kills the device.
struct NrRetired
{
    void* feature = nullptr;
    ID3D12Resource* resource = nullptr;
    int framesLeft = 32;
};

std::vector<NrRetired> g_nrRetired;

void ParkNrFeature(void*& feature)
{
    if (feature == nullptr)
        return;

    NrRetired r;
    r.feature = feature;
    feature = nullptr;
    g_nrRetired.push_back(r);
}

void ParkNrResource(ID3D12Resource*& res)
{
    if (res == nullptr)
        return;

    NrRetired r;
    r.resource = res;
    res = nullptr;
    g_nrRetired.push_back(r);
}

void TickNrRetired()
{
    for (size_t i = 0; i < g_nrRetired.size();)
    {
        if (--g_nrRetired[i].framesLeft > 0)
        {
            ++i;
            continue;
        }

        if (g_nrRetired[i].feature != nullptr && g_nr.release != nullptr)
            g_nr.release(g_nrRetired[i].feature);

        if (g_nrRetired[i].resource != nullptr)
            g_nrRetired[i].resource->Release();

        g_nrRetired.erase(g_nrRetired.begin() + i);
    }
}

// The inject point decides which buffer is being measured -- the upscaler's linear output or the
// finished frame in swapchain format -- so a reading taken before a change describes a different
// picture to one taken after. Everything else that depends on the format is invalidated here.
void ForgetCalibration()
{
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";
}

void ReleaseSurfacesIfFormatChanged(DXGI_FORMAT needed)
{
    if (g_nr.output == nullptr || g_nr.output->GetDesc().Format == needed)
        return;

    LOG_INFO("DLSS-NR rebuilding surfaces: format {} -> {} (inject point changed)",
             (int) g_nr.output->GetDesc().Format, (int) needed);

    ForgetCalibration();

    ParkNrFeature(g_nr.feature);
    g_nr.featurePendingSubmission = false;

    // The extras go with it: they were built for this raster and this tuning too.
    for (unsigned int i = 0; i < 10; ++i)
    {
        ParkNrFeature(g_nr.passFeature[i]);
        g_nr.passNeedsReset[i] = false;
        g_nr.passCreateFailed[i] = false;
        g_nr.passPendingSubmission[i] = false;
    }

    for (ID3D12Resource** r : { &g_nr.output, &g_nr.passScratch, &g_nr.colorCopy, &g_nr.hdrCopy, &g_nr.colorSmall,
                                &g_nr.outputNative, &g_nr.residualEdited, &g_nr.residualHistory[0],
                                &g_nr.residualHistory[1], &g_nr.residualComposed })
        ParkNrResource(*r);

    g_nr.passScratchFailed = false;
    g_nr.residualStoreValid = false;
    g_nr.residualHistoryIndex = 0;
    g_nr.residualHistoryPrimed = false;
    g_nr.reset = true;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to);

// The meter's grid is R32_FLOAT, which makes a row exactly 64 * 4 = 256 bytes -- the alignment a
// texture-to-buffer copy demands, met without padding, so the readback is a flat array of floats.
constexpr unsigned int kMeterRowBytes = kDlssNrMeterGrid * sizeof(float);
constexpr unsigned int kMeterBytes = kMeterRowBytes * kDlssNrMeterGrid;

// Records the copy of this frame's grid into whichever readback buffer is furthest from being read.
// Same shape as the meter's copy, against the calibration surface and its own ring.
void CopyCalibrationToReadback(ID3D12GraphicsCommandList* cmdList)
{
    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);

    if (g_nr.calibReadback[slot] == nullptr || g_nr.calib == nullptr)
        return;

    D3D12_TEXTURE_COPY_LOCATION srcLoc {};
    srcLoc.pResource = g_nr.calib;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.calibReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    Barrier(cmdList, g_nr.calib, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.calibFrames++;
}

void CopyMeterToReadback(ID3D12GraphicsCommandList* cmdList, ID3D12Device* device,
                         bool exposureBound)
{
    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);

    if (g_nr.meterReadback[slot] == nullptr)
        return;

    // Travels with the grid: read back three frames from now, alongside the tiles it describes.
    g_nr.meterExposureValid[slot] = exposureBound;

    D3D12_TEXTURE_COPY_LOCATION src {};
    src.pResource = g_nr.meter;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dst {};
    dst.pResource = g_nr.meterReadback[slot];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_FLOAT;
    dst.PlacedFootprint.Footprint.Width = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Height = kDlssNrMeterGrid;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = kMeterRowBytes;

    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(cmdList, g_nr.meter, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_nr.meterFrames++;
}

// Takes the game's exposure out of tile 0 of the grid recorded three frames ago.
//
// Only tile 0 is written now. The frame-statistics meter this served was removed: a divisor measured
// off a frame this pass writes is a feedback loop rather than a measurement. What is left is a
// courier -- the game's exposure is a 1x1 texture in a resource state this pass did not set and must
// not transition, so the shader reads it as an SRV and it rides home on a readback that exists.
// Reads the calibration grid written four frames ago and turns it into one number.
//
// A high percentile of tile peaks, not the maximum: the maximum is a sun or a specular hit and would
// normalise the whole picture into the dark. The 90th percentile is high enough to sit at the top of
// the real range and common enough that no single highlight decides it.
void ConsumeCalibrationReadback()
{
    if (g_nr.calibFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.calibFrames % 4);
    ID3D12Resource* buffer = g_nr.calibReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, kMeterBytes };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    std::vector<float> tiles;
    tiles.reserve(kDlssNrMeterGrid * kDlssNrMeterGrid);

    for (unsigned int i = 0; i < kDlssNrMeterGrid * kDlssNrMeterGrid; ++i)
    {
        if (std::isfinite(src[i]) && src[i] > 1e-6f)
            tiles.push_back(src[i]);
    }

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);

    if (tiles.size() < 16)
        return;

    const size_t nth = (size_t) ((float) (tiles.size() - 1) * 0.90f);
    std::nth_element(tiles.begin(), tiles.begin() + nth, tiles.end());

    // How much of the frame carries light, measured against its own brightest tile rather than an
    // absolute threshold -- the units here are the game's and there is no absolute scale.
    //
    // This is what separates "the buffer is scaled by 240" from "I am standing in a dark cave". A
    // percentile of tile peaks is a statement about scene content; it only describes the buffer when
    // enough of the picture is lit for the top of the range to actually appear in it.
    float brightest = 0.0f;

    for (float v : tiles)
        brightest = std::max(brightest, v);

    unsigned int lit = 0;

    for (float v : tiles)
    {
        if (v > brightest * 0.10f)
            ++lit;
    }

    const float litFraction = tiles.empty() ? 0.0f : (float) lit / (float) tiles.size();

    // A torn readback survives isfinite and would clamp to exactly the ceiling, which since the
    // ceiling became 2000 is a value the slider can hold -- so a garbage frame could be offered as a
    // real answer. Reject rather than clamp.
    if (!(tiles[nth] > 0.0f) || tiles[nth] >= 1999.0f)
        return;

    const float suggestion = std::clamp(tiles[nth], 0.25f, 1990.0f);

    g_nr.calibUsable = !g_nr.calibPassthrough && litFraction > 0.20f;
    g_nr.calibWhy = g_nr.calibPassthrough  ? "this game hands over a frame it already tone mapped, so there is nothing to normalise"
                    : litFraction <= 0.20f ? "too little of this scene is lit to say where the top of the range is"
                                           : "";

    g_nr.calibHistory[g_nr.calibCount % NrState::kCalibHistory] = suggestion;
    g_nr.calibCount++;
    g_nr.calibSuggestion = suggestion;

    // Confidence is the spread of recent answers, not their absolute size. A number that has held
    // still for a second is one worth taking; one that is swinging means the scene is changing under
    // the measurement, and no single value would serve anyway.
    const unsigned int have = std::min<unsigned int>(g_nr.calibCount, NrState::kCalibHistory);

    if (have >= 8)
    {
        float lo = g_nr.calibHistory[0];
        float hi = g_nr.calibHistory[0];

        for (unsigned int i = 0; i < have; ++i)
        {
            lo = std::min(lo, g_nr.calibHistory[i]);
            hi = std::max(hi, g_nr.calibHistory[i]);
        }

        // A spread of 1.0x is perfect agreement and 2x or worse is none.
        const float spread = hi / lo;
        g_nr.calibSteadiness = std::clamp(1.0f - (spread - 1.0f), 0.0f, 1.0f);
    }
}

void ConsumeMeterReadback()
{
    if (g_nr.meterFrames < 4)
        return;

    const unsigned int slot = (unsigned int) (g_nr.meterFrames % 4);
    ID3D12Resource* buffer = g_nr.meterReadback[slot];

    if (buffer == nullptr)
        return;

    void* mapped = nullptr;
    D3D12_RANGE range { 0, sizeof(float) };

    if (FAILED(buffer->Map(0, &range, &mapped)) || mapped == nullptr)
        return;

    const float* src = (const float*) mapped;

    // Only believed when the frame that wrote this grid actually had an exposure texture bound. With
    // nothing bound DispatchPass substitutes the source picture, and tile 0 is then a scene pixel
    // rather than an exposure -- believing it made the white point follow the top-left corner of the
    // screen, which in Cyberpunk moved by up to 272x between frames and flashed the whole picture.
    //
    // When it is not believed gameExposure keeps its last good value, or stays 0 and lets
    // ResolveWhitePoint fall back to the slider, which is what a game supplying none should get.
    if (g_nr.meterExposureValid[slot] && std::isfinite(src[0]) && src[0] > 0.0f)
        g_nr.gameExposure = src[0];

    D3D12_RANGE nothingWritten { 0, 0 };
    buffer->Unmap(0, &nothingWritten);
}

// Forget everything the meter knows, so nothing read before this moment can be believed after it.
//
// The exposure is written only inside the block that dispatches the meter, and that block does not
// run while the option is off. Nothing used to clear any of this when it stopped, so the reading
// simply froze: switching the option back on returned the value from whenever it was switched off,
// and ResolveWhitePoint took it as current because a held value is exactly what it expects to see.
// GTA V's exposure spans 0.127 to 0.511 in one session, so re-enabling in different light handed the
// encode a white point up to 4x wrong -- which trips the soft knee, scales the model's answer away
// and leaves its hue behind. That is the colour cast, and it looked random because it depends on the
// light at the moment of the PREVIOUS switch-off, which nothing on screen shows.
//
// The readback ring made it worse. `meterFrames` also only advances inside that block, so the four
// slots kept their contents and their valid flags across the gap, and the first frames after
// re-enabling consumed buffers written before it as though they had just arrived.
//
// Zero is not a fallback value here, it is the absence of one: ResolveWhitePoint's `> 1e-6f` guard
// fails and the manual slider is used, which is what a game supplying no exposure already gets.
void InvalidateExposureMeter()
{
    g_nr.gameExposure = 0.0f;

    for (bool& valid : g_nr.meterExposureValid)
        valid = false;

    // Re-arms the `< 4` guard in ConsumeMeterReadback, so nothing is read back until four frames
    // have genuinely been queued since this point.
    g_nr.meterFrames = 0;
}

// Turns what the meter saw into the divisor the encode uses, or falls back to the slider.
//
// `cut` says the exposure may jump rather than drift, and it is the difference between this working
// and not. GTA V's character switch pulls the camera up through the sky: a linear HDR buffer's sky is
// tens of times brighter than the ground, the proxy clips to flat white, and the frame blows out until
// the camera comes back down. Easing across that at two percent a frame takes three and a half
// seconds, which is longer than the transition -- so a meter that only eases would lag through the
// whole thing and fix nothing.
//
// So a cut snaps and a drift eases. Walking out of a cave is a drift; a camera cut is not, and
// pretending otherwise to avoid pumping just moves the failure somewhere more visible.
float ResolveWhitePoint(const Config& cfg, bool isHdrBuffer)
{
    const float slider = cfg.DlssNrWhitePointScale.value_or_default();

    // A frame the game already tone mapped is display-referred: white is at 1 by definition and there
    // is nothing to measure. The slider stays available as a manual exposure on that path.
    if (!isHdrBuffer)
        return slider;

    // The game's own exposure, where it supplies one.
    //
    // Exposure is the step that makes a cave and a field comparable: the renderer works in arbitrary
    // scene-referred units and multiplies by this before tone mapping, which is precisely why one
    // fixed paper white cannot serve both. FSR spells the relationship out -- frame / preExposure *
    // exposure -- so undoing it gives the divisor this pass wants, and paper white becomes a constant
    // on top rather than a value chasing the scene.
    //
    // Unlike anything measured off the frame this cannot be moved by what the pass writes, which is
    // what killed the statistical meter. It is the game's number, decided upstream.
    //
    // Held across the frames where the texture is absent -- GTA V dropped it three times in one
    // session -- because falling back to a default on those frames is a flicker, not a fallback.
    // The scan's anchor, where the game supplies no exposure of its own.
    //
    // Only ratios are used, so the units of the buffer never have to be known -- which is the whole
    // reason this is anchored rather than absolute. The anchor is the user's own white point at the
    // moment they pressed the button; everything after that is the scan moving it.
    //
    // Deliberately below the exposure texture in priority and mutually exclusive with it in the
    // menu. A game that hands over a real exposure has no business being driven by a buffer found by
    // its shape, and two sources fighting over one number is the class of bug worth making
    // unreachable rather than merely unlikely.
    if (cfg.DlssNrWhitePointSource.value_or_default() == 2)
    {
        // Multi-point: one or more calibration points the user placed, interpolated in log space by
        // the current scan value. One point is the original ratio law; more fit the buffer's actual
        // relationship so the white point holds across the whole range, not only near one anchor.
        const float w = DlssNr::ExposureScan::AnchoredWhitePoint(
            DlssNr::ExposureScan::BestValue(), cfg.DlssNrScanInverted.value_or_default(),
            cfg.DlssNrScanTrim.value_or_default());

        if (w > 0.0f)
            return w;
    }

    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && g_nr.gameExposure > 1e-6f)
    {
        // Its own setting, not the manual divisor. See Config: they are different quantities with
        // different units and different sensible ranges, and sharing one value meant adjusting the
        // trim destroyed the divisor somebody had found by hand.
        //
        // Still bounded at the point of use rather than only in the menu that draws it.
        //
        // Bounding it at the slider would have been cosmetic: someone who found 64 by hand on the
        // manual path and then switched the exposure source on keeps that 64 in their ini, and the
        // composition would go on reading it until they happened to touch the control. The picture
        // would be wrong for a reason the menu was no longer showing.
        //
        // Their value is left in the config untouched, so switching back to manual restores the
        // number they arrived at. It is only what this path consumes that is limited.
        const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);

        return std::clamp(g_nr.gamePreExposure / g_nr.gameExposure * trim, 0.01f, 4096.0f);
    }

    // Otherwise the slider, and only the slider.
    //
    // Measuring white from the frame was tried and removed. It could not be made to work because the
    // pass writes the frame it measures: in Enshrouded one session walked the divisor from 0.010 to
    // 97.910, and toggling NR at a fixed spot read 41.31 off and 0.46 on. Two attempts to damp it --
    // a relative lit threshold, then a rate limit with a cut snap -- both treated a coupled system as
    // a noisy one and neither held. A constant cannot do that, which is the whole argument for it,
    // and is what RenoDX has always done.
    return slider;
}

ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width,
                              unsigned int height)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // The model writes its result, so the destination has to be a UAV.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res));
    return res;
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &b);
}

// A typeless resource cannot be viewed, and NGX builds its own views with nothing to tell it which
// format to use. Depth is very often declared typeless, so the typed member of the same family is
// substituted; CopyResource accepts that as a destination for the typeless original.
DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

bool IsTypeless(DXGI_FORMAT f) { return TypedGuideFormat(f) != f; }

// Creates a typed twin of a guide buffer, matching everything but the format.
ID3D12Resource* CreateGuideClone(ID3D12Device* device, ID3D12Resource* source)
{
    D3D12_RESOURCE_DESC desc = source->GetDesc();
    desc.Format = TypedGuideFormat(desc.Format);
    desc.MipLevels = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                    nullptr, IID_PPV_ARGS(&res));
    return res;
}

// Hands back something the model can actually read: the guide itself when it is typed, or a typed copy
// of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time, which is
// a documented contract rather than a guess about any one game's frame graph, so that is the state
// transitioned away from and back to here.
// Freezing is a diagnostic, and it reuses this function because the clone it already keeps is
// exactly the thing a frozen guide is: a private copy the model reads instead of the live resource.
// Freezing is then not a new mechanism but the absence of one -- stop refreshing the copy.
//
// A frozen guide is valid data that is wrong for this frame, which is a far better probe than a
// constant would be. A constant is degenerate and a model may special-case it; stale depth is
// ordinary depth that simply disagrees with the picture, and anything reading it has to notice.
// Hands back something the model can actually read: the guide itself when it is typed, or a typed
// copy of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time,
// which is a documented contract rather than a guess about any one game's frame graph, so that is
// the state transitioned away from and back to here.
ID3D12Resource* ReadableGuide(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                              ID3D12Resource* source, ID3D12Resource** clone)
{
    if (source == nullptr || (!IsTypeless(source->GetDesc().Format) && source->GetDesc().MipLevels == 1))
        return source;

    // A dynamic-resolution game reallocates its depth and motion vectors as the render size moves, so
    // the clone made for the old size no longer matches -- and CopyResource demands identical
    // dimensions. Copying a 1970x1108 source into a 984x554 clone is undefined and removes the device,
    // which is the DRS crash. Rebuild the clone whenever the source's shape has changed under it.
    if (*clone != nullptr)
    {
        const D3D12_RESOURCE_DESC have = (*clone)->GetDesc();
        const D3D12_RESOURCE_DESC want = source->GetDesc();

        if (have.Width != want.Width || have.Height != want.Height ||
            have.Format != TypedGuideFormat(want.Format))
        {
            // Retired, not released: the previous copy may still be in flight on the game's queue.
            ParkNrResource(*clone);
        }
    }

    if (*clone == nullptr)
    {
        *clone = CreateGuideClone(device, source);

        if (*clone == nullptr)
            return nullptr;

        LOG_DEBUG("DLSS-NR cloned a typeless guide as format {}",
                  (int) TypedGuideFormat(source->GetDesc().Format));
    }

    Barrier(cmdList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    if ((*clone)->GetDesc().MipLevels == source->GetDesc().MipLevels)
    {
        cmdList->CopyResource(*clone, source);
    }
    else
    {
        const D3D12_RESOURCE_DESC d = (*clone)->GetDesc();
        if (d.Width > 0 && d.Height > 0)
        {
            D3D12_TEXTURE_COPY_LOCATION dstLoc { *clone, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            dstLoc.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION srcLoc { source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            srcLoc.SubresourceIndex = 0;
            const D3D12_BOX box { 0, 0, 0, static_cast<UINT>(d.Width), static_cast<UINT>(d.Height), 1 };
            cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, &box);
        }
    }
    Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, *clone, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return *clone;
}

// The upscaler's own names differ between super resolution and ray reconstruction, and only one set is
// present on any given block.
// Whether a surface can physically hold linear HDR.
//
// Only a float format can: linear light is open-ended and runs far past 1.0, which a normalised
// integer surface cannot represent. An 8-bit UNORM frame is finished, display-referred output, and
// so is a 10-bit one -- HDR10 is PQ-encoded, which is display-referred too.
//
// The game's IsHDR flag is a statement of intent that is not always true, and believing it over a
// format that cannot hold linear light means encoding an already-encoded frame a second time.
bool FormatCanHoldLinearHdr(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
{
    ID3D12Resource* res = nullptr;

    if (params->Get(a, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    res = nullptr;

    if (params->Get(b, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    // The same key again, as a plain pointer.
    //
    // NVSDK_NGX_Parameter has a typed setter per resource kind and an untyped one, and on a real NGX
    // parameter block those are separate slots: what goes in through Set(name, void*) does not come
    // back out of Get(name, ID3D12Resource**). A game running its own D3D12 upscaler sets these
    // typed, so the typed read above is enough and always was.
    //
    // Both of OptiScaler's bridges write them untyped. IFeature_Dx11wDx12 and IFeature_VkwDx12 turn
    // the game's D3D11 textures or Vulkan images into D3D12 resources and hand them over with
    // Set(name, (void*) resource) -- so the typed read came back null a few lines after the resource
    // had been written, and the pass quietly did nothing. That is the whole reason this never ran in
    // a DirectX 11 or Vulkan game.
    void* untyped = nullptr;

    if (params->Get(a, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    untyped = nullptr;

    if (params->Get(b, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    return nullptr;
}

// A change has to hold still before it is acted on: a slider being dragged reports a new value every
// frame, and each one would otherwise mean a new model.
constexpr unsigned long long kSettleFrames = 30;

// The extras the official integration sets: global tone (read at create) and the interface inputs.
// Written before every create and evaluate, nulls included, so nothing stale ever sits in the block.
void SetExtras(const Config& cfg, ID3D12Resource* ui, ID3D12Resource* backbuffer, unsigned int uiWidth,
               unsigned int uiHeight, unsigned int bbWidth, unsigned int bbHeight)
{
    if (g_nr.setExtras == nullptr || g_nr.capabilityParams == nullptr)
        return;

    // Global tone is written at the model's own default: the control that exposed it changed nothing
    // that could be seen, and the block persists, so a value still has to be put there.
    g_nr.setExtras(g_nr.capabilityParams, 1.0f, ui, ui, backbuffer,
                   uiWidth, uiHeight, bbWidth, bbHeight);
}

uint32_t GetConfiguredPassPreset(const Config& cfg, unsigned int pass)
{
    switch (pass)
    {
    case 1: return cfg.DlssNrPass2Preset.value_or_default();
    case 2: return cfg.DlssNrPass3Preset.value_or_default();
    case 3: return cfg.DlssNrPass4Preset.value_or_default();
    case 4: return cfg.DlssNrPass5Preset.value_or_default();
    case 5: return cfg.DlssNrPass6Preset.value_or_default();
    case 6: return cfg.DlssNrPass7Preset.value_or_default();
    case 7: return cfg.DlssNrPass8Preset.value_or_default();
    case 8: return cfg.DlssNrPass9Preset.value_or_default();
    case 9: return cfg.DlssNrPass10Preset.value_or_default();
    default: return 0;
    }
}

uint32_t GetConfiguredPassStyle(const Config& cfg, unsigned int pass)
{
    switch (pass)
    {
    case 1: return cfg.DlssNrPass2Style.value_or_default();
    case 2: return cfg.DlssNrPass3Style.value_or_default();
    case 3: return cfg.DlssNrPass4Style.value_or_default();
    case 4: return cfg.DlssNrPass5Style.value_or_default();
    case 5: return cfg.DlssNrPass6Style.value_or_default();
    case 6: return cfg.DlssNrPass7Style.value_or_default();
    case 7: return cfg.DlssNrPass8Style.value_or_default();
    case 8: return cfg.DlssNrPass9Style.value_or_default();
    case 9: return cfg.DlssNrPass10Style.value_or_default();
    default: return 0;
    }
}

bool TuningMatchesFeature(const Config& cfg)
{
    if (g_nr.builtPreset != cfg.DlssNrPreset.value_or_default() ||
        g_nr.builtIntensity != cfg.DlssNrIntensity.value_or_default() ||
        g_nr.builtStyle != cfg.DlssNrStyle.value_or_default() ||
        g_nr.builtLocalStructure != cfg.DlssNrLocalStructure.value_or_default() ||
        g_nr.builtLocalTone != cfg.DlssNrLocalTone.value_or_default() ||
        g_nr.builtSkinStructure != cfg.DlssNrSkinStructure.value_or_default() ||
        g_nr.builtAutoMask != cfg.DlssNrAutoMask.value_or_default())
        return false;
    for (unsigned int pass = 1; pass < 10; ++pass)
    {
        if (g_nr.builtPassPreset[pass] != GetConfiguredPassPreset(cfg, pass) ||
            g_nr.builtPassStyle[pass] != GetConfiguredPassStyle(cfg, pass))
            return false;
    }
    return true;
}

void RecordBuiltTuning(const Config& cfg)
{
    g_nr.builtPreset = cfg.DlssNrPreset.value_or_default();
    g_nr.builtIntensity = cfg.DlssNrIntensity.value_or_default();
    g_nr.builtStyle = cfg.DlssNrStyle.value_or_default();
    g_nr.builtLocalStructure = cfg.DlssNrLocalStructure.value_or_default();
    g_nr.builtLocalTone = cfg.DlssNrLocalTone.value_or_default();
    g_nr.builtSkinStructure = cfg.DlssNrSkinStructure.value_or_default();
    g_nr.builtAutoMask = cfg.DlssNrAutoMask.value_or_default();
    for (unsigned int pass = 1; pass < 10; ++pass)
    {
        g_nr.builtPassPreset[pass] = GetConfiguredPassPreset(cfg, pass);
        g_nr.builtPassStyle[pass] = GetConfiguredPassStyle(cfg, pass);
    }
}

int ResolvePassPreset(const Config& cfg, unsigned int pass)
{
    const uint32_t basePreset = std::min(cfg.DlssNrPreset.value_or_default(), 3u);
    if (pass >= 1 && pass < 10)
    {
        const uint32_t p = GetConfiguredPassPreset(cfg, pass);
        return p == 1 ? 1 : (p == 2 ? 2 : (int) basePreset);
    }
    return (int) basePreset;
}

int ResolvePassStyle(const Config& cfg, unsigned int pass)
{
    const uint32_t baseStyle = std::min(cfg.DlssNrStyle.value_or_default(), 2u);
    if (pass >= 1 && pass < 10)
    {
        const uint32_t s = GetConfiguredPassStyle(cfg, pass);
        return (s >= 1 && s <= 3) ? (int) (s - 1) : (int) baseStyle;
    }
    return (int) baseStyle;
}

bool HaloEnabled(const Config& cfg)
{
    const float dark = cfg.DlssNrHaloSuppression.value_or_default();
    const float bright = cfg.DlssNrHaloBrightSuppression.value_or_default();
    return (std::isfinite(dark) && dark > 0.0f) || (std::isfinite(bright) && bright > 0.0f);
}

void ResolveNrRenderDimensions(NVSDK_NGX_Parameter* params, unsigned int& width, unsigned int& height)
{
    if (params != nullptr && (width == 0 || height == 0))
    {
        params->Get(NVSDK_NGX_Parameter_Width, &width);
        params->Get(NVSDK_NGX_Parameter_Height, &height);
    }
    if (width == 0 || height == 0)
    {
        auto* currentFeature = State::Instance().currentFeature;
        if (currentFeature != nullptr && currentFeature->RenderWidth() > 0 && currentFeature->RenderHeight() > 0)
        {
            width = currentFeature->RenderWidth();
            height = currentFeature->RenderHeight();
        }
    }
}

void UnusedRecordStub()
{
}

// Guards the module's state. Every caller is now on the game's render thread, so this is no longer
// holding two threads apart -- but the D3D11-on-D3D12 bridge enters from its own call site, and the
// cost is a CPU-side lock on a path that already records command lists.
std::mutex g_nrMutex;

// Runs the pass inside the same state envelope every other OptiScaler compute pass runs in.
//
// The upscaler's own evaluate is wrapped like this by TryEvaluateOptiFeature: root-signature tracking
// off so the hooks do not record the pass's binds as the game's, heap capture skipped, and RestoreRoot
// afterwards to put the game's compute state back. Neural Rendering ran outside that envelope -- after
// the upscaler had already restored and re-armed -- so it left its own root signature and descriptor
// heaps bound and captured. On an ordinary engine the game rebinds and never notices. On a bindless
// engine (007 First Light, Monster Hunter Wilds, and the rest of the RestoreComputeSig* quirks) the
// game resumes off the pass's bindings and the device is removed.
//
// As RAII so every early return from the pass is covered. RestoreRoot is gated internally on the
// RestoreComputeSignature / RestoreGraphicSignature config, so this is a no-op on games that do not
// ask for it and only acts where it is needed.
struct ScopedNrStateEnvelope
{
    ID3D12GraphicsCommandList* cmd;
    ScopedSkipHeapCapture skipHeap;

    explicit ScopedNrStateEnvelope(ID3D12GraphicsCommandList* c) : cmd(c)
    {
        D3D12Hooks::SetRootSignatureTracking(false);
    }

    ~ScopedNrStateEnvelope()
    {
        D3D12Hooks::RestoreRoot(cmd);
        D3D12Hooks::SetRootSignatureTracking(true);
    }
};

// Every way out of the pass before it does anything is silent on purpose -- an evaluate that carries
// no depth is normal and would otherwise print every frame forever. That silence is fine until the
// pass does nothing at all and the log has no opinion about why.
//
// So each distinct reason is reported once. Once, not once per frame.
void ReportSkipOnce(const char* reason)
{
    static std::set<std::string> seen;

    if (seen.insert(reason).second)
        LOG_INFO("DLSS-NR did not run: {}", reason);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The pass itself. Everything above is what it is made of; everything below is the shape the rest
// of OptiScaler sees.
// ---------------------------------------------------------------------------------------------

DlssNr_Dx12::DlssNr_Dx12(std::string InName, ID3D12Device* InDevice)
    : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    // Five inputs, two outputs, one constant buffer, and a clamped linear sampler.
    //
    // The sampler exists because the model may be run below full resolution, in which case its answer
    // has to be read back at a different size from the frame it is being transferred onto.
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    if (!SetupRootSignature(InDevice, kSrvCount, kUavCount, 1, 0, 0, 1, &sampler))
    {
        LOG_ERROR("[{0}] Failed to setup root signature", _name);
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(DlssNrConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (uint32_t i = 0; i < DLSSNR_NUM_OF_HEAPS; ++i)
    {
        auto result = InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                        IID_PPV_ARGS(&_constantBuffers[i]));

        if (result != S_OK)
        {
            LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
            return;
        }
    }

    // Precompiled, with no source fallback. The shader used to be compiled at runtime from a string,
    // which would have meant no shader at all for anyone leaving UsePrecompiledShaders at its
    // default.
    if (!CreateComputePipeline(InDevice, &_pipelineState, DlssNr_cso, sizeof(DlssNr_cso), nullptr))
    {
        LOG_ERROR("[{0}] Failed to create the compute pipeline", _name);
        return;
    }

    // Second PSO for the ResidualAcrossRR v2 accumulator (its own blob, same root signature).
    // A failure here is not fatal to the class -- only that experimental mode goes unavailable.
    if (!CreateComputePipeline(InDevice, &_residualPipelineState, dlssnr_residual_cso, sizeof(dlssnr_residual_cso),
                               nullptr))
    {
        _residualPipelineState = nullptr;
        LOG_WARN("[{0}] ResidualAcrossRR compute pipeline unavailable", _name);
    }

    _init = InitHeaps(InDevice, _frameHeaps, DLSSNR_NUM_OF_HEAPS);
}

bool DlssNr_Dx12::DispatchPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                                  ID3D12Resource* InSource, ID3D12Resource* InModel,
                                  ID3D12Resource* InOriginal, ID3D12Resource* InMotion,
                                  ID3D12Resource* InPrevEdit, ID3D12Resource* OutTarget,
                                  ID3D12Resource* OutKeep)
{
    if (!_init || InCmdList == nullptr || _device == nullptr || InSource == nullptr || OutTarget == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    // Every slot in the table gets a view, whether the mode reads it or not. An unbound descriptor is
    // not an empty read; it is a read from nothing, and the source stands in wherever a mode has
    // nothing of its own to put there.
    ID3D12Resource* const srvs[kSrvCount] = {
        InSource,
        InModel != nullptr ? InModel : InSource,
        InOriginal != nullptr ? InOriginal : InSource,
        InMotion != nullptr ? InMotion : InSource,
        InPrevEdit != nullptr ? InPrevEdit : InSource,
    };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = {
        OutTarget,
        OutKeep != nullptr ? OutKeep : OutTarget,
    };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    // Sized from the constants rather than from a resource, because the pass that shrinks the proxy
    // writes fewer pixels than its source has.
    const UINT dispatchWidth = (InConstants.Width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (InConstants.Height + _numThreadsY - 1) / _numThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

DlssNr_Dx12::~DlssNr_Dx12()
{
    for (auto& buffer : _constantBuffers)
    {
        if (buffer != nullptr)
        {
            buffer->Release();
            buffer = nullptr;
        }
    }

    if (_residualPipelineState != nullptr)
    {
        _residualPipelineState->Release();
        _residualPipelineState = nullptr;
    }
}

bool DlssNr_Dx12::DispatchResidualPass(ID3D12GraphicsCommandList* InCmdList, const DlssNrConstants& InConstants,
                                       ID3D12Resource* InSource, ID3D12Resource* InModel,
                                       ID3D12Resource* InOriginal, ID3D12Resource* InMotion,
                                       ID3D12Resource* OutTarget)
{
    if (!_init || _residualPipelineState == nullptr || InCmdList == nullptr || _device == nullptr ||
        InSource == nullptr || OutTarget == nullptr)
        return false;

    const uint32_t slot = _heapIndex;
    _heapIndex = (_heapIndex + 1) % DLSSNR_NUM_OF_HEAPS;

    FrameDescriptorHeap& currentHeap = _frameHeaps[slot];

    ID3D12Resource* const srvs[kSrvCount] = {
        InSource,
        InModel != nullptr ? InModel : InSource,
        InOriginal != nullptr ? InOriginal : InSource,
        InMotion != nullptr ? InMotion : InSource,
        InSource,
    };

    for (uint32_t i = 0; i < kSrvCount; ++i)
        CreateShaderResourceView(_device, srvs[i], currentHeap.GetSrvCPU(i));

    ID3D12Resource* const uavs[kUavCount] = { OutTarget, OutTarget };

    for (uint32_t i = 0; i < kUavCount; ++i)
        CreateUnorderedAccessView(_device, uavs[i], currentHeap.GetUavCPU(i), 0);

    if (!CreateConstantsBuffer(_device, _constantBuffers[slot], InConstants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);
    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_residualPipelineState);
    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    const UINT dispatchWidth = (InConstants.Width + _numThreadsX - 1) / _numThreadsX;
    const UINT dispatchHeight = (InConstants.Height + _numThreadsY - 1) / _numThreadsY;
    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

void DlssNr_Dx12::Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour,
                           ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output,
                           const DlssNrFrameInfo& frame, ID3D12CommandQueue* timingQueue)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);
    const Config& cfg = *Config::Instance();

    if (DlssNr::DriverGate::Blocked())
    {
        g_nr.failed = true;
        g_nr.reason = "NVIDIA driver is older than 610.88 (616.64+ recommended)";
        LOG_ERROR("DLSS-NR (driver): {}", DlssNr::DriverGate::BlockedMessage(DlssNr::DriverGate::DetectedVersion()));
        return;
    }

    if (g_nr.failed || cmdList == nullptr || colour == nullptr || depth == nullptr ||
        motion == nullptr || output == nullptr)
    {
        ReportSkipOnce(g_nr.failed ? "it already failed this session" : "a resource was missing");
        return;
    }

    ID3D12Resource* target = output;

    // A completed upscaler output normally arrives as a UAV. The pre-SR colour input instead arrives
    // readable. Track every transition so both paths return the resource exactly as their caller gave
    // it to us; a pre-SR resource without UAV support is written through a scratch-and-copy fallback.
    const D3D12_RESOURCE_STATES outputArrival =
        frame.BeforeUpscale
            ? (Config::Instance()->ColorResourceBarrier.has_value()
                   ? (D3D12_RESOURCE_STATES) Config::Instance()->ColorResourceBarrier.value()
                   : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
            : (Config::Instance()->OutputResourceBarrier.has_value()
                   ? (D3D12_RESOURCE_STATES) Config::Instance()->OutputResourceBarrier.value()
                   : D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_STATES targetState = outputArrival;
    const auto TransitionTarget = [&](D3D12_RESOURCE_STATES to)
    {
        Barrier(cmdList, target, targetState, to);
        targetState = to;
    };

    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    const D3D12_RESOURCE_DESC desc = target->GetDesc();
    const auto width = (unsigned int) desc.Width;
    const auto height = desc.Height;
    const bool targetSupportsUav = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
    const bool residualAcrossRr = frame.ResidualAcrossRr && frame.BeforeUpscale;

    const D3D12_RESOURCE_DESC guideDesc = depth->GetDesc();
    unsigned int guideWidth = (unsigned int) guideDesc.Width;
    unsigned int guideHeight = guideDesc.Height;

    if (guideWidth == 0 || guideHeight == 0)
    {
        guideWidth = width;
        guideHeight = height;
    }

    const D3D12_RESOURCE_DESC motionDesc = motion->GetDesc();
    unsigned int motionWidth = (unsigned int) motionDesc.Width;
    unsigned int motionHeight = motionDesc.Height;
    if (motionWidth == 0 || motionHeight == 0)
    {
        motionWidth = guideWidth;
        motionHeight = guideHeight;
    }

    if (frame.RenderSubrectWidth != 0 && frame.RenderSubrectHeight != 0)
    {
        const unsigned int subW = std::min(frame.RenderSubrectWidth, guideWidth);
        const unsigned int subH = std::min(frame.RenderSubrectHeight, guideHeight);

        if (subW != guideWidth || subH != guideHeight)
        {
            static unsigned int saidW = 0, saidH = 0;

            if (saidW != subW || saidH != subH)
            {
                saidW = subW;
                saidH = subH;
                LOG_INFO("DLSS-NR guides: the game renders {}x{} into a {}x{} texture, so the model is "
                         "told the smaller number",
                         subW, subH, guideWidth, guideHeight);
            }
        }

        guideWidth = subW;
        guideHeight = subH;
        motionWidth = std::min(frame.RenderSubrectWidth, motionWidth);
        motionHeight = std::min(frame.RenderSubrectHeight, motionHeight);
    }

    g_nr.guideWidth = guideWidth;
    g_nr.guideHeight = guideHeight;
    g_nr.guideDepthInverted = frame.DepthInverted;

    g_nr.guideMvScaleX = frame.MvScaleX;
    g_nr.guideMvScaleY = frame.MvScaleY;

    if (frame.Reset)
    {
        g_nr.reset = true;
        g_nr.residualHistoryPrimed = false;
        g_nr.residualStoreValid = false;

        static unsigned long long resets = 0;
        ++resets;

        if (resets <= 3 || resets % 100 == 0)
            LOG_INFO("DLSS-NR: the game asked for a history reset ({} so far)", resets);
    }

    struct GuideReport
    {
        bool valid;
        bool depthInverted;
        float mvScaleX;
        float mvScaleY;
        unsigned int guideW;
        unsigned int guideH;
        unsigned int frameW;
        unsigned int frameH;
    };

    static GuideReport loggedGuides {};

    const GuideReport guidesNow { true,       g_nr.guideDepthInverted, g_nr.guideMvScaleX,
                                  g_nr.guideMvScaleY, guideWidth,      guideHeight,
                                  width,      (unsigned int) height };

    if (!loggedGuides.valid || loggedGuides.depthInverted != guidesNow.depthInverted ||
        loggedGuides.mvScaleX != guidesNow.mvScaleX || loggedGuides.mvScaleY != guidesNow.mvScaleY ||
        loggedGuides.guideW != guidesNow.guideW || loggedGuides.guideH != guidesNow.guideH ||
        loggedGuides.frameW != guidesNow.frameW || loggedGuides.frameH != guidesNow.frameH)
    {
        loggedGuides = guidesNow;
        LOG_INFO("DLSS-NR guides: depth {}, motion vector scale {} x {}, guides {}x{} for a {}x{} frame",
                 g_nr.guideDepthInverted ? "inverted" : "not inverted", g_nr.guideMvScaleX,
                 g_nr.guideMvScaleY, guideWidth, guideHeight, width, height);
    }

    const bool restoreRequired = cfg.RestoreComputeSignature.value_or_default() ||
                                 cfg.RestoreGraphicSignature.value_or_default();

    if (restoreRequired && !D3D12Hooks::CanRestoreRootSignature(cmdList))
    {
        ReportSkipOnce("the upscaler could not restore state this frame");
        device->Release();
        return;
    }

    ScopedNrStateEnvelope stateEnvelope(cmdList);

    if (cfg.DlssNrProxyProbe.value_or_default())
        ProbeProxyDispatch(cmdList);

    if (!EnsureForwarder() || !EnsureCapabilityParams(device))
    {
        g_nr.failed = true;
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    float workScale = cfg.DlssNrWorkingScale.value_or_default();
    if (!std::isfinite(workScale))
        workScale = 1.0f;
    workScale = workScale < 0.25f ? 0.25f : (workScale > 2.0f ? 2.0f : workScale);
    const auto workWidth = (unsigned int) (width * workScale + 0.5f);
    const auto workHeight = (unsigned int) (height * workScale + 0.5f);
    const bool reduced = workWidth != width || workHeight != height;
    const unsigned int configuredPasses = std::clamp(cfg.DlssNrPasses.value_or_default(), 1u, 10u);
    const bool proxyBackend = cfg.DlssNrUseProxy.value_or_default();
    const unsigned int requestedPasses = proxyBackend ? 1u : configuredPasses;

    ReleaseSurfacesIfFormatChanged(desc.Format);

    const bool resolutionChanged = g_nr.width != width || g_nr.height != height ||
                                   g_nr.workWidth != workWidth || g_nr.workHeight != workHeight;
    const bool placementChanged =
        g_nr.feature != nullptr &&
        (g_nr.beforeUpscale != frame.BeforeUpscale || g_nr.rayReconstruction != frame.RayReconstruction);
    const bool tuningChanged = !TuningMatchesFeature(cfg);
    if (tuningChanged)
        g_nr.residualHistoryPrimed = false;

    if (g_nr.feature != nullptr && (resolutionChanged || tuningChanged || placementChanged))
    {
        ParkNrFeature(g_nr.feature);
        g_nr.featurePendingSubmission = false;

        for (unsigned int i = 0; i < 10; ++i)
        {
            ParkNrFeature(g_nr.passFeature[i]);
            g_nr.passNeedsReset[i] = false;
            g_nr.passCreateFailed[i] = false;
            g_nr.passPendingSubmission[i] = false;
        }

        if (resolutionChanged || placementChanged)
        {
            if (placementChanged)
                ForgetCalibration();

            ParkNrResource(g_nr.output);
            ParkNrResource(g_nr.passScratch);
            ParkNrResource(g_nr.colorCopy);
            ParkNrResource(g_nr.hdrCopy);
            ParkNrResource(g_nr.colorSmall);
            ParkNrResource(g_nr.depthSmall);
            ParkNrResource(g_nr.motionSmall);
            g_nr.loggedGuideMatch = false;
            ParkNrResource(g_nr.outputNative);
            ParkNrResource(g_nr.residualEdited);
            ParkNrResource(g_nr.residualHistory[0]);
            ParkNrResource(g_nr.residualHistory[1]);
            ParkNrResource(g_nr.residualComposed);
            g_nr.residualStoreValid = false;
            g_nr.residualHistoryIndex = 0;
            g_nr.residualHistoryPrimed = false;
            g_nr.passScratchFailed = false;
        }
    }

    if (g_nr.output == nullptr)
    {
        g_nr.output = CreateScratch(device, desc.Format, workWidth, workHeight);
        g_nr.colorCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.hdrCopy = CreateScratch(device, desc.Format, width, height);
        g_nr.workWidth = workWidth;
        g_nr.workHeight = workHeight;
    }

    if (residualAcrossRr)
    {
        auto ensureReadable = [&](ID3D12Resource*& resource, DXGI_FORMAT format, unsigned w, unsigned h)
        {
            if (resource == nullptr)
            {
                resource = CreateScratch(device, format, w, h);
                if (resource)
                    Barrier(cmdList, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            return resource != nullptr;
        };
        if (_residualPipelineState == nullptr || !ensureReadable(g_nr.residualEdited, desc.Format, width, height) ||
            !ensureReadable(g_nr.residualHistory[0], DXGI_FORMAT_R16G16B16A16_FLOAT, width, height) ||
            !ensureReadable(g_nr.residualHistory[1], DXGI_FORMAT_R16G16B16A16_FLOAT, width, height) ||
            !ensureReadable(g_nr.residualComposed, g_nr.residualOutputFormat, g_nr.residualOutputWidth,
                            g_nr.residualOutputHeight))
        {
            ReportSkipOnce("RR residual resources unavailable; preserving the game's colour");
            g_nr.residualStoreValid = false;
            g_nr.residualHistoryPrimed = false;
            device->Release();
            return;
        }
    }

    const bool haloActive = HaloEnabled(cfg);
    if (requestedPasses == 1 && !haloActive)
    {
        ParkNrResource(g_nr.passScratch);
        g_nr.passScratchFailed = false;
    }
    else if (g_nr.passScratch == nullptr && !g_nr.passScratchFailed)
    {
        g_nr.passScratch = CreateScratch(device, desc.Format, workWidth, workHeight);
        g_nr.passScratchFailed = g_nr.passScratch == nullptr;

        if (g_nr.passScratchFailed)
            LOG_ERROR("DLSS-NR: could not allocate the model-output ping-pong; extra passes are disabled");
    }

    if (reduced && g_nr.colorSmall == nullptr)
        g_nr.colorSmall = CreateScratch(device, desc.Format, workWidth, workHeight);

    if (workScale > 1.0f && g_nr.outputNative == nullptr)
        g_nr.outputNative = CreateScratch(device, desc.Format, width, height);

    if (g_nr.meter == nullptr)
    {
        g_nr.meter = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, kDlssNrMeterGrid, kDlssNrMeterGrid);

        D3D12_HEAP_PROPERTIES readback {};
        readback.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC bufferDesc {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = kMeterBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        for (auto& rb : g_nr.meterReadback)
        {
            if (FAILED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&rb))))
            {
                rb = nullptr;
                LOG_WARN("DLSS-NR: the white point meter could not allocate its readback; falling back "
                         "to the paper white slider");
            }
        }

        if (g_nr.meter != nullptr)
            LOG_INFO("DLSS-NR: white point meter up, {}x{} tiles", kDlssNrMeterGrid, kDlssNrMeterGrid);
    }

    if (g_nr.feature == nullptr && g_nr.output != nullptr && g_nr.colorCopy != nullptr &&
        g_nr.hdrCopy != nullptr)
    {
        auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

        if (!snippet.has_value())
            snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

        if (!snippet.has_value())
        {
            g_nr.failed = true;
            g_nr.reason = "nvngx_dlssnr.dll was not found beside OptiScaler or the game";
            LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
            device->Release();
            return;
        }

        SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
        g_nr.feature =
            g_nr.create(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                        device, cmdList, g_nr.capabilityParams, workWidth, workHeight,
                        (int) cfg.DlssNrPreset.value_or_default(),
                        cfg.DlssNrIntensity.value_or_default(), ResolvePassStyle(cfg, 0),
                        cfg.DlssNrLocalStructure.value_or_default(), cfg.DlssNrLocalTone.value_or_default(),
                        cfg.DlssNrSkinStructure.value_or_default(),
                        cfg.DlssNrAutoMask.value_or_default() ? 1 : 0,
                        1);

        if (g_nr.feature == nullptr)
        {
            g_nr.featurePendingSubmission = false;
            g_nr.failed = true;
            g_nr.reason = "the model would not initialise";
            const auto initResult = (unsigned int) (g_nr.lastInit != nullptr ? *g_nr.lastInit : 0);
            const auto createResult = (unsigned int) (g_nr.lastCreate != nullptr ? *g_nr.lastCreate : 0);

            LOG_ERROR("DLSS-NR create failed: init 0x{:X} ({}), create 0x{:X} ({})", initResult,
                      NgxResultName(initResult), createResult, NgxResultName(createResult));
            device->Release();
            return;
        }

        g_nr.width = width;
        g_nr.height = height;
        g_nr.beforeUpscale = frame.BeforeUpscale;
        g_nr.rayReconstruction = frame.RayReconstruction;
        g_nr.reset = true;
        g_nr.featurePendingSubmission = true;
        g_nr.featureCreateEpoch = frame.SubmissionEpoch;
        RecordBuiltTuning(cfg);
        LOG_INFO("DLSS-NR running {}: target {}x{}, model {}x{}, guides {}x{} (preset {}, intensity {}, style {}, epoch {})",
                 frame.RayReconstruction ? (frame.BeforeUpscale ? "before RR+SR" : "after RR+SR")
                                         : (frame.BeforeUpscale ? "before SR" : "after SR"),
                 width, height, workWidth, workHeight, guideWidth, guideHeight, g_nr.builtPreset,
                 g_nr.builtIntensity, g_nr.builtStyle, frame.SubmissionEpoch);

        device->Release();
        return;
    }

    if (g_nr.feature == nullptr)
    {
        device->Release();
        return;
    }

    if (g_nr.featurePendingSubmission)
    {
        if (frame.SubmissionEpoch == g_nr.featureCreateEpoch)
        {
            device->Release();
            return;
        }

        g_nr.featurePendingSubmission = false;
        LOG_INFO("DLSS-NR: primary feature ready after submitted epoch {}", g_nr.featureCreateEpoch);
    }

    for (unsigned int pass = 1; pass < 10; ++pass)
    {
        if (pass >= requestedPasses)
        {
            ParkNrFeature(g_nr.passFeature[pass]);
            g_nr.passNeedsReset[pass] = false;
            g_nr.passCreateFailed[pass] = false;
            g_nr.passPendingSubmission[pass] = false;
        }
    }

    for (unsigned int pass = 1; pass < requestedPasses; ++pass)
    {
        if (!g_nr.passPendingSubmission[pass])
            continue;

        if (frame.SubmissionEpoch == g_nr.passCreateEpoch[pass])
        {
            device->Release();
            return;
        }

        g_nr.passPendingSubmission[pass] = false;
        LOG_INFO("DLSS-NR: feature for pass {} ready after submitted epoch {}", pass + 1, g_nr.passCreateEpoch[pass]);
    }

    if (g_nr.passScratch != nullptr)
    {
        for (unsigned int pass = 1; pass < requestedPasses; ++pass)
        {
            if (g_nr.passFeature[pass] != nullptr)
                continue;

            if (g_nr.passCreateFailed[pass])
                break;

            auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");
            if (!snippet.has_value())
                snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

            if (!snippet.has_value())
            {
                g_nr.passCreateFailed[pass] = true;
                LOG_ERROR("DLSS-NR: pass {} feature not built because nvngx_dlssnr.dll disappeared", pass + 1);
            }
            else
            {
                SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);
                // Later passes zero LocalTone to prevent compounding local contrast burn across cascaded passes.
                g_nr.passFeature[pass] = g_nr.create(
                    snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(), device, cmdList,
                    g_nr.capabilityParams, workWidth, workHeight, ResolvePassPreset(cfg, pass),
                    cfg.DlssNrIntensity.value_or_default(), ResolvePassStyle(cfg, pass),
                    cfg.DlssNrLocalStructure.value_or_default(), 0.0f, cfg.DlssNrSkinStructure.value_or_default(),
                    cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, 1);

                if (g_nr.passFeature[pass] != nullptr)
                {
                    g_nr.passNeedsReset[pass] = true;
                    g_nr.passPendingSubmission[pass] = true;
                    g_nr.passCreateEpoch[pass] = frame.SubmissionEpoch;
                    LOG_INFO("DLSS-NR: feature for pass {} built at epoch {}; waiting for submission",
                             pass + 1, frame.SubmissionEpoch);
                }
                else
                {
                    g_nr.passPendingSubmission[pass] = false;
                    g_nr.passCreateFailed[pass] = true;
                    LOG_ERROR("DLSS-NR: feature for pass {} failed to build; using {} ready pass(es)", pass + 1, pass);
                }
            }

            device->Release();
            return;
        }
    }

    const bool gameSaysHdr = frame.ColourIsLinearHdr;
    const bool isHdrBuffer = gameSaysHdr && FormatCanHoldLinearHdr(desc.Format);

    static bool reportedHdr = false;
    static bool reportedHdrValue = false;
    static bool reportedBefore = false;

    if (!reportedHdr || reportedHdrValue != isHdrBuffer || reportedBefore != frame.BeforeUpscale)
    {
        reportedHdr = true;
        reportedHdrValue = isHdrBuffer;
        reportedBefore = frame.BeforeUpscale;
        LOG_INFO("DLSS-NR {} SR: the game's DLSS buffer is {} so the colour transform is {}",
                 frame.BeforeUpscale ? "before" : "after",
                 isHdrBuffer ? "linear HDR" : "already tone-mapped",
                 isHdrBuffer ? "on" : "off");
    }

    const bool haveCodec = IsInit();

    if (!haveCodec)
    {
        g_nr.failed = true;
        g_nr.reason = "the colour codec would not compile";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        device->Release();
        return;
    }

    ++g_frames;
    TickNrRetired();
    CheckCaptureTrigger();

    if (g_captureWriteAtFrame != 0 && g_frames >= g_captureWriteAtFrame)
    {
        g_captureWriteAtFrame = 0;
        const auto captureDir = Util::DllPath().remove_filename() / "dlssnr-capture";
        const auto written = g_capture.write(captureDir);

        if (!written.empty())
            LOG_INFO("DLSS-NR wrote matched before/after frames to {}", written);
    }

    if (g_gpuTime == nullptr)
        g_gpuTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_ngxTime == nullptr)
        g_ngxTime = std::make_unique<GpuTime_Dx12>(device);

    if (g_gpuTime != nullptr)
        g_gpuTime->Start(cmdList);

    const bool exposureSettingOn = cfg.DlssNrWhitePointSource.value_or_default() == 1;

    if (exposureSettingOn && !g_nr.exposureSettingWasOn)
    {
        InvalidateExposureMeter();
        LOG_INFO("DLSS-NR exposure: option switched on, held reading discarded");
    }

    g_nr.exposureSettingWasOn = exposureSettingOn;

    const bool wantExposure = exposureSettingOn && frame.ExposureTexture != nullptr;

    if (g_nr.meter != nullptr && wantExposure)
    {
        DlssNrConstants meterParams {};
        meterParams.Mode = DlssNrMode_Meter;
        meterParams.Width = 1;
        meterParams.Height = 1;

        const D3D12_RESOURCE_STATES priorTargetState = targetState;
        TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        DispatchPass(cmdList, meterParams, target, nullptr, nullptr,
                     (ID3D12Resource*) frame.ExposureTexture, nullptr, g_nr.meter, nullptr);
        TransitionTarget(priorTargetState);

        CopyMeterToReadback(cmdList, device, true);
        ConsumeMeterReadback();
    }

    g_nr.gamePreExposure = frame.PreExposure;

    float whitePoint = ResolveWhitePoint(cfg, isHdrBuffer);

    ID3D12Resource* exposureTex = nullptr;
    uint32_t useGameExposure = 0;
    float exposurePreMul = 0.0f;

    if (cfg.DlssNrWhitePointSource.value_or_default() == 1 && frame.ExposureTexture != nullptr)
    {
        exposureTex = (ID3D12Resource*) frame.ExposureTexture;
        useGameExposure = 1;
        const float trim = std::clamp(cfg.DlssNrWhitePointTrim.value_or_default(), 0.25f, 4.0f);
        exposurePreMul = g_nr.gamePreExposure * trim;
    }

    {
        const bool hold = cfg.DlssNrHoldFrame.value_or_default();

        if (hold)
        {
            const D3D12_RESOURCE_DESC td = target->GetDesc();
            const bool needCapture = !g_nr.heldActive || g_nr.heldColor == nullptr ||
                                     (unsigned int) td.Width != g_nr.heldWidth ||
                                     td.Height != g_nr.heldHeight || td.Format != g_nr.heldFormat;

            if (needCapture)
            {
                if (g_nr.heldColor != nullptr)
                    ParkNrResource(g_nr.heldColor);

                g_nr.heldColor = CreateScratch(device, td.Format, (unsigned int) td.Width, td.Height);

                if (g_nr.heldColor != nullptr)
                {
                    const D3D12_RESOURCE_STATES priorTargetState = targetState;
                    TransitionTarget(D3D12_RESOURCE_STATE_COPY_SOURCE);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_COPY_DEST);
                    cmdList->CopyResource(g_nr.heldColor, target);
                    Barrier(cmdList, g_nr.heldColor, D3D12_RESOURCE_STATE_COPY_DEST,
                            D3D12_RESOURCE_STATE_COPY_SOURCE);
                    TransitionTarget(priorTargetState);

                    g_nr.heldActive = true;
                    g_nr.heldWidth = (unsigned int) td.Width;
                    g_nr.heldHeight = td.Height;
                    g_nr.heldFormat = td.Format;
                    g_nr.heldWhitePoint = whitePoint;
                }
            }
            else
            {
                const D3D12_RESOURCE_STATES priorTargetState = targetState;
                TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
                cmdList->CopyResource(target, g_nr.heldColor);
                TransitionTarget(priorTargetState);
            }

            if (g_nr.heldActive)
                whitePoint = g_nr.heldWhitePoint;
        }
        else if (g_nr.heldActive)
        {
            if (g_nr.heldColor != nullptr)
                ParkNrResource(g_nr.heldColor);
            g_nr.heldActive = false;
        }
    }

    DlssNrConstants encodeParams {};
    encodeParams.Mode = DlssNrMode_Encode;
    encodeParams.Passthrough = isHdrBuffer ? 0u : 1u;
    encodeParams.WhitePoint = whitePoint;
    encodeParams.UseGameExposure = useGameExposure;
    encodeParams.ExposurePreMul = exposurePreMul;
    encodeParams.AntiStarvation = cfg.DlssNrAntiStarvation.value_or_default() ? 1u : 0u;
    encodeParams.ExposureBalance = cfg.DlssNrExposureBalance.value_or_default();
    encodeParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
    encodeParams.Width = width;
    encodeParams.Height = height;

    TransitionTarget(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DispatchPass(cmdList, encodeParams, target, nullptr, nullptr, nullptr, exposureTex,
                        g_nr.colorCopy, g_nr.hdrCopy);

    if (targetSupportsUav && !residualAcrossRr)
        TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12Resource* modelInput = g_nr.colorCopy;

    if (reduced && g_nr.colorSmall != nullptr)
    {
        bool built = false;

        if (workScale > 1.0f)
        {
            const Scaler nrScaler = cfg.DlssNrScalingDownscaler.value_or_default();
            if (g_nr.nrScaler != nrScaler)
            {
                if (g_nr.superUp != nullptr)   { delete g_nr.superUp;   g_nr.superUp = nullptr; }
                if (g_nr.superDown != nullptr) { delete g_nr.superDown; g_nr.superDown = nullptr; }
                g_nr.nrScaler = nrScaler;
            }
            if (g_nr.superUp == nullptr)
                g_nr.superUp = new OS_Dx12("DLSS-NR supersample up", device, true, nrScaler);
            if (g_nr.superDown == nullptr)
                g_nr.superDown = new OS_Dx12("DLSS-NR supersample down", device, false, nrScaler);

            if (g_nr.superUp != nullptr &&
                g_nr.superUp->Dispatch(cmdList, g_nr.colorCopy, g_nr.colorSmall))
            {
                Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                built = true;
            }
        }

        if (!built)
        {
            if (workScale > 1.0f)
            {
                static bool warnedSuper = false;
                if (!warnedSuper)
                {
                    warnedSuper = true;
                    LOG_WARN("DLSS-NR supersample: upscaler unavailable, falling back to a blocky enlarge.");
                }
            }

            DlssNrConstants down {};
            down.Mode = DlssNrMode_Downsample;
            down.Width = workWidth;
            down.Height = workHeight;
            DispatchPass(cmdList, down, modelInput, nullptr, nullptr, nullptr, nullptr,
                                g_nr.colorSmall, nullptr);
            Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        modelInput = g_nr.colorSmall;
    }

    DlssNr::ExposureScan::Tick(device, cmdList);

    ID3D12Resource* depthIn = ReadableGuide(device, cmdList, depth, &g_nr.depthClone);
    ID3D12Resource* motionIn = ReadableGuide(device, cmdList, motion, &g_nr.motionClone);

    if (depthIn == nullptr || motionIn == nullptr)
    {
        g_nr.failed = true;
        g_nr.reason = "the game's depth or motion vectors could not be made readable";
        LOG_ERROR("DLSS-NR unavailable: {}", g_nr.reason);
        TransitionTarget(outputArrival);
        device->Release();
        return;
    }

    bool matchedGuides = false;
    if (reduced && workWidth < width && cfg.DlssNrMatchGuides.value_or_default())
    {
        if (g_nr.depthSmall == nullptr)
            g_nr.depthSmall = CreateScratch(device, DXGI_FORMAT_R32_FLOAT, workWidth, workHeight);
        if (g_nr.motionSmall == nullptr)
            g_nr.motionSmall = CreateScratch(device, DXGI_FORMAT_R32G32_FLOAT, workWidth, workHeight);
        if (g_nr.depthSmall != nullptr && g_nr.motionSmall != nullptr)
        {
            DlssNrConstants resize {};
            resize.Mode = DlssNrResidualMode_ResampleGuide;
            resize.Width = workWidth;
            resize.Height = workHeight;
            resize.GuideWidth = guideWidth;
            resize.GuideHeight = guideHeight;
            const bool okDepth =
                DispatchResidualPass(cmdList, resize, depthIn, nullptr, nullptr, nullptr, g_nr.depthSmall);
            const bool okMotion =
                okDepth && DispatchResidualPass(cmdList, resize, motionIn, nullptr, nullptr, nullptr, g_nr.motionSmall);
            if (okMotion)
            {
                Barrier(cmdList, g_nr.depthSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(cmdList, g_nr.motionSmall, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                depthIn = g_nr.depthSmall;
                motionIn = g_nr.motionSmall;
                matchedGuides = true;
            }
        }
        if (!g_nr.loggedGuideMatch)
        {
            g_nr.loggedGuideMatch = true;
            if (matchedGuides)
                LOG_INFO("DLSS-NR guides matched to the working size: depth and motion {}x{} for a {}x{} model "
                         "(the frame's guides are {}x{})",
                         workWidth, workHeight, workWidth, workHeight, guideWidth, guideHeight);
            else
                LOG_WARN("DLSS-NR guides could not be matched to the working size; the model keeps the frame's "
                         "{}x{} guides for its {}x{} colour",
                         guideWidth, guideHeight, workWidth, workHeight);
        }
    }

    const unsigned int evalGuideWidth = matchedGuides ? workWidth : guideWidth;
    const unsigned int evalGuideHeight = matchedGuides ? workHeight : guideHeight;
    const bool renderMotionScale = cfg.DlssNrRenderMotionScale.value_or_default();
    const auto motionScale = DlssNrModelMotionScale(
        { renderMotionScale, matchedGuides, frame.MotionVectorsLowResolution, workWidth, workHeight,
          guideWidth, guideHeight, width, height, frame.RenderSubrectWidth, frame.RenderSubrectHeight,
          frame.OutputWidth, frame.OutputHeight });
    const float mvToWorkX = motionScale.x;
    const float mvToWorkY = motionScale.y;
    if (g_nr.loggedMotionReference != motionScale.referenceW ||
        g_nr.loggedMotionTexture != motionScale.textureW || g_nr.loggedMotionWork != workWidth)
    {
        g_nr.loggedMotionReference = motionScale.referenceW;
        g_nr.loggedMotionTexture = motionScale.textureW;
        g_nr.loggedMotionWork = workWidth;
        LOG_INFO("DLSS-NR model motion scale {:.1f} x {:.1f}: game scale {} x {} measured against {}x{} ({}), "
                 "motion texture {}x{} ({}), model {}x{}",
                 g_nr.guideMvScaleX * mvToWorkX, g_nr.guideMvScaleY * mvToWorkY, g_nr.guideMvScaleX, g_nr.guideMvScaleY,
                 motionScale.referenceW, motionScale.referenceH,
                 !renderMotionScale                 ? "frame size, RenderMotionScale=false"
                 : frame.MotionVectorsLowResolution ? "render size, low-resolution vectors"
                                                    : "output size",
                 motionScale.textureW, motionScale.textureH,
                 matchedGuides ? "matched to the working size" : "the game's region", workWidth, workHeight);
    }

    SetExtras(cfg, nullptr, nullptr, 0, 0, 0, 0);

    if (cfg.DlssNrUseProxy.value_or_default())
    {
        const unsigned int proxyResult = DlssNr::Proxy::Run(
            cmdList, device, modelInput, depthIn, motionIn, g_nr.output, workWidth, workHeight,
            evalGuideWidth, evalGuideHeight, g_nr.guideDepthInverted, g_nr.reset,
            g_nr.guideMvScaleX * mvToWorkX, g_nr.guideMvScaleY * mvToWorkY);

        g_nr.reset = false;

        if (proxyResult != 1)
        {
            g_nr.failed = true;
            g_nr.reason = "the proxy path could not run the model";
            LOG_ERROR("DLSS-NR (proxy): evaluate returned 0x{:X} ({}), disabling for this session",
                      proxyResult, NgxResultName(proxyResult));
        }

        TransitionTarget(outputArrival);
        device->Release();
        return;
    }

    if (g_ngxTime != nullptr)
        g_ngxTime->Start(cmdList);

    unsigned int effectivePasses = 1;
    if (g_nr.passScratch != nullptr)
    {
        for (unsigned int pass = 1; pass < requestedPasses; ++pass)
        {
            if (g_nr.passFeature[pass] == nullptr || g_nr.passPendingSubmission[pass])
                break;
            ++effectivePasses;
        }
    }

    {
        static unsigned int loggedConfigured = 0;
        static unsigned int loggedEffective = 0;
        if (loggedConfigured != configuredPasses || loggedEffective != effectivePasses)
        {
            loggedConfigured = configuredPasses;
            loggedEffective = effectivePasses;
            LOG_INFO("DLSS-NR model passes: configured {}, effective {}", configuredPasses, effectivePasses);
        }
    }

    ID3D12Resource* passInput = modelInput;
    ID3D12Resource* passOutput = g_nr.output;
    ID3D12Resource* finalAnswer = nullptr;
    bool outputReadable = false;
    bool scratchReadable = false;

    const auto MakeModelReadable = [&](ID3D12Resource* resource)
    {
        bool& readable = resource == g_nr.output ? outputReadable : scratchReadable;
        if (!readable)
        {
            Barrier(cmdList, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            readable = true;
        }
    };

    const auto MakeModelWritable = [&](ID3D12Resource* resource)
    {
        bool& readable = resource == g_nr.output ? outputReadable : scratchReadable;
        if (readable)
        {
            Barrier(cmdList, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            readable = false;
        }
    };

    int result = NVSDK_NGX_Result_Success;

    for (unsigned int pass = 0; pass < effectivePasses && result == NVSDK_NGX_Result_Success; ++pass)
    {
        void* const passFeature = pass == 0 ? g_nr.feature : g_nr.passFeature[pass];
        const bool passReset = g_nr.reset || (pass > 0 && g_nr.passNeedsReset[pass]);
        const float passLocalTone = pass == 0 ? cfg.DlssNrLocalTone.value_or_default() : 0.0f;

        MakeModelWritable(passOutput);
        result = g_nr.evaluate(
            cmdList, passFeature, g_nr.capabilityParams, passInput, depthIn, motionIn, passOutput,
            workWidth, workHeight, evalGuideWidth, evalGuideHeight, g_nr.guideDepthInverted ? 1 : 0,
            passReset ? 1 : 0, cfg.DlssNrIntensity.value_or_default(),
            ResolvePassStyle(cfg, pass), cfg.DlssNrLocalStructure.value_or_default(),
            passLocalTone, cfg.DlssNrSkinStructure.value_or_default(),
            cfg.DlssNrAutoMask.value_or_default() ? 1 : 0, g_nr.guideMvScaleX * mvToWorkX,
            g_nr.guideMvScaleY * mvToWorkY);

        if (result != NVSDK_NGX_Result_Success)
            break;

        if (pass > 0)
            g_nr.passNeedsReset[pass] = false;

        finalAnswer = passOutput;
        MakeModelReadable(finalAnswer);

        if (pass + 1 < effectivePasses)
        {
            passInput = finalAnswer;
            passOutput = passOutput == g_nr.output ? g_nr.passScratch : g_nr.output;
        }
    }

    if (g_ngxTime != nullptr)
        g_ngxTime->End(cmdList);

    g_nr.reset = false;

    if (result == NVSDK_NGX_Result_Success && finalAnswer != nullptr && haloActive &&
        g_nr.passScratch != nullptr && cfg.DlssNrDebugView.value_or_default() != 2)
    {
        ID3D12Resource* haloTarget = (finalAnswer == g_nr.output) ? g_nr.passScratch : g_nr.output;
        MakeModelWritable(haloTarget);
        DlssNrConstants halo {};
        halo.Mode = DlssNrResidualMode_SuppressHalo;
        halo.Width = workWidth;
        halo.Height = workHeight;
        halo.HaloSuppression = std::clamp(cfg.DlssNrHaloSuppression.value_or_default(), 0.0f, 1.0f);
        halo.HaloBrightSuppression = std::clamp(cfg.DlssNrHaloBrightSuppression.value_or_default(), 0.0f, 1.0f);
        halo.HaloRadius = std::clamp(cfg.DlssNrHaloRadius.value_or_default(), 1.0f, 4.0f);
        if (DispatchResidualPass(cmdList, halo, modelInput, finalAnswer, nullptr, nullptr, haloTarget))
        {
            finalAnswer = haloTarget;
            MakeModelReadable(finalAnswer);
        }
    }

    if (workWidth > width || workHeight > height)
    {
        static unsigned int lastSuper = 0;
        if (lastSuper != workWidth || result != 1)
        {
            lastSuper = workWidth;
            LOG_INFO("DLSS-NR SUPERSAMPLE: model at {}x{} = {:.2f}x native {}x{}, evaluate result {} ({})",
                     workWidth, workHeight, (float) workWidth / (float) width, width, height, result,
                     NgxResultName((unsigned int) result));
        }
    }

    static bool tuningReported = false;

    if (!tuningReported && g_frames > 240)
    {
        tuningReported = true;

        auto report = [](const char* name, float wrote)
        {
            float value = 0.0f;
            const NVSDK_NGX_Result r = g_nr.capabilityParams->Get(name, &value);
            LOG_INFO("DLSS-NR readback {} -> {} (we wrote {}, result 0x{:X})", name, value, wrote,
                     (uint32_t) r);
        };

        const Config& rcfg = *Config::Instance();
        report("DLSSNR.Intensity", rcfg.DlssNrIntensity.value_or_default());
        report("DLSSNR.LocalStructureStrength", rcfg.DlssNrLocalStructure.value_or_default());
        report("DLSSNR.LocalToneStrength", rcfg.DlssNrLocalTone.value_or_default());
        report("DLSSNR.SkinStructureStrength", rcfg.DlssNrSkinStructure.value_or_default());

        unsigned int style = 0;
        const NVSDK_NGX_Result styleResult = g_nr.capabilityParams->Get("DLSSNR.Style", &style);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Style -> {} (result 0x{:X})", style, (uint32_t) styleResult);

        unsigned int preset = 0;
        const NVSDK_NGX_Result presetResult =
            g_nr.capabilityParams->Get("DLSSNR.Hint.Render.Preset", &preset);
        LOG_DEBUG("DLSS-NR readback DLSSNR.Hint.Render.Preset -> {} (result 0x{:X}, we wrote {})", preset,
                 (uint32_t) presetResult, cfg.DlssNrPreset.value_or_default());

        LOG_DEBUG("DLSS-NR wrote intensity {}, local structure {}, local tone {}, skin {}, style {}",
                 cfg.DlssNrIntensity.value_or_default(), cfg.DlssNrLocalStructure.value_or_default(),
                 cfg.DlssNrLocalTone.value_or_default(), cfg.DlssNrSkinStructure.value_or_default(),
                 cfg.DlssNrStyle.value_or_default());
    }

    if (result == NVSDK_NGX_Result_Success)
    {
        DlssNrConstants resolveParams {};
        resolveParams.Mode = DlssNrMode_Resolve;
        resolveParams.WhitePoint = whitePoint;
        resolveParams.UseGameExposure = useGameExposure;
        resolveParams.ExposurePreMul = exposurePreMul;
        resolveParams.AntiStarvation = cfg.DlssNrAntiStarvation.value_or_default() ? 1u : 0u;
        resolveParams.ExposureBalance = cfg.DlssNrExposureBalance.value_or_default();
        resolveParams.Width = width;
        resolveParams.Height = height;
        resolveParams.TransferStrength = cfg.DlssNrTransferStrength.value_or_default();
        resolveParams.ColourStrength = cfg.DlssNrColourStrength.value_or_default();
        resolveParams.DebugView = cfg.DlssNrDebugView.value_or_default();
        resolveParams.MaxRatio = cfg.DlssNrMaxRatio.value_or_default();
        resolveParams.Transfer = cfg.DlssNrTransfer.value_or_default();
        resolveParams.DebugScale = cfg.DlssNrWhitePointScale.value_or_default();
        resolveParams.Passthrough = isHdrBuffer ? 0u : 1u;
        resolveParams.ReversibleMode = cfg.DlssNrReversibleMode.value_or_default();
        resolveParams.ApplyModel = cfg.DlssNrApplyModel.value_or_default() ? 1u : 0u;
        resolveParams.CompareMode = cfg.DlssNrCompare.value_or_default();
        resolveParams.CompareSplit = cfg.DlssNrCompareSplit.value_or_default();
        resolveParams.CompareZoom = std::max(1.0f, cfg.DlssNrCompareZoom.value_or_default());
        resolveParams.CompareSwap = cfg.DlssNrCompareSwap.value_or_default() ? 1u : 0u;

        struct ComposeReport
        {
            bool valid;
            float whitePoint;
            float transfer;
            float colour;
            float maxRatio;
            unsigned int passthrough;
            unsigned int debugView;
            unsigned int compareMode;
            unsigned int residual;
            unsigned int workW;
            unsigned int workH;
            unsigned int passes;
        };

        static ComposeReport loggedCompose {};

        const ComposeReport composeNow { true,
                                         std::round(resolveParams.WhitePoint * 100.0f) / 100.0f,
                                         resolveParams.TransferStrength,
                                         resolveParams.ColourStrength,
                                         resolveParams.MaxRatio,
                                         resolveParams.Passthrough,
                                         resolveParams.DebugView,
                                         resolveParams.CompareMode,
                                         resolveParams.Transfer,
                                         g_nr.workWidth,
                                         g_nr.workHeight,
                                         effectivePasses };

        if (!loggedCompose.valid || loggedCompose.whitePoint != composeNow.whitePoint ||
            loggedCompose.transfer != composeNow.transfer || loggedCompose.colour != composeNow.colour ||
            loggedCompose.maxRatio != composeNow.maxRatio ||
            loggedCompose.passthrough != composeNow.passthrough ||
            loggedCompose.debugView != composeNow.debugView ||
            loggedCompose.compareMode != composeNow.compareMode ||
            loggedCompose.residual != composeNow.residual || loggedCompose.workW != composeNow.workW ||
            loggedCompose.workH != composeNow.workH || loggedCompose.passes != composeNow.passes)
        {
            loggedCompose = composeNow;
            LOG_INFO("DLSS-NR composition: paper white {:.2f}x, detail {:.2f}, colour {:.2f}, guard "
                     "{:.1f}x, colour transform {}, transfer {}, model {}x{}, passes {}, debug view {}, compare {}",
                     composeNow.whitePoint, composeNow.transfer, composeNow.colour, composeNow.maxRatio,
                     composeNow.passthrough != 0 ? "off (frame already tone mapped)" : "on (linear HDR)",
                     composeNow.residual == 1 ? "matched residual" : "classic", composeNow.workW,
                     composeNow.workH, composeNow.passes, composeNow.debugView, composeNow.compareMode);
        }

        bool superDownOk = false;
        if (workScale > 1.0f && g_nr.superDown != nullptr && g_nr.outputNative != nullptr &&
            g_nr.superDown->Dispatch(cmdList, finalAnswer, g_nr.outputNative))
        {
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            superDownOk = true;
        }

        ID3D12Resource* resolveProxy = superDownOk ? g_nr.colorCopy : modelInput;
        ID3D12Resource* resolveAnswer = superDownOk ? g_nr.outputNative : finalAnswer;

        ID3D12Resource* resolveOriginal =
            residualAcrossRr ? g_nr.hdrCopy : (targetSupportsUav ? g_nr.hdrCopy : target);
        ID3D12Resource* resolveTarget =
            residualAcrossRr ? g_nr.residualEdited : (targetSupportsUav ? target : g_nr.hdrCopy);

        if (residualAcrossRr)
        {
            Barrier(cmdList, g_nr.residualEdited, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else if (targetSupportsUav)
        {
            TransitionTarget(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        else
        {
            Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }

        if (residualAcrossRr)
            resolveParams.TransferStrength = 1.0f;

        const bool resolved = DispatchPass(cmdList, resolveParams, resolveProxy, resolveAnswer, resolveOriginal,
                                           motionIn, exposureTex, resolveTarget, nullptr);

        if (residualAcrossRr)
        {
            Barrier(cmdList, g_nr.residualEdited, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const unsigned prev = g_nr.residualHistoryIndex & 1u;
            const unsigned cur = prev ^ 1u;
            Barrier(cmdList, g_nr.residualHistory[cur], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            DlssNrConstants accum {};
            accum.Mode = DlssNrResidualMode_Accumulate;
            accum.Width = width;
            accum.Height = height;
            accum.ResidualBlend = std::clamp(cfg.DlssNrResidualAcrossRrBlend.value_or_default(), 0.01f, 1.0f);
            const bool rejectHistory = cfg.DlssNrHistoryRejection.value_or_default();
            accum.ResidualHistoryValid =
                (g_nr.residualHistoryPrimed && g_nr.residualHistoryRejection == rejectHistory) ? 1u : 0u;
            accum.HistoryRejection = rejectHistory ? 1u : 0u;
            accum.ResidualConfidenceSensitivity =
                std::clamp(cfg.DlssNrResidualConfidenceSensitivity.value_or_default(), 0.0f, 2.0f);
            accum.ExposurePreMul = frame.PreExposure > 1e-6f ? frame.PreExposure : 1.0f;
            accum.HistoryExposureRatio =
                (g_nr.residualHistoryPreExposure > 1e-6f && frame.PreExposure > 1e-6f)
                    ? (frame.PreExposure / g_nr.residualHistoryPreExposure)
                    : 1.0f;
            g_nr.residualHistoryRejection = rejectHistory;
            g_nr.residualHistoryPreExposure = frame.PreExposure > 1e-6f ? frame.PreExposure : 1.0f;
            accum.GuideWidth = motionWidth;
            accum.GuideHeight = motionHeight;
            accum.ResidualMotionBaseX = 0;
            accum.ResidualMotionBaseY = 0;
            accum.MvScaleX = frame.MvScaleX / (float) width;
            accum.MvScaleY = frame.MvScaleY / (float) height;
            const bool accumulated =
                resolved && DispatchResidualPass(cmdList, accum, g_nr.hdrCopy, g_nr.residualEdited,
                                                 g_nr.residualHistory[prev], motionIn, g_nr.residualHistory[cur]);
            Barrier(cmdList, g_nr.residualHistory[cur], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            g_nr.residualHistoryPrimed = accumulated;
            g_nr.residualStoreValid = accumulated;
            if (accumulated)
                g_nr.residualHistoryIndex = cur;
        }
        else if (!targetSupportsUav)
        {
            Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_RESOURCE_STATES priorTargetState = targetState;
            TransitionTarget(D3D12_RESOURCE_STATE_COPY_DEST);
            cmdList->CopyResource(target, g_nr.hdrCopy);
            TransitionTarget(priorTargetState);
            Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_COPY_SOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }

        MakeModelWritable(g_nr.output);
        if (g_nr.passScratch != nullptr)
            MakeModelWritable(g_nr.passScratch);

        if (superDownOk)
            Barrier(cmdList, g_nr.outputNative, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (g_capture.isActive())
        {
            g_capture.record(cmdList, device, g_nr.colorCopy,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, target,
                             targetState);

            if (g_capture.readyToWrite() && g_captureWriteAtFrame == 0)
                g_captureWriteAtFrame = g_frames + 8;
        }
    }
    else
    {
        g_nr.failed = true;
        g_nr.reason = "the model refused to run";
        LOG_ERROR("DLSS-NR evaluate returned 0x{:X} ({}), disabling for this session", (uint32_t) result,
                  NgxResultName((unsigned int) result));
    }

    MakeModelWritable(g_nr.output);
    if (g_nr.passScratch != nullptr)
        MakeModelWritable(g_nr.passScratch);

    Barrier(cmdList, g_nr.hdrCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (g_gpuTime != nullptr)
    {
        g_gpuTime->End(cmdList);

        auto* queue = timingQueue != nullptr ? timingQueue
                                             : (ID3D12CommandQueue*) State::Instance().currentCommandQueue;

        if (queue != nullptr)
        {
            if (auto ms = g_gpuTime->ReadGpuTime(queue); ms.has_value())
                g_lastGpuTime = ms;

            if (g_ngxTime != nullptr)
            {
                if (auto ngx = g_ngxTime->ReadGpuTime(queue); ngx.has_value())
                    g_lastNgxTime = ngx;
            }

            static unsigned long long lastSplitLog = 0;

            if (g_lastGpuTime.has_value() && g_lastNgxTime.has_value() && g_frames - lastSplitLog > 600)
            {
                lastSplitLog = g_frames;
                const double total = g_lastGpuTime.value();
                const double ngx = g_lastNgxTime.value();
                LOG_INFO("DLSS-NR cost: {:.2f} ms total = {:.2f} ms model + {:.2f} ms ours ({:.0f}% ours)",
                         total, ngx, total - ngx, total > 0.0 ? 100.0 * (total - ngx) / total : 0.0);
            }
        }
    }

    if (g_nr.depthClone != nullptr)
        Barrier(cmdList, g_nr.depthClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (g_nr.motionClone != nullptr)
        Barrier(cmdList, g_nr.motionClone, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COPY_DEST);

    if (reduced && g_nr.colorSmall != nullptr)
        Barrier(cmdList, g_nr.colorSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (matchedGuides)
    {
        Barrier(cmdList, g_nr.depthSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(cmdList, g_nr.motionSmall, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    Barrier(cmdList, g_nr.colorCopy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    TransitionTarget(outputArrival);

    device->Release();
}

namespace DlssNr
{
void RetryAfterFailure()
{
    g_nr.failed = false;
    g_nr.reason = "";
    g_nr.reset = true;
}

void ApplyResidualAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params)
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    if (cmdList == nullptr || params == nullptr)
        return;

    auto* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    const bool paired = g_nr.residualPair.Take(cmdList, params, output);
    const bool ready = g_nr.residualStoreValid;
    g_nr.residualStoreValid = false;

    if (!paired || !ready || !g_compose || !output || !g_nr.residualComposed)
    {
        g_nr.residualHistoryPrimed = false;
        return;
    }

    const auto& cfg = *Config::Instance();
    const float strength = cfg.DlssNrTransferStrength.value_or_default();
    if (!cfg.DlssNrApplyModel.value_or_default() || !std::isfinite(strength) || strength <= 0.0f)
    {
        g_nr.residualHistoryPrimed = false;
        return;
    }

    if ((cfg.RestoreComputeSignature.value_or_default() || cfg.RestoreGraphicSignature.value_or_default()) &&
        !D3D12Hooks::CanRestoreRootSignature(cmdList))
        return;

    ScopedNrStateEnvelope stateEnvelope(cmdList);
    const auto desc = output->GetDesc();
    const auto carrier = g_nr.residualComposed->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        desc.Width != carrier.Width || desc.Height != carrier.Height || desc.Format != carrier.Format ||
        desc.DepthOrArraySize != 1 || desc.MipLevels != 1)
        return;

    auto arrival = cfg.OutputResourceBarrier.has_value() ? (D3D12_RESOURCE_STATES) cfg.OutputResourceBarrier.value()
                                                         : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Barrier(cmdList, output, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, g_nr.residualComposed, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    DlssNrConstants apply {};
    apply.Mode = DlssNrResidualMode_Apply;
    apply.Width = (unsigned) desc.Width;
    apply.Height = desc.Height;
    apply.TransferStrength = std::clamp(strength, 0.0f, 1.0f);

    const bool composed =
        g_compose->DispatchResidualPass(cmdList, apply, output, g_nr.residualHistory[g_nr.residualHistoryIndex & 1u],
                                        nullptr, nullptr, g_nr.residualComposed);
    if (composed)
    {
        Barrier(cmdList, g_nr.residualComposed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmdList, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmdList->CopyResource(output, g_nr.residualComposed);
        Barrier(cmdList, output, D3D12_RESOURCE_STATE_COPY_DEST, arrival);
        Barrier(cmdList, g_nr.residualComposed, D3D12_RESOURCE_STATE_COPY_SOURCE,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        static bool announcedResidualAcrossRr = false;
        if (!announcedResidualAcrossRr)
        {
            announcedResidualAcrossRr = true;
            LOG_INFO("DLSS-NR: residual-across-RR active -- MV-reprojected accumulator, blend {:.2f}",
                     std::clamp(cfg.DlssNrResidualAcrossRrBlend.value_or_default(), 0.01f, 1.0f));
        }
    }
    else
    {
        Barrier(cmdList, output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival);
        Barrier(cmdList, g_nr.residualComposed, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g_nr.residualHistoryPrimed = false;
    }
}

void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                          ID3D12CommandQueue* timingQueue)
{
    if (!Config::Instance()->DlssNrEnabled.value_or_default())
    {
        ReportSkipOnce("it is switched off");
        return;
    }

    if (cmdList == nullptr || params == nullptr)
    {
        ReportSkipOnce("no command list or no parameter block");
        return;
    }

    {
        static ApiUpscalerInput saidApi = (ApiUpscalerInput) -1;
        const ApiUpscalerInput api = State::Instance().currentInputApiName;

        if (saidApi != api)
        {
            saidApi = api;
            LOG_INFO("DLSS-NR reached through the game's {} input", ApiUpscalerInputName(api));
        }
    }

    ID3D12Resource* target = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    if (target == nullptr || depth == nullptr || motion == nullptr)
    {
        ReportSkipOnce(target == nullptr    ? "the parameters carried no output texture"
                       : depth == nullptr   ? "the parameters carried no depth"
                                            : "the parameters carried no motion vectors");
        return;
    }

    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);

    DlssNrFrameInfo frame {};
    frame.DepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.MotionVectorsLowResolution = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    frame.ColourIsLinearHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
    const auto targetDesc = target->GetDesc();
    frame.OutputWidth = (unsigned int) targetDesc.Width;
    frame.OutputHeight = targetDesc.Height;
    frame.BeforeUpscale = false;
    frame.RayReconstruction =
        State::Instance().dlssdActive || GetResource(params, "DLSSD.Color", "DLSSD.Output") != nullptr;
    frame.ResidualAcrossRr = false;
    frame.SubmissionEpoch = State::Instance().frameCount;

    {
        unsigned int gameReset = 0;

        if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success)
            frame.Reset = gameReset != 0;
    }

    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);
    ResolveNrRenderDimensions(params, frame.RenderSubrectWidth, frame.RenderSubrectHeight);

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX) != NVSDK_NGX_Result_Success)
        frame.MvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY) != NVSDK_NGX_Result_Success)
        frame.MvScaleY = 1.0f;

    {
        float preExposure = 0.0f;
        const bool havePre =
            params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

        void* exposureTex = nullptr;
        params->Get(NVSDK_NGX_Parameter_ExposureTexture, &exposureTex);

        frame.ExposureTexture = exposureTex;
        frame.PreExposure = havePre && preExposure > 1e-6f ? preExposure : 1.0f;

        g_nr.exposureOfferedNow = exposureTex != nullptr;
        g_nr.exposureEverOffered = g_nr.exposureEverOffered || g_nr.exposureOfferedNow;
        g_nr.exposureFrames++;

        const bool autoExposureFlag = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) != 0;

        struct ExposureReport
        {
            bool valid;
            float pre;
            bool havePre;
            bool haveTexture;
            bool autoFlag;
        };

        static ExposureReport logged {};
        const ExposureReport now { true, havePre ? preExposure : 0.0f, havePre, exposureTex != nullptr,
                                   autoExposureFlag };

        if (!logged.valid || logged.havePre != now.havePre || logged.haveTexture != now.haveTexture ||
            logged.autoFlag != now.autoFlag ||
            std::abs(logged.pre - now.pre) > std::max(0.01f * std::abs(now.pre), 1e-4f))
        {
            logged = now;
            LOG_INFO("DLSS-NR exposure from the game: DLSS.Pre.Exposure {}, ExposureTexture {}, "
                     "auto-exposure flag {}",
                     now.havePre ? std::to_string(now.pre) : std::string("not supplied"),
                     now.haveTexture ? "supplied" : "not supplied", now.autoFlag ? "set" : "clear");
        }

        static float loggedExposure = -1.0f;

        if (g_nr.gameExposure > 1e-6f &&
            std::abs(loggedExposure - g_nr.gameExposure) > std::max(0.02f * g_nr.gameExposure, 1e-5f))
        {
            loggedExposure = g_nr.gameExposure;
            LOG_INFO("DLSS-NR game exposure {:.5f} (pre-exposure {:.3f}) -> white point would be {:.2f}",
                     g_nr.gameExposure, g_nr.gamePreExposure, g_nr.gamePreExposure / g_nr.gameExposure);
        }

        {
            int which = 0;
            float low = 0.0f, high = 0.0f;
            const float scanned = DlssNr::ExposureScan::BestValue(&which, &low, &high);

            static float loggedScan = -1.0f;

            if (scanned > 0.0f && std::abs(loggedScan - scanned) > std::max(0.02f * scanned, 1e-6f))
            {
                loggedScan = scanned;

                if (g_nr.gameExposure > 1e-6f)
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  the "
                             "game's own exposure is {:.5f}  |  ratio {:.4f}",
                             which, scanned, low, high, g_nr.gameExposure, scanned / g_nr.gameExposure);
                else
                    LOG_INFO("DLSS-NR exposure scan: candidate {} = {:.5f} ({:.5f}..{:.5f})  |  this "
                             "game supplies no exposure to compare against",
                             which, scanned, low, high);
            }
        }
    }

    ID3D12Device* device = nullptr;

    if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
    {
        ReportSkipOnce("the output texture belongs to no D3D12 device");
        return;
    }

    if (g_compose == nullptr)
        g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device);
    if (Config::Instance()->DlssNrDetailReuseEnabled.value_or_default() && g_detailReuse == nullptr)
        g_detailReuse = std::make_unique<DlssNr_DetailReuse_Dx12>("DLSS-NR DetailReuse", device);

    device->Release();

    if (g_compose == nullptr)
    {
        ReportSkipOnce("the pass could not be created");
        return;
    }

    const auto outArrival = Config::Instance()->OutputResourceBarrier.has_value()
                                ? (D3D12_RESOURCE_STATES) Config::Instance()->OutputResourceBarrier.value()
                                : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (g_detailReuse != nullptr &&
        g_detailReuse->TryReuseOrPrepareCapture(cmdList, target, depth, motion, frame.OutputWidth, frame.OutputHeight, frame, outArrival))
        return;

    g_compose->Dispatch(cmdList, target, depth, motion, target, frame, timingQueue);
    if (g_detailReuse != nullptr)
        g_detailReuse->CompleteCaptureAfterNr(cmdList, target, depth, motion, frame.OutputWidth, frame.OutputHeight, frame, outArrival);
}

void EvaluateBeforeUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,
                           ID3D12CommandQueue* timingQueue)
{
    const Config& cfg = *Config::Instance();
    {
        std::lock_guard<std::mutex> nrLock(g_nrMutex);
        if (g_nr.residualStoreValid)
            g_nr.residualHistoryPrimed = false;
        g_nr.residualPair.Cancel();
        g_nr.residualStoreValid = false;
    }

    const bool useDenoiseFirst = cfg.DlssNrDenoiseFirst.value_or_default();
    if (!cfg.DlssNrEnabled.value_or_default() || (!cfg.DlssNrPreSR.value_or_default() && !useDenoiseFirst))
        return;

    if (cmdList == nullptr || params == nullptr)
        return;

    ID3D12Resource* color = GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color");
    ID3D12Resource* output = GetResource(params, NVSDK_NGX_Parameter_Output, "DLSSD.Output");
    ID3D12Resource* depth = GetResource(params, NVSDK_NGX_Parameter_Depth, "DLSSD.Depth");
    ID3D12Resource* motion = GetResource(params, NVSDK_NGX_Parameter_MotionVectors, "DLSSD.MotionVectors");

    if (color == nullptr || depth == nullptr || motion == nullptr)
        return;

    D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    if (colorDesc.Width == 0 || colorDesc.Height == 0)
        return;

    // Guardian dev4: Blindaje SwapChain reactivo frente a resoluciones minimas, redimensionados y cargas asincronas (MSFS 2024 / UE5)
    static uint64_t s_lastWidth = 0;
    static uint32_t s_lastHeight = 0;
    static uint32_t s_stableFrames = 0;

    if (colorDesc.Width < 320 || colorDesc.Height < 240)
    {
        s_stableFrames = 0;
        return;
    }

    if (colorDesc.Width != s_lastWidth || colorDesc.Height != s_lastHeight)
    {
        s_lastWidth = colorDesc.Width;
        s_lastHeight = colorDesc.Height;
        s_stableFrames = 0;
        return;
    }

    if (++s_stableFrames < 30)
        return;

    const bool rayReconstruction =
        State::Instance().dlssdActive || GetResource(params, "DLSSD.Color", "DLSSD.Output") != nullptr;
    const bool residualAcrossRr = !useDenoiseFirst && cfg.DlssNrResidualAcrossRr.value_or_default() && rayReconstruction;

    if (residualAcrossRr &&
        (cfg.DlssNrHoldFrame.value_or_default() || cfg.DlssNrDebugView.value_or_default() != 0 ||
         cfg.DlssNrCompare.value_or_default() != 0))
    {
        std::lock_guard<std::mutex> nrLock(g_nrMutex);
        g_nr.residualHistoryPrimed = false;
        ReportSkipOnce("disable Hold frame, Compare and Debug view for residual-across-RR");
        return;
    }

    unsigned int createFlags = 0;
    params->Get(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &createFlags);

    ID3D12Resource* colourAuthority = output != nullptr ? output : color;

    DlssNrFrameInfo frame {};
    frame.DepthInverted = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
    frame.MotionVectorsLowResolution = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_MVLowRes) != 0;
    frame.ColourIsLinearHdr = (createFlags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0 &&
                              colourAuthority != nullptr &&
                              FormatCanHoldLinearHdr(colourAuthority->GetDesc().Format);
    frame.BeforeUpscale = true;
    frame.RayReconstruction = rayReconstruction;
    frame.ResidualAcrossRr = residualAcrossRr;
    frame.SubmissionEpoch = State::Instance().frameCount;

    if (output != nullptr)
    {
        const auto outDesc = output->GetDesc();
        frame.OutputWidth = (unsigned int) outDesc.Width;
        frame.OutputHeight = outDesc.Height;

        if (residualAcrossRr)
        {
            if (outDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || outDesc.SampleDesc.Count != 1 ||
                outDesc.DepthOrArraySize != 1 || outDesc.MipLevels != 1)
                return;

            std::lock_guard<std::mutex> nrLock(g_nrMutex);
            if (g_nr.residualOutputWidth != outDesc.Width || g_nr.residualOutputHeight != outDesc.Height ||
                g_nr.residualOutputFormat != outDesc.Format)
            {
                ParkNrResource(g_nr.residualComposed);
                g_nr.residualHistoryPrimed = false;
                g_nr.residualOutputWidth = (unsigned) outDesc.Width;
                g_nr.residualOutputHeight = outDesc.Height;
                g_nr.residualOutputFormat = outDesc.Format;
            }
        }
    }
    else if (residualAcrossRr)
    {
        return;
    }

    unsigned int gameReset = 0;
    if (params->Get(NVSDK_NGX_Parameter_Reset, &gameReset) == NVSDK_NGX_Result_Success)
        frame.Reset = gameReset != 0;

    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.RenderSubrectWidth);
    params->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.RenderSubrectHeight);
    ResolveNrRenderDimensions(params, frame.RenderSubrectWidth, frame.RenderSubrectHeight);

    if (frame.RenderSubrectWidth == 0 || frame.RenderSubrectHeight == 0)
    {
        frame.RenderSubrectWidth = static_cast<unsigned int>(colorDesc.Width);
        frame.RenderSubrectHeight = colorDesc.Height;
    }

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX) != NVSDK_NGX_Result_Success)
        frame.MvScaleX = 1.0f;

    if (params->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.MvScaleY) != NVSDK_NGX_Result_Success)
        frame.MvScaleY = 1.0f;

    {
        float preExposure = 0.0f;
        const bool havePre =
            params->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &preExposure) == NVSDK_NGX_Result_Success;

        void* exposureTex = nullptr;
        params->Get(NVSDK_NGX_Parameter_ExposureTexture, &exposureTex);

        frame.ExposureTexture = exposureTex;
        frame.PreExposure = havePre && preExposure > 1e-6f ? preExposure : 1.0f;
    }

    ID3D12Device* device = nullptr;
    if (FAILED(color->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr)
        return;

    if (g_compose == nullptr)
    {
        try {
            g_compose = std::make_unique<DlssNr_Dx12>("Neural Rendering", device);
        } catch (...) {
            device->Release();
            return;
        }
    }

    if (useDenoiseFirst && g_denoiseFirst == nullptr)
    {
        try {
            g_denoiseFirst = std::make_unique<DlssNr_DenoiseFirst_Dx12>("DLSS-NR DenoiseFirst", device);
        } catch (...) {
            g_denoiseFirst.reset();
        }
    }

    if (cfg.DlssNrDetailReuseEnabled.value_or_default() && g_detailReuse == nullptr)
    {
        try {
            g_detailReuse = std::make_unique<DlssNr_DetailReuse_Dx12>("DLSS-NR DetailReuse", device);
        } catch (...) {
            g_detailReuse.reset();
        }
    }

    device->Release();

    if (g_compose != nullptr)
    {
        const uint32_t activeW = std::min<uint32_t>(frame.RenderSubrectWidth, (uint32_t) colorDesc.Width);
        const uint32_t activeH = std::min<uint32_t>(frame.RenderSubrectHeight, colorDesc.Height);
        const auto inArrival = cfg.ColorResourceBarrier.has_value()
                                   ? (D3D12_RESOURCE_STATES) cfg.ColorResourceBarrier.value()
                                   : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        if (!residualAcrossRr && g_detailReuse != nullptr &&
            g_detailReuse->TryReuseOrPrepareCapture(cmdList, color, depth, motion, activeW, activeH, frame, inArrival))
            return;

        bool ranDenoiseFirst = false;
        if (useDenoiseFirst && g_denoiseFirst != nullptr)
            ranDenoiseFirst = g_denoiseFirst->EvaluateAndCompose(cmdList, params, g_compose.get(), color, depth, motion, frame, timingQueue);
        if (!ranDenoiseFirst)
        {
            if (!cfg.DlssNrPreSR.value_or_default())
                return;
            if (residualAcrossRr)
            {
                std::lock_guard<std::mutex> nrLock(g_nrMutex);
                g_nr.residualPair.Arm(cmdList, params, output);
            }
            g_compose->Dispatch(cmdList, color, depth, motion, color, frame, timingQueue);
        }
        if (!residualAcrossRr && g_detailReuse != nullptr)
            g_detailReuse->CompleteCaptureAfterNr(cmdList, color, depth, motion, activeW, activeH, frame, inArrival);
    }
}

void ProbeD3D11(void* d3d11Device)
{
    static bool done = false;

    if (done || d3d11Device == nullptr)
        return;

    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    if (!Config::Instance()->DlssNrProbeD3D11.value_or_default())
        return;

    done = true;

    if (!EnsureForwarder())
        return;

    auto probe = (int (*)(const wchar_t*)) GetProcAddress(g_nr.forwarder, "dlssnr_d3d11_probe");
    auto init = (int (*)(const wchar_t*, const wchar_t*, void*, int, int*, int*)) GetProcAddress(
        g_nr.forwarder, "dlssnr_d3d11_init");

    if (probe == nullptr || init == nullptr)
    {
        LOG_INFO("DLSS-NR D3D11: this forwarder has no D3D11 probe");
        return;
    }

    auto snippet = Util::FindFilePath(g_dllDir, "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        snippet = Util::FindFilePath(Util::ExePath().remove_filename(), "nvngx_dlssnr.dll");

    if (!snippet.has_value())
        return;

    const int bits = probe(snippet->wstring().c_str());

    auto requirements = (int (*)(const wchar_t*, void*, unsigned int*, unsigned int*, unsigned int*))
        GetProcAddress(g_nr.forwarder, "dlssnr_d3d11_requirements");

    if (requirements != nullptr)
    {
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory1* factory = nullptr;

        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory != nullptr)
            factory->EnumAdapters(0, &adapter);

        unsigned int supported = 0xFFFFFFFFu;
        unsigned int minArch = 0;
        unsigned int minOs = 0;
        const int rc = requirements(snippet->wstring().c_str(), adapter, &supported, &minArch, &minOs);

        const char* meaning = supported == 0        ? "SUPPORTED"
                              : (supported & 16)    ? "NotImplemented -- the feature has no D3D11 path"
                              : (supported & 4)     ? "AdapterUnsupported"
                              : (supported & 2)     ? "DriverVersionUnsupported"
                              : (supported & 8)     ? "OSVersionBelowMinimum"
                              : (supported & 1)     ? "CheckNotPresent"
                                                    : "unknown";

        LOG_WARN("DLSS-NR D3D11: GetFeatureRequirements {} ({}), FeatureSupported 0x{:X} -- {}. "
                 "minimum architecture 0x{:X}, minimum OS 0x{:X}",
                 rc, NgxResultName((unsigned int) rc), supported, meaning, minArch, minOs);

        if (adapter != nullptr)
            adapter->Release();

        if (factory != nullptr)
            factory->Release();
    }

    LOG_INFO("DLSS-NR D3D11: entry points resolved {}/15 (init {}, create {}, evaluate {}, release {})",
             bits, (bits & 1) ? "yes" : "no", (bits & 2) ? "yes" : "no", (bits & 4) ? "yes" : "no",
             (bits & 8) ? "yes" : "no");

    if (bits != 15)
    {
        LOG_INFO("DLSS-NR D3D11: incomplete surface, the bridge stays the only route");
        return;
    }

    int attempt = 0;
    int results[4] = { -9, -9, -9, -9 };

    const int result = init(snippet->wstring().c_str(), State::Instance().NVNGX_ApplicationDataPath.c_str(),
                            d3d11Device, 0x0000015, &attempt, results);

    static const char* kNames[4] = { "Init_Ext on our own copy", "Init on our own copy",
                                     "Init_Ext on the shared module", "Init on the shared module" };

    for (int i = 0; i < 4; ++i)
    {
        LOG_INFO("DLSS-NR D3D11:   {} -> {} ({})", kNames[i], results[i],
                 results[i] == -2   ? "module not loaded"
                 : results[i] == -3 ? "export missing"
                 : results[i] == -9 ? "not reached"
                                    : NgxResultName((unsigned int) results[i]));
    }

    if (result == 1)
        LOG_WARN("DLSS-NR D3D11: initialised, via {}. The feature already said this platform is "
                 "supported; now the call works too. Next is a feature create on a device context.",
                 attempt > 0 ? kNames[attempt - 1] : "?");
    else
        LOG_WARN("DLSS-NR D3D11: every init variant refused, last {} ({}) -- though the feature itself "
                 "reports this platform as supported, so the obstacle is in how it is being called",
                 result, NgxResultName((unsigned int) result));
}

CalibrationReading Calibration()
{
    CalibrationReading r {};
    r.suggestion = g_nr.calibSuggestion;
    r.steadiness = g_nr.calibSteadiness;
    r.samples = g_nr.calibCount;
    r.usable = g_nr.calibUsable;
    r.why = g_nr.calibWhy;
    return r;
}

bool IsRunning() { return g_nr.feature != nullptr && !g_nr.failed; }

const char* FailureReason() { return g_nr.failed ? g_nr.reason : ""; }

ExposureStatus GameExposureStatus()
{
    ExposureStatus s {};
    s.seenFrames = g_nr.exposureFrames;
    s.offeredNow = g_nr.exposureOfferedNow;
    s.everOffered = g_nr.exposureEverOffered;
    s.exposure = g_nr.gameExposure;
    s.preExposure = g_nr.gamePreExposure;
    return s;
}

std::optional<double> LastGpuTime() { return g_lastGpuTime; }

void RequestCapture(unsigned int frames)
{
    ClearCaptureDirectory();
    g_capture.request(frames);
}

bool CaptureInProgress() { return g_capture.isActive(); }

void Shutdown()
{
    std::lock_guard<std::mutex> nrLock(g_nrMutex);

    for (auto& r : g_nrRetired)
    {
        if (r.feature != nullptr && g_nr.release != nullptr)
            g_nr.release(r.feature);

        if (r.resource != nullptr)
            r.resource->Release();
    }

    g_nrRetired.clear();

    if (g_nr.feature != nullptr && g_nr.release != nullptr)
        g_nr.release(g_nr.feature);

    g_nr.feature = nullptr;
    g_nr.featurePendingSubmission = false;

    for (unsigned int i = 0; i < 10; ++i)
    {
        if (g_nr.passFeature[i] != nullptr && g_nr.release != nullptr)
            g_nr.release(g_nr.passFeature[i]);

        g_nr.passFeature[i] = nullptr;
        g_nr.passNeedsReset[i] = false;
        g_nr.passCreateFailed[i] = false;
        g_nr.passPendingSubmission[i] = false;
    }

    for (ID3D12Resource** res :
         { &g_nr.output, &g_nr.passScratch, &g_nr.colorCopy, &g_nr.hdrCopy, &g_nr.colorSmall, &g_nr.depthSmall,
           &g_nr.motionSmall, &g_nr.outputNative, &g_nr.residualEdited, &g_nr.residualHistory[0],
           &g_nr.residualHistory[1], &g_nr.residualComposed })
    {
        if (*res != nullptr)
        {
            (*res)->Release();
            *res = nullptr;
        }
    }

    g_nr.passScratchFailed = false;
    g_nr.residualPair.Cancel();
    g_nr.residualStoreValid = false;
    g_nr.residualHistoryIndex = 0;
    g_nr.residualHistoryPrimed = false;

    if (g_nr.superUp != nullptr)
    {
        delete g_nr.superUp;
        g_nr.superUp = nullptr;
    }

    if (g_nr.superDown != nullptr)
    {
        delete g_nr.superDown;
        g_nr.superDown = nullptr;
    }

    if (g_nr.heldColor != nullptr)
    {
        g_nr.heldColor->Release();
        g_nr.heldColor = nullptr;
    }
    g_nr.heldActive = false;

    if (g_nr.meter != nullptr)
    {
        g_nr.meter->Release();
        g_nr.meter = nullptr;
    }

    if (g_nr.calib != nullptr)
    {
        g_nr.calib->Release();
        g_nr.calib = nullptr;
    }

    for (auto& r : g_nr.calibReadback)
    {
        if (r != nullptr)
        {
            r->Release();
            r = nullptr;
        }
    }

    g_nr.calibFrames = 0;
    g_nr.calibCount = 0;
    g_nr.calibSuggestion = 0.0f;
    g_nr.calibSteadiness = 0.0f;
    g_nr.calibUsable = false;
    g_nr.calibWhy = "measuring...";

    for (auto& rb : g_nr.meterReadback)
    {
        if (rb != nullptr)
        {
            rb->Release();
            rb = nullptr;
        }
    }

    for (bool& valid : g_nr.meterExposureValid)
        valid = false;

    g_nr.meterFrames = 0;

    if (g_nr.depthClone != nullptr)
    {
        g_nr.depthClone->Release();
        g_nr.depthClone = nullptr;
    }

    if (g_nr.motionClone != nullptr)
    {
        g_nr.motionClone->Release();
        g_nr.motionClone = nullptr;
    }

    g_capture.release();
    g_gpuTime.reset();
    g_ngxTime.reset();
    g_lastNgxTime.reset();
    g_lastGpuTime.reset();

    g_detailReuse.reset();
    g_denoiseFirst.reset();
    g_compose.reset();
}
} // namespace DlssNr
