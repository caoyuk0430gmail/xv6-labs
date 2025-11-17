#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

#define MAX_LINE_LEN 512

int
main(int argc, char *argv[])
{
  // 1. PREPARE THE BASE COMMAND
  // -------------------------------------------------------------------
  // The command to execute is given as arguments to xargs itself.
  // We copy these arguments, skipping argv[0] which is "xargs".
  // This will form the base of the command we execute in the child.
  
  char *command = argv[1];      // The program to execute (e.g., "echo")
  char *exec_argv[MAXARG];      // The argument list for exec()

  // We need at least a command to run.
  if(argc < 2){
    fprintf(2, "Usage: xargs <command> [args...]\n");
    exit(1);
  }

  // Copy the command and its initial arguments from argv.
  // Example: if command is "xargs echo bye", this copies "echo" and "bye".
  // The last slot in exec_argv will be used for the line from stdin.
  // important to copy also argv[0] into exec_argv, as exec() later will need the name of the program
  for (int i = 1; i < argc; i++) {
    exec_argv[i - 1] = argv[i];
  }


  // 2. READ LINES FROM STDIN AND EXECUTE
  // -------------------------------------------------------------------
  char line_buf[MAX_LINE_LEN];
  char c;
  int current_pos = 0;

  // Read one character at a time from standard input (fd 0).
  // The loop ends when read() returns 0, indicating End-Of-File.
  while(read(0, &c, 1) > 0) {
    if (c == '\n') {
      // A full line has been read. Time to execute the command.
      
      line_buf[current_pos] = '\0'; // Null-terminate the line to make it a valid string.

      // The line we just read from stdin becomes the last argument.
      // For "xargs echo bye", argc is 3. The base command "echo bye"
      // is at exec_argv[0] and exec_argv[1]. The new argument
      // goes at exec_argv[3-1] = exec_argv[2].
      exec_argv[argc - 1] = line_buf;
      
      // The argument list for exec must be terminated by a null pointer.
      exec_argv[argc] = 0;

      // Fork a child process to run the command.
      if (fork() == 0) {
        // --- Child Process ---
        exec(command, exec_argv);
        // If exec fails, it returns. We must print an error and exit.
        fprintf(2, "xargs: exec %s failed\n", command);
        exit(1);
      } else {
        // --- Parent Process ---
        // Wait for the child to finish before processing the next line.
        wait(0);
      }
      
      // Reset the buffer position for the next line.
      current_pos = 0;

    } else {
      // Not a newline, so add the character to our line buffer.
      // Includes a check to prevent buffer overflow.
      if (current_pos < MAX_LINE_LEN - 1) {
        line_buf[current_pos] = c;
        current_pos++;
      }
    }
  }

  exit(0);
}