/* Minimal ptracer: exec a (glibc) child, wait for the seccomp SIGSYS
 * signal-delivery-stop, read x8 (the attempted syscall number) from the
 * tracee's registers. Bionic-compiled; ptrace is kernel-level so tracing a
 * glibc tracee is fine. Avoids strace's per-syscall machinery (which crashes
 * on this glibc binary under Android). */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <signal.h>

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

struct aarch64_regs {
    unsigned long long regs[31];
    unsigned long long sp;
    unsigned long long pc;
    unsigned long long pstate;
};

int main(int argc, char** argv, char** envp){
    if(argc<2){ fprintf(stderr,"usage: %s <prog> [args...]\n",argv[0]); return 2; }
    pid_t pid=fork();
    if(pid==0){
        ptrace(PTRACE_TRACEME,0,0,0);
        execve(argv[1], &argv[1], envp);
        perror("execve");
        _exit(127);
    }
    int status;
    waitpid(pid,&status,0);                 /* initial exec stop */
    ptrace(PTRACE_SETOPTIONS,pid,0,(void*)PTRACE_O_EXITKILL);
    ptrace(PTRACE_CONT,pid,0,0);
    while(waitpid(pid,&status,0)==pid){
        if(WIFEXITED(status)){ fprintf(stderr,"[tracer] child exited %d\n",WEXITSTATUS(status)); break; }
        if(WIFSIGNALED(status)){ fprintf(stderr,"[tracer] child killed by signal %d\n",WTERMSIG(status)); break; }
        if(WIFSTOPPED(status)){
            int sig=WSTOPSIG(status);
            if(sig==SIGSYS){
                struct aarch64_regs r; memset(&r,0,sizeof r);
                struct iovec io={ &r, sizeof r };
                if(ptrace(PTRACE_GETREGSET,pid,(void*)NT_PRSTATUS,&io)==0){
                    unsigned long long pc=r.pc;
                    fprintf(stderr,"*** SIGSYS trapped: syscall x8=%llu  pc=0x%llx\n",r.regs[8],pc);
                    fprintf(stderr,"    x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n",
                            r.regs[0],r.regs[1],r.regs[2],r.regs[3]);
                    /* map pc -> module + file offset via /proc/pid/maps */
                    char mp[64]; snprintf(mp,sizeof mp,"/proc/%d/maps",pid);
                    FILE* m=fopen(mp,"r"); char line[512];
                    while(m && fgets(line,sizeof line,m)){
                        unsigned long long lo,hi,foff; char perms[8],path[400]={0};
                        if(sscanf(line,"%llx-%llx %7s %llx %*s %*s %399[^\n]",&lo,&hi,perms,&foff,path)>=4){
                            if(pc>=lo && pc<hi){
                                fprintf(stderr,"    module: %s\n",path);
                                fprintf(stderr,"    file offset of svc = 0x%llx\n", pc - lo + foff);
                                break;
                            }
                        }
                    }
                    if(m) fclose(m);
                } else perror("PTRACE_GETREGSET");
                ptrace(PTRACE_KILL,pid,0,0);
                break;
            }
            /* forward non-stop signals so startup proceeds normally */
            int fwd=(sig==SIGTRAP||sig==SIGSTOP)?0:sig;
            ptrace(PTRACE_CONT,pid,0,(void*)(long)fwd);
        }
    }
    return 0;
}
