#include "../tools/unix_fd_bridge.c"
#include <assert.h>
#include <sys/wait.h>

int main(void) {
 char directory[]="/tmp/dlss-shim-test-XXXXXX";assert(mkdtemp(directory));
 char path[512],link[512];snprintf(path,sizeof(path),"%s/bridge",directory);snprintf(link,sizeof(link),"%s/link",directory);
 int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);assert(fd>=0 && ftruncate(fd,4096)==0);
 struct Header *h=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);assert(h!=MAP_FAILED);
 atomic_store(&h->magic,0x42524731);atomic_store(&h->pid,getpid());
 assert(!retire_dead_bridge(path)); // A live owner must retain its bridge.
 pid_t child=fork();assert(child>=0);if(!child)_exit(0);assert(waitpid(child,NULL,0)==child);
 atomic_store(&h->pid,(uint32_t)child);
 assert(symlink(path,link)==0);assert(!retire_dead_bridge(link));assert(unlink(link)==0);
 assert(fchmod(fd,0644)==0);assert(!retire_dead_bridge(path));assert(fchmod(fd,0600)==0);
 atomic_store(&h->magic,0);assert(!retire_dead_bridge(path));atomic_store(&h->magic,0x42524731);
 assert(retire_dead_bridge(path));assert(access(path,F_OK)<0 && errno==ENOENT);
 munmap(h,4096);close(fd);assert(rmdir(directory)==0);
 puts("PASS: dead bridge reclaimed; live owner, symlink, public file and invalid header preserved");
}
