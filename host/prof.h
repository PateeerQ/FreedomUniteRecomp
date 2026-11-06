/* MHFU-Android -- a sampling profiler for the desktop (host/prof.c), MHFU_PROFILE=1. */
#ifndef MHFU_PROF_H
#define MHFU_PROF_H

void mhfu_prof_start(void);
/* The busiest functions per thread and each thread's CPU time; in the run's report. */
void mhfu_prof_report(void);

#endif
