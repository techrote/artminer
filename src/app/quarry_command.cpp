#include "app/quarry_command.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include "app/quarry_window.hpp"
#include "core/recipe.hpp"
#include "quarry/quarry.hpp"

namespace artminer::app {
namespace {

[[nodiscard]] std::optional<std::string> read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        return std::nullopt;
    }
    return text;
}

[[nodiscard]] std::optional<core::Recipe> load_recipe(const std::filesystem::path& path) {
    const auto text = read_text(path);
    if (!text.has_value()) {
        std::wcerr << L"quarry error: could not read recipe " << path.wstring() << L'\n';
        return std::nullopt;
    }
    auto parsed = core::parse_recipe(*text);
    if (parsed.is_error()) {
        std::cerr << "quarry error: recipe parse failed: " << parsed.error().message << '\n';
        return std::nullopt;
    }
    core::Recipe recipe = std::move(parsed).value();
    const auto validation = core::validate_recipe(recipe);
    if (!validation.empty()) {
        std::cerr << "quarry error: recipe validation failed: " << validation.front().message << '\n';
        return std::nullopt;
    }
    return recipe;
}

[[nodiscard]] std::optional<core::u64> parse_u64_arg(const wchar_t* text) {
    if (text == nullptr || *text == L'\0' || *text == L'-') {
        return std::nullopt;
    }
    std::wstring_view wide(text);
    std::string narrow;
    narrow.reserve(wide.size());
    for (const wchar_t character : wide) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        narrow.push_back(static_cast<char>(character));
    }
    core::u64 value = 0U;
    const auto parsed = std::from_chars(narrow.data(), narrow.data() + narrow.size(), value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != narrow.data() + narrow.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] core::u32 default_worker_count() noexcept {
    const unsigned int hardware = std::thread::hardware_concurrency();
    if (hardware == 0U) {
        return 1U;
    }
    return static_cast<core::u32>((std::min)(hardware, quarry::kMaximumQuarryWorkers));
}

void print_quarry_help() {
    std::cout
        << "Usage:\n"
        << "  ArtMiner quarry create <base.amr> <job.amq> <count> [seed] [width] [height]\n"
        << "  ArtMiner quarry run <job.amq> [workers]\n"
        << "  ArtMiner quarry resume <job.amq> [workers]\n"
        << "  ArtMiner quarry inspect <job.amq>\n"
        << "  ArtMiner quarry ui <job.amq>\n\n"
        << "create embeds the canonical base recipe and all result-affecting search semantics.\n"
        << "run/resume share the same bounded deterministic engine; resume verifies the checkpoint\n"
        << "and committed result prefix before continuing. Worker count is execution policy only.\n";
}

void print_quarry_error(const quarry::QuarryError& error) {
    std::cerr << "quarry error: " << error.message << '\n';
}

}  // namespace

int run_quarry_command(const int argc, wchar_t* argv[]) {
    if (argc < 3) {
        print_quarry_help();
        return 2;
    }
    const std::wstring_view action(argv[2]);
    if (action == L"create") {
        if (argc < 6 || argc > 9) {
            print_quarry_help();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        const auto count = parse_u64_arg(argv[5]);
        const auto seed = argc >= 7 ? parse_u64_arg(argv[6]) : std::optional<core::u64>{1U};
        const auto width = argc >= 8 ? parse_u64_arg(argv[7]) : std::optional<core::u64>{128U};
        const auto height = argc >= 9 ? parse_u64_arg(argv[8]) : std::optional<core::u64>{128U};
        if (!recipe.has_value() || !count.has_value() || !seed.has_value() ||
            !width.has_value() || !height.has_value() ||
            *width > (std::numeric_limits<core::u32>::max)() || *height > (std::numeric_limits<core::u32>::max)()) {
            std::cerr << "quarry create error: invalid recipe/count/seed/dimensions\n";
            return 2;
        }
        auto manifest = quarry::make_job_manifest(
            *recipe,
            *seed,
            *count,
            static_cast<core::u32>(*width),
            static_cast<core::u32>(*height));
        if (manifest.is_error()) {
            print_quarry_error(manifest.error());
            return 6;
        }
        const std::filesystem::path job_path(argv[4]);
        auto written = quarry::write_job_manifest(job_path, manifest.value());
        if (written.is_error()) {
            print_quarry_error(written.error());
            return 8;
        }
        std::cout << "quarry job created " << manifest.value().identity << " candidates "
                  << manifest.value().candidate_count << '\n';
        return 0;
    }
    if (action == L"run" || action == L"resume") {
        if (argc < 4 || argc > 5) {
            print_quarry_help();
            return 2;
        }
        core::u32 workers = default_worker_count();
        if (argc == 5) {
            const auto parsed_workers = parse_u64_arg(argv[4]);
            if (!parsed_workers.has_value() || *parsed_workers > (std::numeric_limits<core::u32>::max)()) {
                std::cerr << "quarry run error: workers must be an unsigned integer\n";
                return 2;
            }
            workers = static_cast<core::u32>(*parsed_workers);
        }
        auto result = quarry::run_job(
            std::filesystem::path(argv[3]), workers, nullptr,
            [](const quarry::JobProgress& progress) {
                std::cout << "quarry progress " << progress.committed << '/' << progress.total
                          << " cache-hits " << progress.cache_hits << '\n';
            });
        if (result.is_error()) {
            print_quarry_error(result.error());
            return 9;
        }
        std::cout << "quarry " << (result.value().complete ? "complete" : "stopped") << ' '
                  << result.value().committed << '/' << result.value().total << '\n';
        return result.value().complete ? 0 : 10;
    }
    if (action == L"inspect") {
        if (argc != 4) {
            print_quarry_help();
            return 2;
        }
        auto manifest = quarry::read_job_manifest(std::filesystem::path(argv[3]));
        if (manifest.is_error()) {
            print_quarry_error(manifest.error());
            return 6;
        }
        auto progress = quarry::inspect_job(std::filesystem::path(argv[3]));
        if (progress.is_error()) {
            print_quarry_error(progress.error());
            return 9;
        }
        std::cout << "ArtMiner Quarry job\n"
                  << "identity: " << manifest.value().identity << '\n'
                  << "base: " << manifest.value().base_recipe_fingerprint << '\n'
                  << "candidate range: " << manifest.value().first_candidate << ".."
                  << (manifest.value().first_candidate + manifest.value().candidate_count - 1U) << '\n'
                  << "render: " << manifest.value().render_width << 'x' << manifest.value().render_height << '\n'
                  << "metrics: ";
        for (std::size_t index = 0U; index < manifest.value().metrics.size(); ++index) {
            if (index != 0U) {
                std::cout << ',';
            }
            std::cout << manifest.value().metrics[index];
        }
        std::cout << '\n'
                  << "progress: " << progress.value().committed << '/' << progress.value().total << '\n'
                  << "complete: " << (progress.value().complete ? "yes" : "no") << '\n';
        return 0;
    }
    if (action == L"ui") {
        if (argc != 4) {
            print_quarry_help();
            return 2;
        }
        return run_quarry_application(std::filesystem::path(argv[3]));
    }
    print_quarry_help();
    return 2;
}

}  // namespace artminer::app
