#ifdef MACOS
	#include <sys/sysctl.h>
#else
	#include <sys/sysinfo.h>
#endif

#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

#include <ThreadPool.h>
#include <Context.h>
#include <Error.h>

typedef struct {
	pthread_t threads[MAX_THREADS];
	int32_t threadAmount;
	pthread_mutex_t jobMutex;
	int32_t run;
	void *jobStack[MAX_THREADS];
	void *argStack[MAX_THREADS];
	int32_t jobStackSize;
	RuvmAlloc alloc;
} ThreadPool;

void ruvmMutexGet(void *pThreadPool, void **pMutex) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	*pMutex = pState->alloc.pMalloc(sizeof(pthread_mutex_t));
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
	pState->alloc.pFree(pMutex);
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
                        RuvmAlloc *pAlloc) {
	ThreadPool *pState = pAlloc->pCalloc(1, sizeof(ThreadPool));
	*pThreadPool = pState;
	pState->alloc = *pAlloc;
	pthread_mutex_init(&pState->jobMutex, NULL);
	pState->run = 1;
#ifdef MACOS
	uint64_t count = 0;
	size_t size = sizeof(uint64_t);
	int32_t result = sysctlbyname("hw.physicalcpu", &count, &size, NULL, 0);
	if (result < 0) {
		RUVM_ASSERT("Unable to get core count\n", 0);
	}
	pState->threadAmount = count;
#else
	pState->threadAmount = get_nprocs();
#endif
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
	pState->alloc.pFree(pState);
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
