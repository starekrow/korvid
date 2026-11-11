// The Metal layer on iOS, where the window is platform_hid's own: native_handle() is a UIView
// whose layer class is CAMetalLayer. The view is the window's; nothing here to destroy.
#include "../renderer_internal.h"
#import <UIKit/UIKit.h>

namespace kv {

CAMetalLayer* mt_platform_layer(void* native_window, void** view_out) {
    *view_out = nullptr;
    UIView* view = (UIView*)native_window;
    if (!view || ![view.layer isKindOfClass:[CAMetalLayer class]]) {
        KV_ERROR("metal", "the window's view has no CAMetalLayer (was it opened for Metal?)");
        return nil;
    }
    return (CAMetalLayer*)view.layer;
}

void mt_platform_release(void*) {}

} // namespace kv
