#include <pthread.h>
#include "ThreadPool.h"
#include <sys/sysinfo.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include "Types.h"
#include <stdlib.h>

pthread_t threads[MAX_THREADS];
int32_t threadAmount = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t jobMutex = PTHREAD_MUTEX_INITIALIZER;
int32_t run = 0;
void *jobStack[MAX_THREADS];
void *argStack[MAX_THREADS];
int32_t jobTop = 0;

void mutexLock() {
	pthread_mutex_lock(&mutex);
}

void mutexUnlock() {
	pthread_mutex_unlock(&mutex);
}

FunctionPtr getJob(void **argPtr) {
	if (jobTop <= 0) {
		*argPtr = NULL;
		return NULL;
	}
	jobTop--;
	*argPtr = argStack[jobTop];
	return jobStack[jobTop];
}

void executeJobIfPresent() {
	FunctionPtr job;
	void *jobArgPtr = NULL;
	pthread_mutex_lock(&jobMutex);
	if ((job = getJob(&jobArgPtr))) {
		pthread_mutex_unlock(&jobMutex);
		job(jobArgPtr);
	}
	pthread_mutex_unlock(&jobMutex);
}

void *threadLoop(void *argPtr) {
	int32_t threadId = *(int32_t *)argPtr;
	struct timespec remaining, request = {0, 25};
	while(1) {
		if (!run) {
			break;
		}
		executeJobIfPresent();
		nanosleep(&request, &remaining);
	}
	return NULL;
}

int32_t pushJobs(int32_t jobAmount, FunctionPtr job, void **jobArgs) {
	pthread_mutex_lock(&jobMutex);
	int32_t nextTop = jobTop + jobAmount;
	if (nextTop > MAX_THREADS) {
		pthread_mutex_unlock(&jobMutex);
		return 1;
	}
	for (int32_t i = 0; i < jobAmount; ++i) {
		argStack[jobTop] = jobArgs[i];
		jobStack[jobTop] = job;
		jobTop++;
	}
	int32_t prioMin, prioMax;
	prioMin = sched_get_priority_min(SCHED_RR);
	prioMax = sched_get_priority_max(SCHED_RR);
	pthread_mutex_unlock(&jobMutex);
	return 0;
}

void createThreadPool() {
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	int32_t policy;
	run = 1;
	threadAmount = get_nprocs();
	if (threadAmount > MAX_THREADS) {
		threadAmount = MAX_THREADS;
	}
	if (threadAmount <= 1) {
		return;
	}
	for (int32_t i = 0; i < threadAmount; ++i) {
		pthread_create(&threads[i], &attr, threadLoop, &i);
	}
}

void destroyThreadPool() {
	if (threadAmount <= 1) {
		return;
	}
	run = 0;
	for (int32_t i = 0; i < threadAmount; ++i) {
		pthread_join(threads[i], NULL);
	}
}
