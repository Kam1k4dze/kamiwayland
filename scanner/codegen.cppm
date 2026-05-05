export module kscanner:codegen;
import :parser;
import std;

export namespace kscanner {
void generate(const Protocol& proto, const std::filesystem::path& outdir, std::string_view ns_override = {});
} // namespace kscanner
