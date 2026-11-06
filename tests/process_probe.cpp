#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--wait") {
        std::this_thread::sleep_for(std::chrono::minutes(1));
        return 0;
    }
    if (std::getenv("LEGO_TLS_SKIP_VERIFY"))
        return 4;
    if (argc != 3 || std::string(argv[2]) != "argument with spaces and \"quotes\"")
        return 5;
    std::cout << "child ready\n" << std::flush;
    std::string line;
    std::getline(std::cin, line);
    std::cout << "child done\n";
    return 0;
}
