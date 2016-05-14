#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "filesys/file.h"
static void syscall_handler (struct intr_frame *);
/* System call handler functions(2 defined in syscall.h for further use) */
static void arguments_read(struct intr_frame *,int);
static int write(int,const void *,unsigned int);
static void is_valid_buffer(const void *, int);
static void is_valid_ptr(void * );
static tid_t wait(tid_t);
static tid_t exec(const char *);
static bool create(const char *, int); 
static int read(int,void *,unsigned);
static off_t file_size(int);
static void seek(int, unsigned int);
static unsigned tell(int);
static void close(int);
/*User defined functions and variables */
static void close_all_files(void);
static void remove_self_from_list(void); 
struct lock file_syst;
int args[3];  //Even though this is global,the values in this array are passed to functions for better readability and modularization      
//Size of array is 3 as not more than 3 arguments are ever passed in a system call till now  
/* Initializes system call handler */
void
syscall_init (void) 
{
  lock_init(&file_syst);	
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/*Initializes system calls and dispatches them accordingly to the required function */
static void
syscall_handler (struct intr_frame *f UNUSED) 
{
	is_valid_ptr((void *)f->esp);
	int sys_call = *(int *)f->esp;
	switch (sys_call)
	{
		case SYS_CREATE:
			arguments_read(f,2);
			is_valid_buffer((const void *)args[0],1);
			f->eax = create((const char *)args[0],(unsigned int)args[1]); 
     		break;
		case SYS_OPEN:
			 arguments_read(f,1);
			 is_valid_buffer((const void *)args[0],1);
    		 args[0]  = (int)pagedir_get_page(thread_current()->pagedir,(void *) args[0]); //Conversion takes place before in this case
			 f->eax = open((const char *)args[0]);  // as open() is also called from start_process which should not try to convert 	
			break;								 	//from user to kernel space(file_name argument)
		case SYS_READ:
			arguments_read(f,3);
			is_valid_buffer((const void *)args[1], args[2]);
			f->eax = read(args[0],(void *)args[1], (unsigned)args[2]);
			break;
		case SYS_WRITE:
		    arguments_read(f,3);
			f->eax = write(args[0],(const void *)args[1],(unsigned) args[2]);
			break;
		case SYS_SEEK:
			arguments_read(f,2);
			seek(args[0], (unsigned)args[1]);
			break;
		case SYS_TELL:
			break;
			arguments_read(f,1);
		    f->eax = tell(args[0]);	
		case SYS_CLOSE:
			arguments_read(f,1);
			close(args[0]);
		case SYS_FILESIZE:
			arguments_read(f,1);
			f->eax = file_size(args[0]);
			break;
		case SYS_EXEC:
			arguments_read(f,1);
        	f->eax = exec((const char *)args[0]);
			break;
		case SYS_WAIT:
			arguments_read(f,1);
			f->eax = wait(args[0]);			
			break;
		case SYS_HALT:
			shutdown_power_off();
			break;
		case SYS_EXIT:
			 arguments_read(f,1);
			 sys_exit(args[0]);
		     break;
	} 
}
/* Reads arguments passed by the user program in the system call
 * Stores it in variabe args which is global to all these functions */ 
void arguments_read(struct intr_frame *f,int n)
{

	int *ptr = (int *)f->esp+1;
	for(int i=0;i<n;i++)
    {
		is_valid_ptr(ptr); 	
		args[i] = *ptr++;	
	}
}

/* Creates file of size bytes with name file_name 
 * Returns true if successful, false if not 
 * For all file system calls lock file_syst is used as a sort of
 * single point of entry and exit */  
static bool create(const char *file_name, int size)
{
	file_name = pagedir_get_page(thread_current()->pagedir, file_name);
	bool success;
	if(file_name)
     {
		lock_acquire(&file_syst);
		success =  filesys_create(file_name,size);
		lock_release(&file_syst);
		return success;
	}
	return false;	
}
/* Opens file with name file_name. Returns -1 if error
 * Also stores all the file information in structure file_data which is then pushed into the
 * current thread's file_list */
int open(const char *file_name)
{
	if(file_name)
	{
		lock_acquire(&file_syst);
		struct file *file_ptr = filesys_open (file_name);
		lock_release(&file_syst);
		if(!file_ptr) 						//If error while opening file return error
			return SYS_ERROR;
	   int count = thread_current()->file_count;
	   struct file_data *file_handle = malloc(sizeof(struct file_data));
	   file_handle->file = file_ptr;
	   file_handle->fd = count+2;		//File descriptors 0 and 1 are reserved 
	   list_push_back(&thread_current()->file_list,&file_handle->elem);
	   thread_current()->file_count++;
	   return file_handle->fd;
	}
	return SYS_ERROR;
}
/*Reads size bytes into buffer from file with file_descriptor fd
 * Returns -1 if wrong file descriptor is passed or buffer is out of bounds(Not mapped or above PHYS_BASE)  */
static int read(int fd,void *buffer,unsigned size)
{
	buffer = pagedir_get_page(thread_current()->pagedir, (const void *)buffer);
	if(!buffer)
		return SYS_ERROR;
	if(fd == STDIN_FILENO)					// Read from keyboard input
    	return input_getc();
	struct file *file = NULL;
	file = get_file(fd);
	if(!fd)
		return SYS_ERROR; 			
	lock_acquire(&file_syst);
	int bytes_read = file_read (file, buffer, size);	
	lock_release(&file_syst);
	return bytes_read;	
}
/*Changes the next byte to be read or written in open file fd to position, expressed in bytes from the beginning of the file. */
static void seek(int fd, unsigned int pos)
{
	struct file *file = get_file(fd);
	lock_acquire(&file_syst);
	file_seek(file , pos);	
	lock_release(&file_syst);	
}

/* Gets the file stored in the current thread's list of file_data and returns it.
 * returns NULL if the file is not present */
struct file *get_file(int fd)
{
	struct list_elem *entry;
	struct file_data *handle;
	struct thread *t = thread_current();
	entry = list_begin(&t->file_list);
	while(entry != list_end(&t->file_list))
	{
		handle = list_entry(entry, struct file_data, elem);
		if(handle->fd == fd)
			return handle->file;
		else
			entry = list_next(entry);
	}	
	return NULL;
}
/* Returns the position of the next byte to be read or written in open file fd, expressed in bytes from the beginning of the file. */
static unsigned tell(int fd)
{
	struct file *file = get_file(fd);
	if(fd)
	{
		lock_acquire(&file_syst);
		unsigned offset = file_tell(file);
		lock_release(&file_syst);  
		return offset;
	}
	return SYS_ERROR;
}
/* Returns the size of the file opened with file descriptor fd 
 * Returns -1 if file does not exist */ 
static off_t file_size(int fd)
{
	struct file *file = get_file(fd);
	if(!file)
		return SYS_ERROR;
	lock_acquire(&file_syst);
	off_t size = file_length(file); 
	lock_release(&file_syst);
	return size;		
}
 
/* Writes data to console or to another file depending on the file descriptor 
 * Returns -1 if there are errors while reading(ex EOF) */  
int write(int fd,const void *buffer,unsigned size)
{
	if( fd == STDOUT_FILENO )                //Writes to output console
	{
		putbuf(buffer,size);
		return size;
	}
	else
	{
		is_valid_buffer((const void *)args[1],args[2]);
		buffer = pagedir_get_page (thread_current()->pagedir, buffer);
	    if(buffer)
		{
			struct file *file = get_file(fd);
			if(!file)
				return SYS_ERROR;								//Error finding file
			lock_acquire(&file_syst);
			int bytes_written = file_write (file, buffer, size);
			lock_release(&file_syst);
			return bytes_written; 			
		}
	}
	return SYS_ERROR;
}

/*Closes a file if it is open by the current process mapped with fd */
void close(int fd)
{
	struct list_elem *entry;
    struct file_data *handle;
    struct thread *t = thread_current();
    entry = list_begin(&t->file_list);
    while(entry != list_end(&t->file_list))
    {
        handle = list_entry(entry, struct file_data, elem);
        if(handle->fd == fd)
        {
			file_close (handle->file);
			list_remove(entry);
			free(handle);
			return;	
	    }
        else
            entry = list_next(entry);
    }
}

/*Checks to see if the pointer passed to it points to a valid user address  
 *If not exits with system error */
static void is_valid_ptr(void *ptr)
{
	if(!is_user_vaddr((const void *)ptr))
		sys_exit(SYS_ERROR);; 	
}

/* Checks to see if the buffer+size bytes are mapped or unmapped and below PHYS_BASE or not
 * Exits if it is not using sys_exit */
static void is_valid_buffer(const void *buffer, int size)
{
	for(int i=0;i<size;i++)
	{
		is_valid_ptr((void *)buffer+i);
		if(!pagedir_get_page (thread_current()->pagedir, buffer+i))
			sys_exit(SYS_ERROR);
	}
	
}

/* Executes the arguments specified on the command line
 * Waits till the child process is loaded and returns a value 1 if successful,0 if unsuccessful
 * For synchornization, checks on a variable shared by parent and child is done */
static tid_t exec(const char *cmd_line)
{
   void *cmd = NULL;
   cmd = pagedir_get_page(thread_current()->pagedir, cmd_line);
   if(cmd)
{ 
  int pid = process_execute((const char *)cmd);
	if(pid == SYS_ERROR)
		return SYS_ERROR;
	struct list_elem *entry;
    struct child_data *child;
    entry = list_begin(&thread_current()->child_list);
	while(entry != list_end(&thread_current()->child_list))
	{
	 	child = list_entry(entry, struct child_data, elem);
		if(child->pid == pid)
		{
			while(!child->loaded)         //Wait till signal is received whether child process is loaded or not.
				barrier();				  //Compiler should not optimize here or else infinite loop will occur 		  		
			if(child->load_value == 0)	  //If loaded successfully return child's pid
				return child->pid;			
			else						  //else return -1
		   {
			list_remove(entry);
			return SYS_ERROR;
		   }
		}
		else entry = list_next(entry);
	}
 }
	return SYS_ERROR;
}
/* Waits for a process with pid pid to exit before resuming.
 * process_wait has more info about the exact working
 */ 
static int wait(tid_t pid)
{
	return process_wait(pid);
}

/* Exits when the user program needs to exit or some fault has occurred
 * Also called from the execption handler for bad memory access or other errors from  user programs
 * Releases its locks, closes its files and signals to the parent that it has exited( Sema_up() )    
 */
void sys_exit(int exit_value)
{
	struct child_data *c = thread_current()->my_data;
    c->exit_status = exit_value;	
	close_all_files();
	if(!c->wait)											 //If wait has not been called on this process free memory here itself
		remove_self_from_list();
    printf("%s: exit(%d)\n",thread_current()->name, exit_value);
	if(lock_held_by_current_thread (&file_syst))			//Release file system lock if held and terminated by the
		lock_release(&file_syst);							//kernel due to an error
	if(is_thread(thread_current()->parent) && c->wait)		//If parent still exists and wait has been called
		sema_up(&thread_current()->parent->wait_list);		//remove self from that list  
	thread_exit();
}

/* Closes all files open by the process, including its own.  
 * Simply traverses its file list, closes them and removes all files from its list */ 
static void close_all_files(void)
{
	struct list_elem *entry;
	struct thread *t = thread_current();
	struct file_data *handle;
	entry = list_begin(&t->file_list);
	while(entry != list_end(&t->file_list))
     {
         handle = list_entry(entry, struct file_data, elem);
         file_close (handle->file);
         entry = list_remove(entry);
         free(handle);
     }
}

/*Removes self from parents list.
 * If wait has been called for a child process then it is removed from the list from process_wait itself
 * Not freeing child_data if wait is called as parent process still needs data(exit value etc) about the child  */
void remove_self_from_list(void)
{
   struct thread *t = thread_current()->parent;
   if(!list_empty(&t->child_list))
   {
     struct list_elem *entry;
     struct child_data *child;
     entry = list_begin(&t->child_list);
     while(entry != list_end(&t->child_list))             //Finds itself in parents'list and removes itself 
     {                                              
         child = list_entry(entry, struct child_data, elem);    
         if(child->pid == t->tid)                               
         {
             list_remove(entry);
             free(child);
             return;
         }
         else
             entry = list_next(entry);
     }
  }
} 
