#include "korvid/platform_hid/host_window.h"
#include "korvid/platform_hid/events.h"
#include "korvid/core/log.h"
#include "../hid_backend.h"
#include "../sdl3_pads/sdl_pads.h"
#include <SDL3/SDL.h>

namespace kv {

bool hid_video_up() {
    if (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) return true;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        KV_ERROR("hid", "SDL video unavailable: %s", SDL_GetError());
        return false;
    }
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) KV_WARN("input", "SDL gamepads unavailable: %s", SDL_GetError());
    KV_INFO("hid", "video driver '%s'", SDL_GetCurrentVideoDriver());
    return true;
}

void hid_video_down() {
    if (SDL_WasInit(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    if (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

bool hid_backend_startup() {
    SDL_SetHint(SDL_HINT_APP_NAME, "korvid");
    // Headless until a window opens: video and gamepads come up with the first window
    // (HostWindow::open), so tests run where there is no display.
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        KV_ERROR("hid", "SDL events unavailable: %s", SDL_GetError());
        return false;
    }
    int ver = SDL_GetVersion();
    KV_INFO("hid", "SDL %d.%d.%d, headless until a window opens",
            SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver), SDL_VERSIONNUM_MICRO(ver));
    return true;
}

void hid_backend_shutdown() {
    hid_video_down();
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    if (SDL_WasInit(0) == 0) SDL_Quit(); // the last SDL user out (platform_audio may still hold audio)
}

bool HostWindow::open(const WindowDesc& desc) {
    KV_ASSERT(!native_);
    if (!hid_video_up()) return false; // the module starts headless
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (desc.surface == WindowSurface::Vulkan) flags |= SDL_WINDOW_VULKAN; // also loads the Vulkan loader
    if (desc.borderless) flags |= SDL_WINDOW_BORDERLESS;
    SDL_Window* w = SDL_CreateWindow(desc.title, (int)desc.width, (int)desc.height, flags);
    if (!w) {
        KV_ERROR("window", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    native_ = w;
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
    if (!native_) return;
    SDL_DestroyWindow((SDL_Window*)native_);
    native_ = nullptr;
    width_ = height_ = 0;
    focused_ = false;
    KV_INFO("window", "closed");
}

void HostWindow::present_blank() {
    SDL_Window* w = (SDL_Window*)native_;
    if (!w || surface_ != WindowSurface::Software) return;
    SDL_Surface* s = SDL_GetWindowSurface(w);
    if (!s) {
        KV_WARN("window", "SDL_GetWindowSurface failed: %s", SDL_GetError());
        return;
    }
    SDL_FillSurfaceRect(s, nullptr, SDL_MapSurfaceRGB(s, 24, 24, 28));
    SDL_UpdateWindowSurface(w);
}

// Framebuffer pixels per window unit (2 on a scaled desktop); 1 when SDL has no answer.
f32 HostWindow::pixel_scale() const {
    int ww = 0, wh = 0;
    if (!native_ || !SDL_GetWindowSize((SDL_Window*)native_, &ww, &wh) || ww <= 0) return 1.0f;
    return (f32)width_ / (f32)ww;
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

        // SDL reports the pointer in window units; the engine works in framebuffer pixels.
        case SDL_EVENT_MOUSE_MOTION: {
            f32 s = pixel_scale();
            MouseState& m = input.raw.mouse;
            f32 nx = e.motion.x * s, ny = e.motion.y * s;
            bus.emit(MouseMoved{nx, ny, e.motion.xrel * s, e.motion.yrel * s});
            m.x = nx; m.y = ny;
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            MouseButton b;
            switch (e.button.button) {
            case SDL_BUTTON_LEFT:   b = MouseButton::Left; break;
            case SDL_BUTTON_MIDDLE: b = MouseButton::Middle; break;
            case SDL_BUTTON_RIGHT:  b = MouseButton::Right; break;
            case SDL_BUTTON_X1:     b = MouseButton::X1; break;
            case SDL_BUTTON_X2:     b = MouseButton::X2; break;
            default: goto done;
            }
            {
                f32 s = pixel_scale();
                MouseState& m = input.raw.mouse;
                m.x = e.button.x * s; m.y = e.button.y * s;
                bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                m.set(b, down);
                if (down) bus.emit(MouseButtonDown{b, m.x, m.y});
                else      bus.emit(MouseButtonUp{b, m.x, m.y});
            }
        done:
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            input.raw.mouse.wheel += e.wheel.y;
            bus.emit(MouseWheel{e.wheel.x, e.wheel.y});
            break;
        }

        default:
            input_gamepad_event(&e, bus, input); // pads arrive on the same queue
            break;
        }
    }
}

} // namespace kv
