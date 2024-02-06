#include <pthread.h>
#include "ThreadPool.h"
#include <sys/sysinfo.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include "Types.h"
#include <stdlib.h>
#include <Context.h>

typedef struct {
	pthread_t threads[MAX_THREADS];
	int32_t threadAmount;
	pthread_mutex_t jobMutex;
	int32_t run;
	void *jobStack[MAX_THREADS];
	void *argStack[MAX_THREADS];
	int32_t jobStackSize;
	RuvmAllocator allocator;
} ThreadPool;

void ruvmMutexGet(void *pThreadPool, void **pMutex) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	*pMutex = pState->allocator.pMalloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(*pMutex, NULL);
}

void ruvmMutexLock(void *pThreadPool, void *pMutex) {
	pthread_mutex_lock(pMutex);
}

void ruvmMutexUnlock(void *pThreadPool, void *pMutex) {
	pthread_mutex_unlock(pMutex);
}

void ruvmMutexDestroy(void *pThreadPool, void *pMutex) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	pthread_mutex_destroy(pMutex);
	pState->allocator.pFree(pMutex);
}

void ruvmJobStackGetJob(void *pThreadPool, void (**pJob)(void *), void **pArgs) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	pthread_mutex_lock(&pState->jobMutex);
	if (pState->jobStackSize > 0) {
		pState->jobStackSize--;
		*pJob = pState->jobStack[pState->jobStackSize];
		*pArgs = pState->argStack[pState->jobStackSize];
	}
	else {
		*pJob = *pArgs = NULL;
	}
	pthread_mutex_unlock(&pState->jobMutex);
	return;
}

static void *threadLoop(void *pArgs) {
	ThreadPool *pState = (ThreadPool *)pArgs;
	struct timespec remaining, request = {0, 25};
	while(1) {
		if (!pState->run) {
			break;
		}
		void (*pJob)(void *) = NULL;
		void *pJobArgs = NULL;
		ruvmJobStackGetJob(pState, &pJob, &pJobArgs);
		if (pJob) {
			pJob(pJobArgs);
		}
		else {
			nanosleep(&request, &remaining);
		}
	}
	return NULL;
}

int32_t ruvmJobStackPushJobs(void *pThreadPool, int32_t jobAmount,
                             void (*pJob)(void *), void **pJobArgs) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	pthread_mutex_lock(&pState->jobMutex);
	int32_t nextTop = pState->jobStackSize + jobAmount;
	if (nextTop > MAX_THREADS) {
		pthread_mutex_unlock(&pState->jobMutex);
		return 1;
	}
	for (int32_t i = 0; i < jobAmount; ++i) {
		pState->argStack[pState->jobStackSize] = pJobArgs[i];
		pState->jobStack[pState->jobStackSize] = pJob;
		pState->jobStackSize++;
	}
	pthread_mutex_unlock(&pState->jobMutex);
	return 0;
}

void ruvmThreadPoolInit(void **pThreadPool, int32_t *pThreadCount,
                        RuvmAllocator *pAllocator) {
	ThreadPool *pState = pAllocator->pCalloc(1, sizeof(ThreadPool));
	*pThreadPool = pState;
	pState->allocator = *pAllocator;
	pthread_mutex_init(&pState->jobMutex, NULL);
	pState->run = 1;
	pState->threadAmount = get_nprocs();
	if (pState->threadAmount > MAX_THREADS) {
		pState->threadAmount = MAX_THREADS;
	}
	*pThreadCount = pState->threadAmount;
	if (pState->threadAmount <= 1) {
		return;
	}
	for (int32_t i = 0; i < pState->threadAmount; ++i) {
		pthread_create(&pState->threads[i], NULL, threadLoop, pState);
	}
}

void ruvmThreadPoolDestroy(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	pthread_mutex_destroy(&pState->jobMutex);
	if (pState->threadAmount > 1) {
		pState->run = 0;
		for (int32_t i = 0; i < pState->threadAmount; ++i) {
			pthread_join(pState->threads[i], NULL);
		}
	}
	pState->allocator.pFree(pState);
}

void ruvmThreadPoolSetCustom(RuvmContext context, RuvmThreadPool *pThreadPool) {
	if (!pThreadPool->pInit || !pThreadPool->pDestroy || !pThreadPool->pMutexGet ||
	    !pThreadPool->pMutexLock || !pThreadPool->pMutexUnlock || !pThreadPool->pMutexDestroy ||
		!pThreadPool->pJobStackGetJob || !pThreadPool->pJobStackPushJobs) {
		printf("Failed to set custom thread pool. One or more functions were NULL");
		return;
	}
	context->threadPool.pDestroy(context);
	context->threadPool = *pThreadPool;
}

void ruvmThreadPoolSetDefault(RuvmContext context) {
	context->threadPool.pInit = ruvmThreadPoolInit;
	context->threadPool.pDestroy = ruvmThreadPoolDestroy;
	context->threadPool.pMutexGet = ruvmMutexGet;
	context->threadPool.pMutexLock = ruvmMutexLock;
	context->threadPool.pMutexUnlock = ruvmMutexUnlock;
	context->threadPool.pMutexDestroy = ruvmMutexDestroy;
	context->threadPool.pJobStackGetJob = ruvmJobStackGetJob;
	context->threadPool.pJobStackPushJobs = ruvmJobStackPushJobs;
}
