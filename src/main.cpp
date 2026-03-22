#include "Application.h"
#include <iostream>

int main() {
    std::cout << "============================\n";
    std::cout << "Eternal Sonata Studio v1.0\n";
    std::cout << "============================\n\n";

    AppConfig config;
    config.title = "Eternal Sonata Studio";
    config.width = 1280;
    config.height = 720;

    Application app(config);
    app.Run();

    return 0;
}
//aaaa