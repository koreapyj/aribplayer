#pragma once
// Video GPU selection: which adapter libffplay's presenter device (shared with d3d11va decode, the D3D11 filters and CUDA) runs on.
// The UI device stays on Windows' default adapter; only the video side is chosen here.
//
// Automatic: every candidate adapter (hardware, D3D11 video) gets its best plan for the current request from its own probed
// capabilities. An adapter that does not drive the window's monitor costs one more copy per frame (Windows copies the presented
// frames across adapters). That copy is counted as a device copy: it is a GPU-to-GPU transfer like an interop copy, so it never
// outweighs a host copy or the software-decode penalty (a dGPU that avoids software decoding still wins), but it breaks every
// tie in favour of the display adapter. The cheapest total wins; on a tie the display adapter, then candidate order.
// Manual: the preferred adapter always wins (matched by LUID, else by vendor + device id since LUIDs change across reboots); when
// it is not present Automatic is used for the session (manualMissing).
// Pure logic: no Win32, no libffplay.
#include <optional>
#include <string>
#include <vector>

#include "CapabilitySet.h"
#include "PipelinePlanner.h"

namespace wvp {

struct AdapterCandidate {
    std::string luid;               // "HHHHHHHH:LLLLLLLL"
    std::string name;
    int vendorId = 0, deviceId = 0;
    CapabilitySet caps;             // this adapter's probe merged with BuildCapabilities
};

/// Settings > Playback > Video GPU. Empty luid and zero ids = Automatic.
struct VideoGpuPref {
    std::string luid, name;
    int vendorId = 0, deviceId = 0;
    bool Automatic() const { return luid.empty() && vendorId == 0 && deviceId == 0; }
    bool operator==(const VideoGpuPref&) const = default;
};

struct VideoGpuChoice {
    int index = -1;                 // into the candidates; -1 = no hardware adapter (software decoding + SDL)
    bool manual = false;            // the manual preference decided
    bool manualMissing = false;     // a manual preference is set but its adapter is not present: Automatic for this session
    bool crossAdapter = false;      // the chosen adapter does not drive the window's monitor
    Plan plan;                      // the chosen adapter's plan (its cost without the cross-adapter copy)
    PlanCost total;                 // plan cost + the cross-adapter copy (as a device copy)
    int runnerUp = -1;              // next best candidate (Automatic), -1 when none
    PlanCost runnerUpTotal;
    std::string reason;             // "automatic: display adapter, 0 copies" ...
};

/// One DXGI hardware adapter as enumerated: physical GPUs are identified by PCI location, not by LUID or name (one GPU can
/// appear as several DXGI adapters, e.g. behind an indirect display driver; two identical cards are two GPUs).
struct AdapterEntry {
    std::string luid, name;
    int vendorId = 0, deviceId = 0;
    bool hasPci = false;            // D3DKMT reported a PCI bus/device/function
    int bus = 0, device = 0, function = 0;
    bool hasOutputs = false;        // drives at least one monitor (DXGI outputs)
};

struct PhysicalGpu {
    AdapterEntry canonical;               // the entry with outputs, else the first in DXGI order
    std::vector<std::string> aliasLuids;  // every other entry of the same GPU
};

/// Groups entries by PCI address. An entry without one is an alias of the single physical GPU with the same vendor and device
/// id, and is dropped (never a candidate) when there is no such GPU or more than one.
std::vector<PhysicalGpu> GroupPhysicalGpus(const std::vector<AdapterEntry>& entries);
/// The canonical LUID of the GPU an adapter LUID belongs to (itself when canonical); "" when the LUID is not in any group.
std::string CanonicalLuid(const std::vector<PhysicalGpu>& gpus, const std::string& luid);

/// Total cost of a plan on an adapter: the plan's cost plus one device copy when the adapter is not the display adapter.
PlanCost TotalWithCrossAdapter(const PlanCost& planCost, bool crossAdapter);

VideoGpuChoice SelectVideoGpu(const std::vector<AdapterCandidate>& candidates, const std::string& displayLuid, const VideoGpuPref& pref,
                              const PlanRequest& request);

/// Index of the candidate the preference names (LUID first, then vendor + device id), -1 when none.
int FindPreferred(const std::vector<AdapterCandidate>& candidates, const VideoGpuPref& pref);

}
