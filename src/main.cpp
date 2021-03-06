#include <csignal>
#include <cxxopts.hpp>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sysexits.h>
#include <unistd.h>

#include "Modbus_TCP_Slave.hpp"
#include "modbus_shm.hpp"

//! terminate flag
static volatile bool terminate = false;

//! modbus socket (to be closed if termination is requested)
static int socket = -1;

/*! \brief signal handler (SIGINT and SIGTERM)
 *
 */
static void sig_term_handler(int) {
    if (socket != -1) close(socket);
    terminate = true;
    alarm(1);  // force termination after 1s
}

/*! \brief main function
 *
 * @param argc number of arguments
 * @param argv arguments as char* array
 * @return exit code
 */
int main(int argc, char **argv) {
    const std::string exe_name = std::filesystem::path(argv[0]).filename().string();
    cxxopts::Options  options(exe_name, "Modbus client that uses shared memory objects to store its register values");

    auto exit_usage = [&exe_name]() {
        std::cerr << "Use '" << exe_name << " --help' for more information." << std::endl;
        exit(EX_USAGE);
    };

    auto euid = geteuid();
    if (!euid) std::cerr << "!!!! WARNING: You should not execute this program with root privileges !!!!" << std::endl;

    // establish signal handler
    if (signal(SIGINT, sig_term_handler) || signal(SIGTERM, sig_term_handler)) {
        perror("Failed to establish signal handler");
        exit(EX_OSERR);
    }

    if (signal(SIGALRM, [](int) { exit(EX_OK); })) {
        perror("Failed to establish signal handler");
        exit(EX_OSERR);
    }

    // all command line arguments
    // clang-format off
    options.add_options()("i,ip",
                          "ip to listen for incoming connections",
                          cxxopts::value<std::string>()->default_value("0.0.0.0"))
                         ("p,port",
                          "port to listen for incoming connections",
                          cxxopts::value<std::uint16_t>()->default_value("502"))
                         ("n,name-prefix",
                          "shared memory name prefix",
                          cxxopts::value<std::string>()->default_value("modbus_"))
                         ("do-registers",
                          "number of digital output registers",
                          cxxopts::value<std::size_t>()->default_value("65536"))
                         ("di-registers",
                          "number of digital input registers",
                          cxxopts::value<std::size_t>()->default_value("65536"))
                         ("ao-registers",
                          "number of analog output registers",
                          cxxopts::value<std::size_t>()->default_value("65536"))
                         ("ai-registers",
                          "number of analog input registers",
                          cxxopts::value<std::size_t>()->default_value("65536"))
                         ("m,monitor",
                          "output all incoming and outgoing packets to stdout")
                         ("r,reconnect",
                          "do not terminate if Master disconnects.")
                         ("h,help",
                          "print usage");
    // clang-format on

    // parse arguments
    cxxopts::ParseResult args;
    try {
        args = options.parse(argc, argv);
    } catch (cxxopts::OptionParseException &e) {
        std::cerr << "Failed to parse arguments: " << e.what() << '.' << std::endl;
        exit_usage();
    }

    // print usage
    if (args.count("help")) {
        options.set_width(120);
        std::cout << options.help() << std::endl;
        std::cout << std::endl;
        std::cout << "The modbus registers are mapped to shared memory objects:" << std::endl;
        std::cout << "    type | name                      | master-access   | shm name" << std::endl;
        std::cout << "    -----|---------------------------|-----------------|----------------" << std::endl;
        std::cout << "    DO   | Discrete Output Coils     | read-write      | <name-prefix>DO" << std::endl;
        std::cout << "    DI   | Discrete Input Coils      | read-only       | <name-prefix>DI" << std::endl;
        std::cout << "    AO   | Discrete Output Registers | read-write      | <name-prefix>AO" << std::endl;
        std::cout << "    AI   | Discrete Input Registers  | read-only       | <name-prefix>AI" << std::endl;
        std::cout << std::endl;
        std::cout << "This application uses the following libraries:" << std::endl;
        std::cout << "  - cxxopts by jarro2783 (https://github.com/jarro2783/cxxopts)" << std::endl;
        std::cout << "  - libmodbus by St??phane Raimbault (https://github.com/stephane/libmodbus)" << std::endl;
        std::cout << std::endl;
        std::cout << std::endl;
        std::cout << "MIT License:" << std::endl;
        std::cout << std::endl;
        std::cout << "Copyright (c) 2021 Nikolas Koesling" << std::endl;
        std::cout << std::endl;
        std::cout << "Permission is hereby granted, free of charge, to any person obtaining a copy" << std::endl;
        std::cout << "of this software and associated documentation files (the \"Software\"), to deal" << std::endl;
        std::cout << "in the Software without restriction, including without limitation the rights" << std::endl;
        std::cout << "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell" << std::endl;
        std::cout << "copies of the Software, and to permit persons to whom the Software is" << std::endl;
        std::cout << "furnished to do so, subject to the following conditions:" << std::endl;
        std::cout << std::endl;
        std::cout << "The above copyright notice and this permission notice shall be included in all" << std::endl;
        std::cout << "copies or substantial portions of the Software." << std::endl;
        std::cout << std::endl;
        std::cout << "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR" << std::endl;
        std::cout << "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY," << std::endl;
        std::cout << "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE" << std::endl;
        std::cout << "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER" << std::endl;
        std::cout << "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM," << std::endl;
        std::cout << "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE" << std::endl;
        std::cout << "SOFTWARE." << std::endl;
        exit(EX_OK);
    }

    // check arguments
    if (args["do-registers"].as<std::size_t>() > 0x10000) {
        std::cerr << "to many do_registers (maximum: 65536)." << std::endl;
        exit_usage();
    }

    if (args["di-registers"].as<std::size_t>() > 0x10000) {
        std::cerr << "to many do_registers (maximum: 65536)." << std::endl;
        exit_usage();
    }

    if (args["ao-registers"].as<std::size_t>() > 0x10000) {
        std::cerr << "to many do_registers (maximum: 65536)." << std::endl;
        exit_usage();
    }

    if (args["ai-registers"].as<std::size_t>() > 0x10000) {
        std::cerr << "to many do_registers (maximum: 65536)." << std::endl;
        exit_usage();
    }

    // create shared memory object for modbus registers
    Modbus::shm::Shm_Mapping mapping(args["do-registers"].as<std::size_t>(),
                                     args["di-registers"].as<std::size_t>(),
                                     args["ao-registers"].as<std::size_t>(),
                                     args["ai-registers"].as<std::size_t>(),
                                     args["name-prefix"].as<std::string>());

    // create slave
    std::unique_ptr<Modbus::TCP::Slave> slave;
    try {
        slave = std::make_unique<Modbus::TCP::Slave>(
                args["ip"].as<std::string>(), args["port"].as<uint16_t>(), mapping.get_mapping());
        slave->set_debug(args.count("monitor"));
    } catch (const std::runtime_error &e) {
        std::cerr << e.what() << std::endl;
        exit(EX_SOFTWARE);
    }
    socket = slave->get_socket();

    // connection loop
    do {
        // connect client
        std::cerr << "Waiting for Master to establish a connection..." << std::endl;
        try {
            slave->connect_client();
        } catch (const std::runtime_error &e) {
            if (!terminate) {
                std::cerr << e.what() << std::endl;
                exit(EX_SOFTWARE);
            }
            break;
        }

        std::cerr << "Master established connection." << std::endl;

        // ========== MAIN LOOP ========== (handle requests)
        bool connection_closed = false;
        while (!terminate && !connection_closed) {
            try {
                connection_closed = slave->handle_request();
            } catch (const std::runtime_error &e) {
                // clang-tidy (LLVM 12.0.1) warning "Condition is always true" is not correct
                if (!terminate) std::cerr << e.what() << std::endl;
                break;
            }
        }

        if (connection_closed) std::cerr << "Master closed connection." << std::endl;
    } while (args.count("reconnect"));

    std::cerr << "Terminating..." << std::endl;
}
