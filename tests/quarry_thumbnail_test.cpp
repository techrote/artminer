#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include "core/recipe.hpp"
#include "nodes/static_evaluator.hpp"
#include "quarry/quarry.hpp"

namespace {

int g_failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] artminer::core::Recipe base_recipe() {
    constexpr std::string_view text = R"AMR(amr 1
evaluator 1
seed 19
render 16 16 reference
node source core.scalar.constant 1
param source value f64 0.5
node image core.image.from_scalar 1
param image palette enum grayscale
edge source value image source
output main image value
)AMR";
    auto parsed = artminer::core::parse_recipe(text);
    if (parsed.is_error()) {
        ++g_failures;
        std::cerr << "FAIL: thumbnail base recipe parse failed: " << parsed.error().message << '\n';
        return {};
    }
    return std::move(parsed).value();
}

[[nodiscard]] std::filesystem::path fresh_root() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "artminer-am010-thumbnail-cache-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root, ignored);
    return root;
}

[[nodiscard]] std::filesystem::path find_thumbnail(const std::filesystem::path& cache) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(cache, error), end; !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == L".qthumb") {
            return it->path();
        }
    }
    return {};
}

void test_read_through_thumbnail_cache() {
    const auto root = fresh_root();
    auto manifest_result = artminer::quarry::make_job_manifest(
        base_recipe(), 0x1122334455667788ULL, 8U, 24U, 20U, 0.4,
        {"entropy", "edge_density"});
    expect(manifest_result.is_ok(), "thumbnail manifest should be constructible");
    if (manifest_result.is_error()) {
        return;
    }
    const auto manifest = manifest_result.value();
    const auto cache = root / "cache";

    auto first = artminer::quarry::read_cached_thumbnail(manifest, 3U, cache);
    expect(first.is_ok(), "first thumbnail request should render through a cache miss");
    if (first.is_error()) {
        return;
    }
    expect(first.value().width == 24U && first.value().height == 20U,
           "thumbnail should use manifest render dimensions");
    const std::string reference_hash = artminer::nodes::image_fingerprint(first.value());

    const auto thumbnail_path = find_thumbnail(cache);
    expect(!thumbnail_path.empty(), "first thumbnail request should persist a disposable cache entry");

    auto second = artminer::quarry::read_cached_thumbnail(manifest, 3U, cache);
    expect(second.is_ok(), "cached thumbnail should load");
    if (second.is_ok()) {
        expect(artminer::nodes::image_fingerprint(second.value()) == reference_hash,
               "thumbnail cache hit must preserve canonical pixels");
    }

    if (!thumbnail_path.empty()) {
        std::ofstream corrupt(thumbnail_path, std::ios::binary | std::ios::trunc);
        corrupt << "ARTMINER_QUARRY_THUMBNAIL 1\nkey plausible-but-wrong\n";
    }
    auto after_corruption = artminer::quarry::read_cached_thumbnail(manifest, 3U, cache);
    expect(after_corruption.is_ok(), "corrupt thumbnail cache should be a safe regenerative miss");
    if (after_corruption.is_ok()) {
        expect(artminer::nodes::image_fingerprint(after_corruption.value()) == reference_hash,
               "corrupt thumbnail cache must not change canonical pixels");
    }

    std::error_code ignored;
    std::filesystem::remove_all(cache, ignored);
    auto after_delete = artminer::quarry::read_cached_thumbnail(manifest, 3U, cache);
    expect(after_delete.is_ok(), "deleted thumbnail cache should regenerate");
    if (after_delete.is_ok()) {
        expect(artminer::nodes::image_fingerprint(after_delete.value()) == reference_hash,
               "thumbnail cache deletion must change performance only");
    }
}

void test_explicit_worker_bound() {
    const auto root = fresh_root();
    auto manifest_result = artminer::quarry::make_job_manifest(
        base_recipe(), 7U, 1U, 8U, 8U, 0.25, {"entropy"});
    expect(manifest_result.is_ok(), "worker-bound manifest should be constructible");
    if (manifest_result.is_error()) {
        return;
    }
    const auto path = root / "bounded.amq";
    expect(artminer::quarry::write_job_manifest(path, manifest_result.value()).is_ok(),
           "worker-bound manifest should write");
    auto rejected = artminer::quarry::run_job(path, artminer::quarry::kMaximumQuarryWorkers + 1U);
    expect(rejected.is_error() &&
               rejected.error().code == artminer::quarry::QuarryErrorCode::resource_limit,
           "scheduler must reject worker counts outside the explicit bounded queue policy");
}

}  // namespace

int main() {
    test_read_through_thumbnail_cache();
    test_explicit_worker_bound();
    if (g_failures != 0) {
        std::cerr << g_failures << " Quarry thumbnail/bound test(s) failed\n";
        return 1;
    }
    std::cout << "ArtMiner Quarry thumbnail/bound tests passed\n";
    return 0;
}
