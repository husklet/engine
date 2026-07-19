#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>
static volatile sig_atomic_t caught;
static void on_ill(int sig, siginfo_t *info, void *opaque) { (void)sig; (void)info; ((ucontext_t *)opaque)->uc_mcontext.pc += 4; caught=1; }
static void unsupported(void) { __asm__ volatile(".inst 0xce63c004"); }
int main(void) { struct sigaction a={.sa_sigaction=on_ill,.sa_flags=SA_SIGINFO}; sigemptyset(&a.sa_mask); sigaction(SIGILL,&a,0); unsupported(); int continued=caught; pid_t p=fork(); if(!p){signal(SIGILL,SIG_DFL);unsupported();_exit(0);} int s=0;waitpid(p,&s,0); printf("sigill-probe caught=%d continued=%d default_sigill=%d\n",caught!=0,continued,WIFSIGNALED(s)&&WTERMSIG(s)==SIGILL); }
