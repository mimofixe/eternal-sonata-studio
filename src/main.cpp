#include "Application.h"
#include <filesystem>
#include <iostream>

// Resolve argv[1] into a folder to open at startup. Relative paths are taken
// against the current working directory, so the studio can be launched as
//   .\out\build\Release\es_studio.exe ..\EternalSonataReprise\assets
// Passing a file opens the folder containing it, which makes shell completion
// on a specific .e usable as a shortcut to its directory.
static std::string ResolveStartFolder(const char* arg) {
    namespace fs = std::filesystem;
    std::error_code ec;

    fs::path p = fs::absolute(fs::path(arg), ec);
    if (ec) {
        std::cerr << "Cannot resolve path: " << arg << " (" << ec.message() << ")\n";
        return {};
    }
    // Canonical form collapses the ".." so the browser's root label reads as the
    // real folder name rather than the literal argument.
    fs::path canon = fs::weakly_canonical(p, ec);
    if (!ec && !canon.empty()) p = canon;

    if (!fs::exists(p, ec)) {
        std::cerr << "No such file or folder: " << p.string() << "\n";
        return {};
    }
    if (!fs::is_directory(p, ec)) {
        p = p.parent_path();
        std::cout << "Argument is a file; opening its folder instead.\n";
    }
    return p.string();
}

int main(int argc, char** argv) {
    std::cout << "============================\n";
    std::cout << "Eternal Sonata Studio\n";
    std::cout << "============================\n\n";

    AppConfig config;
    config.title = "Eternal Sonata Studio";
    config.width = 1280;
    config.height = 720;

    if (argc > 1) {
        const std::string a = argv[1];
        if (a == "-h" || a == "--help") {
            std::cout << "Usage: " << argv[0] << " [folder]\n"
                << "  folder   open this folder in the file browser at startup\n"
                << "           (relative paths resolve against the current directory)\n";
            return 0;
        }
        config.start_folder = ResolveStartFolder(argv[1]);
        // A bad path is not fatal: the studio still starts, and the folder can be
        // picked from File > Open Folder... as usual.
        if (!config.start_folder.empty())
            std::cout << "Opening folder: " << config.start_folder << "\n";
        if (argc > 2)
            std::cerr << "Ignoring extra arguments after '" << argv[1] << "'\n";
    }

    Application app(config);
    app.Run();

    return 0;
}
