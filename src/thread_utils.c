/* 
SPDX-FileCopyrightText: 2025 Caleb Dawson
SPDX-License-Identifier: Apache-2.0
*/

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

#include <pixenals_thread_utils.h>

typedef int16_t I16;
typedef int32_t I32;
typedef uint32_t U32;
typedef int64_t I64;
typedef float F32;

static
PixErr logAction(
	PixthThreadArgs *pArgs,
	PixthJob *pJob,
	PixthLogAction action,
	I32 stoleFrom
) {
	PixErr err = PIX_ERR_SUCCESS;
	if (!pArgs->pCtx->logging) {
		return err;
	}
	struct _timespec64 ts;
	PIX_ERR_RETURN_IFNOT_COND(err, TIME_UTC == _timespec64_get(&ts, TIME_UTC), "");
	I32 newIdx = 0;
	PIXALC_DYN_ARR_ADD(PixthLogEntry, pArgs->pCtx->core.pAlloc, &pArgs->log, newIdx);
	pArgs->log.pArr[newIdx] = (PixthLogEntry) {
		.timeS = ts.tv_sec,
		.timeNs = (I32)ts.tv_nsec,
		.job = pJob ? pJob->hash : 0,
		.action = (I16)action,
		.stoleFrom = stoleFrom
	};
	return err;
}

static
I32 getLessOrEqualPrime(I32 num) {
	I32 primes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31};
	I32 primesSize = sizeof(primes) / 4;
	I32 max = primes[primesSize - 1];
	if (num >= max) {
		return max;
	}
	for (I32 i = 1; i < primesSize; ++i) {
		if (num == primes[i]) {
			return primes[i];
		}
		if (num < primes[i]) {
			return primes[i - 1];
		}
	}
	PIX_ERR_ASSERT("", false);
	return 0;
}

static
PixthJob *jobDequeGet(PixthJobDeque *pDeque, I64 idx) {
	return pDeque->deque[idx % PIXTH_DEQUE_SIZE];
}

static
void jobDequeSet(PixthJobDeque *pDeque, I64 idx, PixthJob *pJob) {
	pDeque->deque[idx % PIXTH_DEQUE_SIZE] = pJob;
}

static
PixErr jobDequePushB(PixthJobDeque *pDeque, PixthJob *pJob) {
	PixErr err = PIX_ERR_SUCCESS;
	I64 b = pDeque->bottom;
	//last read value of top is used first
	//to reduce potential cache misses caused by stealing
	I64 t = pDeque->topAprox;
	I64 size = b - t;
	PIX_ERR_ASSERT("", size < PIXTH_DEQUE_SIZE);
	if (size == PIXTH_DEQUE_SIZE - 1) {
		//aprox size has hit maximum.
		//check again using real value of top, and ret error if we've actually hit max
		t = pDeque->top;
		size = b - t;
		PIX_ERR_ASSERT("", size < PIXTH_DEQUE_SIZE);
		PIX_ERR_RETURN_IFNOT_COND(
			err, 
			size != PIXTH_DEQUE_SIZE - 1,
			"deque hit max size"
		);
		pDeque->topAprox = t;//update local copy
	}
	jobDequeSet(pDeque, b, pJob);
	pDeque->bottom = b + 1;
	return err;
}

static
PixthJob *jobDequePopB(PixthPoolCtx *pCtx, PixthJobDeque *pDeque) {
	I64 b = pDeque->bottom - 1;
	pDeque->bottom = b;
	I64 t = pDeque->top;
	I64 size = b - t;
	if (size < 0) {
		pDeque->bottom = t;
		return NULL;
	}
	PixthJob *pJob = jobDequeGet(pDeque, pDeque->bottom);
	if (size > 0) {
		return pJob;
	}
	if (t != pixthAtomicCmpAndSwapI64(pCtx, &pDeque->top, t, t + 1)) {
		pJob = NULL;
	}
	pDeque->bottom = t + 1;
	pDeque->topAprox = t;//update local copy of top (used in pushB)
	return pJob;
}

static
U32 stucFnvHash(const U8 *value, I32 valueSize, U32 size) {
	PIX_ERR_ASSERT("", value && valueSize > 0 && size > 0);
	U32 hash = 2166136261;
	for (I32 i = 0; i < valueSize; ++i) {
		hash ^= value[i];
		hash *= 16777619;
	}
	hash %= size;
	PIX_ERR_ASSERT("", hash >= 0);
	return hash;
}

static
PixthJob *jobDequeSteal(PixthPoolCtx *pCtx, PixthJobDeque *pDeque) {
	I64 t = pDeque->top;
	I64 b = pDeque->bottom;
	I64 size = b - t;
	if (size <= 0) {
		return NULL;
	}
	PixthJob *pJob = jobDequeGet(pDeque, t);
	if (t != pixthAtomicCmpAndSwapI64(pCtx, &pDeque->top, t, t + 1)) {
		return NULL;
	}
	return pJob;
}

static
I32 pickRandThread(PixthPoolCtx *pCtx, I32 seed) {
	return stucFnvHash((U8 *)&seed, sizeof(I32), pCtx->threadCount);
}

PixErr pixthJobStackGetJob(PixthPoolCtx *pCtx, void **ppJob, I32 id, U32 tick) {
	PixErr err = PIX_ERR_SUCCESS;
	*ppJob = jobDequePopB(pCtx, &pCtx->args[id].jobs);
	if (*ppJob) {
		err = logAction(pCtx->args + id, *ppJob, PIX_THREAD_ACTION_POP, 0);
		PIX_ERR_RETURN_IFNOT(err, "");
		return err;
	}
	I32 target;
	U32 attempts = 0;
	do {
		target = pickRandThread(pCtx, tick + attempts);
		++attempts;
	} while (target == id);
	*ppJob = jobDequeSteal(pCtx, &pCtx->args[target].jobs);
	if (*ppJob) {
		err = logAction(pCtx->args + id, *ppJob, PIX_THREAD_ACTION_STEAL, target);
		PIX_ERR_RETURN_IFNOT(err, "");
	}
	return err;
}

static
PixErr pixthGetAndDoJob(PixthPoolCtx *pCtx, I32 threadId, U32 tick, bool *pGotJob) {
	PixErr err = PIX_ERR_SUCCESS;
	PixthJob *pJob = NULL;
	err = pixthJobStackGetJob(pCtx, (void **)&pJob, threadId, tick);
	PIX_ERR_RETURN_IFNOT(err, "");
	if (!pJob) {
		*pGotJob = false;
		return err;
	}
	PixErr jobErr = pJob->pJob(pJob->pArgs, threadId);
	err = logAction(pCtx->args + threadId, pJob, PIX_THREAD_ACTION_FINISH, 0);
	PIX_ERR_RETURN_IFNOT(err, "");

	jobErr = pixthAtomicCmpAndSwapI32(
		pCtx,
		(volatile I32 *)&pJob->err,
		PIX_ERR_NOT_SET,
		jobErr
	);
	PIX_ERR_ASSERT("multiple threads executed the same job", jobErr == PIX_ERR_NOT_SET);
	*pGotJob = true;
	return err;
}

static
void handleDirectiveChange(PixthThreadArgs *pArgs, PixthPoolDirective newDirective) {
	PixthLogAction action = 0;
	switch (newDirective) {
		case THREAD_SLEEP:
			action = PIX_THREAD_ACTION_SLEEP;
			break;
		case THREAD_WAKE:
			action = PIX_THREAD_ACTION_WAKE;
			break;
		case THREAD_PAUSE:
			action = PIX_THREAD_ACTION_PAUSE;
			break;
		case THREAD_STOP:
			action = PIX_THREAD_ACTION_STOP;
			break;
		default:
			PIX_ERR_ASSERT("invalid directive sent to thread", false);
	}
	PixErr err = logAction(pArgs, NULL, action, 0);
	//TODO set directive to THREAD_STOP if err, and handle on main thread
	PIX_ERR_ASSERT("", err == PIX_ERR_SUCCESS);
	if (newDirective == THREAD_PAUSE) {
		do {
			I32 paused = pArgs->pCtx->paused;
			if (paused == 
				pixthAtomicCmpAndSwapI32(
					pArgs->pCtx,
					&pArgs->pCtx->paused,
					paused,
					paused + 1
			)) {
				break;
			}
		} while(true);
	}
}

static
void threadHandleAwake(PixthThreadArgs *pArgs, I32 *pNoJobs, I32 *pTick) {
	bool gotJob = false;
	PixErr err = pixthGetAndDoJob(pArgs->pCtx, pArgs->id, *pTick, &gotJob);
	PIX_ERR_ASSERT("", err == PIX_ERR_SUCCESS);
	if (!gotJob) {
		++*pNoJobs;
		if (*pNoJobs > PIXTH_NO_JOB_THRES) {
			PixthPoolDirective cmp = pixthAtomicCmpAndSwapI32(
				pArgs->pCtx,
				(volatile I32 *)&pArgs->pCtx->directive,
				THREAD_WAKE,
				THREAD_SLEEP	
			);
			*pNoJobs = 0;
			if (cmp == THREAD_WAKE) {
				err = logAction(pArgs, NULL, PIX_THREAD_ACTION_SET_SLEEP, 0);
				PIX_ERR_ASSERT("", err == PIX_ERR_SUCCESS);
			}
		}
	}
	++*pTick;
}

//returns true if process should fallthrough to wake state
static
bool threadHandleSleep(PixthThreadArgs *pArgs) {
	if (pArgs->jobs.bottom == pArgs->jobs.top) {
		pixthSleep(pArgs->pCtx, 25);
		return false;
	}
	PixthPoolDirective cmp = pixthAtomicCmpAndSwapI32(
		pArgs->pCtx,
		(volatile I32 *)&pArgs->pCtx->directive,
		THREAD_SLEEP,
		THREAD_WAKE
	);
	if (cmp == THREAD_STOP) {
		return false;
	}
	if (cmp == THREAD_SLEEP) {
		PixErr err = logAction(pArgs, NULL, PIX_THREAD_ACTION_SET_WAKE, 0);
		PIX_ERR_ASSERT("", err == PIX_ERR_SUCCESS);
	}
	return true;
}

static
I32 threadLoop(void *pArgsVoid) {
	PixErr err = PIX_ERR_SUCCESS;
	PixthThreadArgs *pArgs = pArgsVoid;
	U32 tick = pArgs->id;
	I32 noJobs = 0;
	PixthPoolDirective directive = THREAD_SLEEP;
	do {
		{
			PixthPoolDirective newDirective = pArgs->pCtx->directive;
			if (directive != newDirective) {
				handleDirectiveChange(pArgs, newDirective);
			}
			directive = newDirective;
		}
		switch (directive) {
			case THREAD_SLEEP:
				if (!threadHandleSleep(pArgs)) {
					break;
				}
				/* v fallthrough v */
			case THREAD_WAKE:
				threadHandleAwake(pArgs, &noJobs, &tick);
				break;
			case THREAD_PAUSE:
				pixthSleep(pArgs->pCtx, 25);
				break;
			case THREAD_STOP:
			default:
				return 0; //TODO should default return 1?
		}
	} while(true);
}

static
void verifyThreadId(PixthPoolCtx *pCtx, I32 *pId) {
	PIX_ERR_ASSERT("", *pId >= 0 && *pId < pCtx->threadCount);
}

void pixthJobsInit(PixthJob *pJobs, I32 count, PixErr(*func)(void *, I32), void **ppArgs) {
	for (I32 i = 0; i < count; ++i) {
		pJobs[i] = (PixthJob){
			.pJob = func,
			.pArgs = ppArgs[i]
		};
	}
}

//TODO look into switching to work stealing, is sync overhead high enough to warrent it?
PixErr pixthJobStackPushJobs(
	PixthPoolCtx *pCtx,
	I32 id,
	I32 jobCount,
	PixthJob *pJobs
) {
	PixErr err = PIX_ERR_SUCCESS;
	verifyThreadId(pCtx, &id);
	I32 totalPushed = 0;
	//TODO add a timeout
	do {
		I32 pushed = 0;
		for (I32 i = totalPushed; i < jobCount; ++i) {
			struct _timespec64 ts;
			PIX_ERR_RETURN_IFNOT_COND(err, TIME_UTC == _timespec64_get(&ts, TIME_UTC), "");
			pJobs[i].hash =
				stucFnvHash((U8 *)&ts.tv_sec, 8, UINT64_MAX) +
				stucFnvHash((U8 *)&ts.tv_nsec, 8, UINT64_MAX) +
				stucFnvHash((U8 *)(pJobs + i), sizeof(intptr_t), UINT64_MAX);
			PixErr pushErr = jobDequePushB(&pCtx->args[id].jobs, pJobs + i);
			if (pushErr != PIX_ERR_SUCCESS) {
				err = logAction(
					pCtx->args + id,
					pJobs + i,
					PIX_THREAD_ACTION_PUSH_FAIL,
					0
				);
				PIX_ERR_RETURN_IFNOT(err, "");
				break;
			}
			err = logAction(pCtx->args + id, pJobs + i, PIX_THREAD_ACTION_PUSH, 0);
			PIX_ERR_RETURN_IFNOT(err, "");
			++pushed;
		}
		totalPushed += pushed;
		pixthAtomicCmpAndSwapI32(
			pCtx,
			(I32 *)&pCtx->directive,
			THREAD_SLEEP,
			THREAD_WAKE
		);
		PIX_ERR_ASSERT("", totalPushed >= 0 && totalPushed <= jobCount);
		if (totalPushed == jobCount) {
			break;
		}
		pixthSleep(pCtx, 25);
		//printf(" <> waiting to push\n");
	} while(true);
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

void *pixthArgGet(PixthPoolCtx *pCtx, I32 idx) {
	PIX_ERR_ASSERT("", idx + 1 < pCtx->threadCount);
	return pCtx->args + idx + 1;
}

PixErr pixthThreadPoolInitIntern(const PixalcFPtrs *pAlloc, PixthPoolCtx *pCtx) {
	PixErr err = PIX_ERR_SUCCESS;
	pCtx->threadCount = getLessOrEqualPrime(pCtx->threadCount);
	err = pixthThreadPoolInitPlatform(
		pAlloc,
		&pCtx->core.pPlatform,
		pCtx->threadCount - 1,
		threadLoop
	);
	PIX_ERR_RETURN_IFNOT(err, "");

	return err;
}

PixErr pixthWaitForJobs(
	PixthPoolCtx *pCtx,
	I32 jobCount,
	PixthJob *pJobs,
	I32 id,
	bool wait,
	bool *pDone
) {
	PixErr err = PIX_ERR_SUCCESS;
	PIX_ERR_ASSERT("", jobCount > 0);
	PIX_ERR_ASSERT("if wait is false, pDone must not be null", pDone || wait);
	verifyThreadId(pCtx, &id);
	I32 finished = 0;
	bool checked[PIXTH_DEQUE_SIZE] = {0};
	if (!wait) {
		*pDone = false;
	}
	U32 tick = id;
	do {
		bool gotJob = false;
		if (wait) {
			err = pixthGetAndDoJob(pCtx, id, tick, &gotJob);
			PIX_ERR_RETURN_IFNOT(err, "");
			++tick;
		}
		for (I32 i = 0; i < jobCount; ++i) {
			if (checked[i]) {
				continue;
			}
			if (pJobs[i].err) {
				checked[i] = true;
				finished++;
			}
		}
		PIX_ERR_ASSERT("", finished <= jobCount && finished >= 0);
		if (finished == jobCount) {
			if (!wait) {
				*pDone = true;
			}
			break;
		}
		else if (!gotJob) {
			pixthSleep(pCtx, 25);
		}
	} while(wait);
	//printf("finished waiting for jobs\n");
	return err;
}

PixErr pixthGetJobErr(PixthPoolCtx *pCtx, PixthJob *pJobHandle, PixErr *pJobErr) {
	PixErr err = PIX_ERR_SUCCESS;
	PIX_ERR_ASSERT("", pCtx && pJobHandle && pJobErr);
	*pJobErr = pJobHandle->err;
	PIX_ERR_CATCH(0, err, ;);
	return err;
}

#define PIX_THREAD_LOG_ACTION_MAX_LEN 32

static
PixErr printLogEntry(
	PixalcFPtrs *pAlloc,
	PixtyI8Arr *pLog,
	const PixthLogEntry *pEntry,
	I32 thread
) {
	PixErr err = PIX_ERR_SUCCESS;
#ifndef PIX_THREAD_LOG_DISABLE
	char actionStr[PIX_THREAD_ACTION_ENUM_SIZE][PIX_THREAD_LOG_ACTION_MAX_LEN] = {
		"",
		"pushed",
		"push failed",
		"popped",
		"stole",
		"finished",
		"set wake",
		"woke up",
		"set sleep",
		"fell asleep",
		"paused",
		"stopped"
	};
	bool listJob[PIX_THREAD_ACTION_ENUM_SIZE] = {
		false,
		true,
		false,
		true,
		true,
		true,
		false,
		false,
		false,
		false,
		false,
		false
	};
	char stoleFromStr[32];
	I32 written =
		snprintf(stoleFromStr, sizeof(stoleFromStr), "from thread %d", pEntry->stoleFrom);
	PIX_ERR_ASSERT("", written > 0 && written < sizeof(stoleFromStr));

	char jobStr[32];
	written =
		snprintf(jobStr, sizeof(jobStr), "job %#"PRIx64"", pEntry->job);
	PIX_ERR_ASSERT("", written > 0 && written < sizeof(jobStr));


	const char *format = "%#"PRIx64" sec, %#x nsec - thread %d - %s %s %s\n";
	#define PIX_THREAD_LOG_ENTRY_MAX_LEN\
		32 +/*timestamp*/\
		PIX_THREAD_LOG_ACTION_MAX_LEN +\
		sizeof(jobStr) +\
		sizeof(stoleFromStr) +\
		sizeof(format)

	char buf[PIX_THREAD_LOG_ENTRY_MAX_LEN + 1];
	written = snprintf(
		buf,
		PIX_THREAD_LOG_ENTRY_MAX_LEN,
		format,
		pEntry->timeS, pEntry->timeNs,
		thread, 
		actionStr[pEntry->action],
		listJob[pEntry->action] ? jobStr : "",
		(PixthLogAction)pEntry->action == PIX_THREAD_ACTION_STEAL ? stoleFromStr : ""
	);
	PIX_ERR_ASSERT("", written > 0 && written < PIX_THREAD_LOG_ENTRY_MAX_LEN);

	PIXALC_DYN_ARR_RESIZE(int8_t, pAlloc, pLog, pLog->count + written + 1);
	memcpy(pLog->pArr + pLog->count, buf, written + 1);
	pLog->count += written;
#endif
	return err;
}

PixErr pixthThreadPoolLogDump(PixthPoolCtx *pCtx, PixtyI8Arr *pLog) {
	PixErr err = PIX_ERR_SUCCESS;
	PIX_ERR_RETURN_IFNOT_COND(err, pCtx->logging, "can't dump log, logging is disabled");
	PixalcFPtrs *pAlloc = pCtx->core.pAlloc;
	*pLog = (PixtyI8Arr){0};
	pCtx->paused = 1;
	err = logAction(pCtx->args, NULL, PIX_THREAD_ACTION_PAUSE, 0);
	PIX_ERR_RETURN_IFNOT(err, "");
	pCtx->directive = THREAD_PAUSE;
	do {
		PIX_ERR_ASSERT("", pCtx->paused <= pCtx->threadCount);
	} while(pCtx->paused < pCtx->threadCount);

	I32 logPtr[PIXTH_MAX_THREADS] = {0};
	I32 written = 0;
	do {
		I32 thread = 0;
		PixthLogEntry *pActive = NULL;
		for (I32 i = 0; i < pCtx->threadCount; ++i) {
			PixthLogEntry *pEntry = pCtx->args[i].log.pArr + logPtr[i];
			if (logPtr[i] == pCtx->args[i].log.count) {
				continue;
			}
			PIX_ERR_ASSERT("", logPtr[i] < pCtx->args[i].log.count);
			if (pActive && (
					pEntry->timeS > pActive->timeS ||
					pEntry->timeS == pActive->timeS && pEntry->timeNs >= pActive->timeNs
			)) {
				continue;
			}
			pActive = pCtx->args[i].log.pArr + logPtr[i];
			thread = i;
		}
		if (!pActive) {
			break;
		}
		printLogEntry(pAlloc, pLog, pActive, thread);
		++logPtr[thread];
		++written;
	} while(true);
	I32 entryTotal = 0;
	for (I32 i = 0; i < pCtx->threadCount; ++i) {
		entryTotal += pCtx->args[i].log.count;
	}
	PIX_ERR_ASSERT("", written == entryTotal);

	pCtx->paused = 0;
	pCtx->directive = THREAD_WAKE;
	return err;
}

void pixthThreadPoolDestroy(PixthPoolCtx *pCtx) {
	if (pCtx->threadCount > 0) {
		PIX_ERR_ASSERT("", pCtx->threadCount != 1);
		pCtx->directive = THREAD_STOP;
	}
	PixalcFPtrs alloc = *pCtx->core.pAlloc;
	PixalcFPtrs *pAlloc = &alloc;
	pixthPlatformDestroy(pCtx->core.pPlatform, pCtx->threadCount - 1);

	PixtyI8Arr log = {0};
	*pCtx = (PixthPoolCtx){0};
}
