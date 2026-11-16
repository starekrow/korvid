// The Metal layer through SDL3, where the window is SDL's: SDL makes a view over the window's
// content (a window opened with SDL_WINDOW_METAL) whose layer is a CAMetalLayer. The view is
// SDL's to destroy, the layer the view's.
#include "../renderer_internal.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

namespace kv { namespace KV_RB_NS {

CAMetalLayer* mt_platform_layer(void* native_window, void** view_out) {
    *view_out = nullptr;
    SDL_MetalView view = SDL_Metal_CreateView((SDL_Window*)native_window);
    if (!view) { KV_ERROR("metal", "SDL_Metal_CreateView: %s (was the window opened for Metal?)", SDL_GetError()); return nil; }
    CAMetalLayer* layer = (CAMetalLayer*)SDL_Metal_GetLayer(view);
    if (!layer) {
        KV_ERROR("metal", "SDL_Metal_GetLayer: %s", SDL_GetError());
        SDL_Metal_DestroyView(view);
        return nil;
    }
    *view_out = view;
    return layer;
}

void mt_platform_release(void* view) {
    if (view) SDL_Metal_DestroyView((SDL_MetalView)view);
}

} } // namespace kv::KV_RB_NS
