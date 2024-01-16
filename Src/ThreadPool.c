#include <pthread.h>
#include "ThreadPool.h"
#include <sys/sysinfo.h>
#include <time.h>
#include "Types.h"

pthread_t threads[MAX_THREADS];
int32_t threadAmount = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
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

void *threadLoop(void *argPtr) {
	int32_t threadId = *(int32_t *)argPtr;
	struct timespec remaining, request = {0, 10000};
	FunctionPtr job;
	void *jobArgPtr = NULL;
	while(1) {
		if (!run) {
			break;
		}
		if ((job = getJob(&jobArgPtr))) {
			job(jobArgPtr);
		}
		nanosleep(&request, &remaining);
	}
	return NULL;
}

int32_t pushJobs(int32_t jobAmount, FunctionPtr job, void **jobArgs) {
	int32_t nextTop = jobTop + jobAmount;
	if (nextTop > MAX_THREADS) {
		return 1;
	}
	for (int32_t i = 0; i < jobAmount; ++i) {
		argStack[jobTop] = jobArgs[i];
		jobStack[jobTop] = job;
		jobTop++;
	}
	return 0;
}

void createThreadPool() {
	run = 1;
	threadAmount = get_nprocs();
	if (threadAmount > MAX_THREADS) {
		threadAmount = MAX_THREADS;
	}
	for (int32_t i = 0; i < threadAmount; ++i) {
		pthread_create(&threads[i], NULL, threadLoop, &i);
	}
}

void destroyThreadPool() {
	run = 0;
	for (int32_t i = 0; i < threadAmount; ++i) {
		pthread_join(threads[i], NULL);
	}
}
