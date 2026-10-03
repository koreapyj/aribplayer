// Presenter diagnostics in ffp_get_debug_info: route, adapter, fallbacks, debug layer.
#include "wvptest.h"

#include "wvp/DebugInfo.h"

using namespace wvp;

TEST_CASE(DebugInfo_parses_presenter_adapter_fallbacks_and_debug_layer) {
    DebugInfo d = ParseDebugInfo(R"({"presenter":{"name":"d3d11","requested":"d3d11","route":"shader","frames_shader":12,
        "frames_d3d11_copied":3,"adapter":{"name":"Intel(R) Arc(TM) Graphics","vendor_id":32902,"luid":"00000000:0000a1b2"},
        "debug_layer":true,"debug_messages":2,"fallbacks":["decoder_pool_no_srv","p010_vp_unsupported:shader","force_caps:p010=0"]}})");
    const DebugPresenter& p = d.Presenter;
    CHECK(p.Present);
    CHECK_EQ(p.Route, std::string("shader"));
    CHECK(p.FramesShader == 12 && p.FramesD3d11Copied == 3);
    CHECK_EQ(p.AdapterName, std::string("Intel(R) Arc(TM) Graphics"));
    CHECK_EQ(p.AdapterLuid, std::string("00000000:0000a1b2"));
    CHECK(p.AdapterVendor == 0x8086);
    CHECK(p.DebugLayer == true && p.DebugMessages == 2);
    REQUIRE(p.Fallbacks.size() == 3);
    CHECK_EQ(p.Fallbacks[1], std::string("p010_vp_unsupported:shader"));

    DebugInfo old = ParseDebugInfo(R"({"presenter":{"name":"d3d11"}})");
    CHECK(old.Presenter.Route.empty() && old.Presenter.AdapterLuid.empty() && !old.Presenter.DebugLayer && old.Presenter.Fallbacks.empty());
}
