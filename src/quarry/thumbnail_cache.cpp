#include "quarry/quarry.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "core/hash.hpp"
#include "core/prng.hpp"
#include "nodes/motion_evaluator.hpp"
#include "nodes/static_evaluator.hpp"

namespace artminer::quarry {
namespace {

constexpr std::string_view kThumbnailMagic = "ARTMINER_QUARRY_THUMBNAIL";
constexpr core::u64 kCandidateSeedDomain = 0x514152525943414eULL;

[[nodiscard]] QuarryError make_error(const QuarryErrorCode code, std::string message) {
    return QuarryError{code, std::move(message)};
}

[[nodiscard]] bool parse_u64(const std::string_view text, core::u64& value) noexcept {
    if (text.empty()) {
        return false;
    }
    core::u64 parsed_value = 0U;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, parsed_value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return false;
    }
    value = parsed_value;
    return true;
}

[[nodiscard]] bool split_field(
    const std::string& line,
    const std::string_view expected_name,
    std::string_view& value) noexcept {
    if (line.size() <= expected_name.size() ||
        line.compare(0U, expected_name.size(), expected_name) != 0 ||
        line[expected_name.size()] != ' ') {
        return false;
    }
    value = std::string_view(line).substr(expected_name.size() + 1U);
    return !value.empty();
}

[[nodiscard]] core::Result<core::Recipe, QuarryError> candidate_recipe(
    const JobManifest& manifest,
    const core::u64 candidate_index) {
    if (candidate_index < manifest.first_candidate ||
        candidate_index - manifest.first_candidate >= manifest.candidate_count) {
        return core::Result<core::Recipe, QuarryError>::failure(make_error(
            QuarryErrorCode::invalid_job, "thumbnail candidate index is outside the manifest range"));
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
        return core::Result<core::Recipe, QuarryError>::failure(make_error(
            QuarryErrorCode::mutation_failed,
            "Quarry thumbnail mutation failed: " + mutated.error().message));
    }
    core::Recipe recipe = std::move(mutated).value();
    recipe.render.width = manifest.render_width;
    recipe.render.height = manifest.render_height;
    return core::Result<core::Recipe, QuarryError>::success(std::move(recipe));
}

[[nodiscard]] std::string thumbnail_key(
    const JobManifest& manifest,
    const core::u64 candidate_index,
    const core::Recipe& recipe) {
    std::ostringstream stream;
    stream << "thumbnail-cache=" << kThumbnailCacheVersion << '\n'
           << "job=" << manifest.identity << '\n'
           << "candidate=" << candidate_index << '\n'
           << "recipe=" << core::semantic_fingerprint(recipe) << '\n'
           << "evaluator=" << manifest.evaluator_semantic_version << '\n'
           << "render=" << manifest.render_width << 'x' << manifest.render_height << '\n'
           << "animation=" << manifest.animation.first_tick << ',' << manifest.animation.frame_count << ','
           << manifest.animation.tick_stride << '\n';
    return core::hex_u64(core::fnv1a64(stream.str()));
}

[[nodiscard]] std::filesystem::path thumbnail_path(
    const std::filesystem::path& directory,
    const std::string& key) {
    return directory / (key + ".qthumb");
}

[[nodiscard]] std::optional<nodes::Image> try_read_thumbnail(
    const std::filesystem::path& path,
    const std::string& expected_key,
    const core::u32 expected_width,
    const core::u32 expected_height) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    std::string line;
    if (!std::getline(input, line)) {
        return std::nullopt;
    }
    {
        std::istringstream header(line);
        std::string magic;
        core::u32 version = 0U;
        if (!(header >> magic >> version) || magic != kThumbnailMagic || version != kThumbnailCacheVersion) {
            return std::nullopt;
        }
    }

    std::string key_line;
    std::string width_line;
    std::string height_line;
    std::string bytes_line;
    std::string hash_line;
    std::string data_line;
    if (!std::getline(input, key_line) || !std::getline(input, width_line) ||
        !std::getline(input, height_line) || !std::getline(input, bytes_line) ||
        !std::getline(input, hash_line) || !std::getline(input, data_line)) {
        return std::nullopt;
    }

    std::string_view value;
    if (!split_field(key_line, "key", value) || value != expected_key) {
        return std::nullopt;
    }

    core::u64 width = 0U;
    if (!split_field(width_line, "width", value) || !parse_u64(value, width) || width != expected_width) {
        return std::nullopt;
    }
    core::u64 height = 0U;
    if (!split_field(height_line, "height", value) || !parse_u64(value, height) || height != expected_height) {
        return std::nullopt;
    }
    core::u64 bytes = 0U;
    if (!split_field(bytes_line, "bytes", value) || !parse_u64(value, bytes)) {
        return std::nullopt;
    }
    std::string expected_hash;
    if (!split_field(hash_line, "hash", value)) {
        return std::nullopt;
    }
    expected_hash.assign(value.begin(), value.end());
    if (data_line != "data") {
        return std::nullopt;
    }

    const core::u64 pixels = width * height;
    if (width == 0U || height == 0U || pixels > (std::numeric_limits<std::size_t>::max)() / 4U ||
        bytes != pixels * 4U || bytes > (std::numeric_limits<std::size_t>::max)()) {
        return std::nullopt;
    }

    nodes::Image image;
    image.width = static_cast<core::u32>(width);
    image.height = static_cast<core::u32>(height);
    try {
        image.rgba.resize(static_cast<std::size_t>(bytes));
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
    if (!image.rgba.empty()) {
        input.read(reinterpret_cast<char*>(image.rgba.data()), static_cast<std::streamsize>(image.rgba.size()));
        if (input.gcount() != static_cast<std::streamsize>(image.rgba.size())) {
            return std::nullopt;
        }
    }
    char trailing = '\0';
    if (input.get(trailing)) {
        return std::nullopt;
    }
    if (nodes::image_fingerprint(image) != expected_hash) {
        return std::nullopt;
    }
    return image;
}

void write_thumbnail_best_effort(
    const std::filesystem::path& path,
    const std::string& key,
    const nodes::Image& image) {
    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
        return;
    }

    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return;
        }
        output << kThumbnailMagic << ' ' << kThumbnailCacheVersion << '\n'
               << "key " << key << '\n'
               << "width " << image.width << '\n'
               << "height " << image.height << '\n'
               << "bytes " << image.rgba.size() << '\n'
               << "hash " << nodes::image_fingerprint(image) << '\n'
               << "data\n";
        if (!image.rgba.empty()) {
            output.write(
                reinterpret_cast<const char*>(image.rgba.data()),
                static_cast<std::streamsize>(image.rgba.size()));
        }
        output.flush();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return;
        }
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        std::filesystem::remove(temporary, ignored);
    }
}

[[nodiscard]] core::Result<nodes::Image, QuarryError> render_thumbnail(
    const JobManifest& manifest,
    const core::Recipe& recipe) {
    if (manifest.animation.frame_count == 1U && manifest.animation.first_tick == 0U) {
        auto rendered = nodes::render_reference(recipe);
        if (rendered.is_error()) {
            return core::Result<nodes::Image, QuarryError>::failure(make_error(
                QuarryErrorCode::render_failed,
                "Quarry thumbnail render failed: " + rendered.error().message));
        }
        return core::Result<nodes::Image, QuarryError>::success(std::move(rendered).value());
    }

    nodes::FrameSnapshotCache cache(1U);
    auto rendered = nodes::render_animation_reference(recipe, manifest.animation.first_tick, "main", &cache);
    if (rendered.is_error()) {
        return core::Result<nodes::Image, QuarryError>::failure(make_error(
            QuarryErrorCode::render_failed,
            "Quarry thumbnail animation render failed at tick " +
                std::to_string(manifest.animation.first_tick) + ": " + rendered.error().message));
    }
    return core::Result<nodes::Image, QuarryError>::success(std::move(rendered).value());
}

}  // namespace

core::Result<nodes::Image, QuarryError> read_cached_thumbnail(
    const JobManifest& manifest,
    const core::u64 candidate_index,
    const std::filesystem::path& cache_directory) {
    auto recipe_result = candidate_recipe(manifest, candidate_index);
    if (recipe_result.is_error()) {
        return core::Result<nodes::Image, QuarryError>::failure(recipe_result.error());
    }
    core::Recipe recipe = std::move(recipe_result).value();
    const std::string key = thumbnail_key(manifest, candidate_index, recipe);
    const std::filesystem::path path = thumbnail_path(cache_directory, key);
    if (auto cached = try_read_thumbnail(path, key, manifest.render_width, manifest.render_height); cached.has_value()) {
        return core::Result<nodes::Image, QuarryError>::success(std::move(*cached));
    }

    auto rendered = render_thumbnail(manifest, recipe);
    if (rendered.is_error()) {
        return rendered;
    }
    nodes::Image image = std::move(rendered).value();
    write_thumbnail_best_effort(path, key, image);
    return core::Result<nodes::Image, QuarryError>::success(std::move(image));
}

}  // namespace artminer::quarry
