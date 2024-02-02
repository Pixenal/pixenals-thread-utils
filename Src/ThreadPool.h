#pragma once
#include <stdint.h>

#define MAX_THREADS 8
typedef void(*FunctionPtr)(void *);

void executeJobIfPresent();
void mutexLock();
void mutexUnlock();
int32_t pushJobs(int32_t jobAmount, FunctionPtr job, void **pJobArgs);
void createThreadPool();
void destroyThreadPool();
