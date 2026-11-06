#include "cloudiemail/outbound.hpp"
#include <iostream>
int main(int argc, char **argv) {
    try {
        cm::crypto_init();
        if (argc != 3 && argc != 5)
            return 2;
        cm::QueuedMail mail;
        mail.sender = "alice@example.test";
        mail.recipient = "bob@example.org";
        std::atomic_bool stop = false;
        auto result =
            cm::smtp_deliver_to(argc == 5 ? argv[3] : "127.0.0.1",
                                static_cast<unsigned short>(std::stoul(argv[1])), "mail.example.test", mail,
                                "From: alice@example.test\r\nTo: bob@example.org\r\nSubject: "
                                "transport test\r\n\r\nhello\r\n.dot\r\n",
                                stop, std::string(argv[2]) == "allow-loopback-test",
                                argc == 5 && std::string(argv[4]) == "allow-ipv6");
        std::cout << cm::Json({{"success", result.success},
                               {"permanent", result.permanent},
                               {"error", result.error}})
                         .dump()
                  << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what();
        return 1;
    }
}
