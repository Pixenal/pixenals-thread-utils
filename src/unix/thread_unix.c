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

#include <pixenals_thread_utils.h>

typedef int32_t I32;

#define JOB_STACK_SIZE 128

typedef struct PixJob {
	PixErr (*pJob) (void *);
	void *pArgs;
	pthread_mutex_t *pMutex;
	PixErr err;
} PixJob;

typedef struct PixJobStack{
	PixJob *stack[JOB_STACK_SIZE];
	I32 count;
} PixJobStack;

typedef struct ThreadPool{
	pthread_t threads[PIX_THREAD_MAX_THREADS];
	PixJobStack jobs;
	PixalcFPtrs alloc;
	pthread_mutex_t *pJobMutex;
	I32 threadAmount;
	I32 run;
} ThreadPool;

void pixthMutexGet(void *pThreadPool, void **ppMutex) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	*ppMutex = pState->alloc.fpMalloc(sizeof(pthread_mutex_t));
	pthread_mutex_init(*ppMutex, NULL);
}

void pixthMutexLock(void *pThreadPool, void *pMutex) {
	pthread_mutex_lock(pMutex);
}

void pixthMutexUnlock(void *pThreadPool, void *pMutex) {
	pthread_mutex_unlock(pMutex);
}

void pixthMutexDestroy(void *pThreadPool, void *pMutex) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	pthread_mutex_destroy(pMutex);
	pState->alloc.fpFree(pMutex);
}

void pixthJobStackGetJob(void *pThreadPool, void **ppJob) {
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
bool checkRunFlag(const ThreadPool *pState) {
	pthread_mutex_lock(pState->pJobMutex);
	bool run = pState->run;
	pthread_mutex_unlock(pState->pJobMutex);
	return run;
}

bool pixthGetAndDoJob(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	PixJob *pJob = NULL;
	pixthJobStackGetJob(pState, (void **)&pJob);
	if (!pJob) {
		return false;
	}
	PixErr err = pJob->pJob(pJob->pArgs);
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
		bool gotJob = pixthGetAndDoJob(pArgs);
		if (!gotJob) {
			nanosleep(&request, &remaining);
		}
	}
	return NULL;
}

PixErr pixthJobStackPushJobs(
	void *pThreadPool,
	I32 jobAmount,
	void **ppJobHandles,
	PixErr(*pJob)(void *),
	void **pJobArgs
) {
	PixErr err = PIX_ERR_SUCCESS;
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
			PixJob *pJobEntry = pState->alloc.fpCalloc(1, sizeof(PixJob));
			pJobEntry->pJob = pJob;
			pJobEntry->pArgs = pJobArgs[i];
			pixthMutexGet(pThreadPool, (void **)&pJobEntry->pMutex);
			pState->jobs.stack[pState->jobs.count] = pJobEntry;
			pState->jobs.count++;
			ppJobHandles[i] = pJobEntry;
			jobsPushed++;
		}
		pthread_mutex_unlock(pState->pJobMutex);
		PIX_ERR_ASSERT("", jobsPushed >= 0 && jobsPushed <= jobAmount);
		if (jobsPushed == jobAmount) {
			break;
		}
		else {
			nanosleep(&request, &remaining);
		}
	} while(true);
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

void pixthThreadPoolInit(
		void **pThreadPool,
		I32 *pThreadCount,
        const PixalcFPtrs *pAlloc
) {
	ThreadPool *pState = pAlloc->fpCalloc(1, sizeof(ThreadPool));
	*pThreadPool = pState;
	pState->alloc = *pAlloc;
	pixthMutexGet(pState, (void **)&pState->pJobMutex);
	pState->run = 1;
#ifdef __APPLE_CC__
	uint64_t count = 0;
	size_t size = sizeof(uint64_t);
	I32 err = sysctlbyname("hw.physicalcpu", &count, &size, NULL, 0);
	if (err < 0) {
		PIX_ERR_ASSERT("Unable to get core count\n", 0);
	}
	pState->threadAmount = count;
#else
	pState->threadAmount = get_nprocs();
#endif
	if (pState->threadAmount > PIX_THREAD_MAX_THREADS) {
		pState->threadAmount = PIX_THREAD_MAX_THREADS;
	}
	*pThreadCount = pState->threadAmount;
	if (pState->threadAmount <= 1) {
		return;
	}
	for (I32 i = 0; i < pState->threadAmount; ++i) {
		pthread_create(&pState->threads[i], NULL, threadLoop, pState);
	}
}

void pixthThreadPoolDestroy(void *pThreadPool) {
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	if (pState->threadAmount > 1) {
		pthread_mutex_lock(pState->pJobMutex);
		pState->run = 0;
		pthread_mutex_unlock(pState->pJobMutex);
		for (I32 i = 0; i < pState->threadAmount; ++i) {
			pthread_join(pState->threads[i], NULL);
		}
	}
	pixthMutexDestroy(pState, pState->pJobMutex);
	pState->alloc.fpFree(pState);
}

//TODO there's enough duplicate logic in this and the win file to warrent abstracting funcs
//like below into an agnostic source file
PixErr pixthWaitForJobsIntern(
	void *pThreadPool,
	I32 jobCount,
	void **ppJobsVoid,
	bool wait,
	bool *pDone
) {
	PixErr err = PIX_ERR_SUCCESS;
	PIX_ERR_ASSERT("", jobCount > 0);
	PIX_ERR_ASSERT("if wait is false, pDone must not be null", pDone || wait);
	struct timespec remaining = {0};
	struct timespec request = {0, 25};
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	PixJob **ppJobs = (PixJob **)ppJobsVoid;
	I32 finished = 0;
	bool *pChecked = pState->alloc.fpCalloc(jobCount, sizeof(bool));
	if (!wait) {
		*pDone = false;
	}
	do {
		bool gotJob = false;
		if (wait) {
			gotJob = pixthGetAndDoJob(pThreadPool);
		}
		for (I32 i = 0; i < jobCount; ++i) {
			if (pChecked[i]) {
				continue;
			}
			pthread_mutex_lock(ppJobs[i]->pMutex);
			if (ppJobs[i]->err != PIX_ERR_NOT_SET) {
				pChecked[i] = true;
				finished++;
			}
			pthread_mutex_unlock(ppJobs[i]->pMutex);
		}
		PIX_ERR_ASSERT("", finished <= jobCount && finished >= 0);
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
	PIX_ERR_CATCH(1, err, ;);
	pState->alloc.fpFree(pChecked);
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

PixErr pixthGetJobErr(void *pThreadPool, void *pJobHandle, PixErr *pJobErr) {
	PixErr err = PIX_ERR_SUCCESS;
	PIX_ERR_ASSERT("", pThreadPool && pJobHandle && pJobErr);
	PixJob *pJob = pJobHandle;
	*pJobErr = pJob->err;
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

PixErr pixthJobHandleDestroy(void *pThreadPool, void **ppJobHandle) {
	PixErr err = PIX_ERR_SUCCESS;
	PIX_ERR_ASSERT("", pThreadPool && ppJobHandle);
	ThreadPool *pState = (ThreadPool *)pThreadPool;
	PixJob *pJob = *ppJobHandle;
	if (*ppJobHandle) {
		pixthMutexDestroy(pThreadPool, pJob->pMutex);
		pState->alloc.fpFree(pJob);
		*ppJobHandle = NULL;
	}
	PIX_ERR_CATCH(0, err, ;);
	return err;
}
