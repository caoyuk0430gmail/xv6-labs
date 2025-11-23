#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

// de vs st: You read a directory to get a list of dirents (names and numbers).
// For each dirent, you use its name to construct a full path and call stat to get the stat struct (the full details)
char*
fmtname(char *path)
{
  static char buf[DIRSIZ+1];
  char *p;

  // Find first character after last slash.
  // e.g. path is "a/b/c.txt", p will point to "c"
  for(p=path+strlen(path); p >= path && *p != '/'; p--)
    ;
  p++;

  // Return blank-padded name.
  // If the filename is too long, give up on formatting and just return the filename as-is.
  if(strlen(p) >= DIRSIZ)
    return p;
  // if it's short enough, copy it to a buffer and fill the rest of the buffer with spaces ('blanks') to make it a fixed width. This makes the output of ls line up nicely in colums
  // This line copies the filename from its original location into our clean, static buffer buf. memmove(destination, source, number_of_bytes)
  memmove(buf, p, strlen(p));
  // This line fills the remaining space in the buffer with space characters ('blanks'). memset(start_address, value, number_of_bytes)
  memset(buf+strlen(p), ' ', DIRSIZ-strlen(p));
  // e.g. filename is cat, buf: [ 'c', 'a', 't', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ? ]
  return buf;
}

void
ls(char *path)
{
  char buf[512], *p;
  int fd;
  // struct dirent {
  //   ushort inum;         // The inode number for this file
  //   char name[DIRSIZ];   // The filename
  // };
  struct dirent de;
  struct stat st;

  // Opens the given path. In xv6, the second argument 0 means open for reading (O_RDONLY).
  if((fd = open(path, 0)) < 0){
    fprintf(2, "ls: cannot open %s\n", path);
    return;
  }
  // fstat is a system call that retrieves metadata (the "stat") for an open file descriptor (fd). It fills the st struct with this informatio
  if(fstat(fd, &st) < 0){
    fprintf(2, "ls: cannot stat %s\n", path);
    close(fd);
    return;
  }

  switch(st.type){
  case T_FILE:
    printf("%s %d %d %l\n", fmtname(path), st.type, st.ino, st.size);
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
      // Copies the filename from the directory entry (de.name) into our buffer (buf) right after the /. buf now contains the full path to the file (e.g., "./user").
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      if(stat(buf, &st) < 0){
        printf("ls: cannot stat %s\n", buf);
        continue;
      }
      printf("%s %d %d %d\n", fmtname(buf), st.type, st.ino, st.size);
    }
    break;
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  int i;

  if(argc < 2){
    ls(".");
    exit(0);
  }
  for(i=1; i<argc; i++)
    ls(argv[i]);
  exit(0);
}
