module kscanner;
import :parser;
import std;

using std::size_t;
using std::uint32_t;
using enum kscanner::ArgType;

static std::string to_pascal(std::string_view s) {
    std::string out;
    bool        cap = true;
    for (char c : s) {
        if (c == '_')
            cap = true;
        else if (cap) {
            out += static_cast<char>(std::toupper(c));
            cap = false;
        } else
            out += c;
    }
    return out;
}

static std::string find_prefix(const kscanner::Protocol& proto) {
    if (proto.interfaces.empty()) return {};
    size_t len = proto.interfaces[0].name.size();
    for (const auto& iface : proto.interfaces) {
        size_t n = std::min(len, iface.name.size());
        size_t i = 0;
        while (i < n && iface.name[i] == proto.interfaces[0].name[i])
            ++i;
        len = i;
    }
    auto pfx = proto.interfaces[0].name.substr(0, len);
    auto pos = pfx.rfind('_');
    return pos != std::string::npos ? pfx.substr(0, pos + 1) : "";
}

static std::string class_name(const std::string& iface_name, const std::string& pfx) {
    std::string_view s = iface_name;
    if (s.starts_with(pfx)) s.remove_prefix(pfx.size());
    return to_pascal(s);
}

static std::string sanitize(std::string_view name) {
    std::string s(name);
    // Enum entry names can start with digits (e.g. wl_output::transform "90").
    if (!s.empty() && std::isdigit(static_cast<unsigned char>(s[0]))) s = 'v' + s;
    // clang-format off
    constexpr std::array keywords{"alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
    "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
    "const_cast", "continue", "contract_assert", "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
    "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int",
    "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
    "public", "register", "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
    "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"};
    // clang-format on
    if (std::ranges::contains(keywords, s)) s += '_';
    return s;
}

static std::string resolve_enum(std::string_view ref, const kscanner::Interface& iface, const kscanner::Protocol& proto,
                                const std::string& pfx) {
    auto dot = ref.find('.');
    if (dot == std::string_view::npos) return class_name(iface.name, pfx) + to_pascal(ref);
    auto iface_part = std::string(ref.substr(0, dot));
    auto enum_part = ref.substr(dot + 1);
    for (const auto& other : proto.interfaces) {
        auto        cn = class_name(other.name, pfx);
        std::string cn_lower(cn.size(), '\0');
        std::ranges::transform(cn, cn_lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string stripped = other.name.starts_with(pfx) ? other.name.substr(pfx.size()) : other.name;
        if (cn_lower == iface_part || stripped == iface_part || other.name == iface_part) return cn + to_pascal(enum_part);
    }
    return to_pascal(iface_part + "_" + std::string(enum_part));
}

static std::string ev_cb_type(const kscanner::Arg& arg, const kscanner::Interface& iface, const kscanner::Protocol& proto,
                              const std::string& pfx) {
    if (arg.type == int32) {
        if (!arg.enum_ref.empty()) return resolve_enum(arg.enum_ref, iface, proto, pfx);
        return "int32_t";
    }
    if (arg.type == uint32) {
        if (!arg.enum_ref.empty()) return resolve_enum(arg.enum_ref, iface, proto, pfx);
        return "uint32_t";
    }
    if (arg.type == fixed) return "kamiwayland::Fixed";
    if (arg.type == string) return "std::string_view";
    if (arg.type == object) return "uint32_t";
    if (arg.type == fd) return "int";
    if (arg.type == array) return "std::span<const std::byte>";
    if (arg.type == new_id) {
        bool is_local = !arg.interface.empty() && arg.interface.starts_with(pfx);
        return is_local ? std::format("std::unique_ptr<{}>", class_name(arg.interface, pfx)) : "uint32_t";
    }
    return "uint32_t";
}

static std::pair<std::string, std::string> req_arg_cxx(const kscanner::Arg& arg, const std::string& pfx, const kscanner::Interface& iface,
                                                       const kscanner::Protocol& proto) {
    std::string nm = sanitize(arg.name);
    if (arg.type == int32) {
        if (!arg.enum_ref.empty()) return {resolve_enum(arg.enum_ref, iface, proto, pfx), nm};
        return {"int32_t", nm};
    }
    if (arg.type == uint32) {
        if (!arg.enum_ref.empty()) return {resolve_enum(arg.enum_ref, iface, proto, pfx), nm};
        return {"uint32_t", nm};
    }
    if (arg.type == fixed) return {"kamiwayland::Fixed", nm};
    if (arg.type == string) return {"std::string_view", nm};
    if (arg.type == fd) return {"int", std::format("kamiwayland::Fd{{{}}}", nm)};
    if (arg.type == array) return {"std::span<const std::byte>", nm};
    if (arg.type == object) {
        bool        is_local = !arg.interface.empty() && arg.interface.starts_with(pfx);
        std::string cn = is_local ? class_name(arg.interface, pfx) : "kamiwayland::Object";
        return {arg.nullable ? cn + "*" : cn + "&", nm};
    }
    return {"uint32_t", nm};
}

static std::vector<size_t> topo_sort(const kscanner::Protocol& proto) {
    size_t                        n = proto.interfaces.size();
    std::map<std::string, size_t> idx;
    for (size_t i = 0; i < n; ++i)
        idx[proto.interfaces[i].name] = i;

    std::vector<std::vector<size_t>> deps(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& iface = proto.interfaces[i];
        auto        add = [&](const std::string& name) {
            auto it = idx.find(name);
            if (it != idx.end() && it->second != i) deps[i].push_back(it->second);
        };
        for (const auto& req : iface.requests)
            for (const auto& arg : req.args)
                if (arg.type == new_id && !arg.interface.empty()) add(arg.interface);
        for (const auto& ev : iface.events)
            for (const auto& arg : ev.args)
                if (arg.type == new_id && !arg.interface.empty()) add(arg.interface);
    }

    // Kahn's algorithm
    std::vector<size_t>              in_degree(n, 0);
    std::vector<std::vector<size_t>> rev(n);
    for (size_t i = 0; i < n; ++i)
        for (size_t d : deps[i]) {
            rev[d].push_back(i);
            ++in_degree[i];
        }

    std::queue<size_t> q;
    for (size_t i = 0; i < n; ++i)
        if (in_degree[i] == 0) q.push(i);

    std::vector<size_t> order;
    order.reserve(n);
    while (!q.empty()) {
        size_t cur = q.front();
        q.pop();
        order.push_back(cur);
        for (size_t nxt : rev[cur])
            if (--in_degree[nxt] == 0) q.push(nxt);
    }
    if (order.size() != n) {
        std::set<size_t> done(order.begin(), order.end());
        for (size_t i = 0; i < n; ++i)
            if (!done.contains(i)) order.push_back(i);
    }
    return order;
}

static std::string emit_enum(const kscanner::Enum& en, const kscanner::Interface& iface, const std::string& pfx) {
    std::string full = class_name(iface.name, pfx) + to_pascal(en.name);
    std::string out;
    out += std::format("enum class {} : uint32_t {{\n", full);
    for (const auto& e : en.entries)
        out += std::format("    {} = {},\n", sanitize(e.name), e.value);
    out += "};\n";
    if (en.bitfield) {
        out += std::format("constexpr {0} operator|({0} a, {0} b) noexcept {{\n"
                           "    return static_cast<{0}>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));\n"
                           "}}\n"
                           "constexpr {0} operator&({0} a, {0} b) noexcept {{\n"
                           "    return static_cast<{0}>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));\n"
                           "}}\n"
                           "constexpr {0} operator~({0} a) noexcept {{\n"
                           "    return static_cast<{0}>(~static_cast<uint32_t>(a));\n"
                           "}}\n"
                           "constexpr bool has({0} a, {0} b) noexcept {{\n"
                           "    return static_cast<uint32_t>(a & b) != 0;\n"
                           "}}\n",
                           full);
    }
    out += "\n";
    return out;
}

static std::string emit_factory(const kscanner::Message& req, size_t nid, const std::string& pfx, const kscanner::Interface& iface,
                                const kscanner::Protocol& proto) {
    const std::string ret = class_name(req.args[nid].interface, pfx);
    std::string       params, send_args;
    for (const auto& arg : req.args) {
        if (arg.type == new_id) {
            send_args += (send_args.empty() ? "" : ", ") + std::string("obj->id()");
            continue;
        }
        auto [pt, se] = req_arg_cxx(arg, pfx, iface, proto);
        params += (params.empty() ? "" : ", ") + pt + " " + sanitize(arg.name);
        send_args += (send_args.empty() ? "" : ", ") + se;
    }
    std::string out;
    out += std::format("    [[nodiscard]] std::unique_ptr<{}> {}({}) {{\n", ret, req.name, params);
    if (req.since > 1) out += std::format("        version_check({});\n", req.since);
    out += std::format("        auto obj = alloc_child<{}>(m_version);\n", ret);
    out += std::format("        send({}{});\n", req.opcode, send_args.empty() ? "" : ", " + send_args);
    out += "        return obj;\n    }\n\n";
    return out;
}

static std::string emit_template_bind(const kscanner::Message& req, const std::string& pfx, const kscanner::Interface& iface,
                                      const kscanner::Protocol& proto) {
    std::string params, send_args;
    for (const auto& arg : req.args) {
        if (arg.type == new_id) {
            send_args += (send_args.empty() ? "" : ", ") + std::string("T::interface_name, ver, obj->id()");
            continue;
        }
        auto [pt, se] = req_arg_cxx(arg, pfx, iface, proto);
        params += (params.empty() ? "" : ", ") + pt + " " + sanitize(arg.name);
        send_args += (send_args.empty() ? "" : ", ") + se;
    }
    params += (params.empty() ? "" : ", ") + std::string("uint32_t version");
    std::string out;
    // bind<T>: caps version at the negotiated interface version
    out += "    template<kamiwayland::WaylandInterface T>\n";
    out += std::format("    [[nodiscard]] std::unique_ptr<T> {}({}) {{\n", req.name, params);
    if (req.since > 1) out += std::format("        version_check({});\n", req.since);
    out += "        const uint32_t ver = std::min(version, T::interface_version);\n";
    out += "        auto obj = alloc_child<T>(ver);\n";
    out += std::format("        send({}{});\n", req.opcode, send_args.empty() ? "" : ", " + send_args);
    out += "        return obj;\n    }\n\n";
    // try_bind<T>: type-safe helper for on_global handlers
    out += "    template<kamiwayland::WaylandInterface T>\n";
    out += std::format("    bool try_{}(std::string_view iface, {}, std::unique_ptr<T>& target) {{\n", req.name, params);
    out += std::format("        if (iface != T::interface_name) return false;\n");
    out += std::format("        target = {}<T>({});\n", req.name, [&] {
        std::string args;
        for (const auto& arg : req.args) {
            if (arg.type == new_id) continue;
            args += (args.empty() ? "" : ", ") + sanitize(arg.name);
        }
        args += (args.empty() ? "" : ", ") + std::string("version");
        return args;
    }());
    out += "        return true;\n    }\n\n";
    return out;
}

static std::string emit_regular_request(const kscanner::Message& req, const std::string& pfx, const kscanner::Interface& iface,
                                        const kscanner::Protocol& proto) {
    std::string params, send_args;
    for (const auto& arg : req.args) {
        auto [pt, se] = req_arg_cxx(arg, pfx, iface, proto);
        params += (params.empty() ? "" : ", ") + pt + " " + sanitize(arg.name);
        send_args += (send_args.empty() ? "" : ", ") + se;
    }
    if (req.since <= 1) {
        if (send_args.empty()) return std::format("    void {}() {{ send({}); }}\n\n", req.name, req.opcode);
        return std::format("    void {}({}) {{ send({}, {}); }}\n\n", req.name, params, req.opcode, send_args);
    }
    std::string out;
    out += std::format("    void {}({}) {{\n", req.name, params);
    out += std::format("        version_check({});\n", req.since);
    out +=
        send_args.empty() ? std::format("        send({});\n", req.opcode) : std::format("        send({}, {});\n", req.opcode, send_args);
    out += "    }\n\n";
    return out;
}

static void emit_dispatch_arg(const kscanner::Arg& arg, const kscanner::Interface& iface, const std::string& pfx,
                              const kscanner::Protocol& proto, std::string& decls, std::string& call_args) {
    std::string vn = sanitize(arg.name);

    if (arg.type == fd) {
        decls += std::format("            int {} = m_display.take_fd();\n", vn);
        call_args += ", " + vn;
    } else if (arg.type == array) {
        decls += std::format("            auto {0}_len = read<uint32_t>(s);\n", vn);
        decls += std::format("            std::vector<std::byte> {0}({0}_len);\n", vn);
        decls += std::format("            s.read(reinterpret_cast<char*>({0}.data()), static_cast<std::size_t>({0}_len));\n", vn);
        decls += std::format("            s.skip((4u - ({0}_len & 3u)) & 3u);\n", vn);
        call_args += std::format(", std::span<const std::byte>({})", vn);
    } else if (arg.type == new_id && !arg.interface.empty() && arg.interface.starts_with(pfx)) {
        std::string rc = class_name(arg.interface, pfx);
        decls += std::format("            auto {0}_id = read<uint32_t>(s);\n", vn);
        decls += std::format("            auto {0} = std::make_unique<{1}>(m_display, {0}_id, m_version);\n", vn, rc);
        call_args += std::format(", std::move({})", vn);
    } else if (arg.type == string) {
        decls += std::format("            auto {} = read<std::string>(s);\n", vn);
        call_args += ", " + vn;
    } else if (arg.type == int32) {
        if (!arg.enum_ref.empty()) {
            decls += std::format("            auto {} = static_cast<{}>(read<int32_t>(s));\n", vn,
                                 resolve_enum(arg.enum_ref, iface, proto, pfx));
        } else {
            decls += std::format("            auto {} = read<int32_t>(s);\n", vn);
        }
        call_args += ", " + vn;
    } else {
        std::string rtype = (arg.type == fixed) ? "kamiwayland::Fixed" : "uint32_t";
        if (!arg.enum_ref.empty()) {
            decls += std::format("            auto {} = static_cast<{}>(read<{}>(s));\n", vn, resolve_enum(arg.enum_ref, iface, proto, pfx),
                                 rtype);
        } else {
            decls += std::format("            auto {} = read<{}>(s);\n", vn, rtype);
        }
        call_args += ", " + vn;
    }
}

static std::string emit_dispatch_case(const kscanner::Message& ev, const kscanner::Interface& iface, const std::string& pfx,
                                      const kscanner::Protocol& proto) {
    std::string decls, call_args;
    for (const auto& arg : ev.args)
        emit_dispatch_arg(arg, iface, pfx, proto, decls, call_args);

    std::string tail;
    if (ev.is_destructor) tail = "            m_destroyed = true;\n";

    return std::format("        case {0}: {{\n"
                       "{1}"
                       "            if (on_{2}) on_{2}(*this{3});\n"
                       "{4}"
                       "            break;\n"
                       "        }}\n",
                       ev.opcode, decls, ev.name, call_args, tail);
}

static std::string emit_dispatch(const kscanner::Interface& iface, const std::string& pfx, const kscanner::Protocol& proto) {
    if (iface.events.empty()) return "    void dispatch(uint16_t, std::span<const std::byte>) override {}\n";

    std::string cases;
    for (const auto& ev : iface.events)
        cases += emit_dispatch_case(ev, iface, pfx, proto);

    return std::format("    void dispatch(uint16_t opcode, std::span<const std::byte> payload) override {{\n"
                       "        using kamiwayland::wire::read;\n"
                       "        kamiwayland::wire::SpanReader s(payload);\n"
                       "        switch (opcode) {{\n"
                       "{}"
                       "        default: break;\n"
                       "        }}\n    }}\n",
                       cases);
}

static std::string emit_interface(const kscanner::Interface& iface, const std::string& pfx, const kscanner::Protocol& proto) {
    const std::string cname = class_name(iface.name, pfx);

    const kscanner::Message* dtor = nullptr;
    for (const auto& req : iface.requests)
        if (req.is_destructor) {
            dtor = &req;
            break;
        }

    std::string out;
    out += std::format("class {} final : public kamiwayland::Object {{\npublic:\n", cname);
    out += std::format("    static constexpr std::string_view interface_name    = \"{}\";\n", iface.name);
    out += std::format("    static constexpr uint32_t         interface_version = {};\n", iface.version);
    out += "    using kamiwayland::Object::Object;\n\n";

    for (const auto& req : iface.requests) {
        if (req.is_destructor) continue;
        int nid = -1;
        for (int i = 0; i < static_cast<int>(req.args.size()); ++i)
            if (req.args[i].type == new_id) {
                nid = i;
                break;
            }
        if (nid >= 0 && req.args[nid].interface.empty())
            out += emit_template_bind(req, pfx, iface, proto);
        else if (nid >= 0)
            out += emit_factory(req, nid, pfx, iface, proto);
        else
            out += emit_regular_request(req, pfx, iface, proto);
    }

    if (dtor)
        out += std::format("    void destroy() {{ if (!m_destroyed) {{ m_destroyed = true; send({}); }} }}\n"
                           "    ~{}() override {{ destroy(); }}\n\n",
                           dtor->opcode, cname);

    for (const auto& ev : iface.events) {
        std::string sig = cname + "&";
        for (const auto& arg : ev.args)
            sig += ", " + ev_cb_type(arg, iface, proto, pfx);
        out += std::format("    std::function<void({})> on_{};\n", sig, ev.name);
    }
    if (!iface.events.empty()) out += "\n";

    out += emit_dispatch(iface, pfx, proto);
    out += "};\n\n";
    return out;
}

static std::string emit_display_helpers(const kscanner::Interface& display_iface, const std::string& pfx) {
    std::string out;
    for (const auto& en : display_iface.enums)
        out += emit_enum(en, display_iface, pfx);

    for (const auto& req : display_iface.requests) {
        int nid = -1;
        for (int i = 0; i < static_cast<int>(req.args.size()); ++i)
            if (req.args[i].type == new_id) {
                nid = i;
                break;
            }
        if (nid < 0) continue;
        const std::string ret = class_name(req.args[nid].interface, pfx);
        out += std::format("inline std::unique_ptr<{}> {}(kamiwayland::Display& d) {{\n", ret, sanitize(req.name));
        out += std::format("    return d.request_object<{}>({});\n", ret, req.opcode);
        out += "}\n\n";
    }
    return out;
}

namespace kscanner {

void generate(const Protocol& proto, const std::filesystem::path& outdir, std::string_view ns_override) {
    const std::string pfx = find_prefix(proto);
    const std::string ns = ns_override.empty() ? proto.name : std::string(ns_override);

    auto sorted = topo_sort(proto);

    std::string out;
    out += std::format("export module {};\n", ns);
    out += "import kamiwayland;\nimport std;\n\n";
    out += "using std::uint16_t;\nusing std::uint32_t;\nusing std::int32_t;\n\n";
    out += std::format("export namespace {} {{\n\n", ns);

    for (size_t i : sorted) {
        if (proto.interfaces[i].name == "wl_display") continue;
        out += std::format("class {};\n", class_name(proto.interfaces[i].name, pfx));
    }
    out += "\n";

    for (size_t i : sorted) {
        if (proto.interfaces[i].name == "wl_display") continue;
        for (const auto& en : proto.interfaces[i].enums)
            out += emit_enum(en, proto.interfaces[i], pfx);
    }

    for (size_t i : sorted) {
        if (proto.interfaces[i].name == "wl_display") continue;
        out += emit_interface(proto.interfaces[i], pfx, proto);
    }

    for (const auto& iface : proto.interfaces) {
        if (iface.name != "wl_display") continue;
        out += emit_display_helpers(iface, pfx);
    }

    out += std::format("}} // namespace {}\n", ns);

    auto          out_path = outdir / (ns + ".cppm");
    std::ofstream f(out_path);
    if (!f) throw std::runtime_error(std::format("generate: cannot open {}", out_path.string()));
    f << out;
}

} // namespace kscanner