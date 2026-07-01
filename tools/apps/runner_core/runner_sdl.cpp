#include "runner_sdl.h"

#include "runner_core.h"
#include "panel_sim.h"  // Pi Zero panel-falloff LUT (identity unless enabled)

#include "lvgl.h"  // LV_KEY_* codes

namespace runner_sdl {

uint32_t map_sdl_keycode(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP:       return LV_KEY_UP;
        case SDLK_DOWN:     return LV_KEY_DOWN;
        case SDLK_LEFT:     return LV_KEY_LEFT;
        case SDLK_RIGHT:    return LV_KEY_RIGHT;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return LV_KEY_ENTER;  // center-click / select
        // Number keys 1/2/3 emit the aux-key codes '1'/'2'/'3' (the intuitive
        // 1→KEY1 binding); F1/F2/F3 stay as aliases. RETURN remains center-click.
        case SDLK_1: case SDLK_KP_1: case SDLK_EXCLAIM: case SDLK_F1: return static_cast<uint32_t>('1');
        case SDLK_2: case SDLK_KP_2: case SDLK_AT:      case SDLK_F2: return static_cast<uint32_t>('2');
        case SDLK_3: case SDLK_KP_3: case SDLK_HASH:    case SDLK_F3: return static_cast<uint32_t>('3');
        default: return 0;
    }
}

void blit_framebuffer_to_texture(SDL_Texture* texture) {
    const int w = runner_core::width();
    const int h = runner_core::height();
    const uint16_t* fb = runner_core::framebuffer();

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) != 0) return;

    // Pi Zero panel-falloff simulation. `lut` is the identity table unless the web
    // runner has turned the sim on, so this is a no-op passthrough by default (the
    // native runner never enables it) — see panel_sim.h.
    const uint8_t* lut = panel_sim::lut();

    for (int y = 0; y < h; ++y) {
        uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pixels) + y * pitch);
        for (int x = 0; x < w; ++x) {
            // RGB565 → ARGB8888, with each channel mapped through the panel LUT.
            uint16_t c = fb[static_cast<size_t>(y) * w + x];
            uint8_t r = lut[static_cast<uint8_t>((c >> 11) << 3)];
            uint8_t g = lut[static_cast<uint8_t>(((c >> 5) & 0x3F) << 2)];
            uint8_t b = lut[static_cast<uint8_t>((c & 0x1F) << 3)];
            row[x] = 0xFF000000u | (static_cast<uint32_t>(r) << 16) |
                     (static_cast<uint32_t>(g) << 8) | b;
        }
    }

    SDL_UnlockTexture(texture);
}

}  // namespace runner_sdl
