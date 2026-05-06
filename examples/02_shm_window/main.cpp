#include <sys/mman.h>
#include <poll.h>
#include <unistd.h>

import kamiwayland;
import wl;
import xdg;
import xdg_decoration;
import std;

using std::int32_t;
using std::uint32_t;

static constexpr uint32_t COLOR = 0xFF'20'60'A0; // ARGB: opaque blue

struct App {
    std::unique_ptr<wl::Compositor>                      compositor;
    std::unique_ptr<wl::Shm>                             shm;
    std::unique_ptr<xdg::WmBase>                         wm_base;
    std::unique_ptr<xdg_decoration::DecorationManagerV1> deco_mgr;

    std::unique_ptr<wl::Surface>                          surface;
    std::unique_ptr<xdg::Surface>                         xdg_surface;
    std::unique_ptr<xdg::Toplevel>                        toplevel;
    std::unique_ptr<xdg_decoration::ToplevelDecorationV1> decoration;

    std::unique_ptr<wl::ShmPool> pool;
    std::unique_ptr<wl::Buffer>  buffer;
    void*                        pixels = nullptr;
    int                          shm_fd = -1;
    int32_t                      map_size = 0; // actual mapped size - needed for munmap

    int32_t width = 640, height = 480;
    int32_t nwidth = 640, nheight = 480; // compositor-requested size

    bool running = true;
    bool configured = false;
    bool buf_busy = false; // compositor holds the buffer, don't destroy
};

static void create_buffer(App& app);

static void do_draw(App& app) {
    app.surface->attach(app.buffer.get(), 0, 0);
    app.surface->damage(0, 0, app.width, app.height); // mark repainted region; compositor may skip unchanged areas
    app.buf_busy = true;
    app.surface->commit();
}

static void create_buffer(App& app) {
    if (app.pixels) {
        munmap(app.pixels, app.map_size); // use tracked size, not current width*height
        app.pixels = nullptr;
    }
    if (app.shm_fd >= 0) {
        close(app.shm_fd);
        app.shm_fd = -1;
    }

    int32_t stride = app.width * 4;
    app.map_size = stride * app.height;

    // memfd_create gives an anonymous file both sides mmap - pixels reach the compositor with no copy
    app.shm_fd = memfd_create("shm_window", MFD_CLOEXEC);
    ftruncate(app.shm_fd, app.map_size);
    app.pixels = mmap(nullptr, app.map_size, PROT_READ | PROT_WRITE, MAP_SHARED, app.shm_fd, 0);
    std::fill_n(static_cast<uint32_t*>(app.pixels), app.width * app.height, COLOR);

    app.pool = app.shm->create_pool(app.shm_fd, app.map_size);
    app.buffer = app.pool->create_buffer(0, app.width, app.height, stride, wl::ShmFormat::argb8888);
    app.buffer->on_release = [&app](wl::Buffer&) { app.buf_busy = false; };
}

int main() {
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
            app.wm_base->on_ping = [](xdg::WmBase& wm, uint32_t serial) {
                wm.pong(serial);
            }; // compositor may mark us as unresponsive if we don't pong
        }
    };

    // on_global fires for each compositor global during this call
    if (auto r = display.roundtrip(); !r) {
        std::print(std::cerr, "error: {}\n", r.error());
        return 1;
    }
    if (!app.compositor || !app.shm || !app.wm_base) {
        std::print(std::cerr, "error: missing required globals\n");
        return 1;
    }

    app.surface = app.compositor->create_surface();
    // xdg_shell turns a bare wl_surface into a desktop window (title bar, resize, close)
    app.xdg_surface = app.wm_base->get_xdg_surface(*app.surface);
    app.toplevel = app.xdg_surface->get_toplevel();

    app.toplevel->set_title("shm_window");
    app.toplevel->set_app_id("kamiwayland.shm_window");
    app.toplevel->set_min_size(160, 120);

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
        surf.ack_configure(serial); // configure = compositor requesting new size/state; ack before next commit

        bool size_changed = (app.nwidth != app.width || app.nheight != app.height);

        if (!app.configured) {
            app.width = app.nwidth;
            app.height = app.nheight;
            create_buffer(app);
            app.configured = true;
            do_draw(app);
        } else if (size_changed) {
            // Resize immediately - waiting for buf_busy to clear can deadlock against the compositor.
            app.buf_busy = false;
            app.width = app.nwidth;
            app.height = app.nheight;
            create_buffer(app);
            do_draw(app);
        }
    };

    app.surface->commit(); // trigger initial configure

    struct pollfd pfd{display.fd(), POLLIN, 0};

    while (app.running) {
        // Drain already-buffered messages before sleeping on the socket
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
