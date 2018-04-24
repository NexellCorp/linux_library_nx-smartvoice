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

#ifndef __BUFFERMANAGER_H__
#define __BUFFERMANAGER_H__

#include <NXQueue.h>
using namespace android;

struct DataBuffer {
	int size;
	char *buf;
	char *bufUser;
};

struct DoneBuffer {
	DataBuffer *pcmBuffer;
	DataBuffer *pcmBuffer2;
	DataBuffer *refBuffer;
};

class BufferManager
{
public:
	BufferManager();
	virtual ~BufferManager();

	void Init(int pcmBufSize, int pcmBufSize2, int refBufSize, int outBufSize);
	DataBuffer *getPcmBuffer();
	void putPcmBuffer(DataBuffer *b);
	DataBuffer *getPcmBuffer2();
	void putPcmBuffer2(DataBuffer *b);
	DataBuffer *getRefBuffer();
	void putRefBuffer(DataBuffer *b);
	bool getRefFreeSync();
	DoneBuffer *getDoneBuffer();
	DoneBuffer *getDoneBufferNoLock();
	void putDoneBuffer(DoneBuffer *b);
	DataBuffer *getOutBuffer();
	void putOutBuffer(DataBuffer *b);
	DataBuffer *getDoneOutBuffer();
	void putDoneOutBuffer(DataBuffer *b);
	void printQStatus();

	/* client buffer */
	void queueClientFreeBuffer(DataBuffer *b);
	DataBuffer *dequeueClientFreeBuffer();
	void queueClientDoneBuffer(DataBuffer *b);
	DataBuffer *dequeueClientDoneBuffer();

private:
	int PcmBufSize;
	int PcmBufSize2;
	int RefBufSize;
	int OutBufSize;

	NXQueue<DataBuffer *> PcmFreeQ;
	NXQueue<DataBuffer *> PcmDoneQ;
	NXQueue<DataBuffer *> PcmFreeQ2;
	NXQueue<DataBuffer *> PcmDoneQ2;
	NXQueue<DataBuffer *> RefFreeQ;
	NXQueue<DataBuffer *> RefDoneQ;
	NXQueue<DataBuffer *> OutFreeQ;
	NXQueue<DataBuffer *> OutDoneQ;
	/* client buffer */
	NXQueue<DataBuffer *> ClientFreeQ;
	NXQueue<DataBuffer *> ClientDoneQ;

	pthread_mutex_t Mutex;
	pthread_cond_t Cond;
	pthread_mutex_t OutMutex;
	pthread_cond_t OutCond;
	pthread_mutex_t ClientMutex;
	pthread_cond_t ClientCond;
};

#endif
