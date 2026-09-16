#include "app/quarry_command.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

[[nodiscard]] std::optional<std::vector<std::string>> parse_metric_list(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return std::nullopt;
    }
    const std::wstring_view wide(text);
    std::string narrow;
    narrow.reserve(wide.size());
    for (const wchar_t character : wide) {
        if (character > 127) {
            return std::nullopt;
        }
        narrow.push_back(static_cast<char>(character));
    }
    std::vector<std::string> metrics;
    std::size_t begin = 0U;
    while (begin <= narrow.size()) {
        const std::size_t comma = narrow.find(',', begin);
        const std::size_t end = comma == std::string::npos ? narrow.size() : comma;
        if (end == begin) {
            return std::nullopt;
        }
        metrics.emplace_back(narrow.substr(begin, end - begin));
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1U;
    }
    return metrics;
}

[[nodiscard]] bool begins_option(const wchar_t* text) noexcept {
    return text != nullptr && text[0] == L'-' && text[1] == L'-';
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
        << "      [--metrics name,name,...] [--animation first-tick frame-count tick-stride]\n"
        << "  ArtMiner quarry run <job.amq> [workers]\n"
        << "  ArtMiner quarry resume <job.amq> [workers]\n"
        << "  ArtMiner quarry inspect <job.amq>\n"
        << "  ArtMiner quarry ui <job.amq>\n\n"
        << "create embeds the canonical base recipe and all result-affecting search semantics.\n"
        << "The default metric set is still-image only. Select motion_energy and/or\n"
        << "temporal_flicker with --metrics and provide at least two fixed-tick samples\n"
        << "with --animation for animated Quarry jobs.\n"
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
        if (argc < 6) {
            print_quarry_help();
            return 2;
        }
        auto recipe = load_recipe(std::filesystem::path(argv[3]));
        const auto count = parse_u64_arg(argv[5]);
        if (!recipe.has_value() || !count.has_value()) {
            std::cerr << "quarry create error: invalid recipe or candidate count\n";
            return 2;
        }

        core::u64 seed = 1U;
        core::u64 width = 128U;
        core::u64 height = 128U;
        int argument = 6;
        core::u64* positional_targets[]{&seed, &width, &height};
        std::size_t positional_index = 0U;
        while (argument < argc && positional_index < std::size(positional_targets) && !begins_option(argv[argument])) {
            const auto value = parse_u64_arg(argv[argument]);
            if (!value.has_value()) {
                std::cerr << "quarry create error: seed/width/height must be unsigned integers\n";
                return 2;
            }
            *positional_targets[positional_index++] = *value;
            ++argument;
        }
        if (width > (std::numeric_limits<core::u32>::max)() ||
            height > (std::numeric_limits<core::u32>::max)()) {
            std::cerr << "quarry create error: dimensions exceed the supported integer range\n";
            return 2;
        }

        std::vector<std::string> metrics = quarry::default_still_metric_names();
        quarry::AnimationSampling animation;
        while (argument < argc) {
            const std::wstring_view option(argv[argument]);
            if (option == L"--metrics") {
                if (argument + 1 >= argc) {
                    std::cerr << "quarry create error: --metrics requires a comma-separated metric list\n";
                    return 2;
                }
                auto parsed_metrics = parse_metric_list(argv[argument + 1]);
                if (!parsed_metrics.has_value()) {
                    std::cerr << "quarry create error: invalid comma-separated metric list\n";
                    return 2;
                }
                metrics = std::move(*parsed_metrics);
                argument += 2;
                continue;
            }
            if (option == L"--animation") {
                if (argument + 3 >= argc) {
                    std::cerr << "quarry create error: --animation requires first-tick frame-count tick-stride\n";
                    return 2;
                }
                const auto first_tick = parse_u64_arg(argv[argument + 1]);
                const auto frame_count = parse_u64_arg(argv[argument + 2]);
                const auto tick_stride = parse_u64_arg(argv[argument + 3]);
                if (!first_tick.has_value() || !frame_count.has_value() || !tick_stride.has_value() ||
                    *frame_count > (std::numeric_limits<core::u32>::max)() ||
                    *tick_stride > (std::numeric_limits<core::u32>::max)()) {
                    std::cerr << "quarry create error: invalid fixed-tick animation sampling arguments\n";
                    return 2;
                }
                animation.first_tick = *first_tick;
                animation.frame_count = static_cast<core::u32>(*frame_count);
                animation.tick_stride = static_cast<core::u32>(*tick_stride);
                argument += 4;
                continue;
            }
            std::wcerr << L"quarry create error: unknown option " << option << L'\n';
            return 2;
        }

        auto manifest = quarry::make_job_manifest(
            *recipe,
            seed,
            *count,
            static_cast<core::u32>(width),
            static_cast<core::u32>(height),
            0.25,
            std::move(metrics),
            animation);
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
                  << "animation: first=" << manifest.value().animation.first_tick
                  << " frames=" << manifest.value().animation.frame_count
                  << " stride=" << manifest.value().animation.tick_stride << '\n'
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
