 #include <stdio.h>
 #include <syscall.h>
 
 
int
 main (int argc UNUSED, char *argv[])
 {
	char block[512];
	int size = sizeof(block);
	 int fd = open("sample.txt"); 
	 read(fd, block, size);
	 return 0;
 }  

