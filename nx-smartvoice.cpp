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
#include <string.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <android/log.h>
#include <tinyalsa/asoundlib.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sound/asound.h>

#include "nx_pdm.h"
#include "resample.h"
#include "buffermanager.h"
#include "nx-smartvoice.h"
#include "ecnr_wrapper.h"

//#define TRACE_TIME

#ifdef TRACE_TIME
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
#endif

#define BASE_INTERVAL_US		80000
#define SEC_TO_US(s)			(s*1000*1000)
#define MAX_THREAD_NUMBER		5
#define OUT_RATE				16000

#define PDM_SPI_RATE			64000
#define PDM_SPI_PERIOD_SIZE		512
#define PDM_SPI_PERIOD_COUNT	16
#define PDM_I2S_RATE			32000
#define PDM_I2S_PERIOD_SIZE		2560
#define PDM_I2S_PERIOD_COUNT	2
#define PDM_BITS				16

#define REF_RATE				48000
#define REF_PERIOD_SIZE			3840
#define REF_PERIOD_COUNT		2
#define REF_BITS				16

#define FEEDBACK_RATE			OUT_RATE
#define FEEDBACK_PERIOD_SIZE	1280
#define FEEDBACK_PERIOD_COUNT	2
#define FEEDBACK_BITS			16

#define LOGI(fmt, args...)      __android_log_print(ANDROID_LOG_INFO , TAG, fmt, ##args)
#define LOGD(fmt, args...)      __android_log_print(ANDROID_LOG_DEBUG, TAG, fmt, ##args)
#define LOGW(fmt, args...)      __android_log_print(ANDROID_LOG_WARN , TAG, fmt, ##args)
#define LOGE(fmt, args...)      __android_log_print(ANDROID_LOG_ERROR, TAG, fmt, ##args)

static const char * TAG         = "SVOICE";
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
	bool pdmExit2;
	bool pdmExited;
	bool pdmExited2;
	bool refStarted;
	bool refExit;
	bool refExited;
	bool ecnrExit;
	bool ecnrExited;
	bool feedbackExit;
	bool feedbackExited;
};

#ifdef TRACE_TIME
static struct timeval pdm_tv_before;
static struct timeval pdm2_tv_before;
static struct timeval ref_tv_before;
#endif

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

static int calcUnitSize(long long interval_us, long long rate, int bits, int channel_num)
{
	return ((interval_us * rate) / SEC_TO_US(1)) * (bits / 8) * channel_num;
}

static void *thread_pdm(void *arg)
{
	struct nx_voice_context *ctx = (struct nx_voice_context *)arg;
	BufferManager *manager = ctx->bufManager;
#ifdef TRACE_TIME
	struct timeval tv_after;
#endif
	struct pcm_config config;
	struct pcm *pcm;
	pdm_STATDEF *pdm_st;
	pdm_Init(&pdm_st);

	memset(&config, 0, sizeof(config));
	config.channels = ctx->config.pdm_chnum;
	if (ctx->config.pdm_chnum == 4) {
		config.rate = PDM_SPI_RATE;
		config.period_size = PDM_SPI_PERIOD_SIZE;
		config.period_count = PDM_SPI_PERIOD_COUNT;
	} else {
		config.rate = PDM_I2S_RATE;
		config.period_size = PDM_I2S_PERIOD_SIZE;
		config.period_count = PDM_I2S_PERIOD_COUNT;
	}
	config.format = PCM_FORMAT_S16_LE;
	config.start_threshold = 0;
	config.stop_threshold = 0;
	config.silence_threshold = 0;

	int unit_size = ctx->pdmUnitSize;
	print_thread_info("pdm", &config, unit_size);

#ifdef NOUGAT
	pcm = pcm_open(0, ctx->config.pdm_devnum, PCM_IN, &config);
#else
	pcm = pcm_open(ctx->config.pdm_devnum, 0, PCM_IN, &config);
#endif
	if (!pcm || !pcm_is_ready(pcm)) {
		LOGE("%s: unable_to open PCM device(%s)\n",
			 __func__, pcm_get_error(pcm));
		return NULL;
	}

	char *buffer = (char *)malloc(unit_size);
	DataBuffer *outBuffer = NULL;

	int ret, i;
#ifdef FILE_DUMP
	FILE *file;

	file = fopen("/data/tmp/pdm.raw", "wb");
	if (!file) {
		fprintf(stderr, "%s: failed to create\n", __func__);
	}
#endif
	if (0 != pdm_SetParam(pdm_st, PDM_PARAM_GAIN, ctx->config.pdm_gain))
		LOGE("failed: pdm gain parameter [%d]",
			 ctx->config.pdm_gain);

#ifdef TRACE_TIME
	gettimeofday(&pdm_tv_before, NULL);
#endif
	while (!ctx->pdmExit) {
		if (ctx->config.pdm_chnum == 4) {
			for (i = 0; i < 5; i++) {
				ret = pcm_read(pcm, (buffer + unit_size/10 * (i * 2)), unit_size/10);
				if (ret) {
					LOGE("%s: failed to pcm_read(%d)\n", __func__, ret);
					break;
				}

				ret = pcm_read(pcm, (buffer + unit_size/10 * (i * 2 + 1)), unit_size/10);
				if (ret) {
					LOGE("%s: failed to pcm_read\n", __func__);
					break;
				}
#ifdef TRACE_TIME
				gettimeofday(&tv_after, NULL);
				LOGE("pdm  %d\n", (int)abs(tv_after.tv_usec - pdm_tv_before.tv_usec));
				pdm_tv_before = tv_after;
#endif
				if (!outBuffer)
					outBuffer = manager->getPcmBuffer();
				if (!outBuffer) {
					LOGE("%s: failed to getPcmBuffer\n", __func__);
					manager->printQStatus();
					break;
				}

				pdm_Run(pdm_st, (short int*)(outBuffer->buf + outBuffer->size/5 * i),
					(int *)(buffer + unit_size/5 * i), 0);
			}
		} else {
			ret = pcm_read(pcm, buffer, unit_size);
			if (ret) {
				LOGE("%s: failed to pcm_read(%d)\n", __func__, ret);
				break;
			}
#ifdef TRACE_TIME
			gettimeofday(&tv_after, NULL);
			LOGE("pdm  %d\n", (int)abs(tv_after.tv_usec - pdm_tv_before.tv_usec));
			pdm_tv_before = tv_after;
#endif
			outBuffer = manager->getPcmBuffer();
			if (!outBuffer) {
				LOGE("%s: failed to getPcmBuffer\n", __func__);
				manager->printQStatus();
				break;
			}
			for (i = 0; i < 5; i++) {
				pdm_Run_filter(pdm_st, (short int*)(outBuffer->buf + outBuffer->size/5 * i),
							   (int *)(buffer + unit_size/5 * i), 0, 0, 0);
			}
		}
#ifdef FILE_DUMP
		fwrite(outBuffer->buf, 1, outBuffer->size, file);
#endif
		manager->putPcmBuffer(outBuffer);
		outBuffer = NULL;
	}
#ifdef FILE_DUMP
	fclose(file);
#endif
	free(buffer);
	pcm_close(pcm);
	pdm_Deinit(pdm_st);

	LOGD("Exit %s\n", __func__);

	ctx->pdmExited = true;

	return NULL;
}

static void *thread_pdm2(void *arg)
{
	struct nx_voice_context *ctx = (struct nx_voice_context *)arg;
	BufferManager *manager = ctx->bufManager;
#ifdef TRACE_TIME
	struct timeval tv_after;
#endif
	struct pcm_config config;
	struct pcm *pcm;
	pdm_STATDEF *pdm_st;
	pdm_Init(&pdm_st);

	memset(&config, 0, sizeof(config));
	config.channels = ctx->config.pdm_chnum;
	config.rate = PDM_I2S_RATE;
	config.period_size = PDM_I2S_PERIOD_SIZE;
	config.period_count = PDM_I2S_PERIOD_COUNT;
	config.format = PCM_FORMAT_S16_LE;
	config.start_threshold = 0;
	config.stop_threshold = 0;
	config.silence_threshold = 0;

	int unit_size = ctx->pdmUnitSize;
	print_thread_info("pdm", &config, unit_size);

#ifdef NOUGAT
	pcm = pcm_open(0, ctx->config.pdm_devnum2, PCM_IN, &config);
#else
	pcm = pcm_open(ctx->config.pdm_devnum2, 0, PCM_IN, &config);
#endif
	if (!pcm || !pcm_is_ready(pcm)) {
		LOGE("%s: unable_to open PCM device(%s)\n",
			 __func__, pcm_get_error(pcm));
		return NULL;
	}

	char *buffer = (char *)malloc(unit_size);
	DataBuffer *outBuffer = NULL;

	int ret, i;
#ifdef FILE_DUMP
	FILE *file;

	file = fopen("/data/tmp/pdm2.raw", "wb");
	if (!file) {
		fprintf(stderr, "%s: failed to create\n", __func__);
	}
#endif
	if (0 != pdm_SetParam(pdm_st, PDM_PARAM_GAIN, ctx->config.pdm_gain))
		LOGE("failed: pdm gain parameter [%d]",
			 ctx->config.pdm_gain);

#ifdef TRACE_TIME
	gettimeofday(&pdm2_tv_before, NULL);
#endif
	while (!ctx->pdmExit2) {
		ret = pcm_read(pcm, buffer, unit_size);
		if (ret) {
			LOGE("%s: failed to pcm_read(%d)\n", __func__, ret);
			break;
		}
#ifdef TRACE_TIME
		gettimeofday(&tv_after, NULL);
		LOGE("pdm  %d\n", (int)abs(tv_after.tv_usec - pdm2_tv_before.tv_usec));
		pdm2_tv_before = tv_after;
#endif
		outBuffer = manager->getPcmBuffer2();
		if (!outBuffer) {
			LOGE("%s: failed to getPcmBuffer2\n", __func__);
			manager->printQStatus();
			break;
		}
		for (i = 0; i < 5; i++) {
			pdm_Run_filter(pdm_st, (short int*)(outBuffer->buf + outBuffer->size/5 * i),
						   (int *)(buffer + unit_size/5 * i), 0, 0, 0);
		}
#ifdef FILE_DUMP
		fwrite(outBuffer->buf, 1, outBuffer->size, file);
#endif
		manager->putPcmBuffer2(outBuffer);
		outBuffer = NULL;
	}
#ifdef FILE_DUMP
	fclose(file);
#endif
	free(buffer);
	pcm_close(pcm);
	pdm_Deinit(pdm_st);

	LOGD("Exit %s\n", __func__);

	ctx->pdmExited2 = true;

	return NULL;
}

static void *thread_ref(void *arg)
{
	struct nx_voice_context *ctx = (struct nx_voice_context *)arg;
	BufferManager *manager = ctx->bufManager;
#ifdef TRACE_TIME
	struct timeval tv_after;
#endif
	struct ReSampleContext *rctx = NULL;
	struct pcm_config config;
	struct pcm *pcm = NULL;
	struct snd_pcm_status status;

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

	int size = 0;
	char *buffer = NULL;
	DataBuffer *outBuffer = NULL;

	int ret, status_fd, config_fd;
	char pb_status[2] = {};
	char pb_config[10] = {};
	const char c[2] = ",";
	char *t;
	bool is_playback = false;
	bool exist_play = false;
#ifdef FILE_DUMP
	FILE *file;

	file = fopen("/data/tmp/ref.raw", "wb");
	if (!file) {
		fprintf(stderr, "%s: failed to create\n", __func__);
	}
#endif
	status_fd = open("/sys/devices/platform/svoice/status", O_RDONLY);
	if (status_fd < 0) {
		LOGE("%s: failed to open playback status\n", __func__);
	}
	read(status_fd, pb_status, 1);
	close(status_fd);
	if (!strcmp("1", pb_status)) {
		if (!pcm) {
			config_fd = open("/sys/devices/platform/svoice/config", O_RDONLY);
			if (config_fd < 0) {
				LOGE("%s: failed to open playback status\n", __func__);
			}
			read(config_fd, pb_config, 10);
			close(config_fd);
			t = strtok(pb_config, c);
			config.rate = atoi(t);
			t = strtok(NULL, c);
			config.format = (atoi(t) == 16) ? PCM_FORMAT_S16_LE : PCM_FORMAT_S24_LE;
			rctx = audio_resample_init(ctx->config.ref_resample_out_chnum,
									   2, OUT_RATE, config.rate,
									   (config.format == PCM_FORMAT_S16_LE)?
									   PCM_FMT_16BIT : PCM_FMT_32BIT);
			ctx->refUnitSize = calcUnitSize(BASE_INTERVAL_US, config.rate,
											(config.format == PCM_FORMAT_S16_LE) ? 16 : 32, 2);
			config.period_size = ctx->refUnitSize/
				((config.format == PCM_FORMAT_S16_LE) ? 4 : 8);
#ifdef NOUGAT
			pcm = pcm_open(0, ctx->config.ref_devnum, PCM_IN, &config);
#else
			pcm = pcm_open(ctx->config.ref_devnum, 0, PCM_IN, &config);
#endif
		if (!pcm || !pcm_is_ready(pcm)) {
			LOGE("%s: unable_to open PCM device(%s)\n",
				 __func__, pcm_get_error(pcm));
			return NULL;
		}
		size = ctx->refUnitSize;
		buffer = (char *)malloc(size);
		ret = pcm_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status, 0);
		if (status.state == SNDRV_PCM_STATE_SETUP)
			pcm_start(pcm);
		}
	}
	ctx->refStarted = true;
#ifdef TRACE_TIME
	gettimeofday(&ref_tv_before, NULL);
#endif
	if (!access("/dev/snd/pcmC0D0p", O_RDONLY))
		exist_play = true;
	while (!ctx->refExit) {
		if (exist_play) {
			status_fd = open("/sys/devices/platform/svoice/status", O_RDONLY);
			if (status_fd < 0) {
				LOGE("%s: failed to open playback status\n", __func__);
			}
			read(status_fd, pb_status, 1);
			close(status_fd);
			if (!strcmp("1", pb_status)) {
				if (!pcm) {
					config_fd = open("/sys/devices/platform/svoice/config", O_RDONLY);
					if (config_fd < 0) {
						LOGE("%s: failed to open playback status\n", __func__);
					}
					read(config_fd, pb_config, 10);
					close(config_fd);
					t = strtok(pb_config, c);
					config.rate = atoi(t);
					t = strtok(NULL, c);
					config.format = (atoi(t) == 16) ? PCM_FORMAT_S16_LE : PCM_FORMAT_S24_LE;
					rctx = audio_resample_init(ctx->config.ref_resample_out_chnum,
											   2, OUT_RATE, config.rate,
											   (config.format == PCM_FORMAT_S16_LE)?
											   PCM_FMT_16BIT : PCM_FMT_32BIT);
					ctx->refUnitSize = calcUnitSize(BASE_INTERVAL_US, config.rate,
						(config.format == PCM_FORMAT_S16_LE) ? 16 : 32, 2);
					config.period_size = ctx->refUnitSize/
						((config.format == PCM_FORMAT_S16_LE) ? 4 : 8);
#ifdef NOUGAT
					pcm = pcm_open(0, ctx->config.ref_devnum, PCM_IN, &config);
#else
					pcm = pcm_open(ctx->config.ref_devnum, 0, PCM_IN, &config);
#endif
					if (!pcm || !pcm_is_ready(pcm)) {
						LOGE("%s: unable_to open PCM device(%s)\n",
						__func__, pcm_get_error(pcm));
						return NULL;
					}
					size = ctx->refUnitSize;
					buffer = (char *)malloc(size);
					ret = pcm_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status, 0);
					if (status.state == SNDRV_PCM_STATE_SETUP)
						pcm_start(pcm);
				}
				ret = pcm_read(pcm, buffer, size);
				if (ret) {
					LOGE("%s: failed to pcm_read(%d)\n", __func__, ret);
					break;
				}
#ifdef TRACE_TIME
				gettimeofday(&tv_after, NULL);
				LOGE("ref1  %d\n", (int)abs(tv_after.tv_usec - ref_tv_before.tv_usec));
				ref_tv_before = tv_after;
#endif
				outBuffer = manager->getRefBuffer();
				if (!outBuffer) {
					LOGE("%s: failed to getRefBuffer\n", __func__);
					manager->printQStatus();
					break;
				}
				audio_resample(rctx, (short *)outBuffer->buf,
							   (short *)buffer,
							   size/((config.format == PCM_FORMAT_S16_LE) ? 4 : 8));
			} else {
				if (pcm) {
					ret = pcm_ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status, 0);
					if (status.state == SNDRV_PCM_STATE_RUNNING)
						pcm_stop(pcm);
					free(buffer);
					pcm_close(pcm);
					pcm = NULL;
					audio_resample_close(rctx);
				}
				while (!ctx->refExit) {
					if (manager->getRefFreeSync()) {
						outBuffer = manager->getRefBuffer();
						if (!outBuffer) {
							LOGE("%s: failed to getRefBuffer\n", __func__);
							manager->printQStatus();
							break;
						}
						memset(outBuffer->buf, 0, outBuffer->size);
						usleep(15*1000);
						break;
					} else
						usleep(15*1000);
				}
#ifdef TRACE_TIME
				gettimeofday(&tv_after, NULL);
				LOGE("ref2  %d\n", (int)abs(tv_after.tv_usec - ref_tv_before.tv_usec));
				ref_tv_before = tv_after;
#endif
			}
		} else {
			if (!pcm) {
				rctx = audio_resample_init(ctx->config.ref_resample_out_chnum,
							    2, OUT_RATE, REF_RATE, PCM_FMT_16BIT);
#ifdef NOUGAT
				pcm = pcm_open(0, ctx->config.ref_devnum, PCM_IN, &config);
#else
				pcm = pcm_open(ctx->config.ref_devnum, 0, PCM_IN, &config);
#endif
				if (!pcm || !pcm_is_ready(pcm)) {
					LOGE("%s: unable_to open PCM device(%s)\n",
					__func__, pcm_get_error(pcm));
					return NULL;
				}
				size = ctx->refUnitSize;
				buffer = (char *)malloc(size);
			}

			ret = pcm_read(pcm, buffer, size);
			if (ret) {
				LOGE("%s: failed to pcm_read\n", __func__);
				break;
			}
#ifdef TRACE_TIME
			gettimeofday(&tv_after, NULL);
			LOGE("ref3  %d\n", (int)abs(tv_after.tv_usec - ref_tv_before.tv_usec));
			ref_tv_before = tv_after;
#endif
			outBuffer = manager->getRefBuffer();
			if (!outBuffer) {
				LOGE("%s: failed to getRefBuffer\n", __func__);
				manager->printQStatus();
				break;
			}

			audio_resample(rctx, (short *)outBuffer->buf, (short *)buffer, size/4);
		}
#ifdef FILE_DUMP
		fwrite(outBuffer->buf, 1, outBuffer->size, file);
#endif
		manager->putRefBuffer(outBuffer);
	}
#ifdef FILE_DUMP
	fclose(file);
#endif
	if (!exist_play) {
		free(buffer);
		pcm_close(pcm);
		audio_resample_close(rctx);
	}

	LOGD("Exit %s\n", __func__);

	ctx->refExited = true;

	return NULL;
}

static void *thread_ecnr(void *arg)
{
	struct nx_voice_context *ctx = (struct nx_voice_context *)arg;
	BufferManager *manager = ctx->bufManager;
	bool useFeedback = ctx->config.use_feedback;
	struct ecnr_callback *cb = &ctx->config.cb;
	int spot_idx = 1;
	/* feedback unit size is same to ecnr out size */
	int size = ctx->feedbackUnitSize;
	int sample_size = (ctx->config.pdm_chnum == 4)? 256 : 512;
	short *tmpBuffer, *tmpBuffer2;

	ctx->config.cb.init = ECNR_Init;
	if (ctx->config.pdm_chnum == 4)
		ctx->config.cb.process = ECNR_Process_4ch;
	else
		ctx->config.cb.process = ECNR_Process_2ch;
	ctx->config.cb.post_process = ECNR_PostProcess;
	ctx->config.cb.deinit = ECNR_DeInit;

	if (cb->init)
		cb->init(0, 0, NULL);

	if (!cb->process) {
		LOGE("ECNR callback process is NULL!!!\n");
		return NULL;
	}

	tmpBuffer = (short *)malloc(sample_size);
	tmpBuffer2 = (short *)malloc(sample_size);

	DoneBuffer *inBuffer = NULL;

	print_thread_info("ecnr", NULL, size);

	DataBuffer *outBuffer = NULL;

	int ret = 0;
	int i;
	while (!ctx->ecnrExit) {
		inBuffer = manager->getDoneBuffer();

		do {
			for (int i = 0; i < 5; i++) {
				if (ctx->config.pdm_chnum == 4) {
					if (useFeedback) {
						outBuffer = manager->getOutBuffer();

						if (outBuffer != NULL) {
							ret = cb->process((short *)(inBuffer->pcmBuffer->buf + (inBuffer->pcmBuffer->size/5 * i)),
											  (short *)(inBuffer->refBuffer->buf + (inBuffer->refBuffer->size/5 * i)),
											  (short *)outBuffer->buf,
											  (short *)outBuffer->bufUser,
											  spot_idx);
						} else {
							LOGE("%s: overrun outBuffer!!!\n", __func__);
							ret = cb->process((short *)(inBuffer->pcmBuffer->buf + (inBuffer->pcmBuffer->size/5 * i)),
											  (short *)(inBuffer->refBuffer->buf + (inBuffer->refBuffer->size/5 * i)),
											  (short *)tmpBuffer,
											  (short *)tmpBuffer2,
											  spot_idx);
						}
					} else {
						ret = cb->process((short *)(inBuffer->pcmBuffer->buf + (inBuffer->pcmBuffer->size/5 * i)),
										  (short *)(inBuffer->refBuffer->buf + (inBuffer->refBuffer->size/5 * i)),
										  (short *)tmpBuffer,
										  (short *)tmpBuffer2,
										  spot_idx);
					}

					if (ctx->config.check_trigger && ret > 0)
						LOGD("Detect Keyword\n");

					if (!useFeedback && cb->post_process) {
						if (outBuffer != NULL) {
							int pret = cb->post_process(size/5/2, (short *)outBuffer->bufUser, ret);
							if (pret > -1) spot_idx = pret;
						} else {
							int pret = cb->post_process(size/5/2, (short *)tmpBuffer2, ret);
							if (pret > -1) spot_idx = pret;
						}
					}

				} else {
					ret = cb->process((short *)(inBuffer->pcmBuffer->buf + (size/5 * i)),
									  (short *)(inBuffer->pcmBuffer2->buf + (size/5 * i)),
									  (short *)(inBuffer->refBuffer->buf + (size/5 * i)),
									  (short *)tmpBuffer,
									  spot_idx);
				}

				if (useFeedback && outBuffer != NULL) {
					if (ctx->clientWait && ctx->pipe[1] > 0)
						write(ctx->pipe[1], outBuffer->buf, outBuffer->size);

					manager->putOutBuffer(outBuffer);
				} else {
					if (ctx->clientWait && ctx->pipe[1] > 0) {
						if (ctx->config.pdm_chnum == 4)
							write(ctx->pipe[1], tmpBuffer, sizeof(short) * sample_size);
						else {
							if (ret == 0)
								write(ctx->pipe[1], tmpBuffer, sizeof(short) * sample_size);
						}
					}
				}
			}
			manager->putDoneBuffer(inBuffer);

			inBuffer = manager->getDoneBufferNoLock();
		} while (inBuffer != NULL);
	}
	if (cb->deinit)
		cb->deinit();

	free(tmpBuffer);
	free(tmpBuffer2);

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

#ifdef NOUGAT
	pcm = pcm_open(0, ctx->config.feedback_devnum, PCM_OUT, &config);
#else
	pcm = pcm_open(ctx->config.feedback_devnum, 0, PCM_OUT, &config);
#endif
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
#define THREAD_IDX_PDM2			1
#define THREAD_IDX_REF			2
#define THREAD_IDX_ECNR			3
#define THREAD_IDX_FEEDBACK		4
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
	if (ctx->config.pdm_chnum == 4)
		ctx->pdmUnitSize = calcUnitSize(BASE_INTERVAL_US, PDM_SPI_RATE,
						PDM_BITS, ctx->config.pdm_chnum);
	else
		ctx->pdmUnitSize = calcUnitSize(BASE_INTERVAL_US, PDM_I2S_RATE,
						PDM_BITS, ctx->config.pdm_chnum);
	ctx->refUnitSize = calcUnitSize(BASE_INTERVAL_US, REF_RATE,
					REF_BITS, 2);
	ctx->feedbackUnitSize = calcUnitSize(BASE_INTERVAL_US, FEEDBACK_RATE,
					     FEEDBACK_BITS, 1);
	if (ctx->config.pdm_chnum == 4)
		ctx->pdmOutSize = calcUnitSize(BASE_INTERVAL_US, OUT_RATE,
					       PDM_BITS, ctx->config.pdm_chnum);
	else
		ctx->pdmOutSize = calcUnitSize(BASE_INTERVAL_US, OUT_RATE,
					       PDM_BITS, ctx->config.pdm_chnum/2);
	ctx->refOutSize = calcUnitSize(BASE_INTERVAL_US, OUT_RATE,
				       REF_BITS,
				       ctx->config.ref_resample_out_chnum);
	ctx->stop = false;
	ctx->pdmExit = false;
	ctx->pdmExited = false;
	ctx->pdmExit2 = false;
	ctx->pdmExited2 = false;
	ctx->refStarted = false;
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

	if (ctx->config.pdm_chnum == 4)
		bufManager->Init(ctx->pdmOutSize, 0, ctx->refOutSize, ctx->feedbackUnitSize);
	else
		bufManager->Init(ctx->pdmOutSize, ctx->pdmOutSize, ctx->refOutSize,
						 ctx->feedbackUnitSize);

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

		pthread_create(&ctx->tid[THREAD_IDX_REF], &sched_attr, thread_ref,
					   (void *)ctx);
		while (!ctx->refStarted)
			usleep(1000);

		pthread_create(&ctx->tid[THREAD_IDX_PDM], &sched_attr, thread_pdm,
					   (void *)ctx);
		if (ctx->config.pdm_chnum == 2)
			pthread_create(&ctx->tid[THREAD_IDX_PDM2], &sched_attr, thread_pdm2,
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
		if (ctx->config.pdm_chnum == 2) {
			ctx->pdmExit2 = true;
			while (!ctx->pdmExited2)
				usleep(1000);
		}

		ctx->refExit = true;
		while (!ctx->refExited)
			usleep(1000);

		bufManager->printQStatus();

		pthread_join(ctx->tid[THREAD_IDX_PDM], (void **)&status);
		if (ctx->config.pdm_chnum == 2)
			pthread_join(ctx->tid[THREAD_IDX_PDM2], (void **)&status);
		pthread_join(ctx->tid[THREAD_IDX_REF], (void **)&status);
		pthread_join(ctx->tid[THREAD_IDX_ECNR], (void **)&status);
		if (ctx->config.use_feedback)
			pthread_join(ctx->tid[THREAD_IDX_FEEDBACK], (void **)&status);

		delete bufManager;

		munmap(ctx, sizeof(*ctx));

		LOGD("Exit nx-voice\n");
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
	int sample_chk = (ctx->config.pdm_chnum == 4)? 256 : 512;

	if ((sample_count % sample_chk) != 0) {
		LOGE("sample count must be multiple of %d but %d\n",
			 sample_chk, sample_count);
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
