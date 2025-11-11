#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
// The first process feeds the numbers 2 through 35 into the pipeline. For each prime number, you will arrange to create one process that reads from its left neighbor over a pipe and writes to its right neighbor over another pipe.
// we need recursive call on func primes() to create the processes in the pipeline only as they are needed, meaning can not be divided.


int primes(int *fdleft) {
    // the fdleft is for the newly created pipe to read
    close(fdleft[1]);
    int base;
    int n = read(fdleft[0], &base, sizeof(base));
    if (n == 0) {
        // no more numbers, just exit gracefully
        exit(0);
    }
    if (n < 0) {
        printf("pipe read error\n");
        exit(1);
    }
    if (n != sizeof(base)) {
        printf("unexpected read size\n");
        exit(1);
    }
    // once we read the number, it is the 1st time and it is the base number for this pipe
    printf("prime %d\n", base);
    // we need to create pipe and fork now, so the fdright need
    // 1. parent process check if it is new prime, if so it writes to fdright 2. child process wait at fdright to read
    int fdright[2];
    pipe(fdright);
    if (fork() == 0) {
        // child process needs to read only
        close(fdright[1]);
        // fdleft is useless for child process, close it. fdleft[1] already closed in the beginning.
        close(fdleft[0]);
        primes(fdright);
        exit(0);
    } else {
        // parent process
        close(fdright[0]);
        // read from fdleft[0] and if primes, write to fdright[1]
        int num;
        // parent need to keep reading, and concurrently child will proceed
        while(read(fdleft[0], &num, sizeof(num)) == sizeof(num)) {
            if (num % base != 0 ) {
                // only write if needed
                if (write(fdright[1], &num, sizeof(num)) != sizeof(num)) {
                    printf("pipe write failed\n");
                    exit(1);
                }
            }
        }
        // finish all read and write
        close(fdleft[0]);
        close(fdright[1]);
        wait(0);
        exit(0);
    }
}

int
main(int argc, char *argv[])
{
    int fd[2];
    int start = 2;
    int end = 35;
    pipe(fd);
    if (fork() == 0) {
        primes(fd);
        exit(0);
    } else {
        // parent starts reading numbers from 2 to 35
        close(fd[0]);
        for (int i = start; i <= end; i++) {
            if (write(fd[1], &i, sizeof(i)) != sizeof(i)) {
                printf("top pipe write failed\n");
                exit(1);
            }
        }
        close(fd[1]);
        wait(0);
        exit(0);
    }
}