// AM-013 command shim. Keep the established command parser byte-for-byte in
// main.cpp while extending the executable with the material workflow command.
// CMake compiles this translation unit instead of main.cpp; the include renames
// the legacy entry point so all prior commands retain their accepted behaviour.
#define wmain artminer_legacy_wmain
#include "app/main.cpp"
#undef wmain

#include <iostream>
#include <string_view>

#include "app/material_command.hpp"

int wmain(const int argc, wchar_t* argv[]) {
    if (argc >= 2 && std::wstring_view(argv[1]) == L"material") {
        return artminer::app::run_material_command(argc, argv);
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
            << "  ArtMiner material loop-sequence <file.amr> <loop-length> <output-dir> <png|bmp|rgba> [output-name]\n";
        return 0;
    }
    return artminer_legacy_wmain(argc, argv);
}
