#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>

int main(int argc, char *argv[]) {
    openlog("writer", LOG_PID | LOG_CONS, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Invalid number of arguments");
        closelog();
        return 1;
    }

    const char *path = argv[1];
    const char *text = argv[2];

    syslog(LOG_DEBUG, "Writing %s to %s", text, path);

    FILE *file = fopen(path, "w");

    if (file == NULL) {
        syslog(LOG_ERR, "Failed to open file %s", path);
        closelog();
        return 1;
    }

    if (fprintf(file, "%s\n", text) < 0) {
        syslog(LOG_ERR, "Failed to write to file %s", path);
        fclose(file);
        closelog();
        return 1;
    }

    fclose(file);
    closelog();

    return 0;
}