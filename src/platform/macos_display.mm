#include "platform/macos_display.hpp"

#if defined(__APPLE__)

#import <ColorSync/ColorSync.h> // CGDisplayCreateUUIDFromDisplayID
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <string>

#include "recording/monitor_layout.hpp"

namespace screenrec::macos {

namespace {

std::string displayUuidString(const CGDirectDisplayID displayId) {
    std::string result;
    if (CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(displayId)) {
        if (CFStringRef str = CFUUIDCreateString(kCFAllocatorDefault, uuid)) {
            char buffer[64] = {};
            if (CFStringGetCString(str, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
                result = buffer;
            }
            CFRelease(str);
        }
        CFRelease(uuid);
    }
    return result;
}

}

std::vector<MonitorInfo> detectMonitors() {
    std::vector<MonitorInfo> monitors;

    uint32_t displayCount = 0;
    if (CGGetActiveDisplayList(0, nullptr, &displayCount) != kCGErrorSuccess ||
        displayCount == 0) {
        return monitors;
    }

    std::vector<CGDirectDisplayID> displays(displayCount);
    if (CGGetActiveDisplayList(displayCount, displays.data(), &displayCount) !=
        kCGErrorSuccess) {
        return monitors;
    }
    displays.resize(displayCount);

    for (size_t i = 0; i < displays.size(); ++i) {
        const CGDirectDisplayID displayId = displays[i];
        const CGRect bounds = CGDisplayBounds(displayId); // points (global space)

        // Pixels come from the display mode; pixels/points is the backing scale (2.0 on Retina).
        size_t pixelWidth = 0;
        size_t pixelHeight = 0;
        if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayId)) {
            pixelWidth = CGDisplayModeGetPixelWidth(mode);
            pixelHeight = CGDisplayModeGetPixelHeight(mode);
            CGDisplayModeRelease(mode);
        }
        if (pixelWidth == 0 || pixelHeight == 0) {
            pixelWidth = static_cast<size_t>(bounds.size.width);
            pixelHeight = static_cast<size_t>(bounds.size.height);
        }

        const float scaleX =
            bounds.size.width > 0 ? static_cast<float>(pixelWidth) / static_cast<float>(bounds.size.width) : 1.0f;
        const float scaleY =
            bounds.size.height > 0 ? static_cast<float>(pixelHeight) / static_cast<float>(bounds.size.height) : 1.0f;

        MonitorInfo info;
        info.index = static_cast<int>(i);
        info.name = displayUuidString(displayId);
        info.x = static_cast<int>(bounds.origin.x);
        info.y = static_cast<int>(bounds.origin.y);
        info.width = static_cast<int>(pixelWidth);
        info.height = static_cast<int>(pixelHeight);
        info.isPrimary = CGDisplayIsMain(displayId) != 0;
        info.dpiScaleX = scaleX;
        info.dpiScaleY = scaleY;
        monitors.push_back(info);
    }

    normalizeMixedDpiLayout(monitors);
    return monitors;
}

}

#endif
