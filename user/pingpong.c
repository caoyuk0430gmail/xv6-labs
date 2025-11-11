#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
    // we create two fds, parent2child and child2parent
    int parent2child[2];
    int child2parent[2];
    // buf is also copied by fork
    char buf;

    // create pipe
    if (pipe(parent2child) < 0 || pipe(child2parent) < 0) {
        printf("pipe failed\n");
        exit(1);
    }
    // fork will copy the parent2child and child2parent fds also to the child process
    // we need to close the unused fds from both process
    int pid = fork();
    if (pid == 0) {
        // child process ONLY uses parent2child[0] to read and child2parent[1] to write
        close(parent2child[1]);
        close(child2parent[0]);
        
        if (read(parent2child[0], &buf, 1) != 1) {
            printf("pipe read from parent failed\n");
            exit(1);
        }
        printf("%d: received ping\n", getpid());
        if (write(child2parent[1], &buf, 1) != 1) {
            printf("pipe write to parent failed\n");
            exit(1);
        }
        // finish, close the pipe
        close(parent2child[0]);
        close(child2parent[1]);
        exit(0);
    } else {
        // parent process ONLY uses parent2child[1] to write and child2parent[0] to read
        close(parent2child[0]);
        close(child2parent[1]);
        //sent one byte
        buf = 'A';
        if (write(parent2child[1], &buf, 1) != 1) {
            printf("pipe write to child failed\n");
            exit(1);
        }
        // read from child
        if (read(child2parent[0], &buf, 1) != 1) {
            printf("pipe read from parent failed\n");
            exit(1);
        }
        printf("%d: received pong\n", getpid());
        wait(0);
        // finish, close the pipe
        close(parent2child[1]);
        close(child2parent[0]);
        exit(0);
    }
}