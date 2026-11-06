// Standalone diagnostic: deliberately does not link CloudieMail code.
// Build with -fsanitize=address and the official Firebird runtime to investigate LSan output.
#include <cstdio>
#include <ibase.h>

int main(int argc, char **argv) {
    if (argc != 2)
        return 2;
    isc_db_handle database = 0;
    ISC_STATUS status[20]{};
    const char dpb[] = {isc_dpb_version1, isc_dpb_user_name, 6, 'S', 'Y', 'S', 'D', 'B', 'A'};
    auto result = isc_create_database(status, 0, argv[1], &database, sizeof(dpb), dpb, 0);
    if (result) {
        isc_print_status(status);
        return 1;
    }
    result = isc_drop_database(status, &database);
    fb_shutdown(5000, fb_shutrsn_app_stopped);
    return result ? 1 : 0;
}
