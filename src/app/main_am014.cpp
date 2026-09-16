// AM-014 command shim. Keep the accepted legacy parser and AM-013 material
// workflow behavior while adding deterministic topology-mutation commands.
#define wmain artminer_legacy_wmain
#include "app/main.cpp"
#undef wmain

#include <iostream>
#include <string_view>

#include "app/material_command.hpp"
#include "app/topology_command.hpp"

int wmain(const int argc, wchar_t* argv[]) {
    if (argc >= 2 && std::wstring_view(argv[1]) == L"material") {
        return artminer::app::run_material_command(argc, argv);
    }
    if (argc >= 2 && std::wstring_view(argv[1]) == L"topology") {
        return artminer::app::run_topology_command(argc - 1, argv + 1);
    }
    if (argc >= 2 &&
        (std::wstring_view(argv[1]) == L"--help" || std::wstring_view(argv[1]) == L"-h" ||
         std::wstring_view(argv[1]) == L"/?")) {
        print_help();
        std::cout
            << "\nAM-013 material workflows:\n"
            << "  ArtMiner material seam <file.amr> [output-name] [threshold]\n"
            << "  ArtMiner material loop <file.amr> <loop-length> [output-name] [tolerance]\n"
            << "  ArtMiner material ui <file.amr> [output-name]\n"
            << "  ArtMiner material export <file.amr> <output-name> <output-dir> <png|bmp|rgba>\n"
            << "  ArtMiner material loop-sequence <file.amr> <loop-length> <output-dir> <png|bmp|rgba> [output-name]\n"
            << "\nAM-014 topology workflows:\n"
            << "  ArtMiner topology mutate <input.amr> <seed> <strength> <budget> <output.amr> [locks]\n"
            << "  ArtMiner topology grid <input.amr> <seed> <strength> <budget> <output-dir> [locks]\n"
            << "  ArtMiner topology quarry-candidate <job.qjob> <candidate-index> <budget> <output.amr> [locks]\n"
            << "  locks: --lock-node <id> or --lock-upstream <root-id>\n";
        return 0;
    }
    return artminer_legacy_wmain(argc, argv);
}
