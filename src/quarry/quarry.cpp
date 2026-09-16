#include "quarry/quarry.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/graph.hpp"
#include "core/hash.hpp"
#include "core/prng.hpp"
#include "nodes/motion_evaluator.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::quarry {
namespace {

constexpr std::string_view kManifestMagic = "ARTMINER_QUARRY";
constexpr std::string_view kCheckpointMagic = "ARTMINER_QUARRY_CHECKPOINT";
constexpr std::string_view kResultsMagic = "ARTMINER_QUARRY_RESULTS";
constexpr std::string_view kCacheMagic = "ARTMINER_QUARRY_CACHE";
constexpr core::u64 kCandidateSeedDomain = 0x514152525943414eULL;
constexpr std::size_t kMaximumManifestBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumCheckpointBytes = 64U * 1024U;
constexpr std::size_t kMaximumCacheBytes = 256U * 1024U;

struct Checkpoint final {
    std::string job_identity;
    core::u64 committed{0U};
    core::u64 results_hash{core::kFnv1a64Offset};
    bool complete{false};
};

[[nodiscard]] QuarryError make_error(const QuarryErrorCode code, std::string message) {
    return QuarryError{code, std::move(message)};
}

[[nodiscard]] std::string format_double(const double value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << value;
    return stream.str();
}

[[nodiscard]] bool parse_u64(const std::string_view text, core::u64& output) noexcept {
    if (text.empty()) {
        return false;
    }
    core::u64 value = 0U;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return false;
    }
    output = value;
    return true;
}

[[nodiscard]] bool parse_double(const std::string& text, double& output) {
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    double value = 0.0;
    stream >> value;
    if (!stream || !stream.eof() || !std::isfinite(value)) {
        return false;
    }
    output = value;
    return true;
}

[[nodiscard]] std::vector<std::string> split_csv(const std::string_view text) {
    std::vector<std::string> values;
    std::size_t begin = 0U;
    while (begin <= text.size()) {
        const std::size_t comma = text.find(',', begin);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        if (end > begin) {
            values.emplace_back(text.substr(begin, end - begin));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1U;
    }
    return values;
}

[[nodiscard]] std::string join_csv(const std::vector<std::string>& values) {
    std::string output;
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (index != 0U) {
            output.push_back(',');
        }
        output += values[index];
    }
    return output;
}

[[nodiscard]] std::string manifest_identity_payload(const JobManifest& manifest) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "quarry-manifest=" << manifest.version << '\n'
           << "candidate-enumeration=" << manifest.candidate_enumeration_version << '\n'
           << "mutation-operator=" << manifest.mutation_operator_version << '\n'
           << "metric-semantics=" << manifest.metric_semantic_version << '\n'
           << "evaluator-semantics=" << manifest.evaluator_semantic_version << '\n'
           << "root-seed=" << manifest.root_seed << '\n'
           << "first-candidate=" << manifest.first_candidate << '\n'
           << "candidate-count=" << manifest.candidate_count << '\n'
           << "mutation-strength=" << format_double(manifest.mutation_strength) << '\n'
           << "render=" << manifest.render_width << 'x' << manifest.render_height << '\n'
           << "animation=" << manifest.animation.first_tick << ',' << manifest.animation.frame_count << ','
           << manifest.animation.tick_stride << '\n'
           << "metrics=" << join_csv(manifest.metrics) << '\n'
           << "base=" << manifest.base_recipe_fingerprint << '\n';
    return stream.str();
}

[[nodiscard]] std::string compute_manifest_identity(const JobManifest& manifest) {
    return core::hex_u64(core::fnv1a64(manifest_identity_payload(manifest)));
}

[[nodiscard]] core::Result<void, QuarryError> validate_manifest(JobManifest& manifest) {
    if (manifest.version != kQuarryManifestVersion ||
        manifest.candidate_enumeration_version != kCandidateEnumerationVersion ||
        manifest.mutation_operator_version != core::kParameterMutationOperatorVersion ||
        manifest.metric_semantic_version != kMetricSemanticVersion ||
        manifest.evaluator_semantic_version != core::kEvaluatorSemanticVersion) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::unsupported_version,
            "Quarry manifest uses an unsupported manifest/enumerator/mutation/metric/evaluator semantic version"));
    }
    if (manifest.candidate_count == 0U || manifest.candidate_count > kMaximumQuarryCandidates ||
        manifest.first_candidate > kMaximumQuarryCandidates ||
        manifest.candidate_count > kMaximumQuarryCandidates - manifest.first_candidate) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::resource_limit,
            "Quarry candidate range must be non-empty and remain within the 1,000,000-candidate safety limit"));
    }
    if (!(manifest.mutation_strength >= 0.0 && manifest.mutation_strength <= 1.0) ||
        !std::isfinite(manifest.mutation_strength)) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest, "Quarry mutation strength must be finite and within [0,1]"));
    }
    if (manifest.render_width == 0U || manifest.render_height == 0U ||
        manifest.render_width > 4096U || manifest.render_height > 4096U) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::resource_limit,
            "Quarry metric render dimensions must be within 1..4096 on each axis"));
    }
    if (manifest.animation.frame_count == 0U || manifest.animation.frame_count > kMaximumAnimationSamples ||
        manifest.animation.tick_stride == 0U) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest,
            "Quarry animation sampling requires 1..16 frames and a non-zero tick stride"));
    }
    const core::u64 sample_span =
        static_cast<core::u64>(manifest.animation.frame_count - 1U) * manifest.animation.tick_stride;
    if (manifest.animation.first_tick > nodes::kMaximumAnimationTick ||
        sample_span > nodes::kMaximumAnimationTick - manifest.animation.first_tick) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::resource_limit,
            "Quarry animation sample range exceeds the canonical animation tick limit"));
    }
    if (manifest.metrics.empty()) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest, "Quarry jobs must select at least one metric"));
    }
    bool needs_animation = false;
    for (const std::string& metric : manifest.metrics) {
        if (!is_supported_metric(metric)) {
            return core::Result<void, QuarryError>::failure(make_error(
                QuarryErrorCode::invalid_manifest, "Quarry manifest selects unsupported metric '" + metric + "'"));
        }
        needs_animation = needs_animation || metric == "motion_energy" || metric == "temporal_flicker";
    }
    if (needs_animation && manifest.animation.frame_count < 2U) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest,
            "motion_energy and temporal_flicker require at least two animation samples"));
    }
    const auto validation = core::validate_recipe(manifest.base_recipe);
    if (!validation.empty()) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_recipe,
            "Quarry base recipe is invalid: " + validation.front().message));
    }
    const std::string fingerprint = core::semantic_fingerprint(manifest.base_recipe);
    if (!manifest.base_recipe_fingerprint.empty() && manifest.base_recipe_fingerprint != fingerprint) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest,
            "Quarry base recipe fingerprint does not match embedded canonical recipe content"));
    }
    manifest.base_recipe_fingerprint = fingerprint;
    const std::string identity = compute_manifest_identity(manifest);
    if (!manifest.identity.empty() && manifest.identity != identity) {
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest,
            "Quarry manifest identity does not match its deterministic semantic payload"));
    }
    manifest.identity = identity;
    return core::Result<void, QuarryError>::success();
}

[[nodiscard]] core::Result<std::string, QuarryError> read_file(
    const std::filesystem::path& path,
    const std::size_t maximum_bytes,
    const QuarryErrorCode error_code) {
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(path, size_error);
    if (size_error) {
        return core::Result<std::string, QuarryError>::failure(make_error(
            error_code, "could not stat file: " + path.string()));
    }
    if (size > maximum_bytes) {
        return core::Result<std::string, QuarryError>::failure(make_error(
            error_code, "file exceeds bounded parser size: " + path.string()));
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return core::Result<std::string, QuarryError>::failure(make_error(
            error_code, "could not open file: " + path.string()));
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!text.empty()) {
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
    }
    if (!input && !input.eof()) {
        return core::Result<std::string, QuarryError>::failure(make_error(
            error_code, "failed while reading file: " + path.string()));
    }
    return core::Result<std::string, QuarryError>::success(std::move(text));
}

[[nodiscard]] core::Result<void, QuarryError> write_atomic_text(
    const std::filesystem::path& path,
    const std::string_view text) {
    std::error_code directory_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), directory_error);
        if (directory_error) {
            return core::Result<void, QuarryError>::failure(make_error(
                QuarryErrorCode::io_error, "could not create Quarry output directory"));
        }
    }
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return core::Result<void, QuarryError>::failure(make_error(
                QuarryErrorCode::io_error, "could not create temporary Quarry file: " + temporary.string()));
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return core::Result<void, QuarryError>::failure(make_error(
                QuarryErrorCode::io_error, "failed while writing temporary Quarry file"));
        }
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        std::filesystem::remove(temporary, ignored);
        return core::Result<void, QuarryError>::failure(make_error(
            QuarryErrorCode::io_error, "could not publish Quarry file: " + path.string()));
    }
    return core::Result<void, QuarryError>::success();
}

[[nodiscard]] core::u64 update_fnv(core::u64 hash, const std::string_view text) noexcept {
    for (const unsigned char byte : text) {
        hash ^= static_cast<core::u64>(byte);
        hash *= core::kFnv1a64Prime;
    }
    return hash;
}

[[nodiscard]] std::string results_header(const JobManifest& manifest) {
    return std::string(kResultsMagic) + " 1 " + manifest.identity + "\n";
}

[[nodiscard]] std::string serialize_result_row(const CandidateResult& result) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << result.index << '\t' << result.candidate_id << '\t' << result.recipe_fingerprint;
    for (const MetricValue& metric : result.metrics) {
        stream << '\t' << metric.name << '=' << std::setprecision(17) << metric.value;
    }
    stream << '\n';
    return stream.str();
}

[[nodiscard]] core::Result<CandidateResult, QuarryError> parse_result_row(
    const std::string_view row,
    const JobManifest& manifest,
    const core::u64 expected_index) {
    std::vector<std::string_view> fields;
    std::size_t begin = 0U;
    while (begin <= row.size()) {
        const std::size_t tab = row.find('\t', begin);
        const std::size_t end = tab == std::string_view::npos ? row.size() : tab;
        fields.push_back(row.substr(begin, end - begin));
        if (tab == std::string_view::npos) {
            break;
        }
        begin = tab + 1U;
    }
    if (fields.size() != 3U + manifest.metrics.size()) {
        return core::Result<CandidateResult, QuarryError>::failure(make_error(
            QuarryErrorCode::results_corrupt, "Quarry results row has an unexpected field count"));
    }
    CandidateResult result;
    if (!parse_u64(fields[0], result.index) || result.index != expected_index) {
        return core::Result<CandidateResult, QuarryError>::failure(make_error(
            QuarryErrorCode::results_corrupt, "Quarry results are not a contiguous canonical candidate prefix"));
    }
    result.candidate_id = std::string(fields[1]);
    result.recipe_fingerprint = std::string(fields[2]);
    result.metrics.reserve(manifest.metrics.size());
    for (std::size_t metric_index = 0U; metric_index < manifest.metrics.size(); ++metric_index) {
        const std::string_view field = fields[3U + metric_index];
        const std::size_t equals = field.find('=');
        if (equals == std::string_view::npos || field.substr(0U, equals) != manifest.metrics[metric_index]) {
            return core::Result<CandidateResult, QuarryError>::failure(make_error(
                QuarryErrorCode::results_corrupt, "Quarry results metric ordering does not match the manifest"));
        }
        double value = 0.0;
        if (!parse_double(std::string(field.substr(equals + 1U)), value)) {
            return core::Result<CandidateResult, QuarryError>::failure(make_error(
                QuarryErrorCode::results_corrupt, "Quarry results contain an invalid metric value"));
        }
        result.metrics.push_back(MetricValue{manifest.metrics[metric_index], value});
    }
    return core::Result<CandidateResult, QuarryError>::success(std::move(result));
}

[[nodiscard]] std::string serialize_checkpoint(const Checkpoint& checkpoint) {
    std::ostringstream stream;
    stream << kCheckpointMagic << ' ' << kQuarryCheckpointVersion << '\n'
           << "job " << checkpoint.job_identity << '\n'
           << "committed " << checkpoint.committed << '\n'
           << "results_hash " << core::hex_u64(checkpoint.results_hash) << '\n'
           << "complete " << (checkpoint.complete ? 1 : 0) << '\n';
    return stream.str();
}

[[nodiscard]] core::Result<Checkpoint, QuarryError> parse_checkpoint(
    const std::string& text,
    const JobManifest& manifest) {
    std::istringstream stream(text);
    std::string magic;
    core::u32 version = 0U;
    if (!(stream >> magic >> version) || magic != kCheckpointMagic || version != kQuarryCheckpointVersion) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::checkpoint_corrupt,
            "Quarry checkpoint has invalid magic/version; preserve it for inspection and restart explicitly"));
    }
    Checkpoint checkpoint;
    std::string key;
    std::string hash_text;
    unsigned int complete = 0U;
    if (!(stream >> key >> checkpoint.job_identity) || key != "job" ||
        !(stream >> key >> checkpoint.committed) || key != "committed" ||
        !(stream >> key >> hash_text) || key != "results_hash" ||
        !(stream >> key >> complete) || key != "complete" || complete > 1U) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::checkpoint_corrupt, "Quarry checkpoint is truncated or malformed"));
    }
    if (checkpoint.job_identity != manifest.identity) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::checkpoint_incompatible,
            "Quarry checkpoint belongs to a different manifest identity"));
    }
    if (checkpoint.committed > manifest.candidate_count) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::checkpoint_corrupt, "Quarry checkpoint committed count exceeds manifest candidate count"));
    }
    try {
        std::size_t consumed = 0U;
        checkpoint.results_hash = std::stoull(hash_text, &consumed, 16);
        if (consumed != hash_text.size()) {
            throw std::invalid_argument("trailing");
        }
    } catch (const std::exception&) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::checkpoint_corrupt, "Quarry checkpoint contains an invalid results checksum"));
    }
    checkpoint.complete = complete != 0U;
    if (checkpoint.complete != (checkpoint.committed == manifest.candidate_count)) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::checkpoint_corrupt,
            "Quarry checkpoint completion flag disagrees with its committed prefix"));
    }
    return core::Result<Checkpoint, QuarryError>::success(std::move(checkpoint));
}

[[nodiscard]] std::string cache_key(const JobManifest& manifest, const core::Recipe& recipe) {
    std::ostringstream stream;
    stream << "quarry-cache=" << kMetricSemanticVersion << '\n'
           << "evaluator=" << manifest.evaluator_semantic_version << '\n'
           << "recipe=" << core::semantic_fingerprint(recipe) << '\n'
           << "render=" << manifest.render_width << 'x' << manifest.render_height << '\n'
           << "animation=" << manifest.animation.first_tick << ',' << manifest.animation.frame_count << ','
           << manifest.animation.tick_stride << '\n'
           << "metrics=" << join_csv(manifest.metrics) << '\n';
    return core::hex_u64(core::fnv1a64(stream.str()));
}

[[nodiscard]] std::filesystem::path cache_path(
    const std::filesystem::path& directory,
    const std::string& key) {
    return directory / (key + ".qcache");
}

[[nodiscard]] core::Result<MetricVector, QuarryError> try_read_cache(
    const std::filesystem::path& path,
    const std::string& expected_key,
    const std::vector<std::string>& metric_names,
    bool& hit) {
    hit = false;
    std::error_code exists_error;
    if (!std::filesystem::exists(path, exists_error) || exists_error) {
        return core::Result<MetricVector, QuarryError>::success({});
    }
    auto text = read_file(path, kMaximumCacheBytes, QuarryErrorCode::io_error);
    if (text.is_error()) {
        return core::Result<MetricVector, QuarryError>::success({});
    }
    std::istringstream stream(text.value());
    std::string magic;
    core::u32 version = 0U;
    std::string key_name;
    std::string stored_key;
    if (!(stream >> magic >> version) || magic != kCacheMagic || version != kMetricSemanticVersion ||
        !(stream >> key_name >> stored_key) || key_name != "key" || stored_key != expected_key) {
        return core::Result<MetricVector, QuarryError>::success({});
    }
    MetricVector metrics;
    metrics.reserve(metric_names.size());
    for (const std::string& expected_name : metric_names) {
        std::string metric_name;
        double value = 0.0;
        if (!(stream >> metric_name >> value) || metric_name != expected_name || !std::isfinite(value)) {
            return core::Result<MetricVector, QuarryError>::success({});
        }
        metrics.push_back(MetricValue{metric_name, value});
    }
    hit = true;
    return core::Result<MetricVector, QuarryError>::success(std::move(metrics));
}

void write_cache_best_effort(
    const std::filesystem::path& path,
    const std::string& key,
    const MetricVector& metrics) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << kCacheMagic << ' ' << kMetricSemanticVersion << '\n'
           << "key " << key << '\n';
    for (const MetricValue& metric : metrics) {
        stream << metric.name << ' ' << std::setprecision(17) << metric.value << '\n';
    }
    static_cast<void>(write_atomic_text(path, stream.str()));
}

[[nodiscard]] core::Result<std::vector<nodes::Image>, QuarryError> render_metric_frames(
    const JobManifest& manifest,
    const core::Recipe& candidate) {
    std::vector<nodes::Image> frames;
    frames.reserve(manifest.animation.frame_count);
    if (manifest.animation.frame_count == 1U && manifest.animation.first_tick == 0U) {
        auto rendered = nodes::render_reference(candidate);
        if (rendered.is_error()) {
            return core::Result<std::vector<nodes::Image>, QuarryError>::failure(make_error(
                QuarryErrorCode::render_failed, "Quarry candidate render failed: " + rendered.error().message));
        }
        frames.push_back(std::move(rendered).value());
        return core::Result<std::vector<nodes::Image>, QuarryError>::success(std::move(frames));
    }
    nodes::FrameSnapshotCache frame_cache(manifest.animation.frame_count);
    for (core::u32 sample = 0U; sample < manifest.animation.frame_count; ++sample) {
        const core::u64 tick = manifest.animation.first_tick +
            static_cast<core::u64>(sample) * manifest.animation.tick_stride;
        auto rendered = nodes::render_animation_reference(candidate, tick, "main", &frame_cache);
        if (rendered.is_error()) {
            return core::Result<std::vector<nodes::Image>, QuarryError>::failure(make_error(
                QuarryErrorCode::render_failed,
                "Quarry animation sample render failed at tick " + std::to_string(tick) + ": " +
                    rendered.error().message));
        }
        frames.push_back(std::move(rendered).value());
    }
    return core::Result<std::vector<nodes::Image>, QuarryError>::success(std::move(frames));
}

[[nodiscard]] core::Result<Checkpoint, QuarryError> load_verified_checkpoint(
    const JobPaths& paths,
    const JobManifest& manifest) {
    std::error_code exists_error;
    const bool checkpoint_exists = std::filesystem::exists(paths.checkpoint, exists_error) && !exists_error;
    exists_error.clear();
    const bool results_exists = std::filesystem::exists(paths.results, exists_error) && !exists_error;
    if (!checkpoint_exists) {
        if (results_exists) {
            return core::Result<Checkpoint, QuarryError>::failure(make_error(
                QuarryErrorCode::checkpoint_corrupt,
                "Quarry results exist without a checkpoint; refusing to guess a resumable committed prefix"));
        }
        Checkpoint fresh;
        fresh.job_identity = manifest.identity;
        fresh.results_hash = core::kFnv1a64Offset;
        const std::string header = results_header(manifest);
        {
            std::ofstream output(paths.results, std::ios::binary | std::ios::trunc);
            if (!output) {
                return core::Result<Checkpoint, QuarryError>::failure(make_error(
                    QuarryErrorCode::io_error, "could not create Quarry results file"));
            }
            output << header;
            output.flush();
            if (!output) {
                return core::Result<Checkpoint, QuarryError>::failure(make_error(
                    QuarryErrorCode::io_error, "could not initialize Quarry results file"));
            }
        }
        fresh.results_hash = update_fnv(fresh.results_hash, header);
        auto written = write_atomic_text(paths.checkpoint, serialize_checkpoint(fresh));
        if (written.is_error()) {
            return core::Result<Checkpoint, QuarryError>::failure(written.error());
        }
        return core::Result<Checkpoint, QuarryError>::success(std::move(fresh));
    }
    if (!results_exists) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::checkpoint_corrupt,
            "Quarry checkpoint exists but its committed results file is missing"));
    }
    auto checkpoint_text = read_file(paths.checkpoint, kMaximumCheckpointBytes, QuarryErrorCode::checkpoint_corrupt);
    if (checkpoint_text.is_error()) {
        return core::Result<Checkpoint, QuarryError>::failure(checkpoint_text.error());
    }
    auto checkpoint = parse_checkpoint(checkpoint_text.value(), manifest);
    if (checkpoint.is_error()) {
        return checkpoint;
    }
    std::ifstream input(paths.results, std::ios::binary);
    if (!input) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::results_corrupt, "could not open Quarry committed results for verification"));
    }
    std::string line;
    if (!std::getline(input, line)) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::results_corrupt, "Quarry results file is empty"));
    }
    line.push_back('\n');
    if (line != results_header(manifest)) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::checkpoint_incompatible, "Quarry results header belongs to a different job identity"));
    }
    core::u64 hash = update_fnv(core::kFnv1a64Offset, line);
    core::u64 rows = 0U;
    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        auto parsed = parse_result_row(line, manifest, manifest.first_candidate + rows);
        if (parsed.is_error()) {
            return core::Result<Checkpoint, QuarryError>::failure(parsed.error());
        }
        line.push_back('\n');
        hash = update_fnv(hash, line);
        ++rows;
    }
    if (!input.eof()) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::results_corrupt, "failed while reading Quarry committed results"));
    }
    if (rows != checkpoint.value().committed || hash != checkpoint.value().results_hash) {
        return core::Result<Checkpoint, QuarryError>::failure(make_error(
            QuarryErrorCode::checkpoint_corrupt,
            "Quarry checkpoint checksum/count does not match the committed results prefix"));
    }
    return checkpoint;
}

}  // namespace

JobPaths derive_job_paths(const std::filesystem::path& manifest_path) {
    JobPaths paths;
    paths.manifest = manifest_path;
    paths.checkpoint = manifest_path.wstring() + L".checkpoint";
    paths.results = manifest_path.wstring() + L".results.tsv";
    paths.cache_directory = manifest_path.wstring() + L".cache";
    return paths;
}

core::Result<JobManifest, QuarryError> make_job_manifest(
    const core::Recipe& base_recipe,
    const core::u64 root_seed,
    const core::u64 candidate_count,
    const core::u32 render_width,
    const core::u32 render_height,
    const double mutation_strength,
    std::vector<std::string> metrics,
    const AnimationSampling animation) {
    JobManifest manifest;
    manifest.root_seed = root_seed;
    manifest.candidate_count = candidate_count;
    manifest.render_width = render_width;
    manifest.render_height = render_height;
    manifest.mutation_strength = mutation_strength;
    manifest.metrics = std::move(metrics);
    manifest.animation = animation;
    manifest.base_recipe = base_recipe;
    auto valid = validate_manifest(manifest);
    if (valid.is_error()) {
        return core::Result<JobManifest, QuarryError>::failure(valid.error());
    }
    return core::Result<JobManifest, QuarryError>::success(std::move(manifest));
}

std::string serialize_job_manifest(const JobManifest& manifest) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << kManifestMagic << ' ' << manifest.version << '\n'
           << "identity " << manifest.identity << '\n'
           << "candidate_enumeration_version " << manifest.candidate_enumeration_version << '\n'
           << "mutation_operator_version " << manifest.mutation_operator_version << '\n'
           << "metric_semantic_version " << manifest.metric_semantic_version << '\n'
           << "evaluator_semantic_version " << manifest.evaluator_semantic_version << '\n'
           << "root_seed " << manifest.root_seed << '\n'
           << "first_candidate " << manifest.first_candidate << '\n'
           << "candidate_count " << manifest.candidate_count << '\n'
           << "mutation_strength " << format_double(manifest.mutation_strength) << '\n'
           << "render_width " << manifest.render_width << '\n'
           << "render_height " << manifest.render_height << '\n'
           << "animation_first_tick " << manifest.animation.first_tick << '\n'
           << "animation_frame_count " << manifest.animation.frame_count << '\n'
           << "animation_tick_stride " << manifest.animation.tick_stride << '\n'
           << "metrics " << join_csv(manifest.metrics) << '\n'
           << "base_recipe_fingerprint " << manifest.base_recipe_fingerprint << '\n'
           << "recipe.begin\n"
           << core::serialize_recipe_canonical(manifest.base_recipe);
    return stream.str();
}

core::Result<JobManifest, QuarryError> parse_job_manifest(const std::string_view text) {
    const std::size_t marker = text.find("recipe.begin\n");
    if (marker == std::string_view::npos) {
        return core::Result<JobManifest, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest, "Quarry manifest is missing its embedded base recipe"));
    }
    const std::string header(text.substr(0U, marker));
    const std::string_view recipe_text = text.substr(marker + std::string_view("recipe.begin\n").size());
    std::istringstream stream(header);
    JobManifest manifest;
    std::string magic;
    if (!(stream >> magic >> manifest.version) || magic != kManifestMagic) {
        return core::Result<JobManifest, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest, "Quarry manifest magic/version header is malformed"));
    }
    std::string key;
    std::string mutation_strength;
    std::string metrics;
    if (!(stream >> key >> manifest.identity) || key != "identity" ||
        !(stream >> key >> manifest.candidate_enumeration_version) || key != "candidate_enumeration_version" ||
        !(stream >> key >> manifest.mutation_operator_version) || key != "mutation_operator_version" ||
        !(stream >> key >> manifest.metric_semantic_version) || key != "metric_semantic_version" ||
        !(stream >> key >> manifest.evaluator_semantic_version) || key != "evaluator_semantic_version" ||
        !(stream >> key >> manifest.root_seed) || key != "root_seed" ||
        !(stream >> key >> manifest.first_candidate) || key != "first_candidate" ||
        !(stream >> key >> manifest.candidate_count) || key != "candidate_count" ||
        !(stream >> key >> mutation_strength) || key != "mutation_strength" ||
        !(stream >> key >> manifest.render_width) || key != "render_width" ||
        !(stream >> key >> manifest.render_height) || key != "render_height" ||
        !(stream >> key >> manifest.animation.first_tick) || key != "animation_first_tick" ||
        !(stream >> key >> manifest.animation.frame_count) || key != "animation_frame_count" ||
        !(stream >> key >> manifest.animation.tick_stride) || key != "animation_tick_stride" ||
        !(stream >> key >> metrics) || key != "metrics" ||
        !(stream >> key >> manifest.base_recipe_fingerprint) || key != "base_recipe_fingerprint") {
        return core::Result<JobManifest, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest, "Quarry manifest header is truncated or uses an unexpected field order"));
    }
    if (!parse_double(mutation_strength, manifest.mutation_strength)) {
        return core::Result<JobManifest, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_manifest, "Quarry manifest mutation_strength is invalid"));
    }
    manifest.metrics = split_csv(metrics);
    auto recipe = core::parse_recipe(recipe_text);
    if (recipe.is_error()) {
        return core::Result<JobManifest, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_recipe, "Quarry embedded base recipe could not be parsed: " + recipe.error().message));
    }
    manifest.base_recipe = std::move(recipe).value();
    auto valid = validate_manifest(manifest);
    if (valid.is_error()) {
        return core::Result<JobManifest, QuarryError>::failure(valid.error());
    }
    return core::Result<JobManifest, QuarryError>::success(std::move(manifest));
}

core::Result<void, QuarryError> write_job_manifest(
    const std::filesystem::path& path,
    const JobManifest& manifest) {
    JobManifest checked = manifest;
    auto valid = validate_manifest(checked);
    if (valid.is_error()) {
        return core::Result<void, QuarryError>::failure(valid.error());
    }
    return write_atomic_text(path, serialize_job_manifest(checked));
}

core::Result<JobManifest, QuarryError> read_job_manifest(const std::filesystem::path& path) {
    auto text = read_file(path, kMaximumManifestBytes, QuarryErrorCode::io_error);
    if (text.is_error()) {
        return core::Result<JobManifest, QuarryError>::failure(text.error());
    }
    return parse_job_manifest(text.value());
}

core::Result<CandidateResult, QuarryError> evaluate_candidate(
    const JobManifest& manifest,
    const core::u64 candidate_index,
    const std::filesystem::path& cache_directory,
    bool* cache_hit) {
    if (candidate_index < manifest.first_candidate ||
        candidate_index >= manifest.first_candidate + manifest.candidate_count) {
        return core::Result<CandidateResult, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_job, "candidate index is outside the manifest range"));
    }
    const core::u64 operation_seed = core::derive_seed(
        manifest.root_seed,
        core::splitmix64(kCandidateSeedDomain ^ candidate_index));
    core::ParameterLocks no_locks;
    auto mutated = core::mutate_recipe_parameters(
        manifest.base_recipe,
        operation_seed,
        manifest.mutation_operator_version,
        manifest.mutation_strength,
        no_locks);
    if (mutated.is_error()) {
        return core::Result<CandidateResult, QuarryError>::failure(make_error(
            QuarryErrorCode::mutation_failed, "Quarry deterministic mutation failed: " + mutated.error().message));
    }
    core::Recipe recipe = std::move(mutated).value();
    recipe.render.width = manifest.render_width;
    recipe.render.height = manifest.render_height;
    const std::string recipe_fingerprint = core::semantic_fingerprint(recipe);
    const std::string candidate_identity_payload =
        manifest.identity + "|" + std::to_string(candidate_index) + "|" + recipe_fingerprint;
    CandidateResult result;
    result.index = candidate_index;
    result.recipe_fingerprint = recipe_fingerprint;
    result.candidate_id = core::hex_u64(core::fnv1a64(candidate_identity_payload));

    const std::string key = cache_key(manifest, recipe);
    bool hit = false;
    auto cached = try_read_cache(cache_path(cache_directory, key), key, manifest.metrics, hit);
    if (cached.is_error()) {
        return core::Result<CandidateResult, QuarryError>::failure(cached.error());
    }
    if (hit) {
        result.metrics = std::move(cached).value();
        if (cache_hit != nullptr) {
            *cache_hit = true;
        }
        return core::Result<CandidateResult, QuarryError>::success(std::move(result));
    }
    if (cache_hit != nullptr) {
        *cache_hit = false;
    }

    auto frames = render_metric_frames(manifest, recipe);
    if (frames.is_error()) {
        return core::Result<CandidateResult, QuarryError>::failure(frames.error());
    }
    auto metrics = compute_metrics(frames.value(), manifest.metrics);
    if (metrics.is_error()) {
        return core::Result<CandidateResult, QuarryError>::failure(make_error(
            QuarryErrorCode::metric_failed, "Quarry metric extraction failed: " + metrics.error().message));
    }
    result.metrics = std::move(metrics).value();
    write_cache_best_effort(cache_path(cache_directory, key), key, result.metrics);
    return core::Result<CandidateResult, QuarryError>::success(std::move(result));
}

core::Result<JobProgress, QuarryError> run_job(
    const std::filesystem::path& manifest_path,
    const core::u32 worker_count,
    CancellationToken* cancellation,
    ProgressCallback progress) {
    if (worker_count == 0U || worker_count > kMaximumQuarryWorkers) {
        return core::Result<JobProgress, QuarryError>::failure(make_error(
            QuarryErrorCode::resource_limit, "Quarry worker count must be within 1..64"));
    }
    auto manifest_result = read_job_manifest(manifest_path);
    if (manifest_result.is_error()) {
        return core::Result<JobProgress, QuarryError>::failure(manifest_result.error());
    }
    const JobManifest manifest = std::move(manifest_result).value();
    const JobPaths paths = derive_job_paths(manifest_path);
    std::error_code directory_error;
    std::filesystem::create_directories(paths.cache_directory, directory_error);
    if (directory_error) {
        return core::Result<JobProgress, QuarryError>::failure(make_error(
            QuarryErrorCode::io_error, "could not create Quarry cache directory"));
    }
    auto checkpoint_result = load_verified_checkpoint(paths, manifest);
    if (checkpoint_result.is_error()) {
        return core::Result<JobProgress, QuarryError>::failure(checkpoint_result.error());
    }
    Checkpoint checkpoint = std::move(checkpoint_result).value();
    JobProgress state{checkpoint.committed, manifest.candidate_count, 0U, checkpoint.complete, false};
    if (progress) {
        progress(state);
    }
    if (checkpoint.complete) {
        return core::Result<JobProgress, QuarryError>::success(state);
    }

    while (checkpoint.committed < manifest.candidate_count) {
        if (cancellation != nullptr && cancellation->cancelled()) {
            state.cancelled = true;
            return core::Result<JobProgress, QuarryError>::success(state);
        }
        const core::u64 remaining = manifest.candidate_count - checkpoint.committed;
        const core::u32 batch_size = static_cast<core::u32>((std::min<core::u64>)(remaining, worker_count));
        using CandidateEvaluation = core::Result<CandidateResult, QuarryError>;
        std::vector<std::optional<CandidateEvaluation>> evaluations(batch_size);
        std::vector<core::u8> cache_hits(batch_size, 0U);
        std::vector<std::thread> workers;
        workers.reserve(batch_size);
        for (core::u32 slot = 0U; slot < batch_size; ++slot) {
            const core::u64 candidate_index = manifest.first_candidate + checkpoint.committed + slot;
            workers.emplace_back([&, slot, candidate_index]() {
                bool hit = false;
                evaluations[slot].emplace(evaluate_candidate(manifest, candidate_index, paths.cache_directory, &hit));
                cache_hits[slot] = hit ? 1U : 0U;
            });
        }
        for (std::thread& worker : workers) {
            worker.join();
        }
        for (core::u32 slot = 0U; slot < batch_size; ++slot) {
            if (!evaluations[slot].has_value()) {
                return core::Result<JobProgress, QuarryError>::failure(make_error(
                    QuarryErrorCode::invalid_job, "Quarry worker ended without publishing a candidate result"));
            }
            if (evaluations[slot]->is_error()) {
                return core::Result<JobProgress, QuarryError>::failure(evaluations[slot]->error());
            }
        }
        std::ofstream output(paths.results, std::ios::binary | std::ios::app);
        if (!output) {
            return core::Result<JobProgress, QuarryError>::failure(make_error(
                QuarryErrorCode::io_error, "could not append Quarry candidate results"));
        }
        for (core::u32 slot = 0U; slot < batch_size; ++slot) {
            const std::string row = serialize_result_row(evaluations[slot]->value());
            output.write(row.data(), static_cast<std::streamsize>(row.size()));
            if (!output) {
                return core::Result<JobProgress, QuarryError>::failure(make_error(
                    QuarryErrorCode::io_error, "failed while appending Quarry candidate results"));
            }
            checkpoint.results_hash = update_fnv(checkpoint.results_hash, row);
            ++checkpoint.committed;
            if (cache_hits[slot] != 0U) {
                ++state.cache_hits;
            }
        }
        output.flush();
        if (!output) {
            return core::Result<JobProgress, QuarryError>::failure(make_error(
                QuarryErrorCode::io_error, "failed to flush Quarry candidate results before checkpoint commit"));
        }
        checkpoint.complete = checkpoint.committed == manifest.candidate_count;
        auto checkpoint_write = write_atomic_text(paths.checkpoint, serialize_checkpoint(checkpoint));
        if (checkpoint_write.is_error()) {
            return core::Result<JobProgress, QuarryError>::failure(checkpoint_write.error());
        }
        state.committed = checkpoint.committed;
        state.complete = checkpoint.complete;
        state.cancelled = false;
        if (progress) {
            progress(state);
        }
        if (cancellation != nullptr && cancellation->cancelled() && !state.complete) {
            state.cancelled = true;
            return core::Result<JobProgress, QuarryError>::success(state);
        }
    }
    return core::Result<JobProgress, QuarryError>::success(state);
}

core::Result<JobProgress, QuarryError> inspect_job(const std::filesystem::path& manifest_path) {
    auto manifest_result = read_job_manifest(manifest_path);
    if (manifest_result.is_error()) {
        return core::Result<JobProgress, QuarryError>::failure(manifest_result.error());
    }
    const JobManifest manifest = std::move(manifest_result).value();
    const JobPaths paths = derive_job_paths(manifest_path);
    std::error_code exists_error;
    if (!std::filesystem::exists(paths.checkpoint, exists_error) || exists_error) {
        return core::Result<JobProgress, QuarryError>::success(
            JobProgress{0U, manifest.candidate_count, 0U, false, false});
    }
    auto checkpoint = load_verified_checkpoint(paths, manifest);
    if (checkpoint.is_error()) {
        return core::Result<JobProgress, QuarryError>::failure(checkpoint.error());
    }
    return core::Result<JobProgress, QuarryError>::success(JobProgress{
        checkpoint.value().committed,
        manifest.candidate_count,
        0U,
        checkpoint.value().complete,
        false});
}

core::Result<std::vector<CandidateResult>, QuarryError> read_results(
    const std::filesystem::path& manifest_path,
    const std::size_t maximum_results) {
    auto manifest_result = read_job_manifest(manifest_path);
    if (manifest_result.is_error()) {
        return core::Result<std::vector<CandidateResult>, QuarryError>::failure(manifest_result.error());
    }
    const JobManifest manifest = std::move(manifest_result).value();
    const JobPaths paths = derive_job_paths(manifest_path);
    std::error_code exists_error;
    if (!std::filesystem::exists(paths.results, exists_error) || exists_error) {
        return core::Result<std::vector<CandidateResult>, QuarryError>::success({});
    }
    auto checkpoint = load_verified_checkpoint(paths, manifest);
    if (checkpoint.is_error()) {
        return core::Result<std::vector<CandidateResult>, QuarryError>::failure(checkpoint.error());
    }
    std::ifstream input(paths.results, std::ios::binary);
    std::string line;
    std::getline(input, line);
    std::vector<CandidateResult> results;
    const core::u64 limit = maximum_results == 0U
        ? checkpoint.value().committed
        : (std::min<core::u64>)(checkpoint.value().committed, maximum_results);
    results.reserve(static_cast<std::size_t>(limit));
    core::u64 row = 0U;
    while (row < limit && std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        auto parsed = parse_result_row(line, manifest, manifest.first_candidate + row);
        if (parsed.is_error()) {
            return core::Result<std::vector<CandidateResult>, QuarryError>::failure(parsed.error());
        }
        results.push_back(std::move(parsed).value());
        ++row;
    }
    return core::Result<std::vector<CandidateResult>, QuarryError>::success(std::move(results));
}

}  // namespace artminer::quarry
