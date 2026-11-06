#include "korvid/platform/host_window.h"
#include "korvid/platform/events.h"
#include "korvid/core/log.h"
#include <SDL3/SDL.h>

namespace kv {

bool HostWindow::open(const WindowDesc& desc) {
    KV_ASSERT(!sdl_window_);
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (desc.surface == WindowSurface::Vulkan) flags |= SDL_WINDOW_VULKAN; // also loads the Vulkan loader
    if (desc.borderless) flags |= SDL_WINDOW_BORDERLESS;
    SDL_Window* w = SDL_CreateWindow(desc.title, (int)desc.width, (int)desc.height, flags);
    if (!w) {
        KV_ERROR("window", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    sdl_window_ = w;
    surface_    = desc.surface;
    vsync_      = desc.vsync;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    width_   = (u32)pw;
    height_  = (u32)ph;
    focused_ = (SDL_GetWindowFlags(w) & SDL_WINDOW_INPUT_FOCUS) != 0;
    KV_INFO("window", "opened '%s' %ux%u px", desc.title, width_, height_);
    return true;
}

void HostWindow::close() {
    if (!sdl_window_) return;
    SDL_DestroyWindow((SDL_Window*)sdl_window_);
    sdl_window_ = nullptr;
    width_ = height_ = 0;
    focused_ = false;
    KV_INFO("window", "closed");
}

void HostWindow::present_blank() {
    SDL_Window* w = (SDL_Window*)sdl_window_;
    if (!w || surface_ != WindowSurface::Software) return;
    SDL_Surface* s = SDL_GetWindowSurface(w);
    if (!s) {
        KV_WARN("window", "SDL_GetWindowSurface failed: %s", SDL_GetError());
        return;
    }
    SDL_FillSurfaceRect(s, nullptr, SDL_MapSurfaceRGB(s, 24, 24, 28));
    SDL_UpdateWindowSurface(w);
}

void HostWindow::pump(EventBus& bus, Input& input) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            bus.emit(CloseRequested{0});
            break;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
            u32 nw = (u32)e.window.data1, nh = (u32)e.window.data2;
            if (nw != width_ || nh != height_) {
                width_  = nw;
                height_ = nh;
                bus.emit(WindowResized{nw, nh});
            }
            break;
        }

        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            focused_ = true;
            bus.emit(FocusChanged{true});
            break;

        case SDL_EVENT_WINDOW_FOCUS_LOST:
            focused_ = false;
            input.raw.clear(); // keys released while unfocused never report an up edge
            bus.emit(FocusChanged{false});
            break;

        case SDL_EVENT_KEY_DOWN: {
            Key k = key_from_scancode((u32)e.key.scancode);
            if (k == Key::Unknown) break;
            input.raw.set(k, true);
            bus.emit(KeyDown{k, 0, e.key.repeat});
            break;
        }

        case SDL_EVENT_KEY_UP: {
            Key k = key_from_scancode((u32)e.key.scancode);
            if (k == Key::Unknown) break;
            input.raw.set(k, false);
            bus.emit(KeyUp{k, 0});
            break;
        }

        default:
            break;
        }
    }
}

} // namespace kv
