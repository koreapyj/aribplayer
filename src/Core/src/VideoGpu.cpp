#include "wvp/VideoGpu.h"

#include <algorithm>

namespace wvp {

namespace {
std::string CostText(const PlanCost& c) {
    return std::to_string(c.Weighted()) + " host, " + std::to_string(c.deviceCopies) + " device";
}
}

std::vector<PhysicalGpu> GroupPhysicalGpus(const std::vector<AdapterEntry>& entries) {
    std::vector<PhysicalGpu> gpus;
    auto samePci = [](const AdapterEntry& a, const AdapterEntry& b) {
        return a.bus == b.bus && a.device == b.device && a.function == b.function;
    };
    for (const AdapterEntry& e : entries) {
        if (!e.hasPci) continue;
        auto it = std::find_if(gpus.begin(), gpus.end(), [&](const PhysicalGpu& g) { return samePci(g.canonical, e); });
        if (it == gpus.end()) { gpus.push_back({ e, {} }); continue; }
        if (e.hasOutputs && !it->canonical.hasOutputs) {
            it->aliasLuids.push_back(it->canonical.luid);
            it->canonical = e;
        } else {
            it->aliasLuids.push_back(e.luid);
        }
    }
    for (const AdapterEntry& e : entries) {
        if (e.hasPci) continue;
        PhysicalGpu* match = nullptr;
        int n = 0;
        for (PhysicalGpu& g : gpus)
            if (g.canonical.vendorId == e.vendorId && g.canonical.deviceId == e.deviceId) { match = &g; n++; }
        if (n == 1) match->aliasLuids.push_back(e.luid);
    }
    return gpus;
}

std::string CanonicalLuid(const std::vector<PhysicalGpu>& gpus, const std::string& luid) {
    for (const PhysicalGpu& g : gpus) {
        if (g.canonical.luid == luid) return luid;
        if (std::find(g.aliasLuids.begin(), g.aliasLuids.end(), luid) != g.aliasLuids.end()) return g.canonical.luid;
    }
    return "";
}

PlanCost TotalWithCrossAdapter(const PlanCost& planCost, bool crossAdapter) {
    PlanCost t = planCost;
    if (crossAdapter) t.deviceCopies += 1;
    return t;
}

int FindPreferred(const std::vector<AdapterCandidate>& candidates, const VideoGpuPref& pref) {
    if (pref.Automatic()) return -1;
    for (size_t i = 0; i < candidates.size(); i++)
        if (!pref.luid.empty() && candidates[i].luid == pref.luid) return static_cast<int>(i);
    for (size_t i = 0; i < candidates.size(); i++)
        if (pref.vendorId != 0 && candidates[i].vendorId == pref.vendorId && candidates[i].deviceId == pref.deviceId)
            return static_cast<int>(i);
    return -1;
}

VideoGpuChoice SelectVideoGpu(const std::vector<AdapterCandidate>& candidates, const std::string& displayLuid, const VideoGpuPref& pref,
                              const PlanRequest& request) {
    VideoGpuChoice ch;
    auto planOn = [&](size_t i) { return PipelinePlanner(candidates[i].caps).Make(request); };
    if (!pref.Automatic()) {
        int m = FindPreferred(candidates, pref);
        if (m >= 0) {
            ch.index = m;
            ch.manual = true;
            ch.plan = planOn(static_cast<size_t>(m));
            ch.crossAdapter = candidates[static_cast<size_t>(m)].luid != displayLuid;
            ch.total = TotalWithCrossAdapter(ch.plan.cost, ch.crossAdapter);
            ch.reason = "manual: " + candidates[static_cast<size_t>(m)].name;
            return ch;
        }
        ch.manualMissing = true;
    }
    struct Scored { int i; Plan plan; PlanCost total; bool cross; };
    std::vector<Scored> ok;
    for (size_t i = 0; i < candidates.size(); i++) {
        Plan p = planOn(i);
        if (!p.ok) continue;
        bool cross = candidates[i].luid != displayLuid;
        ok.push_back({ static_cast<int>(i), std::move(p), PlanCost{}, cross });
        ok.back().total = TotalWithCrossAdapter(ok.back().plan.cost, cross);
    }
    auto better = [](const Scored& a, const Scored& b) {
        if (a.total < b.total) return true;
        if (b.total < a.total) return false;
        return !a.cross && b.cross;
    };
    int best = -1, second = -1;
    for (int k = 0; k < static_cast<int>(ok.size()); k++) {
        if (best < 0 || better(ok[k], ok[best])) { second = best; best = k; }
        else if (second < 0 || better(ok[k], ok[second])) second = k;
    }
    std::string prefix = ch.manualMissing ? "automatic (preferred " + pref.name + " not present): " : "automatic: ";
    if (best < 0) {
        ch.reason = prefix + "no hardware adapter -> software decoding + SDL";
        return ch;
    }
    const Scored& b = ok[best];
    ch.index = b.i;
    ch.plan = b.plan;
    ch.total = b.total;
    ch.crossAdapter = b.cross;
    ch.reason = prefix + (b.cross ? "not the display adapter (+1 cross-adapter copy), " : "display adapter, ") + CostText(b.total);
    if (second >= 0) {
        ch.runnerUp = ok[second].i;
        ch.runnerUpTotal = ok[second].total;
    }
    return ch;
}

}
