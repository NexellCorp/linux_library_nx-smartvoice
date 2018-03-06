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

/* vim: set ts=4, set sw=4 */
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <android/log.h>
#include <tinyalsa/asoundlib.h>

#include "nx_pdm.h"
#include "resample.h"
// #include "pvpre.h"
#include "buffermanager.h"
#include "nx-smartvoice.h"

#define USE_PCM_FEEDBACK

#define BASE_INTERVAL_US		16000
#define SEC_TO_US(s)			(s*1000*1000)
#define MAX_THREAD_NUMBER		5
#define OUT_RATE				16000

#define PDM_RATE				64000
#define PDM_PERIOD_SIZE			512
#define PDM_PERIOD_COUNT		16
#define PDM_BITS				16

#define REF_RATE				48000
#define REF_PERIOD_SIZE			1024
#define REF_PERIOD_COUNT		16
#define REF_BITS				16

#define FEEDBACK_RATE			OUT_RATE
#define FEEDBACK_PERIOD_SIZE	256
#define FEEDBACK_PERIOD_COUNT	16
#define FEEDBACK_BITS			16

#define LOGI(fmt, args...)      __android_log_print(ANDROID_LOG_INFO , TAG, fmt, ##args)
#define LOGD(fmt, args...)      __android_log_print(ANDROID_LOG_DEBUG, TAG, fmt, ##args)
#define LOGW(fmt, args...)      __android_log_print(ANDROID_LOG_WARN , TAG, fmt, ##args)
#define LOGE(fmt, args...)      __android_log_print(ANDROID_LOG_ERROR, TAG, fmt, ##args)

static const char * TAG         = "POVO";
struct nx_voice_context {
	pthread_t tid[MAX_THREAD_NUMBER];
	BufferManager *bufManager;
	bool stop;
	struct nx_smartvoice_config config;
	int pdmUnitSize;
	int refUnitSize;
	int feedbackUnitSize;
	int pdmOutSize;
	int refOutSize;

	int pipe[2];
	bool clientWait;

	bool pdmExit;
	bool pdmExited;
	bool refExit;
	bool refExited;
	bool ecnrExit;
	bool ecnrExited;
	bool feedbackExit;
	bool feedbackExited;
};

static void print_thread_info(const char *name, struct pcm_config *c,
							  int unit_size)
{
	LOGD("==================================\n");
	LOGD("start thread %s\n", name);
	if (c) {
		LOGD("pcm config info\n");
		LOGD("channel: %d\n", c->channels);
		LOGD("rate: %d\n", c->rate);
		LOGD("period size: %d\n", c->period_size);
		LOGD("period count: %d\n", c->period_count);
	}
	LOGD("unit size: %d\n", unit_size);
	LOGD("==================================\n");
	LOGD("\n");
}

static int calcUnitSize(long interval_us, long rate, int bits, int channel_num)
{
	return ((interval_us * rate) / SEC_TO_US(1)) * (bits / 8) * channel_num;
}

static void *thread_pdm(void *arg)
{
	struct nx_voice_context *ctx = (struct nx_voice_context *)arg;
	BufferManager *manager = ctx->bufManager;

	struct pcm_config config;
	struct pcm *pcm;
	pdm_STATDEF pdm_st;
	pdm_Init(&pdm_st);

	memset(&config, 0, sizeof(config));
	config.channels = ctx->config.pdm_chnum;
	config.rate = PDM_RATE;
	config.period_size = PDM_PERIOD_SIZE;
	config.period_count = PDM_PERIOD_COUNT;
	config.format = PCM_FORMAT_S16_LE;
	config.start_threshold = 0;
	config.stop_threshold = 0;
	config.silence_threshold = 0;

	int unit_size = ctx->pdmUnitSize;
	print_thread_info("pdm", &config, unit_size);

	pcm = pcm_open(0, ctx->config.pdm_devnum, PCM_IN, &config);
	if (!pcm || !pcm_is_ready(pcm)) {
		LOGE("%s: unable_to open PCM device(%s)\n",
			 __func__, pcm_get_error(pcm));
		return NULL;
	}

	char *buffer = (char *)malloc(unit_size);
	DataBuffer *outBuffer = NULL;

	int ret;

	if (0 != pdm_SetParam(&pdm_st, PDM_PARAM_GAIN, ctx->config.pdm_gain))
		LOGE("failed: pdm gain parameter [%d]",
			 ctx->config.pdm_gain);

	while (!ctx->pdmExit) {
		ret = pcm_read(pcm, buffer, unit_size/2);
		if (ret) {
			LOGE("%s: failed to pcm_read\n", __func__);
			break;
		}

		ret = pcm_read(pcm, buffer + unit_size/2, unit_size/2);
		if (ret) {
			LOGE("%s: failed to pcm_read\n", __func__);
			break;
		}

		outBuffer = manager->getPcmBuffer();
		if (!outBuffer) {
			LOGE("%s: failed to getPcmBuffer\n", __func__);
			manager->printQStatus();
			break;
		}

		pdm_Run(&pdm_st, (short int*)outBuffer->buf, (int *)buffer, 0);
		manager->putPcmBuffer(outBuffer);
	}

	free(buffer);
	pcm_close(pcm);

	LOGD("Exit %s\n", __func__);

	ctx->pdmExited = true;
	pthread_exit(NULL);
}

static void *thread_ref(void *arg)
{
	struct nx_voice_context *ctx = (struct nx_voice_context *)arg;
	BufferManager *manager = ctx->bufManager;

	struct ReSampleContext *rctx =
		audio_resample_init(ctx->config.ref_resample_out_chnum, 2, OUT_RATE,
							REF_RATE);

	struct pcm_config config;
	struct pcm *pcm;

	memset(&config, 0, sizeof(config));
	config.channels = 2;
	config.rate = REF_RATE;
	config.period_size = REF_PERIOD_SIZE;
	config.period_count = REF_PERIOD_COUNT;
	config.format = PCM_FORMAT_S16_LE;
	config.start_threshold = 0;
	config.stop_threshold = 0;
	config.silence_threshold = 0;

	print_thread_info("reference", &config, ctx->refUnitSize);

	pcm = pcm_open(0, ctx->config.ref_devnum, PCM_IN, &config);
	if (!pcm || !pcm_is_ready(pcm)) {
		LOGE("%s: unable_to open PCM device(%s)\n",
			__func__, pcm_get_error(pcm));
		return NULL;
	}

	int size = ctx->refUnitSize;
	char *buffer = (char *)malloc(size);
	DataBuffer *outBuffer = NULL;

	int ret;
	while (!ctx->refExit) {
		ret = pcm_read(pcm, buffer, size);
		if (ret) {
			LOGE("%s: failed to pcm_read\n", __func__);
			break;
		}

		outBuffer = manager->getRefBuffer();
		if (!outBuffer) {
			LOGE("%s: failed to getRefBuffer\n", __func__);
			manager->printQStatus();
			break;
		}

		audio_resample(rctx, (short *)outBuffer->buf, (short *)buffer, size/4);
		manager->putRefBuffer(outBuffer);
	}

	free(buffer);
	pcm_close(pcm);
	audio_resample_close(rctx);

	LOGD("Exit %s\n", __func__);
	ctx->refExited = true;

	pthread_exit(NULL);
}

static void *thread_ecnr(void *arg)
{
	struct nx_voice_context *ctx = (struct nx_voice_context *)arg;
	BufferManager *manager = ctx->bufManager;
	bool useFeedback = ctx->config.use_feedback;
	struct ecnr_callback *cb = &ctx->config.cb;
        int spot_idx = 1;

	if (cb->init)
		cb->init(0, 0, NULL);

	if (!cb->process) {
		LOGE("ECNR callback process is NULL!!!\n");
		return NULL;
	}

	/* feedback unit size is same to ecnr out size */
	int size = ctx->feedbackUnitSize;

	short tmpBuffer[256];
	short tmpBuffer2[256];

	DoneBuffer *inBuffer = NULL;

	print_thread_info("ecnr", NULL, size);

	DataBuffer *outBuffer = NULL;

	int ret;
	while (!ctx->ecnrExit) {
		inBuffer = manager->getDoneBuffer();

		do {
			if (useFeedback) {
				outBuffer = manager->getOutBuffer();

				if (outBuffer != NULL) {
					ret = cb->process((short *)inBuffer->pcmBuffer->buf,
									  (short *)inBuffer->refBuffer->buf,
									  (short *)outBuffer->buf,
									  (short *)outBuffer->bufUser,
									  spot_idx);
				} else {
					LOGE("%s: overrun outBuffer!!!\n", __func__);
					ret = cb->process((short *)inBuffer->pcmBuffer->buf,
									  (short *)inBuffer->refBuffer->buf,
									  (short *)tmpBuffer,
									  (short *)tmpBuffer2,
									  spot_idx);
				}
			} else {
				ret = cb->process((short *)inBuffer->pcmBuffer->buf,
								  (short *)inBuffer->refBuffer->buf,
								  (short *)tmpBuffer,
								  (short *)tmpBuffer2,
								  spot_idx);
			}

			if (ctx->config.check_trigger && ret > 0)
				LOGD("Detect Keyword\n");

			if (!useFeedback && cb->post_process) {
				if (outBuffer != NULL) {
                                        int pret = cb->post_process(size/2, (short *)outBuffer->bufUser, ret);
                                        if (pret > -1) spot_idx = pret;
                                } else {
                                        int pret = cb->post_process(size/2, (short *)tmpBuffer2, ret);
                                        if (pret > -1) spot_idx = pret;
                                }
                        }

			manager->putDoneBuffer(inBuffer);

			if (useFeedback && outBuffer != NULL) {
				if (ctx->clientWait && ctx->pipe[1] > 0)
					write(ctx->pipe[1], outBuffer->buf, outBuffer->size);

				manager->putOutBuffer(outBuffer);
			} else {
				if (ctx->clientWait && ctx->pipe[1] > 0)
					write(ctx->pipe[1], tmpBuffer2, size);
			}

			inBuffer = manager->getDoneBufferNoLock();
		} while (inBuffer != NULL);
	}

	if (cb->deinit)
		cb->deinit();

	LOGD("Exit %s\n", __func__);

	ctx->ecnrExited = true;

	return NULL;
}

static void *thread_feedback(void *arg)
{
	struct nx_voice_context *ctx = (struct nx_voice_context *)arg;
	BufferManager *manager = ctx->bufManager;

	struct pcm_config config;
	struct pcm *pcm;

	memset(&config, 0, sizeof(config));
	config.channels = 1;
	config.rate = FEEDBACK_RATE;
	config.period_size = FEEDBACK_PERIOD_SIZE;
	config.period_count = FEEDBACK_PERIOD_COUNT;
	config.format = PCM_FORMAT_S16_LE;
	config.start_threshold = 0;
	config.stop_threshold = 0;
	config.silence_threshold = 0;

	print_thread_info("feedback", &config, ctx->feedbackUnitSize);

	pcm = pcm_open(0, ctx->config.feedback_devnum, PCM_OUT, &config);
	if (!pcm || !pcm_is_ready(pcm)) {
		LOGE("%s: unable_to open PCM device(%s)\n",
		     __func__, pcm_get_error(pcm));
		return NULL;
	}

	int size = ctx->feedbackUnitSize;
	DataBuffer *outBuffer = NULL;
	int ret;

	while (!ctx->feedbackExit) {
		outBuffer = manager->getDoneOutBuffer();
		ret = pcm_write(pcm, outBuffer->buf, outBuffer->size);
		if (ret) {
			LOGE("%s: failed to pcm_write\n", __func__);
			break;
		}
		manager->putDoneOutBuffer(outBuffer);
	}

	pcm_close(pcm);

	LOGD("Exit %s\n", __func__);
	ctx->feedbackExited = true;

	return NULL;
}

#define THREAD_IDX_PDM			0
#define THREAD_IDX_REF			1
#define THREAD_IDX_ECNR			2
#define THREAD_IDX_FEEDBACK		3
extern "C" void *nx_voice_create_handle(void)
{
	struct nx_voice_context *ctx;

	ctx = (struct nx_voice_context *)mmap(NULL, sizeof(*ctx),
										  PROT_READ | PROT_WRITE,
										  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (ctx == MAP_FAILED) {
		LOGE("failed to create handle\n");
		return NULL;
	}
	memset(ctx, 0, sizeof(*ctx));

	int ret = pipe(ctx->pipe);
	if (ret < 0) {
		LOGE("failed to create pipe\n");
		return NULL;
	}

	return ctx;
}

extern "C" void nx_voice_close_handle(void *handle)
{
	munmap(handle, sizeof(struct nx_voice_context));
}

extern "C" int nx_voice_start(void *handle, struct nx_smartvoice_config *c)
{
	pthread_t tid[MAX_THREAD_NUMBER];
	struct nx_voice_context *ctx;

	ctx = (struct nx_voice_context *)handle;

	LOGD("Start nx-voice\n");

	BufferManager *bufManager = new BufferManager();
	if(!bufManager) {
		LOGE("failed to alloc bufManager\n");
		return -ENOMEM;
	}

	memcpy(&ctx->config, c, sizeof(*c));
	ctx->bufManager = bufManager;
	ctx->pdmUnitSize = calcUnitSize(BASE_INTERVAL_US, PDM_RATE, PDM_BITS,
									ctx->config.pdm_chnum);
	ctx->refUnitSize = calcUnitSize(BASE_INTERVAL_US, REF_RATE, REF_BITS, 2);
	ctx->feedbackUnitSize = calcUnitSize(BASE_INTERVAL_US, FEEDBACK_RATE,
										 FEEDBACK_BITS, 1);
	ctx->pdmOutSize = calcUnitSize(BASE_INTERVAL_US, OUT_RATE, PDM_BITS,
								   ctx->config.pdm_chnum);
	ctx->refOutSize = calcUnitSize(BASE_INTERVAL_US, OUT_RATE, REF_BITS,
								   ctx->config.ref_resample_out_chnum);
	ctx->stop = false;
	ctx->pdmExit = false;
	ctx->pdmExited = false;
	ctx->refExit = false;
	ctx->refExited = false;
	ctx->ecnrExit = false;
	ctx->ecnrExited = false;
	ctx->feedbackExit = false;
	ctx->feedbackExited = false;

	LOGD("pdmUnitSize: %d\n", ctx->pdmUnitSize);
	LOGD("refUnitSize: %d\n", ctx->refUnitSize);
	LOGD("feedbackUnitSize: %d\n", ctx->feedbackUnitSize);
	LOGD("pdmOutSize: %d\n", ctx->pdmOutSize);
	LOGD("refOutSize: %d\n", ctx->refOutSize);

	bufManager->Init(ctx->pdmOutSize, ctx->refOutSize, ctx->feedbackUnitSize);

	/* daemonoize */
	pid_t pid = fork();
	if (pid > 0) {
		/* here is parent */
		close(ctx->pipe[1]);
		/* set non block mode */
		fcntl(ctx->pipe[0], F_SETFL,
		      fcntl(ctx->pipe[0], F_GETFL) | O_NONBLOCK);
		return pid;
	} else {
		/* here is child */
		close(ctx->pipe[0]);
		pid_t sid = setsid();
		if (sid < 0) {
			LOGE("failed to setsid: ret %d\n", sid);
			exit(EXIT_FAILURE);
		};

		pthread_attr_t sched_attr;
		int fifoMaxPriority;
		struct sched_param sched_param;

		pthread_attr_init(&sched_attr);
		pthread_attr_setschedpolicy(&sched_attr, SCHED_FIFO);
		fifoMaxPriority = sched_get_priority_max(SCHED_FIFO);

		/* thread_pdm, thread_ref priority is max - 1 */
		sched_param.sched_priority = fifoMaxPriority - 1;
		pthread_attr_setschedparam(&sched_attr, &sched_param);

		pthread_create(&ctx->tid[THREAD_IDX_PDM], &sched_attr, thread_pdm,
					   (void *)ctx);
		pthread_create(&ctx->tid[THREAD_IDX_REF], &sched_attr, thread_ref,
					   (void *)ctx);

		/* thread_ecnr, thread_feedback priority is max */
		sched_param.sched_priority = fifoMaxPriority;
		pthread_attr_setschedparam(&sched_attr, &sched_param);

		pthread_create(&ctx->tid[THREAD_IDX_ECNR], &sched_attr, thread_ecnr,
					   (void *)ctx);
		if (ctx->config.use_feedback)
			pthread_create(&ctx->tid[THREAD_IDX_FEEDBACK], &sched_attr,
						   thread_feedback, (void *)ctx);

		while(!ctx->stop)
			usleep(100000);

		int status;

		if (ctx->config.use_feedback) {
			ctx->feedbackExit = true;
			while (!ctx->feedbackExited)
				usleep(1000);
		}

		ctx->ecnrExit = true;
		while (!ctx->ecnrExited)
			usleep(1000);

		ctx->pdmExit = true;
		while (!ctx->pdmExited)
			usleep(1000);

		ctx->refExit = true;
		while (!ctx->refExited)
			usleep(1000);

		pthread_join(ctx->tid[THREAD_IDX_PDM], (void **)&status);
		pthread_join(ctx->tid[THREAD_IDX_REF], (void **)&status);
		pthread_join(ctx->tid[THREAD_IDX_ECNR], (void **)&status);
		if (ctx->config.use_feedback)
			pthread_join(ctx->tid[THREAD_IDX_FEEDBACK], (void **)&status);

		delete bufManager;

		munmap(ctx, sizeof(*ctx));

		LOGD("Exit nx-voice\n", __func__);
	}

	return 0;
}

extern "C" void nx_voice_stop(void *handle)
{
	struct nx_voice_context *ctx = (struct nx_voice_context *)handle;
	ctx->stop = true;
}

extern "C" int nx_voice_get_data(void *handle, short *data, int sample_count)
{
	struct nx_voice_context *ctx = (struct nx_voice_context *)handle;
	int ret = 0;
	int data_size = sample_count * 2;
	char *p;

	if ((sample_count % 256) != 0) {
		LOGE("sample count must be multiple of 256 but %d\n",
			 sample_count);
		return -EINVAL;
	}

	p = (char *)data;
	ctx->clientWait = true;
	while (data_size > 0) {
		usleep(5000);
		p += ret;
		ret = read(ctx->pipe[0], p, data_size);
		if (ret == -1)
			ret = 0;
		data_size -= ret;
	}
	ctx->clientWait = false;

	return sample_count * 2;
}
