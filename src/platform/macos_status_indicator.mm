#include "platform/macos_status_indicator.hpp"

#import <AppKit/AppKit.h>

#if !__has_feature(objc_arc)
#error "This file requires ARC: gStatusItem must retain the status item or it deallocates (and leaves the menu bar) when the autorelease pool drains."
#endif

namespace {
NSStatusItem* gStatusItem = nil;

NSImage* indicatorImage() {
    NSImage* icon = nil;
    NSString* path = [[NSBundle mainBundle] pathForResource:@"AppIcon" ofType:@"icns"];
    if (path != nil) {
        icon = [[NSImage alloc] initWithContentsOfFile:path];
    }
    if (icon == nil) {
        icon = [NSApp applicationIconImage];
    }
    [icon setSize:NSMakeSize(18, 18)];
    return icon;
}
}

namespace screenrec::macos {

void runMainLoop() {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp run];
    }
}

void stopMainLoop() {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp stop:nil];
        // stop: only takes effect once another event is processed — post one.
        NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                           location:NSZeroPoint
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:0
                                            context:nil
                                            subtype:0
                                              data1:0
                                              data2:0];
        [NSApp postEvent:wake atStart:YES];
    });
}

void showRecordingIndicator() {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (gStatusItem != nil) {
            return;
        }
        gStatusItem = [[NSStatusBar systemStatusBar]
            statusItemWithLength:NSSquareStatusItemLength];
        gStatusItem.button.image = indicatorImage();
        gStatusItem.button.toolTip = @"Screen recording in progress";
        NSLog(@"[Indicator] menu-bar recording indicator shown (visible=%d)",
            gStatusItem.button.window != nil);

        // A full menu bar on notched Macs parks new items under the camera housing: "visible" yet unseeable.
        // Checked after layout settles so the log explains a missing indicator.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)1e9),
            dispatch_get_main_queue(), ^{
                NSWindow* win = gStatusItem.button.window;
                NSScreen* screen = win.screen ?: NSScreen.screens.firstObject;
                if (win == nil || screen == nil || screen.safeAreaInsets.top <= 0) {
                    return;
                }
                if (!NSIntersectsRect(win.frame, screen.auxiliaryTopLeftArea) &&
                    !NSIntersectsRect(win.frame, screen.auxiliaryTopRightArea)) {
                    NSLog(@"[Indicator] menu bar is full: the recording indicator was "
                           "placed behind the camera notch (x=%.0f) and is not visible — "
                           "remove some menu bar icons to make room",
                        win.frame.origin.x);
                }
            });
    });
}

void hideRecordingIndicator() {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (gStatusItem == nil) {
            return;
        }
        [[NSStatusBar systemStatusBar] removeStatusItem:gStatusItem];
        gStatusItem = nil;
    });
}

}
