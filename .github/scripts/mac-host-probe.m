// CI host probe for the macOS jobs of mac.yml: hardware VideoToolbox decode, the Metal device and the GUI session.
// Prints the findings and appends h264, hevc, metal, gui and virtual_gpu (0/1) to $GITHUB_OUTPUT.
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <Metal/Metal.h>
#import <ApplicationServices/ApplicationServices.h>
int main(void) {
    @autoreleasepool {
        BOOL h264 = VTIsHardwareDecodeSupported(kCMVideoCodecType_H264);
        BOOL hevc = VTIsHardwareDecodeSupported(kCMVideoCodecType_HEVC);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        BOOL virtualGPU = device && [device.name containsString:@"Paravirtual"];
        NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
        BOOL gui = [session[(__bridge NSString *)kCGSessionOnConsoleKey] boolValue] &&
                   [session[(__bridge NSString *)kCGSessionLoginDoneKey] boolValue];
        printf("macOS %s\n", NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String);
        printf("VTIsHardwareDecodeSupported: H.264=%d HEVC=%d\n", h264, hevc);
        printf("Metal device: %s\n", device ? device.name.UTF8String : "none");
        printf("GUI session available: %d; %s\n", gui, session.description.UTF8String ?: "none");
        FILE *out = fopen(getenv("GITHUB_OUTPUT"), "a");
        if (!out) return 1;
        fprintf(out, "h264=%d\nhevc=%d\nmetal=%d\ngui=%d\nvirtual_gpu=%d\n", h264, hevc, device != nil, gui, virtualGPU);
        fclose(out);
    }
}
