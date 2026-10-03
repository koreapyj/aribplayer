#pragma once
// Concrete filters that can realize each pipeline operation, discovered from a CapabilitySet at runtime.
// Discovery order for a filter's frame domain:
//   1. pad pixel formats (FilterCap::formats == List): a hw pix_fmt ("cuda", "opencl", ...) -> that domain, only sw formats -> System;
//      formats == Any (passthrough) -> FrameDomain::Any;
//   2. the suffix map ("_cuda", "_npp", "_cuvid" -> Cuda; "_opencl" -> OpenCl; "_qsv" -> Qsv; "_d3d11"/"_d3d11va" -> D3D11;
//      "_d3d12" -> D3D12; "_vulkan" -> Vulkan; "_dxva2" -> Dxva2);
//   3. otherwise System.
// The operation of a filter comes from its base name (name minus the domain suffix: "ivtc_cuda" -> "ivtc") looked up in the
// base-name map, then from the one-line alias table (kAliases). So a new upstream "ivtc_cuda" or a future "ivtc_npp" needs no code change.
// Driver deinterlacers and video post-processors (deinterlace_d3d11/_qsv/_vaapi/_amf, vpp_qsv/_amf, ...) are denied: never a variant.
// Pure logic, no Win32.
#include <optional>
#include <string>
#include <vector>

#include "CapabilitySet.h"

namespace wvp {

enum class PipelineOp {
    Ivtc, Bwdif, Yadif, Transpose, HFlip, VFlip, Eq, Setpts, Scale, Tonemap,
    Unknown
};
const char* OpName(PipelineOp op);

struct FilterVariant {
    PipelineOp op = PipelineOp::Unknown;
    std::string filter;            // filter name; a composite is ','-joined ("fieldmatch,bwdif,decimate")
    FrameDomain domain = FrameDomain::System;
    std::vector<std::string> options;   // exposed AVOption names (from caps)
    bool optionsKnown = false;          // false: every option is assumed to exist
    std::string discoveredBy;           // "pads" | "suffix" | "alias" | "default"
    bool flagsCombed = false;           // ivtc variants: residual-combed frames are flagged interlaced (copied from FilterCap::flagsCombed)
    double cpuLoad = 0;                 // relative CPU cost per frame (System variants > 0, hw variants 0)

    bool HasOption(const std::string& name) const;
};

/// One line per alias: filter name(s) -> operation. Composite names use ','. Kept tiny on purpose.
struct FilterAlias { const char* filter; PipelineOp op; };
extern const FilterAlias kAliases[];
extern const int kAliasCount;

class PipelineTable {
public:
    /// Discovers every variant from caps.filters (a composite alias is available only when each part is in caps.filters).
    static PipelineTable Discover(const CapabilitySet& caps);

    /// Variants of an operation, in discovery order (System variants first, then hw by caps order). Empty when none.
    std::vector<FilterVariant> VariantsFor(PipelineOp op) const;
    /// Variant by filter name (composite names too), nullopt when not discovered.
    std::optional<FilterVariant> Find(const std::string& filter) const;
    /// Domain of any filter of the build (also filters without an operation, e.g. for raw chains): the discovery order above.
    /// Filters not in caps -> System.
    FrameDomain DomainOf(const std::string& filter) const;
    const std::vector<FilterVariant>& All() const { return variants_; }

    /// Domain from pad pixel formats; nullopt when formats are not a List (or the list is empty).
    static std::optional<FrameDomain> DomainFromPixFmts(const FilterCap& f);
    /// Domain from the suffix map; *base receives the name without the suffix ("ivtc_npp" -> "ivtc"). nullopt if no suffix matches.
    static std::optional<FrameDomain> DomainFromSuffix(const std::string& name, std::string* base = nullptr);
    /// Base name -> operation ("ivtc" -> Ivtc, "bwdif" -> Bwdif, ...), Unknown otherwise.
    static PipelineOp OpFromBaseName(const std::string& base);
    /// Operation of one concrete filter name (e.g. a graph node), discovered like the variants: a one-name alias, else the
    /// base name after the suffix map ("bwdif_d3d11" -> Bwdif, "flip_vulkan" -> VFlip). Unknown otherwise.
    static PipelineOp OpOfFilterName(const std::string& name);
    /// Driver deinterlacer / video post-processor that Discover never turns into a variant (explicit list + "deinterlace_" / "vpp_").
    static bool IsDenied(const std::string& name);

private:
    std::vector<FilterVariant> variants_;
    std::vector<std::pair<std::string, FrameDomain>> domains_;   // every filter of caps -> domain
};

}
