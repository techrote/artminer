#include "app/quarry_diversity_command.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "platform/windows/browser_store.hpp"
#include "quarry/diversity.hpp"
#include "quarry/quarry.hpp"

namespace artminer::app {
namespace {

struct Analysis final {
    quarry::JobManifest manifest;
    std::vector<quarry::CandidateResult> results;
    quarry::NormalizationModel normalization;
    std::vector<quarry::CandidateFeatures> features;
};

[[nodiscard]] std::optional<std::string> narrow_ascii(const wchar_t* text) {
    if (text == nullptr) {
        return std::nullopt;
    }
    std::string result;
    while (*text != L'\0') {
        if (*text < 0 || *text > 127) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>(*text));
        ++text;
    }
    return result;
}

[[nodiscard]] std::optional<core::u64> parse_u64(const wchar_t* text) {
    const auto value = narrow_ascii(text);
    if (!value.has_value() || value->empty() || value->front() == '-') {
        return std::nullopt;
    }
    core::u64 parsed_value = 0U;
    const auto parsed = std::from_chars(value->data(), value->data() + value->size(), parsed_value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != value->data() + value->size()) {
        return std::nullopt;
    }
    return parsed_value;
}

[[nodiscard]] std::optional<double> parse_double(const wchar_t* text) {
    const auto value = narrow_ascii(text);
    if (!value.has_value() || value->empty()) {
        return std::nullopt;
    }
    std::istringstream stream(*value);
    stream.imbue(std::locale::classic());
    double parsed = 0.0;
    stream >> parsed;
    if (!stream || !stream.eof() || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return parsed;
}

[[nodiscard]] std::optional<std::vector<quarry::MetricWeight>> parse_weights(const wchar_t* text) {
    const auto value = narrow_ascii(text);
    if (!value.has_value() || value->empty()) {
        return std::nullopt;
    }
    std::vector<quarry::MetricWeight> weights;
    std::size_t begin = 0U;
    while (begin < value->size()) {
        const std::size_t comma = value->find(',', begin);
        const std::size_t end = comma == std::string::npos ? value->size() : comma;
        const std::string_view item(value->data() + begin, end - begin);
        const std::size_t equals = item.find('=');
        if (equals == std::string_view::npos || equals == 0U || equals + 1U >= item.size()) {
            return std::nullopt;
        }
        std::istringstream stream(std::string(item.substr(equals + 1U)));
        stream.imbue(std::locale::classic());
        double weight = 0.0;
        stream >> weight;
        if (!stream || !stream.eof() || !std::isfinite(weight) || weight < 0.0) {
            return std::nullopt;
        }
        weights.push_back(quarry::MetricWeight{std::string(item.substr(0U, equals)), weight});
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1U;
    }
    return weights.empty() ? std::nullopt : std::optional<std::vector<quarry::MetricWeight>>(std::move(weights));
}

[[nodiscard]] bool is_option(const wchar_t* value, const std::wstring_view expected) noexcept {
    return value != nullptr && std::wstring_view(value) == expected;
}

[[nodiscard]] std::optional<std::vector<quarry::MetricWeight>> extract_weights(
    const int argc,
    wchar_t* argv[],
    int first_optional,
    bool& valid) {
    valid = true;
    std::optional<std::vector<quarry::MetricWeight>> weights;
    for (int index = first_optional; index < argc; ++index) {
        if (is_option(argv[index], L"--weights")) {
            if (index + 1 >= argc || weights.has_value()) {
                valid = false;
                return std::nullopt;
            }
            weights = parse_weights(argv[index + 1]);
            if (!weights.has_value()) {
                valid = false;
                return std::nullopt;
            }
            ++index;
        }
    }
    return weights;
}

void print_error(const std::string_view prefix, const std::string& message) {
    std::cerr << prefix << " error: " << message << '\n';
}

void print_normalization(const quarry::NormalizationModel& model) {
    std::cout << "normalization semantic-version " << quarry::kDiversitySemanticVersion << '\n';
    for (const auto& dimension : model.dimensions) {
        std::cout << "metric " << dimension.name
                  << " min=" << std::setprecision(17) << dimension.minimum
                  << " max=" << dimension.maximum
                  << " weight=" << dimension.weight
                  << " degenerate=" << (dimension.degenerate ? 1 : 0) << '\n';
    }
}

[[nodiscard]] std::optional<Analysis> load_analysis(
    const std::filesystem::path& job,
    const std::vector<quarry::MetricWeight>& weights) {
    auto manifest = quarry::read_job_manifest(job);
    if (manifest.is_error()) {
        print_error("quarry analysis", manifest.error().message);
        return std::nullopt;
    }
    auto progress = quarry::inspect_job(job);
    if (progress.is_error()) {
        print_error("quarry analysis", progress.error().message);
        return std::nullopt;
    }
    if (!progress.value().complete) {
        print_error("quarry analysis", "job must be complete before deterministic diversity analysis");
        return std::nullopt;
    }
    auto results = quarry::read_results(job);
    if (results.is_error()) {
        print_error("quarry analysis", results.error().message);
        return std::nullopt;
    }
    auto normalization = quarry::build_normalization_model(results.value(), weights);
    if (normalization.is_error()) {
        print_error("quarry analysis", normalization.error().message);
        return std::nullopt;
    }
    auto features = quarry::build_candidate_features(
        manifest.value(), results.value(), quarry::derive_job_paths(job).cache_directory, normalization.value());
    if (features.is_error()) {
        print_error("quarry analysis", features.error().message);
        return std::nullopt;
    }
    Analysis analysis;
    analysis.manifest = std::move(manifest).value();
    analysis.results = std::move(results).value();
    analysis.normalization = std::move(normalization).value();
    analysis.features = std::move(features).value();
    return analysis;
}

[[nodiscard]] const quarry::CandidateResult* find_result(
    const std::vector<quarry::CandidateResult>& results,
    const core::u64 candidate_index) noexcept {
    const auto found = std::find_if(results.begin(), results.end(), [candidate_index](const auto& result) {
        return result.index == candidate_index;
    });
    return found == results.end() ? nullptr : &*found;
}

[[nodiscard]] int run_dedupe(const int argc, wchar_t* argv[]) {
    if (argc < 4) {
        std::cerr << "usage: ArtMiner quarry dedupe <job.amq> [metric-threshold] [image-threshold] [--weights name=value,...]\n";
        return 2;
    }
    quarry::DedupeSettings settings;
    int optional = 4;
    if (optional < argc && std::wstring_view(argv[optional]).rfind(L"--", 0U) != 0U) {
        const auto value = parse_double(argv[optional++]);
        if (!value.has_value()) {
            return 2;
        }
        settings.metric_distance_threshold = *value;
    }
    if (optional < argc && std::wstring_view(argv[optional]).rfind(L"--", 0U) != 0U) {
        const auto value = parse_double(argv[optional++]);
        if (!value.has_value()) {
            return 2;
        }
        settings.image_mean_absolute_threshold = *value;
    }
    bool valid = false;
    auto weights = extract_weights(argc, argv, optional, valid);
    if (!valid) {
        std::cerr << "quarry dedupe error: invalid --weights option\n";
        return 2;
    }
    auto analysis = load_analysis(argv[3], weights.value_or(std::vector<quarry::MetricWeight>{}));
    if (!analysis.has_value()) {
        return 9;
    }
    auto groups = quarry::deduplicate_candidates(analysis->features, analysis->normalization, settings);
    if (groups.is_error()) {
        print_error("quarry dedupe", groups.error().message);
        return 9;
    }
    print_normalization(analysis->normalization);
    std::cout << "dedupe metric-threshold=" << settings.metric_distance_threshold
              << " image-mean-absolute-threshold=" << settings.image_mean_absolute_threshold
              << " authoritative-results=" << analysis->results.size()
              << " representatives=" << groups.value().size() << '\n';
    for (const auto& group : groups.value()) {
        std::cout << "representative " << group.representative << " members=";
        for (std::size_t index = 0U; index < group.members.size(); ++index) {
            if (index != 0U) {
                std::cout << ',';
            }
            std::cout << group.members[index];
        }
        std::cout << '\n';
    }
    return 0;
}

[[nodiscard]] int run_cluster(const int argc, wchar_t* argv[]) {
    if (argc < 5) {
        std::cerr << "usage: ArtMiner quarry cluster <job.amq> <cluster-count> [--weights name=value,...]\n";
        return 2;
    }
    const auto count = parse_u64(argv[4]);
    if (!count.has_value() || *count == 0U || *count > static_cast<core::u64>((std::numeric_limits<std::size_t>::max)())) {
        return 2;
    }
    bool valid = false;
    auto weights = extract_weights(argc, argv, 5, valid);
    if (!valid) {
        return 2;
    }
    auto analysis = load_analysis(argv[3], weights.value_or(std::vector<quarry::MetricWeight>{}));
    if (!analysis.has_value()) {
        return 9;
    }
    auto dedupe = quarry::deduplicate_candidates(analysis->features, analysis->normalization);
    if (dedupe.is_error()) {
        print_error("quarry cluster", dedupe.error().message);
        return 9;
    }
    auto clusters = quarry::cluster_candidates(
        analysis->features, analysis->normalization, static_cast<std::size_t>(*count), &dedupe.value());
    if (clusters.is_error()) {
        print_error("quarry cluster", clusters.error().message);
        return 9;
    }
    print_normalization(analysis->normalization);
    std::cout << "clustering deterministic-farthest-first representatives-after-default-dedupe="
              << dedupe.value().size() << '\n';
    for (const auto& cluster : clusters.value()) {
        std::cout << "cluster " << cluster.cluster_id << " representative=" << cluster.representative << " members=";
        for (std::size_t index = 0U; index < cluster.members.size(); ++index) {
            if (index != 0U) {
                std::cout << ',';
            }
            std::cout << cluster.members[index];
        }
        std::cout << '\n';
    }
    return 0;
}

[[nodiscard]] int run_rank(const int argc, wchar_t* argv[]) {
    if (argc < 5) {
        std::cerr << "usage: ArtMiner quarry rank <job.amq> <population|cluster> [cluster-count] [--weights name=value,...]\n";
        return 2;
    }
    const std::wstring_view basis_text(argv[4]);
    quarry::UnusualBasis basis = quarry::UnusualBasis::population;
    std::size_t cluster_count = 0U;
    int optional = 5;
    if (basis_text == L"cluster") {
        if (argc < 6) {
            return 2;
        }
        const auto parsed = parse_u64(argv[5]);
        if (!parsed.has_value() || *parsed == 0U || *parsed > static_cast<core::u64>((std::numeric_limits<std::size_t>::max)())) {
            return 2;
        }
        cluster_count = static_cast<std::size_t>(*parsed);
        optional = 6;
        basis = quarry::UnusualBasis::cluster;
    } else if (basis_text != L"population") {
        return 2;
    }
    bool valid = false;
    auto weights = extract_weights(argc, argv, optional, valid);
    if (!valid) {
        return 2;
    }
    auto analysis = load_analysis(argv[3], weights.value_or(std::vector<quarry::MetricWeight>{}));
    if (!analysis.has_value()) {
        return 9;
    }
    std::vector<quarry::Cluster> clusters;
    if (basis == quarry::UnusualBasis::cluster) {
        auto clustered = quarry::cluster_candidates(analysis->features, analysis->normalization, cluster_count);
        if (clustered.is_error()) {
            print_error("quarry rank", clustered.error().message);
            return 9;
        }
        clusters = std::move(clustered).value();
    }
    auto ranked = quarry::rank_unusual(analysis->features, analysis->normalization, clusters, basis);
    if (ranked.is_error()) {
        print_error("quarry rank", ranked.error().message);
        return 9;
    }
    print_normalization(analysis->normalization);
    std::cout << "unusual basis=" << (basis == quarry::UnusualBasis::population ? "population" : "cluster")
              << " meaning=weighted-normalized-metric-distance-not-quality\n";
    for (const auto& score : ranked.value()) {
        std::cout << "candidate " << score.index << " distance=" << std::setprecision(17) << score.distance;
        if (basis == quarry::UnusualBasis::cluster) {
            std::cout << " cluster=" << score.cluster_id;
        }
        std::cout << " contributions=";
        for (std::size_t index = 0U; index < score.contributions.size(); ++index) {
            if (index != 0U) {
                std::cout << ',';
            }
            std::cout << score.contributions[index].name << ':' << score.contributions[index].contribution;
        }
        std::cout << '\n';
    }
    return 0;
}

[[nodiscard]] int run_neighbours(const int argc, wchar_t* argv[]) {
    if (argc < 6) {
        std::cerr << "usage: ArtMiner quarry neighbours <job.amq> <candidate-index> <metric|parameter|combined> [limit] [--weights ...] [--combined-weight v] [--min-diversity v]\n";
        return 2;
    }
    const auto selected = parse_u64(argv[4]);
    if (!selected.has_value()) {
        return 2;
    }
    quarry::NeighbourSettings settings;
    const std::wstring_view mode(argv[5]);
    if (mode == L"metric") {
        settings.mode = quarry::NeighbourMode::metric;
    } else if (mode == L"parameter") {
        settings.mode = quarry::NeighbourMode::parameter;
    } else if (mode == L"combined") {
        settings.mode = quarry::NeighbourMode::combined;
    } else {
        return 2;
    }
    int optional = 6;
    if (optional < argc && std::wstring_view(argv[optional]).rfind(L"--", 0U) != 0U) {
        const auto limit = parse_u64(argv[optional++]);
        if (!limit.has_value() || *limit == 0U || *limit > static_cast<core::u64>((std::numeric_limits<std::size_t>::max)())) {
            return 2;
        }
        settings.maximum_results = static_cast<std::size_t>(*limit);
    }
    std::vector<quarry::MetricWeight> weights;
    while (optional < argc) {
        if (is_option(argv[optional], L"--weights") && optional + 1 < argc) {
            auto parsed = parse_weights(argv[optional + 1]);
            if (!parsed.has_value()) {
                return 2;
            }
            weights = std::move(*parsed);
            optional += 2;
        } else if (is_option(argv[optional], L"--combined-weight") && optional + 1 < argc) {
            const auto parsed = parse_double(argv[optional + 1]);
            if (!parsed.has_value()) {
                return 2;
            }
            settings.combined_metric_weight = *parsed;
            optional += 2;
        } else if (is_option(argv[optional], L"--min-diversity") && optional + 1 < argc) {
            const auto parsed = parse_double(argv[optional + 1]);
            if (!parsed.has_value()) {
                return 2;
            }
            settings.minimum_metric_diversity = *parsed;
            optional += 2;
        } else {
            return 2;
        }
    }
    auto analysis = load_analysis(argv[3], weights);
    if (!analysis.has_value()) {
        return 9;
    }
    auto neighbours = quarry::nearest_neighbours(
        analysis->features, analysis->normalization, *selected, settings);
    if (neighbours.is_error()) {
        print_error("quarry neighbours", neighbours.error().message);
        return 9;
    }
    const char* mode_name = settings.mode == quarry::NeighbourMode::metric ? "metric" :
        settings.mode == quarry::NeighbourMode::parameter ? "parameter" : "combined";
    print_normalization(analysis->normalization);
    std::cout << "neighbour-mode " << mode_name
              << " combined-metric-weight=" << settings.combined_metric_weight
              << " minimum-metric-diversity=" << settings.minimum_metric_diversity << '\n';
    for (const auto& neighbour : neighbours.value()) {
        std::cout << "candidate " << neighbour.index
                  << " distance=" << std::setprecision(17) << neighbour.distance
                  << " metric-distance=" << neighbour.metric_distance
                  << " parameter-distance=" << neighbour.parameter_distance << '\n';
    }
    return 0;
}

[[nodiscard]] int run_project(const int argc, wchar_t* argv[]) {
    if (argc != 6) {
        std::cerr << "usage: ArtMiner quarry project <job.amq> <x-metric> <y-metric>\n";
        return 2;
    }
    const auto x = narrow_ascii(argv[4]);
    const auto y = narrow_ascii(argv[5]);
    if (!x.has_value() || !y.has_value() || x->empty() || y->empty()) {
        return 2;
    }
    auto analysis = load_analysis(argv[3], {});
    if (!analysis.has_value()) {
        return 9;
    }
    auto projected = quarry::project_metric_axes(analysis->features, *x, *y);
    if (projected.is_error()) {
        print_error("quarry project", projected.error().message);
        return 9;
    }
    std::cout << "projection explicit-normalized-metric-axes x=" << *x << " y=" << *y
              << " note=not-literal-high-dimensional-geometry\n";
    for (const auto& point : projected.value()) {
        std::cout << "candidate " << point.index << " x=" << std::setprecision(17) << point.x
                  << " y=" << point.y << '\n';
    }
    return 0;
}

[[nodiscard]] int run_filter(const int argc, wchar_t* argv[]) {
    if (argc < 7 || argc > 8) {
        std::cerr << "usage: ArtMiner quarry filter <job.amq> <metric> <min|*> <max|*> [asc|desc]\n";
        return 2;
    }
    const auto metric = narrow_ascii(argv[4]);
    if (!metric.has_value() || metric->empty()) {
        return 2;
    }
    quarry::MetricRangeFilter filter;
    filter.name = *metric;
    if (std::wstring_view(argv[5]) != L"*") {
        filter.minimum = parse_double(argv[5]);
        if (!filter.minimum.has_value()) {
            return 2;
        }
    }
    if (std::wstring_view(argv[6]) != L"*") {
        filter.maximum = parse_double(argv[6]);
        if (!filter.maximum.has_value()) {
            return 2;
        }
    }
    bool descending = false;
    if (argc == 8) {
        if (std::wstring_view(argv[7]) == L"desc") {
            descending = true;
        } else if (std::wstring_view(argv[7]) != L"asc") {
            return 2;
        }
    }
    auto analysis = load_analysis(argv[3], {});
    if (!analysis.has_value()) {
        return 9;
    }
    auto filtered = quarry::filter_and_sort_candidates(
        analysis->features, {filter}, {quarry::StableSortKind::raw_metric, *metric, descending});
    if (filtered.is_error()) {
        print_error("quarry filter", filtered.error().message);
        return 9;
    }
    std::cout << "filter metric=" << *metric << " stable-order=" << (descending ? "descending" : "ascending") << '\n';
    for (const core::u64 index : filtered.value()) {
        std::cout << "candidate " << index << '\n';
    }
    return 0;
}

[[nodiscard]] int run_save(const int argc, wchar_t* argv[]) {
    if (argc < 6 || argc > 7) {
        std::cerr << "usage: ArtMiner quarry save <job.amq> <candidate-index> <recipes-root> [--favourite]\n";
        return 2;
    }
    const auto candidate_index = parse_u64(argv[4]);
    if (!candidate_index.has_value()) {
        return 2;
    }
    const bool favourite = argc == 7 && is_option(argv[6], L"--favourite");
    if (argc == 7 && !favourite) {
        return 2;
    }
    auto manifest = quarry::read_job_manifest(argv[3]);
    auto progress = quarry::inspect_job(argv[3]);
    auto results = quarry::read_results(argv[3]);
    if (manifest.is_error() || progress.is_error() || results.is_error() || !progress.value().complete) {
        std::cerr << "quarry save error: job must be complete and internally valid\n";
        return 9;
    }
    const quarry::CandidateResult* authoritative = find_result(results.value(), *candidate_index);
    if (authoritative == nullptr) {
        std::cerr << "quarry save error: candidate is absent from authoritative job results\n";
        return 9;
    }
    auto recipe = quarry::materialize_candidate_recipe(manifest.value(), *candidate_index);
    if (recipe.is_error()) {
        print_error("quarry save", recipe.error().message);
        return 9;
    }
    if (core::semantic_fingerprint(recipe.value()) != authoritative->recipe_fingerprint) {
        std::cerr << "quarry save error: reconstructed recipe fingerprint disagrees with authoritative result row\n";
        return 9;
    }
    const std::filesystem::path root(argv[5]);
    auto saved = favourite
        ? platform::windows::save_favourite(root, recipe.value())
        : platform::windows::save_recipe_copy(root, recipe.value());
    if (saved.is_error()) {
        std::wcerr << L"quarry save error: " << saved.error().message << L'\n';
        return 8;
    }
    std::wcout << L"saved Quarry candidate " << *candidate_index << L" to " << saved.value().wstring()
               << L" semantic-fingerprint "
               << std::wstring(authoritative->recipe_fingerprint.begin(), authoritative->recipe_fingerprint.end()) << L'\n';
    return 0;
}

}  // namespace

std::optional<int> try_run_quarry_diversity_command(const int argc, wchar_t* argv[]) {
    if (argc < 3) {
        return std::nullopt;
    }
    const std::wstring_view action(argv[2]);
    if (action == L"dedupe") {
        return run_dedupe(argc, argv);
    }
    if (action == L"cluster") {
        return run_cluster(argc, argv);
    }
    if (action == L"rank") {
        return run_rank(argc, argv);
    }
    if (action == L"neighbours") {
        return run_neighbours(argc, argv);
    }
    if (action == L"project") {
        return run_project(argc, argv);
    }
    if (action == L"filter") {
        return run_filter(argc, argv);
    }
    if (action == L"save") {
        return run_save(argc, argv);
    }
    return std::nullopt;
}

}  // namespace artminer::app
