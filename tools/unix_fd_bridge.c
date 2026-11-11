#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
struct Header { _Atomic uint32_t magic,pid,request; _Atomic int32_t fd; _Atomic uint32_t response; _Atomic int32_t result,after; _Atomic uint32_t quit; };
_Static_assert(sizeof(struct Header)==32,"IPC layout");
static char *target,*mapping;
// A dead helper may leave its bridge file behind. Reclaim only its verified
// private inode; a live owner (including a reused PID) is never displaced.
static int retire_dead_bridge(const char *path){
 int fd=open(path,O_RDWR|O_CLOEXEC|O_NOFOLLOW);if(fd<0)return 0;
 struct stat held,current;int retired=0;
 if(fstat(fd,&held)==0 && S_ISREG(held.st_mode) && held.st_uid==getuid() &&
    (held.st_mode&077)==0 && held.st_size==4096){
  struct Header *h=mmap(NULL,4096,PROT_READ,MAP_SHARED,fd,0);
  if(h!=MAP_FAILED){
   uint32_t magic=atomic_load_explicit(&h->magic,memory_order_acquire),pid=atomic_load(&h->pid);
   if(magic==0x42524731 && pid>0 && pid<=INT32_MAX && kill((pid_t)pid,0)<0 && errno==ESRCH &&
      lstat(path,&current)==0 && current.st_dev==held.st_dev && current.st_ino==held.st_ino)
    retired=unlink(path)==0;
   munmap(h,4096);
  }
 }
 close(fd);return retired;
}
static int is_helper(void){
 FILE *f=fopen("/proc/self/maps","r");if(!f)return 0;
 char line[8192];int match=0;while(fgets(line,sizeof(line),f))if(strstr(line,target)){match=1;break;}fclose(f);return match;
}
static void* serve(void* unused){
 (void)unused;
 int matched=0;for(int i=0;i<2000;i++){if(is_helper()){matched=1;break;}usleep(10000);}if(!matched)return NULL;
 int control=open(mapping,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
 if(control<0 && errno==EEXIST && retire_dead_bridge(mapping))
  control=open(mapping,O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
 if(control<0)return NULL;
 if(ftruncate(control,4096)){close(control);unlink(mapping);return NULL;}
 struct Header *h=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,control,0);
 if(h==MAP_FAILED){close(control);unlink(mapping);return NULL;}
 atomic_store(&h->pid,getpid());atomic_store_explicit(&h->magic,0x42524731,memory_order_release);
 uint32_t seen=0;
 while(!atomic_load_explicit(&h->quit,memory_order_acquire)){
  uint32_t seq=atomic_load_explicit(&h->request,memory_order_acquire);
  if(seq!=seen){
   int fd=atomic_load(&h->fd),result=-EINVAL,after=0;
   if(fd>=0&&fd!=control){result=close(fd);if(result<0)result=-errno;after=fcntl(fd,F_GETFD);if(after<0)after=-errno;}
   atomic_store(&h->result,result);atomic_store(&h->after,after);seen=seq;atomic_store_explicit(&h->response,seq,memory_order_release);
  } else usleep(1000);
 }
 munmap(h,4096);close(control);unlink(mapping);return NULL;
}
__attribute__((constructor)) static void init(void){
 const char *exe=getenv("DLSSNR_UNIX_BRIDGE_EXE"),*shm=getenv("DLSSNR_SHM");
 if(!exe||!shm||shm[0]!='/')return;
 const char *id=getenv("DLSSNR_UNIX_BRIDGE_ID");
 target=strdup(exe);if(asprintf(&mapping,"%s.unix%s%s",shm,id?".":"",id?id:"")<0)return;
 pthread_t thread;if(!pthread_create(&thread,NULL,serve,NULL))pthread_detach(thread);
}
