#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

// we dont need fmtname from ls.c, we just use de.name to fetch filename
// de vs st: You read a directory to get a list of dirents (names and numbers).
// For each dirent, you use its name to construct a full path and call stat to get the stat struct (the full details)
void
find(char *path, char *filename) {
    char buf[512], *p;
    int fd;
    struct dirent de;
    struct stat st;

    if((fd = open(path, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
    }

    if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
    }

    switch(st.type){
    case T_FILE:
        if (strcmp(filename, de.name) == 0) {
            printf("%s/%s\n", path, filename);
        }
        break;
    case T_DIR:
        if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
            printf("ls: path too long\n");
            break;
            }
        strcpy(buf, path);
        p = buf+strlen(buf);
        *p++ = '/';
        // read(fd, &de, sizeof(de)): Reads one directory entry from the open directory file (fd) into the de struct.
        while(read(fd, &de, sizeof(de)) == sizeof(de)){
            if(de.inum == 0)
                continue;
            // skip if it's . or ..
            if (strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0) {
                continue;
            }
            // Copies the filename from the directory entry (de.name) into our buffer (buf) right after the /. buf now contains the full path to the file (e.g., "./user").
            memmove(p, de.name, DIRSIZ);
            p[DIRSIZ] = 0;
            // We now have the full path to a file inside the directory (e.g., "./user"). We must call stat on this full path string to get its metadata (type, size, etc.).
            if(stat(buf, &st) < 0){
                printf("ls: cannot stat %s\n", buf);
                continue;
            }
            if (st.type == T_FILE) { 
                // check if the filename matches
                if (strcmp(filename, de.name) == 0) {
                    printf("%s/%s\n", path, filename);
                }
            } else if (st.type == T_DIR) { 
                find(buf, filename);
            }
        }
        break;
    }
    close(fd);
}


int
main(int argc, char *argv[])
{

    if(argc < 3){
        fprintf(2, "Please enter a dir and a filename!\n");
        exit(0);
    }
    char *path = argv[1];
    char *filename = argv[2];
    find(path, filename);
    exit(0);
}