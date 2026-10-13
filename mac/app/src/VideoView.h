#pragma once
// The video area: a layer-hosting NSView whose layer is the CAMetalLayer the presenter draws into (mac/presenter, created
// by MacVideoPlatform::CreatePresenter on this view). The host keeps drawableSize / contentsScale up to date and tells
// the presenter in use (FfPlayer::CurrentPresenter -> MetalPresenter::ViewChanged = ffp_metal_presenter_view_changed)
// after every change; nothing else touches the layer. Sits at the bottom of the view stack, under the
// transparent ShellView.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <functional>

@interface VideoView : NSView
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
/// Called on the main thread after drawableSize / contentsScale changed (-> Engine::ViewChanged).
@property(nonatomic) std::function<void()> onViewChanged;
- (void)updateDrawableSize;
@end
