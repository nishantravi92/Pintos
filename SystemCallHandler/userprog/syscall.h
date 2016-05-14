#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#define SYS_ERROR -1
void syscall_init (void);
int open(const char *);
struct file *get_file(int);
void sys_exit(int);
#endif /* userprog/syscall.h */
