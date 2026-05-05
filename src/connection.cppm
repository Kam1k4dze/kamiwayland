module;
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

export module kamiwayland:connection;
import std;

using std::uint16_t;
using std::uint32_t;

export namespace kamiwayland {

class Connection {
public:
    struct Message {
        uint32_t               obj_id;
        uint16_t               opcode;
        std::vector<std::byte> payload;
    };

    Connection() = default;
    ~Connection() {
        if (m_fd >= 0) ::close(m_fd);
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& o) noexcept
        : m_fd(std::exchange(o.m_fd, -1))
        , m_recv_buf(std::move(o.m_recv_buf))
        , m_fd_queue(std::move(o.m_fd_queue)) {}

    Connection& operator=(Connection&& o) noexcept {
        if (this != &o) {
            if (m_fd >= 0) ::close(m_fd);
            m_fd = std::exchange(o.m_fd, -1);
            m_recv_buf = std::move(o.m_recv_buf);
            m_fd_queue = std::move(o.m_fd_queue);
        }
        return *this;
    }

    [[nodiscard]] static std::expected<Connection, std::string> connect() {
        const char* display = std::getenv("WAYLAND_DISPLAY");
        if (!display) display = "wayland-0";

        std::string path;
        if (display[0] == '/') {
            path = display;
        } else {
            const char* runtime = std::getenv("XDG_RUNTIME_DIR");
            if (!runtime) return std::unexpected("XDG_RUNTIME_DIR not set");
            path = std::string(runtime) + '/' + display;
        }

        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return std::unexpected(std::format("socket: {}", ::strerror(errno)));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            ::close(fd);
            return std::unexpected("socket path too long");
        }
        std::ranges::copy(path, addr.sun_path);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            auto err = std::format("connect {}: {}", path, ::strerror(errno));
            ::close(fd);
            return std::unexpected(std::move(err));
        }

        return Connection(fd);
    }

    [[nodiscard]] std::expected<void, std::string> send(uint32_t obj_id, uint16_t opcode, std::span<const std::byte> payload,
                                                        std::span<const int> fds = {}) {
        std::array<uint32_t, 2> header{obj_id, (static_cast<uint32_t>(payload.size() + 8) << 16) | opcode};

        std::array<iovec, 2> iov{};
        iov[0].iov_base = header.data();
        iov[0].iov_len = 8;
        iov[1].iov_base = const_cast<std::byte*>(payload.data());
        iov[1].iov_len = payload.size();

        msghdr msg{};
        msg.msg_iov = iov.data();
        msg.msg_iovlen = payload.empty() ? 1 : 2;

        std::vector<std::byte> cmsg_buf;
        if (!fds.empty()) {
            cmsg_buf.resize(CMSG_SPACE(fds.size() * sizeof(int)));
            msg.msg_control = cmsg_buf.data();
            msg.msg_controllen = cmsg_buf.size();
            auto* cmsg = CMSG_FIRSTHDR(&msg);
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;
            cmsg->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
            std::memcpy(CMSG_DATA(cmsg), fds.data(), fds.size() * sizeof(int));
        }

        if (::sendmsg(m_fd, &msg, MSG_NOSIGNAL) < 0) return std::unexpected(std::format("sendmsg: {}", ::strerror(errno)));

        return {};
    }

    [[nodiscard]] std::expected<Message, std::string> recv_one() {
        while (m_recv_buf.size() - m_recv_head < 8) {
            if (auto r = recv_some(); !r) return std::unexpected(r.error());
        }

        auto read_u32 = [&](std::size_t off) noexcept {
            uint32_t v;
            std::memcpy(&v, m_recv_buf.data() + m_recv_head + off, 4);
            return v;
        };

        const uint32_t obj_id = read_u32(0);
        const uint32_t size_op = read_u32(4);
        const uint16_t opcode = static_cast<uint16_t>(size_op & 0xFFFFu);
        const uint32_t msg_size = size_op >> 16;

        if (msg_size < 8) return std::unexpected(std::format("malformed message: size={}", msg_size));

        while (m_recv_buf.size() - m_recv_head < msg_size) {
            if (auto r = recv_some(); !r) return std::unexpected(r.error());
        }

        Message out;
        out.obj_id = obj_id;
        out.opcode = opcode;
        out.payload = std::vector<std::byte>(m_recv_buf.data() + m_recv_head + 8, m_recv_buf.data() + m_recv_head + msg_size);
        // fds are NOT assigned per-message; they stay in m_fd_queue and are
        // consumed lazily via take_fd() in the order they appear in the stream.

        m_recv_head += msg_size;
        return out;
    }

    [[nodiscard]] int  fd() const noexcept { return m_fd; }
    [[nodiscard]] bool has_buffered() const noexcept {
        const auto avail = m_recv_buf.size() - m_recv_head;
        if (avail < 8) return false;
        uint32_t size_op;
        std::memcpy(&size_op, m_recv_buf.data() + m_recv_head + 4, 4);
        return avail >= (size_op >> 16);
    }

    // Consume the next fd from the stream in arrival order.
    // Call this for each 'h'-type argument when dispatching events.
    [[nodiscard]] int take_fd() noexcept {
        if (m_fd_head >= m_fd_queue.size()) return -1;
        int fd = m_fd_queue[m_fd_head++];
        // Compact when fully drained - keeps vector contiguous, avoids repeated allocs.
        if (m_fd_head == m_fd_queue.size()) {
            m_fd_queue.clear();
            m_fd_head = 0;
        }
        return fd;
    }

private:
    explicit Connection(int fd) : m_fd(fd) {}

    [[nodiscard]] std::expected<void, std::string> recv_some() {
        if (m_recv_head > 0) {
            m_recv_buf.erase(m_recv_buf.begin(), m_recv_buf.begin() + m_recv_head);
            m_recv_head = 0;
        }

        std::array<char, 4096>                                          buf{};
        alignas(cmsghdr) std::array<char, CMSG_SPACE(28 * sizeof(int))> cmsg_buf{};

        iovec  iov{.iov_base = buf.data(), .iov_len = buf.size()};
        msghdr hdr{.msg_name = nullptr,
                   .msg_namelen = 0,
                   .msg_iov = &iov,
                   .msg_iovlen = 1,
                   .msg_control = cmsg_buf.data(),
                   .msg_controllen = cmsg_buf.size(),
                   .msg_flags = 0};

        const ssize_t n = ::recvmsg(m_fd, &hdr, 0);
        if (n < 0) return std::unexpected(std::format("recvmsg: {}", ::strerror(errno)));
        if (n == 0) return std::unexpected("connection closed by compositor");

        m_recv_buf.append_range(std::span{reinterpret_cast<const std::byte*>(buf.data()), static_cast<size_t>(n)});

        for (cmsghdr* cmsg = CMSG_FIRSTHDR(&hdr); cmsg; cmsg = CMSG_NXTHDR(&hdr, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                const auto  n_fds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                const auto* data = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
                m_fd_queue.append_range(std::span{data, n_fds});
            }
        }

        return {};
    }

    int                    m_fd = -1;
    std::vector<std::byte> m_recv_buf;
    size_t                 m_recv_head = 0;
    std::vector<int>       m_fd_queue;
    size_t                 m_fd_head = 0;
};

} // namespace kamiwayland