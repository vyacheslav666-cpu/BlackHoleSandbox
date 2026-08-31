#include <exception>
#include <iostream>

#include "Application/AppOptions.hpp"
#include "Application/Application.hpp"

// On a hybrid-graphics laptop the OpenGL context defaults to the integrated
// GPU unless the executable exports these symbols. Both vendors' drivers look
// them up by name at process start, which is why they must live in the
// executable itself (not a static library) and keep exactly these spellings.
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

int main(int argc, char** argv) {
    try {
        bhs::AppOptions options = bhs::parseCommandLine(argc, argv);
        if (options.showHelp) {
            std::cout << bhs::commandLineHelp();
            return 0;
        }
        bhs::Application application(std::move(options));
        return application.run();
    } catch (const std::exception& error) {
        std::cerr << "Black Hole Sandbox could not start:\n" << error.what() << '\n';
        return 1;
    }
}
