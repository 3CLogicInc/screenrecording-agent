#include "platform/macos_permissions.hpp"

#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>

#include <string>

namespace screenrec::macos {

bool screenCaptureAccessPreflight() {
    return CGPreflightScreenCaptureAccess();
}

bool screenCaptureAccessRequest() {
    return CGRequestScreenCaptureAccess();
}

static NSString* tccPromptMarkerPath() {
    NSString* dir = [NSHomeDirectory()
        stringByAppendingPathComponent:@"Library/Application Support/ScreenRecorder"];
    return [dir stringByAppendingPathComponent:@".tcc_prompt_shown"];
}

bool tccPromptAlreadyShown() {
    return [[NSFileManager defaultManager] fileExistsAtPath:tccPromptMarkerPath()];
}

void markTccPromptShown() {
    NSString* path = tccPromptMarkerPath();
    [[NSFileManager defaultManager] createDirectoryAtPath:[path stringByDeletingLastPathComponent]
                              withIntermediateDirectories:YES
                                               attributes:nil
                                                    error:nil];
    [[NSData data] writeToFile:path atomically:YES];
}

MicAccess micAccessStatus() {
    const AVAuthorizationStatus status =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    switch (status) {
    case AVAuthorizationStatusAuthorized:
        return MicAccess::GRANTED;
    case AVAuthorizationStatusNotDetermined:
        return MicAccess::UNDETERMINED;
    case AVAuthorizationStatusDenied:
    case AVAuthorizationStatusRestricted:
    default:
        return MicAccess::DENIED;
    }
}

}

#endif
