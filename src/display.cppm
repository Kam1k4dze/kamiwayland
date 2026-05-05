export module kamiwayland:display;
import :connection;
import :wire;
import std;

using std::uint16_t;
using std::uint32_t;

export namespace kamiwayland {

class Display;

template<typename T>
concept WaylandInterface = requires {
    { T::interface_name } -> std::convertible_to<std::string_view>;
    { T::interface_version } -> std::convertible_to<uint32_t>;
};

class Object {
public:
    Object(Display& display, uint32_t id, uint32_t version);
    virtual ~Object();

    Object(const Object&) = delete;
    Object& operator=(const Object&) = delete;

    [[nodiscard]] uint32_t id() const noexcept { return m_id; }
    [[nodiscard]] uint32_t version() const noexcept { return m_version; }

    virtual void dispatch(uint16_t opcode, std::span<const std::byte> payload) = 0;

protected:
    void version_check(uint32_t required) const noexcept {
        if (m_version < required) {
            std::println(std::cerr, "kamiwayland: request requires version {} but object version is {}", required, m_version);
            std::abort();
        }
    }

    template<typename... Args> void send(uint16_t opcode, Args&&... args) {
        MsgBuf buf;
        if constexpr (sizeof...(args) > 0) (buf << ... << std::forward<Args>(args));
        do_send(opcode, buf);
    }

    template<WaylandInterface T> [[nodiscard]] std::unique_ptr<T> alloc_child(uint32_t ver) {
        return std::make_unique<T>(m_display, do_alloc_id(), ver);
    }

    Display& m_display;
    uint32_t m_id;
    uint32_t m_version;
    bool     m_destroyed = false;

private:
    void     do_send(uint16_t opcode, const MsgBuf& buf);
    uint32_t do_alloc_id();
};

class Display {
public:
    [[nodiscard]] static std::expected<Display, std::string> connect() {
        return Connection::connect().transform([](Connection c) { return Display(std::move(c)); });
    }

    ~Display() = default;
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;
    Display(Display&&) noexcept = default;

    template<WaylandInterface T> [[nodiscard]] std::unique_ptr<T> request_object(uint16_t opcode) {
        uint32_t id = alloc_id();
        MsgBuf   buf;
        buf << id;
        raw_send(1, opcode, buf.data(), {});
        return std::make_unique<T>(*this, id, T::interface_version);
    }

    [[nodiscard]] int  fd() const noexcept { return m_conn.fd(); }
    [[nodiscard]] bool has_buffered() const noexcept { return m_conn.has_buffered(); }

    [[nodiscard]] std::expected<void, std::string> dispatch_once() {
        auto msg = m_conn.recv_one();
        if (!msg) return std::unexpected(msg.error());

        // fds are consumed lazily via take_fd() during dispatch (FIFO across messages)
        if (msg->obj_id == 1) {
            handle_display_event(msg->opcode, msg->payload);
        } else {
            auto it = m_objects.find(msg->obj_id);
            if (it != m_objects.end()) it->second->dispatch(msg->opcode, msg->payload);
        }
        // After a wl_display.error the connection is permanently broken.
        if (m_error) return std::unexpected(*m_error);
        return {};
    }

    // Consume the next fd from the stream in arrival order.
    // Called by generated dispatch code for 'h'-type arguments.
    [[nodiscard]] int take_fd() noexcept { return m_conn.take_fd(); }

    [[nodiscard]] std::expected<void, std::string> roundtrip();

    void register_object(Object* obj) { m_objects.emplace(obj->id(), obj); }
    void unregister_object(uint32_t id) { m_objects.erase(id); }

    [[nodiscard]] uint32_t alloc_id() {
        if (!m_free_ids.empty()) {
            uint32_t id = m_free_ids.back();
            m_free_ids.pop_back();
            return id;
        }
        return m_next_id++;
    }

    void raw_send(uint32_t obj_id, uint16_t opcode, std::span<const std::byte> payload, std::span<const int> fds = {}) {
        // errors surface on next recv_one - deliberately not propagated here
        std::ignore = m_conn.send(obj_id, opcode, payload, fds);
    }

    std::function<void(uint32_t obj_id, uint32_t code, std::string_view msg)> on_error;

private:
    explicit Display(Connection conn) : m_conn(std::move(conn)) {}

    void handle_display_event(uint16_t opcode, std::span<const std::byte> payload);

    Connection                       m_conn;
    std::flat_map<uint32_t, Object*> m_objects;
    std::vector<uint32_t>            m_free_ids;
    std::optional<std::string>       m_error;
    uint32_t                         m_next_id = 2;
};

// MsgBuf overloads for encoding object IDs in request arguments.
// Non-null Object& → its id. Object* → id or 0 for null (nullable object args).
inline MsgBuf& operator<<(MsgBuf& buf, const Object& obj) {
    return buf << obj.id();
}
inline MsgBuf& operator<<(MsgBuf& buf, const Object* obj) {
    return buf << (obj ? obj->id() : 0u);
}

} // namespace kamiwayland

// Definitions that require both Object and Display to be complete

namespace kamiwayland {

// Internal sync callback - not exported
struct SyncCallback final : Object {
    static constexpr std::string_view interface_name = "wl_callback";
    static constexpr uint32_t         interface_version = 1;
    bool                              done = false;
    using Object::Object;
    void dispatch(uint16_t opcode, std::span<const std::byte>) override {
        if (opcode == 0) {
            done = true;
            m_destroyed = true;
        }
    }
};

inline Object::Object(Display& d, uint32_t id, uint32_t version) : m_display(d), m_id(id), m_version(version) {
    d.register_object(this);
}

inline Object::~Object() {
    m_display.unregister_object(m_id);
}

inline void Object::do_send(uint16_t opcode, const MsgBuf& buf) {
    m_display.raw_send(m_id, opcode, buf.data(), buf.fds());
}

inline uint32_t Object::do_alloc_id() {
    return m_display.alloc_id();
}

inline std::expected<void, std::string> Display::roundtrip() {
    auto cb = request_object<SyncCallback>(0);
    while (!cb->done)
        if (auto r = dispatch_once(); !r) return r;
    return {};
}

inline void Display::handle_display_event(uint16_t opcode, std::span<const std::byte> payload) {
    wire::SpanReader s(payload);
    switch (opcode) {
        case 0: { // wl_display::error - connection is permanently broken
            auto id = wire::read<uint32_t>(s);
            auto code = wire::read<uint32_t>(s);
            auto msg = wire::read<std::string>(s);
            m_error = std::format("wl error obj={} code={}: {}", id, code, msg);
            if (on_error) on_error(id, code, *m_error);
            break;
        }
        case 1: { // wl_display::delete_id - ID safe to reuse
            uint32_t id = wire::read<uint32_t>(s);
            m_objects.erase(id);
            m_free_ids.push_back(id);
            break;
        }
    }
}

} // namespace kamiwayland