/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#ifdef __APPLE_CC__
	#include <sys/sysctl.h>
#else
	#include <sys/sysinfo.h>
#endif

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

#include <thread_pool.h>
#include <context.h>
#include <error.h>

#define JOB_STACK_SIZE 128

typedef struct {
	StucResult (*pJob) (void *);
	void *pArgs;
	pthread_mutex_t *pMutex;
	StucResult err;
} StucJob;

typedef struct {
	StucJob *stack[JOB_STACK_SIZE];
	I32 count;
} StucJobStack;

typedef struct {
	pthread_t threads[MAX_THREADS];
	StucJobStack jobs;
	StucAlloc alloc;
	pthread_mutex_t *pJobMutex;
	I32 threadAmount;
	I32 run;
} ThreadPool;

void stucMutexGet(void *pThreadPool, void **ppMutex) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	*ppMutex = pState->alloc.fpMalloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(*ppMutex, NULL);
}

void stucMutexLock(void *pThreadPool, void *pMutex) {
	pthread_mutex_lock(pMutex);
}

void stucMutexUnlock(void *pThreadPool, void *pMutex) {
	pthread_mutex_unlock(pMutex);
}

void stucMutexDestroy(void *pThreadPool, void *pMutex) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	pthread_mutex_destroy(pMutex);
	pState->alloc.fpFree(pMutex);
}

void stucJobStackGetJob(void *pThreadPool, void **ppJob) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	pthread_mutex_lock(pState->pJobMutex);
	if (pState->jobs.count > 0) {
		pState->jobs.count--;
		*ppJob = pState->jobs.stack[pState->jobs.count];
		pState->jobs.stack[pState->jobs.count] = NULL;
	}
	else {
		*ppJob = NULL;
	}
	pthread_mutex_unlock(pState->pJobMutex);
	return;
}

static
bool checkRunFlag(ThreadPool *pState) {
	pthread_mutex_lock(pState->pJobMutex);
	bool run = pState->run;
	pthread_mutex_unlock(pState->pJobMutex);
	return run;
}

bool stucGetAndDoJob(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	StucJob *pJob = NULL;
	stucJobStackGetJob(pState, (void **)&pJob);
	if (!pJob) {
		return false;
	}
	StucResult err = pJob->pJob(pJob->pArgs);
	pthread_mutex_lock(pJob->pMutex);
	pJob->err = err;
	pthread_mutex_unlock(pJob->pMutex);
	return true;
}

static
void *threadLoop(void *pArgs) {
	ThreadPool *pState = (ThreadPool *)pArgs;
	struct timespec remaining = {0};
	struct timespec request = {0, 25};
	while(true) {
		if (!checkRunFlag(pState)) {
			break;
		}
		bool gotJob = stucGetAndDoJob(pArgs);
		if (!gotJob) {
			nanosleep(&request, &remaining);
		}
	}
	return NULL;
}

Result stucJobStackPushJobs(
	void *pThreadPool,
	I32 jobAmount,
	void **ppJobHandles,
	StucResult(*pJob)(void *),
	void **pJobArgs
) {
	Result err = STUC_SUCCESS;
	struct timespec remaining = {0};
	struct timespec request = {0, 25};
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	I32 jobsPushed = 0;
	do {
		I32 batchTop = jobAmount;
		pthread_mutex_lock(pState->pJobMutex);
		I32 nextTop = pState->jobs.count + jobAmount - jobsPushed;
		if (nextTop > JOB_STACK_SIZE) {
			batchTop -= nextTop - JOB_STACK_SIZE;
		}
		for (I32 i = jobsPushed; i < batchTop; ++i) {
			StucJob *pJobEntry = pState->alloc.fpCalloc(1, sizeof(StucJob));
			pJobEntry->pJob = pJob;
			pJobEntry->pArgs = pJobArgs[i];
			stucMutexGet(pThreadPool, (void **)&pJobEntry->pMutex);
			pState->jobs.stack[pState->jobs.count] = pJobEntry;
			pState->jobs.count++;
			ppJobHandles[i] = pJobEntry;
			jobsPushed++;
		}
		pthread_mutex_unlock(pState->pJobMutex);
		STUC_ASSERT("", jobsPushed >= 0 && jobsPushed <= jobAmount);
		if (jobsPushed == jobAmount) {
			break;
		}
		else {
			nanosleep(&request, &remaining);
		}
	} while(true);
	STUC_CATCH(0, err, ;);
	return err;
}

void stucThreadPoolInit(
		void **pThreadPool,
		I32 *pThreadCount,
        const StucAlloc *pAlloc
) {
	ThreadPool *pState = pAlloc->fpCalloc(1, sizeof(ThreadPool));
	*pThreadPool = pState;
	pState->alloc = *pAlloc;
	stucMutexGet(pState, (void **)&pState->pJobMutex);
	pState->run = 1;
#ifdef __APPLE_CC__
	uint64_t count = 0;
	size_t size = sizeof(uint64_t);
	I32 result = sysctlbyname("hw.physicalcpu", &count, &size, NULL, 0);
	if (result < 0) {
		STUC_ASSERT("Unable to get core count\n", 0);
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
	for (I32 i = 0; i < pState->threadAmount; ++i) {
		pthread_create(&pState->threads[i], NULL, threadLoop, pState);
	}
}

void stucThreadPoolDestroy(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	if (pState->threadAmount > 1) {
		pthread_mutex_lock(pState->pJobMutex);
		pState->run = 0;
		pthread_mutex_unlock(pState->pJobMutex);
		for (I32 i = 0; i < pState->threadAmount; ++i) {
			pthread_join(pState->threads[i], NULL);
		}
	}
	stucMutexDestroy(pState, pState->pJobMutex);
	pState->alloc.fpFree(pState);
}

//TODO update to check for new functions
Result stucThreadPoolSetCustom(StucContext pCtx, const StucThreadPool *pThreadPool) {
	if (!pThreadPool->fpInit || !pThreadPool->fpDestroy || !pThreadPool->fpMutexGet ||
	    !pThreadPool->fpMutexLock || !pThreadPool->fpMutexUnlock || !pThreadPool->fpMutexDestroy ||
		!pThreadPool->fpJobStackGetJob || !pThreadPool->pJobStackPushJobs) {
		printf("Failed to set custom thread pool. One or more functions were NULL");
		return STUC_ERROR;
	}
	pCtx->threadPool.fpDestroy(pCtx);
	pCtx->threadPool = *pThreadPool;
	return STUC_SUCCESS;
}

StucResult stucWaitForJobsIntern(
	void *pThreadPool,
	I32 jobCount,
	void **ppJobsVoid,
	bool wait,
	bool *pDone
) {
	StucResult err = STUC_SUCCESS;
	STUC_ASSERT("", jobCount > 0);
	STUC_ASSERT("if wait is false, pDone must not be null", pDone || wait);
	struct timespec remaining = {0};
	struct timespec request = {0, 25};
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	StucJob **ppJobs = (StucJob **)ppJobsVoid;
	I32 finished = 0;
	bool *pChecked = pState->alloc.fpCalloc(jobCount, sizeof(bool));
	if (!wait) {
		*pDone = false;
	}
	do {
		bool gotJob = false;
		if (wait) {
			gotJob = stucGetAndDoJob(pThreadPool);
		}
		for (I32 i = 0; i < jobCount; ++i) {
			if (pChecked[i]) {
				continue;
			}
			pthread_mutex_lock(ppJobs[i]->pMutex);
			if (ppJobs[i]->err != STUC_NOT_SET) {
				pChecked[i] = true;
				finished++;
			}
			pthread_mutex_unlock(ppJobs[i]->pMutex);
		}
		STUC_ASSERT("", finished <= jobCount && finished >= 0);
		if (finished == jobCount) {
			if (!wait) {
				*pDone = true;
			}
			break;
		}
		else if (!gotJob) {
			nanosleep(&request, &remaining);
		}
	} while(wait);
	STUC_CATCH(1, err, ;);
	pState->alloc.fpFree(pChecked);
	STUC_CATCH(0, err, ;);
	return err;
}

StucResult stucGetJobErr(void *pThreadPool, void *pJobHandle, StucResult *pJobErr) {
	StucResult err = STUC_SUCCESS;
	STUC_ASSERT("", pThreadPool && pJobHandle && pJobErr);
	StucJob *pJob = pJobHandle;
	*pJobErr = pJob->err;
	STUC_CATCH(0, err, ;);
	return err;
}

StucResult stucJobHandleDestroy(void *pThreadPool, void **ppJobHandle) {
	StucResult err = STUC_SUCCESS;
	STUC_ASSERT("", pThreadPool && ppJobHandle);
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	StucJob *pJob = *ppJobHandle;
	if (*ppJobHandle) {
		stucMutexDestroy(pThreadPool, pJob->pMutex);
		pState->alloc.fpFree(pJob);
		*ppJobHandle = NULL;
	}
	STUC_CATCH(0, err, ;);
	return err;
}

void stucThreadPoolSetDefault(StucContext pCtx) {
	pCtx->threadPool.fpInit = stucThreadPoolInit;
	pCtx->threadPool.fpWaitForJobs = stucWaitForJobsIntern;
	pCtx->threadPool.fpGetJobErr = stucGetJobErr;
	pCtx->threadPool.fpJobHandleDestroy = stucJobHandleDestroy;
	pCtx->threadPool.fpDestroy = stucThreadPoolDestroy;
	pCtx->threadPool.fpMutexGet = stucMutexGet;
	pCtx->threadPool.fpMutexLock = stucMutexLock;
	pCtx->threadPool.fpMutexUnlock = stucMutexUnlock;
	pCtx->threadPool.fpMutexDestroy = stucMutexDestroy;
	/*
	pCtx->threadPool.fpBarrierGet = stucBarrierGet;
	pCtx->threadPool.fpBarrierWait = stucBarrierWait;
	pCtx->threadPool.fpBarrierDestroy = stucBarrierDestroy;
	*/
	pCtx->threadPool.fpJobStackGetJob = stucJobStackGetJob;
	pCtx->threadPool.pJobStackPushJobs = stucJobStackPushJobs;
	pCtx->threadPool.fpGetAndDoJob = stucGetAndDoJob;
}
