/*
 * Copyright (C) 2017  Nexell Co., Ltd.
 * Author: Sungwoo, Park <swpark@nexell.co.kr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <android/log.h>
#include "buffermanager.h"

#define LOGI(fmt, args...)      __android_log_print(ANDROID_LOG_INFO , TAG, fmt, ##args)
#define LOGD(fmt, args...)      __android_log_print(ANDROID_LOG_DEBUG, TAG, fmt, ##args)
#define LOGW(fmt, args...)      __android_log_print(ANDROID_LOG_WARN , TAG, fmt, ##args)
#define LOGE(fmt, args...)      __android_log_print(ANDROID_LOG_ERROR, TAG, fmt, ##args)

//#define TRACE_BUFFER
#ifdef TRACE_BUFFER
#define tr_b(a...) LOGE(a)
#else
#define tr_b(a...)
#endif

static const char * TAG         = "SVOICE";

BufferManager::BufferManager()
{
	pthread_mutex_init(&Mutex, NULL);
	pthread_cond_init(&Cond, NULL);
	pthread_mutex_init(&OutMutex, NULL);
	pthread_cond_init(&OutCond, NULL);
	pthread_mutex_init(&ClientMutex, NULL);
	pthread_cond_init(&ClientCond, NULL);
}

BufferManager::~BufferManager()
{
	DataBuffer *b;

	while (!PcmFreeQ.isEmpty()) {
		b = PcmFreeQ.dequeue();
		free(b->buf);
		free(b);
	}

	while (!PcmDoneQ.isEmpty()) {
		b = PcmDoneQ.dequeue();
		free(b->buf);
		free(b);
	}

	if (PcmBufSize2) {
		while (!PcmFreeQ2.isEmpty()) {
			b = PcmFreeQ2.dequeue();
			free(b->buf);
			free(b);
		}

		while (!PcmDoneQ2.isEmpty()) {
			b = PcmDoneQ2.dequeue();
			free(b->buf);
			free(b);
		}
	}

	while (!RefFreeQ.isEmpty()) {
		b = RefFreeQ.dequeue();
		free(b->buf);
		free(b);
	}

	while (!RefDoneQ.isEmpty()) {
		b = RefDoneQ.dequeue();
		free(b->buf);
		free(b);
	}

	if (OutBufSize == 0)
		return;

	while (!OutFreeQ.isEmpty()) {
		b = OutFreeQ.dequeue();
		free(b->buf);
		free(b);
	}

	while (!OutDoneQ.isEmpty()) {
		b = OutDoneQ.dequeue();
		free(b->buf);
		free(b);
	}
}

void BufferManager::printQStatus()
{
	LOGD("PcmFreeQ: %d\n", PcmFreeQ.size());
	LOGD("PcmDoneQ: %d\n", PcmDoneQ.size());
	if (PcmBufSize2) {
		LOGD("PcmFreeQ2: %d\n", PcmFreeQ2.size());
		LOGD("PcmDoneQ2: %d\n", PcmDoneQ2.size());
	}
	LOGD("RefFreeQ: %d\n", RefFreeQ.size());
	LOGD("RefDoneQ: %d\n", RefDoneQ.size());
	if (OutBufSize == 0)
		return;
	LOGD("OutFreeQ: %d\n", OutFreeQ.size());
	LOGD("OutDoneQ: %d\n", OutDoneQ.size());
}

#define BUFFER_COUNT	64
void BufferManager::Init(int pcmBufSize, int pcmBufSize2, int refBufSize, int outBufSize = 0)
{
	PcmBufSize = pcmBufSize;
	PcmBufSize2 = pcmBufSize2;
	RefBufSize = refBufSize;
	OutBufSize = outBufSize;

	/* allocate pcmbuffer */
	for (int i = 0; i < BUFFER_COUNT; i++) {
		DataBuffer *b = new DataBuffer();
		b->size = PcmBufSize;
		b->buf = (char *)malloc(PcmBufSize);
		b->bufUser = NULL;
		PcmFreeQ.queue(b);
	}

	if (PcmBufSize2) {
		/* allocate pcmbuffer2 */
		for (int i = 0; i < BUFFER_COUNT; i++) {
			DataBuffer *b = new DataBuffer();
			b->size = PcmBufSize2;
			b->buf = (char *)malloc(PcmBufSize2);
			b->bufUser = NULL;
			PcmFreeQ2.queue(b);
		}
	}

	/* allocate refbuffer */
	for (int i = 0; i < BUFFER_COUNT; i++) {
		DataBuffer *b = new DataBuffer();
		b->size = RefBufSize;
		b->buf = (char *)malloc(RefBufSize);
		b->bufUser = NULL;
		RefFreeQ.queue(b);
	}

	if (OutBufSize == 0)
		return;

	for (int i = 0; i < BUFFER_COUNT; i++) {
		DataBuffer *b = new DataBuffer();
		b->size = OutBufSize;
		b->buf = (char *)malloc(OutBufSize);
		b->bufUser = (char *)malloc(OutBufSize);
		OutFreeQ.queue(b);
	}
}

DataBuffer *BufferManager::getPcmBuffer()
{
	if (PcmFreeQ.isEmpty())
		return NULL;

	tr_b("%s: PcmFreeQ count %d\n", __func__, PcmFreeQ.size());
	return PcmFreeQ.dequeue();
}

void BufferManager::putPcmBuffer(DataBuffer *b)
{
	PcmDoneQ.queue(b);
	tr_b("%s: PcmDoneQ count %d\n", __func__, PcmDoneQ.size());
	pthread_mutex_lock(&Mutex);
	pthread_cond_signal(&Cond);
	pthread_mutex_unlock(&Mutex);
}

DataBuffer *BufferManager::getPcmBuffer2()
{
	if (PcmFreeQ2.isEmpty())
		return NULL;

	tr_b("%s: PcmFreeQ2 count %d\n", __func__, PcmFreeQ2.size());
	return PcmFreeQ2.dequeue();
}

void BufferManager::putPcmBuffer2(DataBuffer *b)
{
	PcmDoneQ2.queue(b);
	tr_b("%s: PcmDoneQ2 count %d\n", __func__, PcmDoneQ2.size());
	pthread_mutex_lock(&Mutex);
	pthread_cond_signal(&Cond);
	pthread_mutex_unlock(&Mutex);
}

DataBuffer *BufferManager::getRefBuffer()
{
	if (RefFreeQ.isEmpty())
		return NULL;

	tr_b("%s: RefFreeQ count %d\n", __func__, RefFreeQ.size());
	return RefFreeQ.dequeue();
}

void BufferManager::putRefBuffer(DataBuffer *b)
{
	RefDoneQ.queue(b);
	tr_b("%s: RefDoneQ count %d\n", __func__, RefDoneQ.size());
	pthread_mutex_lock(&Mutex);
	pthread_cond_signal(&Cond);
	pthread_mutex_unlock(&Mutex);
}

bool BufferManager::getRefFreeSync()
{
	size_t refFreeQSize;

	refFreeQSize = RefFreeQ.size();
	tr_b("%s: RefFreeQ count %d\n", __func__, RefFreeQ.size());

	if ((refFreeQSize < PcmFreeQ.size()) ||
	 (PcmBufSize2 && (refFreeQSize < PcmFreeQ2.size())))
		return false;
	else
		return true;
}

DoneBuffer *BufferManager::getDoneBuffer()
{
	DoneBuffer *b = new DoneBuffer();

	while (PcmDoneQ.isEmpty() ||
		(PcmBufSize2 && PcmDoneQ2.isEmpty()) ||
	       RefDoneQ.isEmpty()) {
		pthread_mutex_lock(&Mutex);
		pthread_cond_wait(&Cond, &Mutex);
		pthread_mutex_unlock(&Mutex);
	}

	b->pcmBuffer = PcmDoneQ.dequeue();
	if (PcmBufSize2)
		b->pcmBuffer2 = PcmDoneQ2.dequeue();
	b->refBuffer = RefDoneQ.dequeue();

	return b;
}

DoneBuffer *BufferManager::getDoneBufferNoLock()
{
	if (PcmDoneQ.size() && (PcmBufSize2 && PcmDoneQ2.size()) && RefDoneQ.size()) {
		DoneBuffer *b = new DoneBuffer();

		b->pcmBuffer = PcmDoneQ.dequeue();
		if (PcmBufSize2)
			b->pcmBuffer2 = PcmDoneQ2.dequeue();
		b->refBuffer = RefDoneQ.dequeue();
		return b;
	}

	return NULL;
}

void BufferManager::putDoneBuffer(DoneBuffer *b)
{
	PcmFreeQ.queue(b->pcmBuffer);
	if (PcmBufSize2)
		PcmFreeQ2.queue(b->pcmBuffer2);
	RefFreeQ.queue(b->refBuffer);
	delete b;
}

DataBuffer *BufferManager::getOutBuffer()
{
	if (OutFreeQ.isEmpty())
		return NULL;

	tr_b("%s: OutFreeQ count %d\n", __func__, OutFreeQ.size());
	return OutFreeQ.dequeue();
}

void BufferManager::putOutBuffer(DataBuffer *b)
{
	OutDoneQ.queue(b);
	tr_b("%s: OutDoneQ count %d\n", __func__, OutDoneQ.size());
	pthread_mutex_lock(&OutMutex);
	pthread_cond_signal(&OutCond);
	pthread_mutex_unlock(&OutMutex);
}

DataBuffer *BufferManager::getDoneOutBuffer()
{
	while (OutDoneQ.isEmpty()) {
		pthread_mutex_lock(&OutMutex);
		pthread_cond_wait(&OutCond, &OutMutex);
		pthread_mutex_unlock(&OutMutex);
	}

	return OutDoneQ.dequeue();
}

void BufferManager::putDoneOutBuffer(DataBuffer *b)
{
	OutFreeQ.queue(b);
}

void BufferManager::queueClientFreeBuffer(DataBuffer *b)
{
	ClientFreeQ.queue(b);
}

DataBuffer *BufferManager::dequeueClientFreeBuffer()
{
	if (ClientFreeQ.isEmpty())
		return NULL;
	return ClientFreeQ.dequeue();
}

void BufferManager::queueClientDoneBuffer(DataBuffer *b)
{
	ClientDoneQ.queue(b);
	pthread_mutex_lock(&ClientMutex);
	pthread_cond_signal(&ClientCond);
	pthread_mutex_unlock(&ClientMutex);
}

#ifndef TIMEVAL_TO_TIMESPEC
#define  TIMEVAL_TO_TIMESPEC(tv, ts) \
	do { \
		(ts)->tv_sec = (tv)->tv_sec; \
		(ts)->tv_nsec = (tv)->tv_usec * 1000; \
	} while (0)
#endif
#ifndef TIMESPEC_TO_TIMEVAL
#define	TIMESPEC_TO_TIMEVAL(tv, ts) \
	do { \
		(tv)->tv_sec = (ts)->tv_sec; \
		(tv)->tv_usec = (ts)->tv_nsec / 1000; \
	} while (0)
#endif

#define MS_TO_NS(ms)	(ms * 1000 * 1000)
DataBuffer *BufferManager::dequeueClientDoneBuffer()
{
#if 0
	if (ClientDoneQ.isEmpty()) {
		struct timespec ts;
		struct timeval tv;
		int ret;

		gettimeofday(&tv, NULL);

		TIMEVAL_TO_TIMESPEC(&tv, &ts);
		// ts.tv_nsec += MS_TO_NS(16);
		ts.tv_nsec += MS_TO_NS(20);
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_mutex_lock(&ClientMutex);
		ret = pthread_cond_timedwait(&ClientCond, &ClientMutex, &ts);
		pthread_mutex_unlock(&ClientMutex);

		if (ret == ETIMEDOUT)
			return NULL;
	}
#else
	while (ClientDoneQ.isEmpty()) {
		pthread_mutex_lock(&ClientMutex);
		pthread_cond_wait(&ClientCond, &ClientMutex);
		pthread_mutex_unlock(&ClientMutex);
	}
#endif

	return ClientDoneQ.dequeue();
}
