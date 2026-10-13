#include "wvp/mac/MacVideoPlatform.h"

#include <utility>

extern "C" {
#include <libavutil/buffer.h>
#include "ffplay.h"
#include "ffp_metal_presenter.h"
}

namespace wvp::mac {

MetalPresenter::MetalPresenter(FFPPresenter* presenter, AVBufferRef* vtDevice, std::string adapterId)
    : presenter_(presenter), vtDevice_(vtDevice), adapterId_(std::move(adapterId)) {}

MetalPresenter::~MetalPresenter() {
    // libffplay has released its references by now (FfPlayer destroys the presenter after ffp_destroy / ffp_set_output).
    ffp_metal_presenter_destroy(presenter_);
    presenter_ = nullptr;
    av_buffer_unref(&vtDevice_);
}

void MetalPresenter::ViewChanged() { ffp_metal_presenter_view_changed(presenter_); }

}
