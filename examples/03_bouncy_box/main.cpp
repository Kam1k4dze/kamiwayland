#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

import kamiwayland;
import wl;
import xdg;
import xdg_decoration;
import std;

using std::int32_t;
using std::uint32_t;

static constexpr uint32_t KEY_LEFT = 105;
static constexpr uint32_t KEY_RIGHT = 106;
static constexpr uint32_t KEY_UP = 103;
static constexpr uint32_t KEY_DOWN = 108;
static constexpr uint32_t BTN_LEFT = 0x110;

static constexpr uint32_t BG_COLOR = 0xFF13111C;
static constexpr int32_t  BOX_SIZE = 100;
static constexpr float    DECAY = 2.0f;
static constexpr float    BASE_SPEED = 600.0f;
static constexpr float    KICK = BASE_SPEED * 2.0f;

struct App {
    std::unique_ptr<wl::Compositor>                      compositor;
    std::unique_ptr<wl::Shm>                             shm;
    std::unique_ptr<xdg::WmBase>                         wm_base;
    std::unique_ptr<wl::Seat>                            seat; // input device group (keyboard, pointer, touch)
    std::unique_ptr<xdg_decoration::DecorationManagerV1> deco_mgr;

    std::unique_ptr<wl::Surface>                          surface;
    std::unique_ptr<xdg::Surface>                         xdg_surface;
    std::unique_ptr<xdg::Toplevel>                        toplevel;
    std::unique_ptr<xdg_decoration::ToplevelDecorationV1> decoration;
    std::unique_ptr<wl::Keyboard>                         keyboard;
    std::unique_ptr<wl::Pointer>                          pointer;

    // Double-buffered SHM: one pool, two buffer objects at different offsets.
    std::unique_ptr<wl::ShmPool> pool;
    void*                        base_pixels = nullptr; // mmap covering both slots
    int                          shm_fd = -1;
    size_t                       frame_size = 0; // bytes per slot = stride * height

    struct Slot {
        std::unique_ptr<wl::Buffer> buffer;
        void*                       pixels = nullptr; // points into base_pixels
        bool                        busy = false;
    };
    std::array<Slot, 2> slots;
    int                 back = 0; // index of the slot to render into next

    int32_t width = 800, height = 600;
    int32_t nwidth = 800, nheight = 600; // compositor-requested size

    std::unique_ptr<wl::Callback> frame_cb;
    uint32_t                      last_ms = 0;

    float bx = 370.0f, by = 270.0f;
    float vx = BASE_SPEED * 0.707f, vy = BASE_SPEED * 0.707f;

    float        ptr_x = 0.0f, ptr_y = 0.0f;
    float        hue = 0.62f;
    float        sat = 0.4f, val = 0.95f;
    std::mt19937 rng{std::random_device{}()};

    bool running = true;
    bool configured = false;
};

static void create_buffers(App& app) {
    app.slots[0].buffer.reset();
    app.slots[1].buffer.reset();
    app.pool.reset();
    if (app.base_pixels) {
        munmap(app.base_pixels, app.frame_size * 2);
        app.base_pixels = nullptr;
    }
    if (app.shm_fd >= 0) {
        close(app.shm_fd);
        app.shm_fd = -1;
    }

    int32_t stride = app.width * 4;
    app.frame_size = static_cast<size_t>(stride) * app.height;
    size_t total = app.frame_size * 2;

    app.shm_fd = memfd_create("bouncy_box", MFD_CLOEXEC);
    if (app.shm_fd < 0 || ftruncate(app.shm_fd, static_cast<off_t>(total)) < 0) {
        std::print(std::cerr, "error: shm setup failed: {}\n", std::strerror(errno));
        app.running = false;
        return;
    }
    app.base_pixels = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, app.shm_fd, 0);
    if (app.base_pixels == MAP_FAILED) {
        std::print(std::cerr, "error: mmap failed: {}\n", std::strerror(errno));
        app.base_pixels = nullptr;
        app.running = false;
        return;
    }

    app.pool = app.shm->create_pool(app.shm_fd, static_cast<int32_t>(total));

    for (int i = 0; i < 2; ++i) {
        auto& s = app.slots[i];
        auto  offset = static_cast<int32_t>(i * app.frame_size);
        s.pixels = static_cast<char*>(app.base_pixels) + offset;
        s.buffer = app.pool->create_buffer(offset, app.width, app.height, stride, wl::ShmFormat::argb8888);
        s.buffer->on_release = [&s](wl::Buffer&) { s.busy = false; };
        s.busy = false;
    }
}

static uint32_t hsv_to_argb(float h, float s, float v) {
    h -= std::floor(h);
    float c = v * s;
    float x = c * (1.0f - std::abs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r, g, b;
    switch (static_cast<int>(h * 6.0f) % 6) {
        case 0:
            r = c;
            g = x;
            b = 0;
            break;
        case 1:
            r = x;
            g = c;
            b = 0;
            break;
        case 2:
            r = 0;
            g = c;
            b = x;
            break;
        case 3:
            r = 0;
            g = x;
            b = c;
            break;
        case 4:
            r = x;
            g = 0;
            b = c;
            break;
        default:
            r = c;
            g = 0;
            b = x;
            break;
    }
    return 0xFF000000u | (uint32_t((r + m) * 255) << 16) | (uint32_t((g + m) * 255) << 8) | uint32_t((b + m) * 255);
}

static void update_and_draw(App& app, App::Slot& slot, uint32_t ms) {
    float dt = (app.last_ms == 0) ? 0.016f : std::min((ms - app.last_ms) / 1000.0f, 0.1f);
    app.last_ms = ms;

    // Decay extra speed above BASE_SPEED, but never below it
    float damp = std::exp(-DECAY * dt);
    app.vx *= damp;
    app.vy *= damp;
    float speed = std::sqrt(app.vx * app.vx + app.vy * app.vy);
    if (speed < BASE_SPEED) {
        float scale = (speed > 0.001f) ? BASE_SPEED / speed : 1.0f;
        app.vx *= scale;
        app.vy = (speed > 0.001f) ? app.vy * scale : BASE_SPEED;
    }

    app.bx += app.vx * dt;
    app.by += app.vy * dt;

    bool hit = false;
    if (app.bx < 0) {
        app.bx = 0;
        app.vx = std::abs(app.vx);
        hit = true;
    } else if (app.bx + BOX_SIZE > static_cast<float>(app.width)) {
        app.bx = static_cast<float>(app.width - BOX_SIZE);
        app.vx = -std::abs(app.vx);
        hit = true;
    }
    if (app.by < 0) {
        app.by = 0;
        app.vy = std::abs(app.vy);
        hit = true;
    } else if (app.by + BOX_SIZE > static_cast<float>(app.height)) {
        app.by = static_cast<float>(app.height - BOX_SIZE);
        app.vy = -std::abs(app.vy);
        hit = true;
    }

    if (hit) {
        std::uniform_real_distribution<float> rnd_h{0.0f, 1.0f};
        std::uniform_real_distribution<float> rnd_s{0.25f, 0.55f};
        std::uniform_real_distribution<float> rnd_v{0.85f, 1.0f};
        app.hue = rnd_h(app.rng);
        app.sat = rnd_s(app.rng);
        app.val = rnd_v(app.rng);
    }
    auto* px = static_cast<uint32_t*>(slot.pixels);
    std::fill_n(px, app.width * app.height, BG_COLOR);

    int x0 = std::max(0, static_cast<int>(app.bx)), y0 = std::max(0, static_cast<int>(app.by));
    int x1 = std::min(static_cast<int>(app.width), x0 + BOX_SIZE);
    int y1 = std::min(static_cast<int>(app.height), y0 + BOX_SIZE);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            px[y * app.width + x] = hsv_to_argb(app.hue, app.sat, app.val);

    app.surface->attach(slot.buffer.get(), 0, 0);
    app.surface->damage(0, 0, app.width, app.height); // mark repainted region; compositor may skip unchanged areas
}

static void request_frame(App& app);

static void on_frame(App& app, uint32_t ms) {
    // request_frame will overwrite app.frame_cb; move first so the lambda we're executing inside stays alive
    auto old_cb = std::move(app.frame_cb);

    // Resize immediately - waiting for slot release can deadlock against the compositor.
    if (app.nwidth != app.width || app.nheight != app.height) {
        app.width = app.nwidth;
        app.height = app.nheight;
        create_buffers(app);
    }

    auto& slot = app.slots[app.back];
    if (slot.busy) {
        // Both slots held by the compositor - shouldn't happen at normal frame rates.
        // Keep the callback chain alive with a no-damage commit and wait.
        request_frame(app);
        app.surface->commit();
        return;
    }

    update_and_draw(app, slot, ms);
    request_frame(app); // register frame callback before commit
    slot.busy = true;
    app.surface->commit(); // commit activates it
    app.back ^= 1;
}

static void request_frame(App& app) {
    // frame callback fires once when the compositor is ready to show the next frame - paces the render loop
    app.frame_cb = app.surface->frame();
    app.frame_cb->on_done = [&app](wl::Callback&, uint32_t ms) { on_frame(app, ms); };
}

int main() {
    std::print("bouncy_box controls:\n  arrow keys - kick the box\n  left click - redirect toward cursor\n\n");

    auto result = kamiwayland::Display::connect();
    if (!result) {
        std::print(std::cerr, "error: {}\n", result.error());
        return 1;
    }
    auto& display = *result;

    App app;

    auto registry = wl::get_registry(display);
    registry->on_global = [&](wl::Registry& reg, uint32_t name, std::string_view iface, uint32_t ver) {
        if (reg.try_bind<wl::Compositor>(iface, name, ver, app.compositor)) return;
        if (reg.try_bind<wl::Shm>(iface, name, ver, app.shm)) return;
        if (reg.try_bind<xdg_decoration::DecorationManagerV1>(iface, name, ver, app.deco_mgr)) return;
        if (reg.try_bind<xdg::WmBase>(iface, name, ver, app.wm_base)) {
            app.wm_base->on_ping = [](xdg::WmBase& wm, uint32_t serial) { wm.pong(serial); };
            return;
        }
        if (reg.try_bind<wl::Seat>(iface, name, ver, app.seat)) {
            // on_capabilities must be wired before roundtrip - initial event fires during it
            app.seat->on_capabilities = [&](wl::Seat&, wl::SeatCapability caps) {
                if (wl::has(caps, wl::SeatCapability::keyboard) && !app.keyboard) {
                    app.keyboard = app.seat->get_keyboard();
                    app.keyboard->on_keymap = [](wl::Keyboard&, wl::KeyboardKeymapFormat, int fd, uint32_t) {
                        close(fd);
                    }; // compositor sends the keymap as an fd; close it since we don't use xkbcommon
                    app.keyboard->on_key = [&](wl::Keyboard&, uint32_t, uint32_t, uint32_t key, wl::KeyboardKeyState state) {
                        if (state != wl::KeyboardKeyState::pressed) return;
                        switch (key) {
                            case KEY_LEFT:
                                app.vx -= KICK;
                                break;
                            case KEY_RIGHT:
                                app.vx += KICK;
                                break;
                            case KEY_UP:
                                app.vy -= KICK;
                                break;
                            case KEY_DOWN:
                                app.vy += KICK;
                                break;
                            default:
                                break;
                        }
                    };
                }
                if (wl::has(caps, wl::SeatCapability::pointer) && !app.pointer) {
                    app.pointer = app.seat->get_pointer();
                    app.pointer->on_motion = [&](wl::Pointer&, uint32_t, kamiwayland::Fixed sx, kamiwayland::Fixed sy) {
                        app.ptr_x = static_cast<float>(sx.as_double());
                        app.ptr_y = static_cast<float>(sy.as_double());
                    };
                    app.pointer->on_button = [&](wl::Pointer&, uint32_t, uint32_t, uint32_t btn, wl::PointerButtonState state) {
                        if (btn != BTN_LEFT || state != wl::PointerButtonState::pressed) return;
                        float dx = app.ptr_x - (app.bx + BOX_SIZE / 2.0f);
                        float dy = app.ptr_y - (app.by + BOX_SIZE / 2.0f);
                        float len = std::sqrt(dx * dx + dy * dy);
                        if (len > 1.0f) {
                            float speed = std::sqrt(app.vx * app.vx + app.vy * app.vy);
                            app.vx = dx / len * speed;
                            app.vy = dy / len * speed;
                        }
                    };
                }
            };
        }
    };

    if (auto r = display.roundtrip(); !r) {
        std::print(std::cerr, "error: {}\n", r.error());
        return 1;
    }
    if (!app.compositor || !app.shm || !app.wm_base || !app.seat) {
        std::print(std::cerr, "error: missing required globals\n");
        return 1;
    }

    app.surface = app.compositor->create_surface();
    app.xdg_surface = app.wm_base->get_xdg_surface(*app.surface);
    app.toplevel = app.xdg_surface->get_toplevel();

    app.toplevel->set_title("bouncy_box");
    app.toplevel->set_app_id("kamiwayland.bouncy_box");
    app.toplevel->set_min_size(200, 150);

    if (app.deco_mgr) {
        app.decoration = app.deco_mgr->get_toplevel_decoration(*app.toplevel);
        app.decoration->set_mode(xdg_decoration::ToplevelDecorationV1Mode::server_side);
    }

    app.toplevel->on_close = [&](xdg::Toplevel&) { app.running = false; };
    app.toplevel->on_configure = [&](xdg::Toplevel&, int32_t w, int32_t h, std::span<const std::byte>) {
        if (w > 0) app.nwidth = w;
        if (h > 0) app.nheight = h;
    };

    app.xdg_surface->on_configure = [&](xdg::Surface& surf, uint32_t serial) {
        surf.ack_configure(serial);

        if (!app.configured) {
            app.width = app.nwidth;
            app.height = app.nheight;
            create_buffers(app);
            app.configured = true;
            update_and_draw(app, app.slots[app.back], 0);
            request_frame(app); // register before commit
            app.slots[app.back].busy = true;
            app.surface->commit(); // activates the frame callback
            app.back ^= 1;
        }
    };

    app.surface->commit();

    struct pollfd pfd{display.fd(), POLLIN, 0};
    while (app.running) {
        while (display.has_buffered()) {
            if (auto r = display.dispatch_once(); !r) {
                std::print(std::cerr, "error: {}\n", r.error());
                return 1;
            }
            if (!app.running) return 0;
        }
        if (poll(&pfd, 1, -1) < 0 || !(pfd.revents & POLLIN)) break;
        if (auto r = display.dispatch_once(); !r) {
            std::print(std::cerr, "error: {}\n", r.error());
            return 1;
        }
    }
}