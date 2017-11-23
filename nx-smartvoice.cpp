#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>

#include <queue>
#include <utils/Mutex.h>

#include <tinyalsa/asoundlib.h>

#include "nx_pdm.h"
#include "resample.h"
#include "pvpre.h"

namespace android {

//#define TRACE_BUFFER
#ifdef TRACE_BUFFER
#define tr_b(a...) printf(a)
#else
#define tr_b(a...)
#endif

template <class T>
class NXQueue
{
public:
    NXQueue() {
    };

    virtual ~NXQueue() {
    }

    void queue(const T& item) {
		Mutex::Autolock l(mLock);
		q.push(item);
    }

    const T& dequeue() {
		Mutex::Autolock l(mLock);
        const T& item = q.front();
        q.pop();
        return item;
    }

    bool isEmpty() {
		Mutex::Autolock l(mLock);
        return q.empty();
    }

    size_t size() {
		Mutex::Autolock l(mLock);
        return q.size();
    }

    const T& getHead() {
		Mutex::Autolock l(mLock);
        return q.front();
    }

private:
	std::queue<T> q;
	Mutex mLock;
};

struct DataBuffer {
	int size;
	char *buf;
};

struct DoneBuffer {
	DataBuffer *pcmBuffer;
	DataBuffer *refBuffer;
};

class BufferManager
{
public:
	BufferManager();
	virtual ~BufferManager();

	void Init(int pcmBufSize, int refBufSize, int outBufSize);
	DataBuffer *getPcmBuffer();
	void putPcmBuffer(DataBuffer *b);
	DataBuffer *getRefBuffer();
	void putRefBuffer(DataBuffer *b);
	DoneBuffer *getDoneBuffer();
	void putDoneBuffer(DoneBuffer *b);
	DataBuffer *getOutBuffer();
	void putOutBuffer(DataBuffer *b);
	DataBuffer *getDoneOutBuffer();
	void putDoneOutBuffer(DataBuffer *b);
	void printQStatus();

private:
	int PcmBufSize;
	int RefBufSize;
	int OutBufSize;

	NXQueue<DataBuffer *> PcmFreeQ;
	NXQueue<DataBuffer *> PcmDoneQ;
	NXQueue<DataBuffer *> RefFreeQ;
	NXQueue<DataBuffer *> RefDoneQ;
	NXQueue<DataBuffer *> OutFreeQ;
	NXQueue<DataBuffer *> OutDoneQ;

	pthread_mutex_t Mutex;
	pthread_cond_t Cond;
	pthread_mutex_t OutMutex;
	pthread_cond_t OutCond;
};

void BufferManager::printQStatus()
{
	printf("PcmFreeQ: %d\n", PcmFreeQ.size());
	printf("PcmDoneQ: %d\n", PcmDoneQ.size());
	printf("RefFreeQ: %d\n", RefFreeQ.size());
	printf("RefDoneQ: %d\n", RefDoneQ.size());
	printf("OutFreeQ: %d\n", OutFreeQ.size());
	printf("OutDoneQ: %d\n", OutDoneQ.size());
}

BufferManager::BufferManager()
{
	pthread_mutex_init(&Mutex, NULL);
	pthread_cond_init(&Cond, NULL);
	pthread_mutex_init(&OutMutex, NULL);
	pthread_cond_init(&OutCond, NULL);
}

BufferManager::~BufferManager()
{
	// TODO: free buffers
}

//#define BUFFER_COUNT	16
#define BUFFER_COUNT	64
void BufferManager::Init(int pcmBufSize, int refBufSize, int outBufSize = 0)
{
	PcmBufSize = pcmBufSize;
	RefBufSize = refBufSize;
	OutBufSize = outBufSize;

	/* allocate pcmbuffer */
	for (int i = 0; i < BUFFER_COUNT; i++) {
		DataBuffer *b = new DataBuffer();
		b->size = PcmBufSize;
		b->buf = (char *)malloc(PcmBufSize);
		PcmFreeQ.queue(b);
	}

	/* allocate refbuffer */
	for (int i = 0; i < BUFFER_COUNT; i++) {
		DataBuffer *b = new DataBuffer();
		b->size = RefBufSize;
		b->buf = (char *)malloc(RefBufSize);
		RefFreeQ.queue(b);
	}

	if (outBufSize == 0)
		return;

	for (int i = 0; i < BUFFER_COUNT; i++) {
		DataBuffer *b = new DataBuffer();
		b->size = OutBufSize;
		b->buf = (char *)malloc(OutBufSize);
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
	pthread_cond_signal(&Cond);
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
	pthread_cond_signal(&Cond);
}

DoneBuffer *BufferManager::getDoneBuffer()
{
	DoneBuffer *b = new DoneBuffer();

	while (PcmDoneQ.isEmpty() || RefDoneQ.isEmpty()) {
		pthread_cond_wait(&Cond, &Mutex);
		pthread_mutex_unlock(&Mutex);
	}

	b->pcmBuffer = PcmDoneQ.dequeue();
	b->refBuffer = RefDoneQ.dequeue();

	return b;
}

void BufferManager::putDoneBuffer(DoneBuffer *b)
{
	PcmFreeQ.queue(b->pcmBuffer);
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
	pthread_cond_signal(&OutCond);
}

DataBuffer *BufferManager::getDoneOutBuffer()
{
	while (OutDoneQ.isEmpty()) {
		pthread_cond_wait(&OutCond, &OutMutex);
		pthread_mutex_unlock(&OutMutex);
	}

	return OutDoneQ.dequeue();
}

void BufferManager::putDoneOutBuffer(DataBuffer *b)
{
	OutFreeQ.queue(b);
}

}

using namespace android;

#define USE_PCM_FEEDBACK

static void print_thread_info(const char *name, struct pcm_config *c,
							  int unit_size)
{
	printf("==================================\n");
	printf("start thread %s\n", name);
	if (c) {
		printf("pcm config info\n");
		printf("channel: %d\n", c->channels);
		printf("rate: %d\n", c->rate);
		printf("period size: %d\n", c->period_size);
		printf("period count: %d\n", c->period_count);
	}
	printf("unit size: %d\n", unit_size);
	printf("==================================\n");
	printf("\n");
}

static void *thread_pdm(void *arg)
{
	BufferManager *manager = (BufferManager *)arg;

	struct pcm_config config;
	struct pcm *pcm;
	pdm_STATDEF pdm_st;
	pdm_Init(&pdm_st);

	memset(&config, 0, sizeof(config));
	config.channels = 4;
	config.rate = 64000;
	config.period_size = 512;
	config.period_count = 16;
	config.format = PCM_FORMAT_S16_LE;
	config.start_threshold = 0;
	config.stop_threshold = 0;
	config.silence_threshold = 0;

	print_thread_info("pdm", &config, 8192);

	pcm = pcm_open(0, 2, PCM_IN, &config);
	if (!pcm || !pcm_is_ready(pcm)) {
		fprintf(stderr, "%s: unable_to open PCM device(%s)\n",
			__func__, pcm_get_error(pcm));
		return NULL;
	}

	int size = 8192;
	char *buffer = (char *)malloc(size);
	DataBuffer *outBuffer = NULL;

	int ret;

	if (0 != pdm_SetParam(&pdm_st, PDM_PARAM_GAIN, 0))
		printf("failed: pdm gain parameter [%d]", 0);

	while (1) {
		ret = pcm_read(pcm, buffer, size/2);
		if (ret) {
			fprintf(stderr, "%s: failed to pcm_read\n", __func__);
			break;
		}

		ret = pcm_read(pcm, buffer + size/2, size/2);
		if (ret) {
			fprintf(stderr, "%s: failed to pcm_read\n", __func__);
			break;
		}

		outBuffer = manager->getPcmBuffer();
		if (!outBuffer) {
			fprintf(stderr, "%s: failed to getPcmBuffer\n", __func__);
			manager->printQStatus();
			break;
		}

		pdm_Run(&pdm_st, (short int*)outBuffer->buf, (int *)buffer, 0);
		manager->putPcmBuffer(outBuffer);
	}

	free(buffer);
	pcm_close(pcm);

	printf("Exit %s\n", __func__);

	return NULL;
}

static void *thread_ref(void *arg)
{
	BufferManager *manager = (BufferManager *)arg;

	struct ReSampleContext *rctx = audio_resample_init(1, 2, 16000, 48000);

	struct pcm_config config;
	struct pcm *pcm;

	memset(&config, 0, sizeof(config));
	config.channels = 2;
	config.rate = 48000;
	config.period_size = 1024;
	config.period_count = 16;
	config.format = PCM_FORMAT_S16_LE;
	config.start_threshold = 0;
	config.stop_threshold = 0;
	config.silence_threshold = 0;

	print_thread_info("reference", &config, 3072);

	pcm = pcm_open(0, 1, PCM_IN, &config);
	if (!pcm || !pcm_is_ready(pcm)) {
		fprintf(stderr, "%s: unable_to open PCM device(%s)\n",
			__func__, pcm_get_error(pcm));
		return NULL;
	}

	int size = 3072;
	char *buffer = (char *)malloc(size);
	DataBuffer *outBuffer = NULL;

	int ret;
	while (1) {
		ret = pcm_read(pcm, buffer, size);
		if (ret) {
			fprintf(stderr, "%s: failed to pcm_read\n", __func__);
			break;
		}

		outBuffer = manager->getRefBuffer();
		if (!outBuffer) {
			fprintf(stderr, "%s: failed to getRefBuffer\n", __func__);
			manager->printQStatus();
			break;
		}

		audio_resample(rctx, (short *)outBuffer->buf, (short *)buffer, size/4);
		manager->putRefBuffer(outBuffer);
	}

	free(buffer);
	pcm_close(pcm);
	audio_resample_close(rctx);

	printf("Exit %s\n", __func__);

	return NULL;
}

static void *thread_ecnr(void *arg)
{
	BufferManager *manager = (BufferManager *)arg;

	PVPRE_Init();

	int size = 512;
	char *tmpBuffer = (char *)malloc(size);
	DoneBuffer *inBuffer = NULL;

	print_thread_info("ecnr", NULL, size);

#ifdef USE_PCM_FEEDBACK
	DataBuffer *outBuffer;
#endif

	int ret;
	while (1) {
		inBuffer = manager->getDoneBuffer();
#ifdef USE_PCM_FEEDBACK
		outBuffer = manager->getOutBuffer();
		if (!outBuffer)
			fprintf(stderr, "%s: overrun outBuffer!!!\n", __func__);
#endif

#ifdef USE_PCM_FEEDBACK
		if (outBuffer != NULL)
			ret = PVPRE_Process_4ch((short *)inBuffer->pcmBuffer->buf,
									(short *)inBuffer->refBuffer->buf,
									(short *)outBuffer->buf,
									1);
		else
			ret = PVPRE_Process_4ch((short *)inBuffer->pcmBuffer->buf,
									(short *)inBuffer->refBuffer->buf,
									(short *)tmpBuffer,
									1);
#else
		ret = PVPRE_Process_4ch((short *)inBuffer->pcmBuffer->buf,
								(short *)inBuffer->refBuffer->buf,
								(short *)tmpBuffer,
								1);
#endif
		if (ret == 1)
			printf("Detect Keyword\n");

#ifndef USE_PCM_FEEDBACK
		ret = PoVoGateSource(256, (short *)tmpBuffer);
		if (ret)
			fprintf(stderr, "%s: failed to PoVoGateSource(ret: %d)\n",
				__func__, ret);
#endif

		manager->putDoneBuffer(inBuffer);

#ifdef USE_PCM_FEEDBACK
		if (outBuffer != NULL)
			manager->putOutBuffer(outBuffer);
#endif
	}

	free(tmpBuffer);

	PVPRE_Close();

	printf("Exit %s\n", __func__);

	return NULL;
}

#ifdef USE_PCM_FEEDBACK
static void *thread_feedback(void *arg)
{
	BufferManager *manager = (BufferManager *)arg;

	struct pcm_config config;
	struct pcm *pcm;

	memset(&config, 0, sizeof(config));
	config.channels = 1;
	config.rate = 16000;
	config.period_size = 256;
	config.period_count = 16;
	config.format = PCM_FORMAT_S16_LE;
	config.start_threshold = 0;
	config.stop_threshold = 0;
	config.silence_threshold = 0;

	print_thread_info("feedback", &config, 512);

	pcm = pcm_open(0, 3, PCM_OUT, &config);
	if (!pcm || !pcm_is_ready(pcm)) {
		fprintf(stderr, "%s: unable_to open PCM device(%s)\n",
			__func__, pcm_get_error(pcm));
		return NULL;
	}

	int size = 512;
	DataBuffer *outBuffer = NULL;
	int ret;

	while (1) {
		outBuffer = manager->getDoneOutBuffer();
		ret = pcm_write(pcm, outBuffer->buf, outBuffer->size);
		if (ret) {
			fprintf(stderr, "%s: failed to pcm_write\n", __func__);
			break;
		}
		manager->putDoneOutBuffer(outBuffer);
	}

	pcm_close(pcm);

	return NULL;
}
#endif

int main(int argc __unused, char *argv[] __unused)
{
	pthread_t tid[4];
	BufferManager *bufManager = new BufferManager();

#ifdef USE_PCM_FEEDBACK
	// resample 2ch
	// bufManager->Init(2048, 1024, 512);
	// resample 1ch
	bufManager->Init(2048, 512, 512);
#else
	// resample 2ch
	// bufManager->Init(2048, 1024);
	// resample 1ch
	bufManager->Init(2048, 512);
#endif

	pthread_attr_t sched_attr;
	int fifo_max_prio;
	struct sched_param sched_param;

	pthread_attr_init(&sched_attr);
	pthread_attr_setschedpolicy(&sched_attr, SCHED_FIFO);
	fifo_max_prio = sched_get_priority_max(SCHED_FIFO);

	/* thread_ref, thread_pdm priority is max - 1 */
	sched_param.sched_priority = fifo_max_prio - 1;
	pthread_attr_setschedparam(&sched_attr, &sched_param);

	pthread_create(&tid[0], &sched_attr, thread_ref, (void *)bufManager);
	pthread_create(&tid[2], &sched_attr, thread_pdm, (void *)bufManager);

	/* thread_ecnr, thread_feedback priority is max */
	sched_param.sched_priority = fifo_max_prio;
	pthread_attr_setschedparam(&sched_attr, &sched_param);

	pthread_create(&tid[1], &sched_attr, thread_ecnr, (void *)bufManager);
#ifdef USE_PCM_FEEDBACK
	pthread_create(&tid[4], &sched_attr, thread_feedback, (void *)bufManager);
#endif

	while(1);
	// delete bufManager;

	return 0;
}

