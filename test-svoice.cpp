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
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include "ecnr_wrapper.h"
#include "buffermanager.h"
#include "nx-smartvoice.h"

#define ID_RIFF 0x46464952
#define ID_WAVE 0x45564157
#define ID_FMT  0x20746d66
#define ID_DATA 0x61746164

#define FORMAT_PCM 1

struct wav_header {
	uint32_t riff_id;
	uint32_t riff_sz;
	uint32_t riff_fmt;
	uint32_t fmt_id;
	uint32_t fmt_sz;
	uint16_t audio_format;
	uint16_t num_channels;
	uint32_t sample_rate;
	uint32_t byte_rate;
	uint16_t block_align;
	uint16_t bits_per_sample;
	uint32_t data_id;
	uint32_t data_sz;
};

static void print_wav_header(struct wav_header *h)
{
	printf("===== WAV HEAER =====\n");
	printf("riff_id: 0x%x\n", h->riff_id);
	printf("riff_sz: %d\n", h->riff_sz);
	printf("riff_fmt: 0x%x\n", h->riff_fmt);
	printf("fmt_id: 0x%x\n", h->fmt_id);
	printf("fmt_sz: 0x%x\n", h->fmt_sz);
	printf("audio_format: 0x%x\n", h->audio_format);
	printf("num_channels: %d\n", h->num_channels);
	printf("sample_rate: %d\n", h->sample_rate);
	printf("byte_rate: %d\n", h->byte_rate);
	printf("block_align: %d\n", h->block_align);
	printf("bits_per_sample: %d\n", h->bits_per_sample);
	printf("data_id: 0x%x\n", h->data_id);
	printf("data_sz: %d\n", h->data_sz);
}

struct filesave_context {
	void *handle;
	const char *fileName;
	BufferManager *bufManager;
	bool fileSaveExit;
	bool fileSaveExited;
	bool getDataExit;
	bool getDataExited = false;
	int sample_count;
};

static void *thread_file_save(void *arg)
{
	struct wav_header header;
	struct filesave_context *ctx = (struct filesave_context *)arg;
	BufferManager *manager = ctx->bufManager;
	FILE *file;

	printf("start %s\n", __func__);

	file = fopen(ctx->fileName, "wb");
	if (!file) {
		fprintf(stderr, "%s: failed to create %s\n", __func__, ctx->fileName);
		pthread_exit(NULL);
	}

	header.riff_id = ID_RIFF;
	header.riff_sz = 0;
	header.riff_fmt = ID_WAVE;
	header.fmt_id = ID_FMT;
	header.fmt_sz = 16;
	header.audio_format = FORMAT_PCM;
	header.num_channels = 1;
	header.sample_rate = 16000;

	header.bits_per_sample = 16;
	header.byte_rate = (header.bits_per_sample / 8) * 1 * 16000;
	header.block_align = 1 * (header.bits_per_sample / 8);
	header.data_id = ID_DATA;

	int sample_count = ctx->sample_count;

	DataBuffer *buffer = NULL;
	int frames = 0;
	/* leave enough room for header */
	fseek(file, sizeof(struct wav_header), SEEK_SET);

	while (!ctx->fileSaveExit) {
		buffer = manager->dequeueClientDoneBuffer();
		if (!buffer) {
			fprintf(stderr, "Timeout to dequeueClientDoneBuffer\n");
		} else {
			if (fwrite(buffer->buf, 1, buffer->size, file) !=
				(size_t)buffer->size) {
				fprintf(stderr, "Error fwrite\n");
				break;
			}
			frames += sample_count;
			manager->queueClientFreeBuffer(buffer);
		}
	}

	header.data_sz = frames * header.block_align;
	header.riff_sz = header.data_sz + sizeof(header) - 8;
	fseek(file, 0, SEEK_SET);
	fwrite(&header, sizeof(struct wav_header), 1, file);
	print_wav_header(&header);

	printf("File %s created: %d frames written\n", ctx->fileName, frames);
	printf("File size must be %d\n", sizeof(struct wav_header) + frames * sample_count * 2);
	fclose(file);

	ctx->fileSaveExited = true;

	return NULL;
}

static void *thread_get_data(void *arg)
{
	struct filesave_context *ctx = (struct filesave_context *)arg;
	BufferManager *manager = ctx->bufManager;
	int sample_count = ctx->sample_count;

	printf("start %s\n", __func__);
	for (int i = 0; i < 64; i++) {
		DataBuffer *b = new DataBuffer();
		b->buf = (char *)malloc(sample_count *2);
		b->size = sample_count *2;
		manager->queueClientFreeBuffer(b);
	}

	DataBuffer *buffer = NULL;
	int frames = 0;
	while (!ctx->getDataExit) {
		buffer = manager->dequeueClientFreeBuffer();
		int ret = nx_voice_get_data(ctx->handle, (short *)buffer->buf,
									buffer->size/2);
		if (ret < 0) {
			fprintf(stderr, "%s: failed to nx_voice_get_data ret(%d)\n",
					__func__, ret);
			pthread_exit(NULL);
		}
		frames += sample_count;

		manager->queueClientDoneBuffer(buffer);
	}

	printf("%s: frames %d\n", __func__, frames);
	ctx->getDataExited = true;

	return NULL;
}

static void printUsage(char **argv)
{
	fprintf(stderr, "Usage: %s [-m pdm_devnum] [-mc pdm_in_ch] [-r ref_devnum] [-rc ref_out_ch] [-c sample_count] [-f fb_devnum] [-s filename] [-p] [-v] [-g agcgain]\n", argv[0]);
	fprintf(stderr, "default: %s [-m 2] [-mc 4] [-r 1] [-rc 1] [-c 256] [-f 3] [-s /data/tmp/client.wav] [-p false] [-v false] [-g 0]\n", argv[0]);
}

static const char *defaultFileName = "/data/tmp/client.wav";
int main(int argc __unused, char *argv[])
{
	struct nx_smartvoice_config c;
	int hnd;
	char input = 0;
	void *handle;
	int ret;
	BufferManager *bufManager = new BufferManager();
	struct filesave_context *ctx = new filesave_context();
	bool filesave = false;
	int gain = 0;
	bool passAfterTrigger = false;
	bool verbose = false;
	int pdm_devnum = 2;
	int pdm_in_ch = 4;
	int ref_devnum = 1;
	int ref_out_ch = 1;
	int fb_devnum = 3;
	int sample_count = 256;

	memset(ctx, 0, sizeof(*ctx));
	ctx->bufManager = bufManager;

	/* parse command line */
	char **myArgv = argv;
	myArgv++;
	while (*myArgv) {
		if (strcmp(*myArgv, "-m") == 0) {
			myArgv++;
			if (*myArgv) {
				pdm_devnum = atoi(*myArgv);
			}
		} else if (strcmp(*myArgv, "-mc") == 0) {
			myArgv++;
			if (*myArgv) {
				pdm_in_ch = atoi(*myArgv);
			}
		} else if (strcmp(*myArgv, "-r") == 0) {
			myArgv++;
			if (*myArgv) {
				ref_devnum = atoi(*myArgv);
			}
		} else if (strcmp(*myArgv, "-rc") == 0) {
			myArgv++;
			if (*myArgv) {
				ref_out_ch = atoi(*myArgv);
			}
		} else if (strcmp(*myArgv, "-c") == 0) {
			myArgv++;
			if (*myArgv) {
				sample_count = atoi(*myArgv);
			}
		} else if (strcmp(*myArgv, "-f") == 0) {
			myArgv++;
			if (*myArgv) {
				fb_devnum = atoi(*myArgv);
			}
		} else if (strcmp(*myArgv, "-s") == 0) {
			filesave = true;
			myArgv++;
			if (*myArgv) {
				ctx->fileName = *myArgv;
			} else {
				fprintf(stderr, "No filename given, using %s\n", defaultFileName);
				ctx->fileName = defaultFileName;
			}
		} else if (strcmp(*myArgv, "-p") == 0) {
			myArgv++;
			passAfterTrigger = true;
		} else if (strcmp(*myArgv, "-g") == 0) {
			myArgv++;
			if (*myArgv) {
				gain = atoi(*myArgv);
			}
		} else if (strcmp(*myArgv, "-v") == 0) {
			myArgv++;
			verbose = true;
		} else if (strcmp(*myArgv, "-h") == 0) {
			printUsage(argv);
			exit(0);
		}

		if (*myArgv)
			myArgv++;
	}

	handle = nx_voice_create_handle();
	if (!handle) {
		fprintf(stderr, "failed to nx_voice_create_handle\n");
		return -1;
	}

	ctx->handle = handle;

	memset(&c, 0, sizeof(c));
	c.use_feedback = false;
	c.pdm_devnum = pdm_devnum;
	c.pdm_devnum2 = pdm_devnum + 1;
	c.ref_devnum = ref_devnum;
	c.feedback_devnum = fb_devnum;
	c.pdm_chnum = pdm_in_ch;
	c.pdm_gain = gain;
	c.ref_resample_out_chnum = ref_out_ch;
	c.sample_count = sample_count;
	c.check_trigger = true;
	c.trigger_done_ret_value = 1;
	c.pass_after_trigger = passAfterTrigger;
	c.verbose = verbose;

	ret = nx_voice_start(handle, &c);
	if (ret < 0) {
		fprintf(stderr, "failed to nx_voice_start, ret %d\n", ret);
		return ret;
	}

	pid_t daemon_pid = ret;
	printf("%s: nx-smartvoice daemon pid %d\n", __func__, daemon_pid);
	if (daemon_pid == 0) {
		printf("This is child return, exit\n");
		return 0;
	}

	pthread_t tid_save;
	pthread_t tid_get_data;

	pthread_attr_t sched_attr;
	int fifoMaxPriority;
	struct sched_param sched_param;

	pthread_attr_init(&sched_attr);
	pthread_attr_setschedpolicy(&sched_attr, SCHED_FIFO);
	fifoMaxPriority = sched_get_priority_max(SCHED_FIFO);

	/* thread_file_save, thread_get data is max - 1 */
	sched_param.sched_priority = fifoMaxPriority - 1;
	pthread_attr_setschedparam(&sched_attr, &sched_param);

	if (filesave == true) {
		ctx->sample_count = sample_count;
		pthread_create(&tid_save, NULL, thread_file_save, (void *)ctx);
		pthread_create(&tid_get_data, NULL, thread_get_data, (void *)ctx);
	}

	printf("If you want to stop nx-voice, Enter 'q'\n");
	while (input != 'q') {
		usleep(500000);
		input = getchar();
	}

	if (filesave == true) {
		ctx->fileSaveExit = true;
		while (!ctx->fileSaveExited) {
			usleep(1000);
		}

		ctx->getDataExit = true;
		while (!ctx->getDataExited) {
			usleep(1000);
		}

		pthread_join(tid_save, NULL);
		pthread_join(tid_get_data, NULL);
	}

	nx_voice_stop(handle);
	nx_voice_close_handle(handle);

	delete bufManager;
	delete ctx;

	printf("%s Exit\n", __func__);
	return 0;
}
