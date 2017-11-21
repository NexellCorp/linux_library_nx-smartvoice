#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>

#include <queue>
#include <utils/Mutex.h>

#include <tinyalsa/asoundlib.h>

#include "nx_pdm.h"
#include "resample.h"
#include "pvo_wrapper.h"

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

	void Init(int pcmBufSize, int refBufSize);
	DataBuffer *getPcmBuffer();
	void putPcmBuffer(DataBuffer *b);
	DataBuffer *getRefBuffer();
	void putRefBuffer(DataBuffer *b);
	DoneBuffer *getDoneBuffer();
	void putDoneBuffer(DoneBuffer *b);
	void printQStatus();

private:
	int PcmBufSize;
	int RefBufSize;

	NXQueue<DataBuffer *> PcmFreeQ;
	NXQueue<DataBuffer *> PcmDoneQ;
	NXQueue<DataBuffer *> RefFreeQ;
	NXQueue<DataBuffer *> RefDoneQ;

	pthread_mutex_t Mutex;
	pthread_cond_t Cond;
};

void BufferManager::printQStatus()
{
	printf("PcmFreeQ: %d\n", PcmFreeQ.size());
	printf("PcmDoneQ: %d\n", PcmDoneQ.size());
	printf("RefFreeQ: %d\n", RefFreeQ.size());
	printf("RefDoneQ: %d\n", RefDoneQ.size());
}

BufferManager::BufferManager()
{
	pthread_mutex_init(&Mutex, NULL);
	pthread_cond_init(&Cond, NULL);
}

BufferManager::~BufferManager()
{
	// TODO: free buffers
}

//#define BUFFER_COUNT	16
#define BUFFER_COUNT	64
void BufferManager::Init(int pcmBufSize, int refBufSize)
{
	PcmBufSize = pcmBufSize;
	RefBufSize = refBufSize;

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

}

using namespace android;

static void *thread_pdm(void *arg)
{
	BufferManager *manager = (BufferManager *)arg;

	struct pcm_config config;
	struct pcm *pcm;
	pdm_STATDEF pdm_st;
	pdm_Init(&pdm_st);

	memset(&config, 0, sizeof(config));
	/* TODO */
	config.channels = 4;
	config.rate = 64000;
	config.period_size = 512;
	config.period_count = 16;
	config.format = PCM_FORMAT_S16_LE;
	config.start_threshold = 0;
	config.stop_threshold = 0;
	config.silence_threshold = 0;

	pcm = pcm_open(0, 2, PCM_IN, &config);
	if (!pcm || !pcm_is_ready(pcm)) {
		fprintf(stderr, "%s: unable_to open PCM device(%s)\n",
			__func__, pcm_get_error(pcm));
		return NULL;
	}

	int size = pcm_frames_to_bytes(pcm, pcm_get_buffer_size(pcm));
	printf("%s: buffer size %d\n", __func__, size);
	printf("%s: pcm_get_buffer_size %d\n", __func__, pcm_get_buffer_size(pcm));

	size = 8192;
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

		// TODO
		pdm_Run(&pdm_st, (short int*)outBuffer->buf, (int *)buffer, 0);
		manager->putPcmBuffer(outBuffer);
	}

	free(buffer);
	pcm_close(pcm);
	return NULL;
}

static void *thread_ref(void *arg)
{
	BufferManager *manager = (BufferManager *)arg;

	// TODO
	struct ReSampleContext *rctx = audio_resample_init(2, 2, 16000, 48000);

	struct pcm_config config;
	struct pcm *pcm;

	memset(&config, 0, sizeof(config));
	/* TODO */
	config.channels = 2;
	config.rate = 48000;
	config.period_size = 1024;
	config.period_count = 16;
	config.format = PCM_FORMAT_S16_LE;
	config.start_threshold = 0;
	config.stop_threshold = 0;
	config.silence_threshold = 0;

	pcm = pcm_open(0, 1, PCM_IN, &config);
	if (!pcm || !pcm_is_ready(pcm)) {
		fprintf(stderr, "%s: unable_to open PCM device(%s)\n",
			__func__, pcm_get_error(pcm));
		return NULL;
	}

	int size = pcm_frames_to_bytes(pcm, pcm_get_buffer_size(pcm));
	printf("%s: buffer size %d\n", __func__, size);
	printf("%s: pcm_get_buffer_size %d\n", __func__, pcm_get_buffer_size(pcm));

	size = 3072;
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

	return NULL;
}

// #define USE_PCM_FEEDBACK
static void *thread_feedback(void *arg)
{
	BufferManager *manager = (BufferManager *)arg;

	pvo_init();

#ifdef USE_PCM_FEEDBACK
	struct pcm_config config;
	struct pcm *pcm;

	memset(&config, 0, sizeof(config));
	/* TODO */
	config.channels = 1;
	config.rate = 16000;
	config.period_size = 256;
	config.period_count = 16;
	config.format = PCM_FORMAT_S16_LE;
	config.start_threshold = 0;
	config.stop_threshold = 0;
	config.silence_threshold = 0;

	pcm = pcm_open(0, 3, PCM_OUT, &config);
	if (!pcm || !pcm_is_ready(pcm)) {
		fprintf(stderr, "%s: unable_to open PCM device(%s)\n",
			__func__, pcm_get_error(pcm));
		return NULL;
	}

	int size = pcm_frames_to_bytes(pcm, pcm_get_buffer_size(pcm));
	printf("%s: buffer size %d\n", __func__, size);
	printf("%s: pcm_get_buffer_size %d\n", __func__, pcm_get_buffer_size(pcm));

	size = 512;
#else
	int size = 512;
#endif
	char *outBuffer = (char *)malloc(size);
	DoneBuffer *inBuffer = NULL;

	int ret;
	while (1) {
		inBuffer = manager->getDoneBuffer();

		ret = pvo_process((short *)inBuffer->pcmBuffer->buf,
				  (short *)outBuffer,
				  (short *)inBuffer->refBuffer->buf);
		if (ret) {
			fprintf(stderr, "%s: failed to pvo_process(ret: %d)\n",
				__func__, ret);
			break;
		}

		ret = PoVoGateSource(256, (short *)outBuffer);
		if (ret)
			fprintf(stderr, "%s: failed to PoVoGateSource(ret: %d)\n",
				__func__, ret);

#ifdef USE_PCM_FEEDBACK
		ret = pcm_write(pcm, outBuffer, size);
		if (ret) {
			fprintf(stderr, "%s: failed to pcm_write\n", __func__);
			break;
		}
#endif
		manager->putDoneBuffer(inBuffer);
	}

	free(outBuffer);
#ifdef USE_PCM_FEEDBACK
	pcm_close(pcm);
	pvo_deinit_bss();
#endif

	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t tid[3];
	BufferManager *bufManager = new BufferManager();

	bufManager->Init(2048, 1024);

	pthread_create(&tid[1], NULL, thread_ref, (void *)bufManager);
	pthread_create(&tid[2], NULL, thread_feedback, (void *)bufManager);
	pthread_create(&tid[0], NULL, thread_pdm, (void *)bufManager);

	while(1);
	// delete bufManager;

	return 0;
}

