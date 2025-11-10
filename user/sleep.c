#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// sleep should pause for a user-specified number of ticks.
// A tick is a notion of time defined by the xv6 kernel,
// namely the time between two interrupts from the timer chip, 10ms
int
main(int argc, char *argv[])
{
    if(argc < 2){
        fprintf(2, "Usage: sleep arg...\n");
        exit(1);
    }
    // ticks to be slept
    int ticks = atoi(argv[1]);
    // to sleep in sec, we do sleep(ticks * 100);
    sleep(ticks);
    exit(0);
}