#pragma once
#include <stdint.h>

#define MAX_THREADS 23
typedef void(*FunctionPtr)(void *);

void mutexLock();
void mutexUnlock();
int32_t pushJobs(int32_t jobAmount, FunctionPtr job, void **jobArgs);
void createThreadPool();
void destroyThreadPool();
