#import "VideoView.h"

#import <Metal/Metal.h>

@implementation VideoView

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawDuringViewResize;
    }
    return self;
}

- (CALayer*)makeBackingLayer {
    CAMetalLayer* l = [CAMetalLayer layer];
    // device stays nil: MacVideoPlatform::CreatePresenter sets the chosen adapter's MTLDevice
    l.pixelFormat = MTLPixelFormatBGRA8Unorm;
    l.opaque = YES;
    l.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    l.contentsGravity = kCAGravityResizeAspect;
    return l;
}

- (CAMetalLayer*)metalLayer { return (CAMetalLayer*)self.layer; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)isOpaque { return YES; }
- (NSView*)hitTest:(NSPoint)p { (void)p; return nil; }   // the ShellView above routes every event

- (void)updateDrawableSize {
    CAMetalLayer* l = self.metalLayer;
    CGFloat scale = self.window ? self.window.backingScaleFactor : [NSScreen mainScreen].backingScaleFactor;
    NSSize sz = self.bounds.size;
    l.contentsScale = scale;
    l.drawableSize = CGSizeMake(MAX(1.0, floor(sz.width * scale)), MAX(1.0, floor(sz.height * scale)));
    if (_onViewChanged) _onViewChanged();
}

- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self updateDrawableSize];
}

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}

@end
