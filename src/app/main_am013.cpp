// AM-015 release command shim. Keep the established command parser in main.cpp
// while extending the executable with material workflows and coherent release
// metadata. CMake compiles this translation unit instead of main.cpp; the include
// renames the legacy entry point so prior command semantics remain compatible.
#define wmain artminer_legacy_wmain
#include "app/main.cpp"
#undef wmain

#include <iostream>
#include <string_view>

#include "app/material_command.hpp"
#include "core/specimen_browser.hpp"
#include "core/topology_mutation.hpp"
#include "export/export.hpp"
#include "quarry/quarry.hpp"

int wmain(const int argc, wchar_t* argv[]) {
    if (argc >= 2 && std::wstring_view(argv[1]) == L"material") {
        return artminer::app::run_material_command(argc, argv);
    }
    if (argc >= 2 &&
        (std::wstring_view(argv[1]) == L"--version" || std::wstring_view(argv[1]) == L"-V")) {
        print_version();
        std::cout
            << "channel " << artminer::core::kReleaseChannel << '\n'
            << "recipe-schema " << artminer::core::kRecipeSchemaVersion << '\n'
            << "evaluator " << artminer::core::kEvaluatorSemanticVersion << '\n'
            << "exporter " << artminer::exporting::kExporterSemanticVersion << '\n'
            << "parameter-mutation " << artminer::core::kParameterMutationOperatorVersion << '\n'
            << "topology-mutation " << artminer::core::kTopologyMutationOperatorVersion << '\n'
            << "quarry-manifest " << artminer::quarry::kQuarryManifestVersion << '\n'
            << "quarry-checkpoint " << artminer::quarry::kQuarryCheckpointVersion << '\n'
            << "quarry-enumeration " << artminer::quarry::kCandidateEnumerationVersion << '\n'
            << "quarry-thumbnail-cache " << artminer::quarry::kThumbnailCacheVersion << '\n';
        return 0;
    }
    if (argc >= 2 &&
        (std::wstring_view(argv[1]) == L"--help" || std::wstring_view(argv[1]) == L"-h" ||
         std::wstring_view(argv[1]) == L"/?")) {
        print_help();
        std::cout
            << "\nMaterial workflows:\n"
            << "  ArtMiner material seam <file.amr> [output-name] [threshold]\n"
            << "  ArtMiner material loop <file.amr> <loop-length> [output-name] [tolerance]\n"
            << "  ArtMiner material ui <file.amr> [output-name]\n"
            << "  ArtMiner material export <file.amr> <output-name> <output-dir> <png|bmp|rgba>\n"
            << "  ArtMiner material loop-sequence <file.amr> <loop-length> <output-dir> <png|bmp|rgba> [output-name]\n"
            << "\nRelease notes:\n"
            << "  Portable package state remains beside ArtMiner.exe; no profile/registry fallback is used.\n"
            << "  See docs/release-readiness.md for limits, recovery, packaging, and benchmark methodology.\n";
        return 0;
    }
    return artminer_legacy_wmain(argc, argv);
}
