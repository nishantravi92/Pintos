/* This program attempts to write to memory at an address that is not mapped.
   This should terminate the process with a -1 exit code. */

#include <stdio.h>
#include <syscall.h>

int
 main (int argc, char **argv)
{
  *(int *)NULL = 42;
  printf("should have exited with -1");
}
