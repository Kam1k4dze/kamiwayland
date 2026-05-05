import kamiwayland;
import wl;
import std;

using std::uint32_t;

int main() {
    // connects to the running compositor via $WAYLAND_DISPLAY
    auto result = kamiwayland::Display::connect();
    if (!result) {
        std::print(std::cerr, "error: {}\n", result.error());
        return 1;
    }
    auto& display = *result;

    auto registry = wl::get_registry(display);
    // on_global fires once per protocol object the compositor advertises (compositor, shm, seat, …)
    registry->on_global = [](wl::Registry&, uint32_t name, std::string_view iface, uint32_t ver) {
        std::println("{:4}  {}  v{}", name, iface, ver);
    };

    // flush pending requests and block until the server finishes - triggers on_global for each advertised global
    if (auto r = display.roundtrip(); !r) {
        std::print(std::cerr, "error: {}\n", r.error());
        return 1;
    }
}
