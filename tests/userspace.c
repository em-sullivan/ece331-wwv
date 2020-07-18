/*
 * Eric Sullivan
 * 03/27/2020
 * Test 1 for wwv kernel drive:
 * gets current UTC date and time and
 * sends it to driver
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include "../wwv.h"

int main (int argc, char *argv[])
{
    int fd;
    time_t t;
    struct tm *utc;
    pid_t pid;

    // Gets the current time
    t = time(NULL);
    utc = gmtime(&t); 
    utc->tm_yday = utc->tm_yday + 1;
    
    // Prints UTC date and time to screen
    printf("Year %d DoY %d ", utc->tm_year + 1900, utc->tm_yday);
    printf("Hour %d Minute %d\n", utc->tm_hour, utc->tm_min);
    
    fd = open("/dev/wwv", O_WRONLY);
    if (fd < 0) {
        printf("Cannot open wwv\n");
        return 1;
    }

    pid = fork();

    // Parent Process
    if (pid > 0) {
        if (ioctl(fd, WWV_TRANSMIT, utc) < 0) {
            printf("Error! Could not acces driver!\n");
            close(fd);
            return 1;
        }

    // Child Process
    } else if (pid == 0) {
        if (ioctl(fd, WWV_TRANSMIT, utc) < 0) {
            printf("Error! Could not acces driver!\n");
            close(fd);
            return 1;
        }
    
    // Error if forking fails
    } else {
        perror("fork() failure\n");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

