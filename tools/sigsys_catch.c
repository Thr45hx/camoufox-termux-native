/* Freestanding LD_PRELOAD SIGSYS catcher — no libc, raw aarch64 syscalls.
 * Loads into a glibc process; installs a process-wide SIGSYS handler in a
 * constructor. On a seccomp trap it hex-dumps the siginfo (so si_syscall at
 * offset 0x18 is readable regardless of struct guesses) and exit_group(42). */

static long sys3(long n, long a, long b, long c){
    register long x8 asm("x8")=n, x0 asm("x0")=a, x1 asm("x1")=b, x2 asm("x2")=c;
    asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2):"memory","cc");
    return x0;
}
static long sys4(long n, long a, long b, long c, long d){
    register long x8 asm("x8")=n, x0 asm("x0")=a, x1 asm("x1")=b, x2 asm("x2")=c, x3 asm("x3")=d;
    asm volatile("svc 0":"+r"(x0):"r"(x8),"r"(x1),"r"(x2),"r"(x3):"memory","cc");
    return x0;
}

#define SYS_write        64
#define SYS_rt_sigaction 134
#define SYS_exit_group   94

static void emit(const char*s,long len){ sys3(SYS_write,2,(long)s,len); }
static char hx(int v){ v&=0xf; return v<10 ? '0'+v : 'a'+v-10; }

/* SA_SIGINFO handler: 3 args (sig, siginfo*, ucontext*) */
static void handler(long sig, void* si, void* uc){
    (void)sig;(void)uc;
    char buf[8 + 3*0x38 + 2];
    int n=0;
    const char* tag="SIGSYS ";
    for(const char*t=tag;*t;t++) buf[n++]=*t;
    unsigned char* p=(unsigned char*)si;
    for(int i=0;i<0x38;i++){ buf[n++]=hx(p[i]>>4); buf[n++]=hx(p[i]); buf[n++]=' '; }
    buf[n++]='\n';
    emit(buf,n);
    sys3(SYS_exit_group,42,0,0);
}

__attribute__((constructor))
static void setup(void){
    /* aarch64 kernel_sigaction: handler(0x00), flags(0x08), mask(0x10). No restorer. */
    unsigned long act[3];
    act[0]=(unsigned long)&handler;
    act[1]=4;   /* SA_SIGINFO */
    act[2]=0;   /* empty mask */
    sys4(SYS_rt_sigaction, 31 /*SIGSYS*/, (long)act, 0, 8 /*sigsetsize*/);
    const char* m="[sigsys_catch] handler armed\n";
    long l=0; for(const char*t=m;*t;t++) l++;
    emit(m,l);
}
