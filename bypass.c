#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "ecnr_wrapper.h"

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

#ifdef FILE_DUMP
static FILE *file = NULL;
static struct wav_header header;
static int frames;
#endif

static short mic0_comb[512];
static short mic1_comb[512];
static short ref_comb[512];
static bool is_ready = false;

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

void	ECNR_Init(int val1 __unused, int val2 __unused, char *dummy __unused)
{
	printf("%s: This is Nexell test version\n", __func__);

#ifdef FILE_DUMP
	file = fopen("/data/tmp/indata.wav", "wb");

	if (!file)
		fprintf(stderr, "fopen failed\n");
	else {
		printf("%s: save dump file to %s\n", __func__, "/data/tmp/indata.wav");

		frames = 0;

		memset(&header, 0, sizeof(header));
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
		header.block_align = 4 * (header.bits_per_sample / 8);
		header.data_id = ID_DATA;

		fseek(file, sizeof(struct wav_header), SEEK_SET);
	}
#endif

}

int	ECNR_Process_4ch(short *mic4_buf, short *ref_buf __unused, short *out_buf __unused,
					  short *out_buf2 __unused, int mode __unused)
{
#ifdef FILE_DUMP
	frames += 256;
	/* 256 samples * 2 bytes * 4 channels*/
	fwrite(mic4_buf, 1, 2048, file);
#else
	int i;
	short *in, *out;

	out = out_buf;
	in = mic4_buf;
	for(i = 0; i < 256; i++) {
		*out = *in;
		out++;
		in += 4;
	}
#endif
	return 0;
}

int	ECNR_Process_2ch(short *mic0_buf, short *mic1_buf __unused,
			 short *ref_buf __unused, short *out_buf __unused, int mode __unused)
{
	void *pdst = NULL;

	if (!is_ready) {
		pdst = mic0_comb;
		memcpy((void *)pdst, (void *)mic0_buf, 512);
		pdst = mic1_comb;
		memcpy((void *)pdst, (void *)mic1_buf, 512);
		pdst = ref_comb;
		memcpy((void *)pdst, (void *)ref_buf, 512);

		is_ready = true;
		return -1;
	} else {
		pdst = mic0_comb + 256;
		memcpy((void *)pdst, (void *)mic0_buf, 512);
		pdst = mic1_comb + 256;
		memcpy((void *)pdst, (void *)mic1_buf, 512);
		pdst = ref_comb + 256;
		memcpy((void *)pdst, (void *)ref_buf, 512);

		is_ready = false;
	}

#ifdef FILE_DUMP
	frames += 512;
	/* 512 samples * 2 bytes */
	fwrite(mic0_comb, 1, 1024, file);
#else
	int i;
	short *in, *out;

	out = out_buf;
	in = mic0_comb;
	for(i = 0; i < 512; i++) {
		*out = *in;
		out++;
		in++;
	}
#endif
	return 0;
}

int	ECNR_PostProcess(int len __unused, short *in __unused, int mode __unused)
{
	return 0;
}

void	ECNR_DeInit(void)
{
#ifdef FILE_DUMP
	if (file) {
		header.data_sz = frames * header.block_align;
		header.riff_sz = header.data_sz + sizeof(header) - 8;
		fseek(file, 0, SEEK_SET);
		fwrite(&header, sizeof(struct wav_header), 1, file);
		fclose(file);
		print_wav_header(&header);
		file = NULL;
	}
#endif
	printf("%s\n", __func__);
}

