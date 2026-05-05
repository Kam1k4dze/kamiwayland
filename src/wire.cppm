export module kamiwayland:wire;
import std;

using std::int32_t;
using std::uint32_t;

export namespace kamiwayland {

// Fixed-point 24.8

struct Fixed {
    int32_t raw;

    [[nodiscard]] static Fixed from_double(double d) noexcept { return {static_cast<int32_t>(d * 256.0)}; }
    [[nodiscard]] static Fixed from_int(int32_t i) noexcept { return {i << 8}; }
    [[nodiscard]] double       as_double() const noexcept { return raw / 256.0; }
    [[nodiscard]] int32_t      as_int() const noexcept { return raw >> 8; }
};

// Out-of-band fd wrapper

struct Fd {
    int value;
};

// Message encoder
//
// Accumulates a request payload + fd list.
// operator<< dispatches at compile time per argument type.

class MsgBuf {
    std::vector<std::byte> m_data;
    std::vector<int>       m_fds;

    void append4(uint32_t v) {
        auto b = std::bit_cast<std::array<std::byte, 4>>(v);
        m_data.append_range(b);
    }

    void pad() { m_data.resize((m_data.size() + 3) & ~3u, std::byte{0}); }

public:
    MsgBuf& operator<<(uint32_t v) {
        append4(v);
        return *this;
    }
    MsgBuf& operator<<(int32_t v) {
        append4(std::bit_cast<uint32_t>(v));
        return *this;
    }
    MsgBuf& operator<<(Fixed f) {
        append4(std::bit_cast<uint32_t>(f.raw));
        return *this;
    }

    template<typename T>
        requires std::is_enum_v<T>
    MsgBuf& operator<<(T v) {
        return *this << std::to_underlying(v);
    }

    MsgBuf& operator<<(std::string_view s) {
        auto len = static_cast<uint32_t>(s.size() + 1); // +1 for null terminator
        append4(len);
        m_data.append_range(std::as_bytes(std::span{s}));
        m_data.push_back(std::byte{0});
        pad();
        return *this;
    }

    MsgBuf& operator<<(std::span<const std::byte> arr) {
        append4(static_cast<uint32_t>(arr.size()));
        m_data.insert(m_data.end(), arr.begin(), arr.end());
        pad();
        return *this;
    }

    MsgBuf& operator<<(Fd fd) {
        m_fds.push_back(fd.value);
        return *this;
    }

    [[nodiscard]] std::span<const std::byte> data() const noexcept { return m_data; }
    [[nodiscard]] std::span<const int>       fds() const noexcept { return m_fds; }
};

// Message decoder
//
// SpanReader is a lightweight cursor over an immutable byte span.
// Used by generated dispatch() methods to decode incoming event payloads.

namespace wire {

class SpanReader {
    const std::byte* m_cur;
    const std::byte* m_end;

public:
    explicit SpanReader(std::span<const std::byte> s) noexcept : m_cur(s.data()), m_end(s.data() + s.size()) {}

    void read(char* dest, std::size_t n) noexcept {
        std::size_t avail = static_cast<std::size_t>(m_end - m_cur);
        std::size_t count = std::min(n, avail);
        std::memcpy(dest, m_cur, count);
        m_cur += count;
    }

    void skip(std::size_t n) noexcept {
        std::size_t avail = static_cast<std::size_t>(m_end - m_cur);
        m_cur += std::min(n, avail);
    }
};

template<typename T> [[nodiscard]] T read(SpanReader&);

template<> [[nodiscard]] inline uint32_t read<uint32_t>(SpanReader& s) {
    std::array<char, 4> b{};
    s.read(b.data(), 4);
    return std::bit_cast<uint32_t>(b);
}

template<> [[nodiscard]] inline int32_t read<int32_t>(SpanReader& s) {
    return std::bit_cast<int32_t>(read<uint32_t>(s));
}

template<> [[nodiscard]] inline Fixed read<Fixed>(SpanReader& s) {
    return Fixed{std::bit_cast<int32_t>(read<uint32_t>(s))};
}

template<> [[nodiscard]] inline std::string read<std::string>(SpanReader& s) {
    uint32_t len = read<uint32_t>(s); // includes null terminator
    if (len == 0) return {};
    std::string str(len, '\0');
    s.read(str.data(), len);
    str.resize(len - 1);             // strip null
    s.skip(((len + 3) & ~3u) - len); // alignment padding
    return str;
}

} // namespace wire
} // namespace kamiwayland