#include "app/topology_command.hpp"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "core/recipe.hpp"
#include "core/specimen_browser.hpp"
#include "core/topology_mutation.hpp"
#include "quarry/quarry.hpp"

namespace artminer::app {
namespace {

[[nodiscard]] std::string narrow_utf8(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(needed), '\0');
    (void)WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), needed, nullptr, nullptr);
    return result;
}

[[nodiscard]] bool parse_u64(const std::wstring_view text, core::u64& value) {
    const std::string utf8 = narrow_utf8(text);
    const auto parsed = std::from_chars(utf8.data(), utf8.data() + utf8.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == utf8.data() + utf8.size();
}

[[nodiscard]] bool parse_u32(const std::wstring_view text, core::u32& value) {
    const std::string utf8 = narrow_utf8(text);
    const auto parsed = std::from_chars(utf8.data(), utf8.data() + utf8.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == utf8.data() + utf8.size();
}

[[nodiscard]] bool parse_real(const std::wstring_view text, double& value) {
    const std::string utf8 = narrow_utf8(text);
    const auto parsed = std::from_chars(
        utf8.data(), utf8.data() + utf8.size(), value, std::chars_format::general);
    return parsed.ec == std::errc{} && parsed.ptr == utf8.data() + utf8.size();
}

[[nodiscard]] bool load_recipe(const std::filesystem::path& path, core::Recipe& recipe) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "could not open recipe\n";
        return false;
    }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    auto parsed = core::parse_recipe(text);
    if (parsed.is_error()) {
        std::cerr << "recipe parse error: " << parsed.error().message << '\n';
        return false;
    }
    recipe = std::move(parsed).value();
    const auto validation = core::validate_recipe(recipe);
    if (!validation.empty()) {
        std::cerr << "recipe validation error: " << validation.front().message << '\n';
        return false;
    }
    return true;
}

[[nodiscard]] bool write_new_recipe(const std::filesystem::path& path, const core::Recipe& recipe) {
    std::error_code error;
    if (std::filesystem::exists(path, error)) {
        std::cerr << "refusing to overwrite existing output\n";
        return false;
    }
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            std::cerr << "could not create output directory\n";
            return false;
        }
    }
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            std::cerr << "could not create temporary output\n";
            return false;
        }
        output << core::serialize_recipe_canonical(recipe);
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary, error);
            std::cerr << "could not write temporary output\n";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        std::cerr << "could not publish output\n";
        return false;
    }
    return true;
}

[[nodiscard]] bool apply_lock_arguments(
    const core::Recipe& recipe,
    const int argc,
    wchar_t* argv[],
    const int first,
    core::StructuralLocks& locks) {
    int index = first;
    while (index < argc) {
        const std::wstring_view flag(argv[index]);
        if ((flag != L"--lock-node" && flag != L"--lock-upstream") || index + 1 >= argc) {
            std::cerr << "lock arguments must be --lock-node <id> or --lock-upstream <root-id>\n";
            return false;
        }
        const std::string id = narrow_utf8(argv[index + 1]);
        const bool exists = std::any_of(recipe.nodes.begin(), recipe.nodes.end(), [&](const core::NodeInstance& node) {
            return node.id == id;
        });
        if (!exists) {
            std::cerr << "structural lock references unknown node: " << id << '\n';
            return false;
        }
        if (flag == L"--lock-node") {
            locks.set_node(id, true);
        } else {
            locks.set_upstream_subgraph(recipe, id, true);
        }
        index += 2;
    }
    return true;
}

[[nodiscard]] bool parse_controls(
    wchar_t* seed_text,
    wchar_t* strength_text,
    wchar_t* budget_text,
    core::u64& seed,
    double& strength,
    core::u32& budget) {
    return parse_u64(seed_text, seed) && parse_real(strength_text, strength) &&
        parse_u32(budget_text, budget) && strength >= 0.0 && strength <= 1.0 &&
        budget >= 1U && budget <= core::kMaximumTopologyMutationBudget;
}

int mutate_command(const int argc, wchar_t* argv[]) {
    if (argc < 7) {
        std::cerr << "usage: ArtMiner topology mutate <input.amr> <seed> <strength> <budget> <output.amr> [locks]\n";
        return 2;
    }
    core::Recipe parent;
    if (!load_recipe(argv[2], parent)) {
        return 2;
    }
    core::u64 seed = 0U;
    core::u32 budget = 0U;
    double strength = 0.0;
    if (!parse_controls(argv[3], argv[4], argv[5], seed, strength, budget)) {
        std::cerr << "seed/strength/budget must be uint64, [0,1], and 1..16\n";
        return 2;
    }
    core::StructuralLocks locks;
    if (!apply_lock_arguments(parent, argc, argv, 7, locks)) {
        return 2;
    }
    auto result = core::mutate_recipe_topology(
        parent, seed, core::kTopologyMutationOperatorVersion, strength, budget, locks);
    if (result.is_error()) {
        std::cerr << "topology mutation failed: " << result.error().message << '\n';
        return 1;
    }
    if (!write_new_recipe(argv[6], result.value().recipe)) {
        return 1;
    }
    std::cout << "topology child " << core::semantic_fingerprint(result.value().recipe)
              << " steps " << result.value().steps.size()
              << "/" << result.value().requested_steps
              << (result.value().exhausted ? " exhausted" : "") << '\n';
    for (const auto& step : result.value().steps) {
        std::cout << "  " << core::to_string(step.kind) << ": " << step.description << '\n';
    }
    return 0;
}

int grid_command(const int argc, wchar_t* argv[]) {
    if (argc < 7) {
        std::cerr << "usage: ArtMiner topology grid <input.amr> <seed> <strength> <budget> <output-dir> [locks]\n";
        return 2;
    }
    core::Recipe parent;
    if (!load_recipe(argv[2], parent)) {
        return 2;
    }
    core::u64 seed = 0U;
    core::u32 budget = 0U;
    double strength = 0.0;
    if (!parse_controls(argv[3], argv[4], argv[5], seed, strength, budget)) {
        std::cerr << "seed/strength/budget must be uint64, [0,1], and 1..16\n";
        return 2;
    }
    core::StructuralLocks locks;
    if (!apply_lock_arguments(parent, argc, argv, 7, locks)) {
        return 2;
    }
    auto generated = core::generate_topology_specimen_grid(
        parent, seed, core::kTopologyMutationOperatorVersion, strength, budget, locks);
    if (generated.is_error()) {
        std::cerr << "topology specimen generation failed: " << generated.error().message << '\n';
        return 1;
    }
    const std::filesystem::path directory(argv[6]);
    std::error_code error;
    if (std::filesystem::exists(directory, error)) {
        std::cerr << "refusing to overwrite existing topology-grid directory\n";
        return 1;
    }
    std::filesystem::create_directories(directory, error);
    if (error) {
        std::cerr << "could not create topology-grid directory\n";
        return 1;
    }
    for (const auto& specimen : generated.value()) {
        std::wstring name = L"specimen_";
        if (specimen.index < 10U) {
            name += L"0";
        }
        name += std::to_wstring(specimen.index);
        name += L".amr";
        if (!write_new_recipe(directory / name, specimen.recipe)) {
            std::filesystem::remove_all(directory, error);
            return 1;
        }
    }
    std::cout << "wrote 16 deterministic topology specimens\n";
    return 0;
}

int quarry_candidate_command(const int argc, wchar_t* argv[]) {
    if (argc < 6) {
        std::cerr << "usage: ArtMiner topology quarry-candidate <job.qjob> <candidate-index> <budget> <output.amr> [locks]\n";
        return 2;
    }
    auto manifest = quarry::read_job_manifest(argv[2]);
    if (manifest.is_error()) {
        std::cerr << "could not read Quarry manifest: " << manifest.error().message << '\n';
        return 2;
    }
    core::u64 index = 0U;
    core::u32 budget = 0U;
    if (!parse_u64(argv[3], index) || !parse_u32(argv[4], budget)) {
        std::cerr << "candidate index/budget must be uint64 and uint32\n";
        return 2;
    }
    quarry::CandidateGenerationSettings settings;
    settings.mode = quarry::CandidateMutationMode::topology;
    settings.topology_budget = budget;
    if (!apply_lock_arguments(manifest.value().base_recipe, argc, argv, 6, settings.structural_locks)) {
        return 2;
    }
    auto candidate = quarry::reconstruct_candidate_recipe(manifest.value(), index, settings);
    if (candidate.is_error()) {
        std::cerr << "topology Quarry candidate failed: " << candidate.error().message << '\n';
        return 1;
    }
    if (!write_new_recipe(argv[5], candidate.value())) {
        return 1;
    }
    std::cout << "topology Quarry candidate " << index
              << " generation " << quarry::candidate_generation_identity(manifest.value(), settings)
              << " recipe " << core::semantic_fingerprint(candidate.value()) << '\n';
    return 0;
}

}  // namespace

int run_topology_command(const int argc, wchar_t* argv[]) {
    if (argc < 2) {
        return 2;
    }
    const std::wstring_view command(argv[1]);
    if (command == L"mutate") {
        return mutate_command(argc, argv);
    }
    if (command == L"grid") {
        return grid_command(argc, argv);
    }
    if (command == L"quarry-candidate") {
        return quarry_candidate_command(argc, argv);
    }
    std::cerr
        << "ArtMiner topology commands:\n"
        << "  topology mutate <input.amr> <seed> <strength> <budget> <output.amr> [--lock-node id|--lock-upstream id ...]\n"
        << "  topology grid <input.amr> <seed> <strength> <budget> <output-dir> [locks]\n"
        << "  topology quarry-candidate <job.qjob> <candidate-index> <budget> <output.amr> [locks]\n";
    return 2;
}

}  // namespace artminer::app
