#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

int main(int argc, char *argv[])
{
    FILE *file;

    openlog("writer", LOG_PID, LOG_USER);

    if (argc != 3) {
        syslog(LOG_ERR, "Expected 2 arguments: writefile writestr");
        closelog();
        return 1;
    }

    syslog(LOG_DEBUG, "Writing %s to %s", argv[2], argv[1]);

    file = fopen(argv[1], "w");
    if (file == NULL) {
        syslog(LOG_ERR, "Failed to open %s: %s", argv[1], strerror(errno));
        closelog();
        return 1;
    }

    if (fprintf(file, "%s", argv[2]) < 0) {
        syslog(LOG_ERR, "Failed to write %s: %s", argv[1], strerror(errno));
        fclose(file);
        closelog();
        return 1;
    }

    if (fclose(file) != 0) {
        syslog(LOG_ERR, "Failed to close %s: %s", argv[1], strerror(errno));
        closelog();
        return 1;
    }

    closelog();
    return 0;
}
