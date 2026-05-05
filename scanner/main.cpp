import kscanner;
import std;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::print(std::cerr, "usage: kscanner [--name <ns>] <protocol.xml>... -o <outdir>\n");
        return 1;
    }

    std::filesystem::path                                      outdir;
    std::vector<std::pair<std::string_view, std::string_view>> inputs; // {path, name_override}

    std::string_view pending_name;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-o" && i + 1 < argc) {
            outdir = argv[++i];
        } else if (arg == "--name" && i + 1 < argc) {
            pending_name = argv[++i];
        } else {
            inputs.emplace_back(arg, pending_name);
            pending_name = {};
        }
    }

    if (outdir.empty() || inputs.empty()) {
        std::print(std::cerr, "usage: kscanner [--name <ns>] <protocol.xml>... -o <outdir>\n");
        return 1;
    }

    std::filesystem::create_directories(outdir);

    for (auto [path, ns] : inputs) {
        auto proto = kscanner::parse(path.data());
        kscanner::generate(proto, outdir, ns);
    }
}