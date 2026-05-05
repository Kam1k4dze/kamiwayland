module;
#include <tinyxml2.h>
export module kscanner:parser;
import std;

using std::uint16_t;
using std::uint32_t;

export namespace kscanner {

enum class ArgType { int32, uint32, fixed, string, object, new_id, array, fd, unknown };

struct Arg {
    std::string name;
    ArgType     type = ArgType::unknown;
    std::string interface; // for object/new_id types
    std::string enum_ref;  // enum attribute (e.g. "format" or "output.transform")
    bool        nullable = false;
    uint32_t    since = 1;
};

struct Enumerant {
    std::string name;
    uint32_t    value = 0;
};

struct Enum {
    std::string            name;
    std::vector<Enumerant> entries;
    bool                   bitfield = false;
};

struct Message {
    std::string      name;
    std::vector<Arg> args;
    uint16_t         opcode = 0;
    uint32_t         since = 1;
    bool             is_destructor = false;
};

struct Interface {
    std::string          name;
    uint32_t             version = 1;
    std::vector<Message> requests;
    std::vector<Message> events;
    std::vector<Enum>    enums;
};

struct Protocol {
    std::string            name;
    std::vector<Interface> interfaces;
};

[[nodiscard]] Protocol parse(const char* xml_path);

} // namespace kscanner

namespace kscanner {

static ArgType normalize_type(std::string_view t) {
    if (t == "int") return ArgType::int32;
    if (t == "uint") return ArgType::uint32;
    if (t == "fixed") return ArgType::fixed;
    if (t == "string") return ArgType::string;
    if (t == "object") return ArgType::object;
    if (t == "new_id") return ArgType::new_id;
    if (t == "array") return ArgType::array;
    if (t == "fd") return ArgType::fd;
    return ArgType::unknown;
}

static Arg parse_arg(const tinyxml2::XMLElement* el) {
    Arg  arg;
    auto get = [&](const char* attr) -> std::string {
        const char* v = el->Attribute(attr);
        return v ? v : "";
    };
    arg.name = get("name");
    arg.type = normalize_type(get("type"));
    arg.interface = get("interface");
    arg.enum_ref = get("enum");
    const char* nullable = el->Attribute("allow-null");
    arg.nullable = nullable && std::string_view(nullable) == "true";
    if (const char* s = el->Attribute("since")) arg.since = std::stoul(s);
    return arg;
}

static Message parse_message(const tinyxml2::XMLElement* el, uint16_t opcode) {
    Message m;
    m.name = el->Attribute("name") ? el->Attribute("name") : "";
    m.opcode = opcode;
    const char* type = el->Attribute("type");
    m.is_destructor = type && std::string_view(type) == "destructor";
    if (const char* s = el->Attribute("since")) m.since = std::stoul(s);
    for (auto* a = el->FirstChildElement("arg"); a; a = a->NextSiblingElement("arg"))
        m.args.push_back(parse_arg(a));
    return m;
}

Protocol parse(const char* xml_path) {
    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(xml_path) != tinyxml2::XML_SUCCESS) throw std::runtime_error(std::format("parse: {}: {}", xml_path, doc.ErrorStr()));

    auto* root = doc.FirstChildElement("protocol");
    if (!root) throw std::runtime_error(std::format("{}: no <protocol> root element", xml_path));

    Protocol proto;
    proto.name = root->Attribute("name") ? root->Attribute("name") : "";

    for (auto* iface_el = root->FirstChildElement("interface"); iface_el; iface_el = iface_el->NextSiblingElement("interface")) {

        Interface iface;
        iface.name = iface_el->Attribute("name") ? iface_el->Attribute("name") : "";
        iface.version = iface_el->UnsignedAttribute("version");

        uint16_t req_op = 0;
        for (auto* el = iface_el->FirstChildElement("request"); el; el = el->NextSiblingElement("request"))
            iface.requests.push_back(parse_message(el, req_op++));

        uint16_t ev_op = 0;
        for (auto* el = iface_el->FirstChildElement("event"); el; el = el->NextSiblingElement("event"))
            iface.events.push_back(parse_message(el, ev_op++));

        for (auto* en_el = iface_el->FirstChildElement("enum"); en_el; en_el = en_el->NextSiblingElement("enum")) {
            Enum en;
            en.name = en_el->Attribute("name") ? en_el->Attribute("name") : "";
            const char* bf = en_el->Attribute("bitfield");
            en.bitfield = bf && std::string_view(bf) == "true";
            for (auto* entry = en_el->FirstChildElement("entry"); entry; entry = entry->NextSiblingElement("entry")) {
                Enumerant e;
                e.name = entry->Attribute("name") ? entry->Attribute("name") : "";
                const char* val = entry->Attribute("value");
                e.value = val ? static_cast<uint32_t>(std::stoul(val, nullptr, 0)) : 0u;
                en.entries.push_back(std::move(e));
            }
            iface.enums.push_back(std::move(en));
        }

        proto.interfaces.push_back(std::move(iface));
    }

    return proto;
}

} // namespace kscanner
