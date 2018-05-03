#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "ecnr_wrapper.h"
#include "mwsr_main.h"

MwsrHandle *m_hMWInst;
static short mic4_comb[2048];
static short mic0_comb[512];
static short mic1_comb[512];
static short ref_comb[512];
static bool is_ready = false;

#ifdef FILE_DUMP
static FILE *mic0_file = NULL;
static FILE *mic1_file = NULL;
static FILE *ref_file = NULL;
#endif

void ECNR_Init(int val1 __unused, int val2 __unused, char *dummy __unused)
{
	printf("%s: This is Mightyworks version\n", __func__);


#ifdef FILE_DUMP
	mic0_file = fopen("/data/tmp/mic0data.raw", "wb");
	if (!mic0_file) {
		fprintf(stderr, "%s: failed to create\n", __func__);
	}
	mic1_file = fopen("/data/tmp/mic1data.raw", "wb");
	if (!mic1_file) {
		fprintf(stderr, "%s: failed to create\n", __func__);
	}
	ref_file = fopen("/data/tmp/refdata.raw", "wb");
	if (!ref_file) {
		fprintf(stderr, "%s: failed to create\n", __func__);
	}
#endif

	Mwsr_Create(&m_hMWInst);
	Mwsr_Init(m_hMWInst, NULL);
}

int ECNR_Process_4ch(short *mic4_buf __unused, short *ref_buf __unused,
			 short *out_buf __unused, short *outbuf2 __unused, int mode __unused)
{
	void *pdst = NULL;

	if (!is_ready) {
		pdst = mic4_comb;
		memcpy((void *)pdst, (void *)mic4_buf, 2048);
		pdst = ref_comb;
		memcpy((void *)pdst, (void *)ref_buf, 512);

		is_ready = true;
		return -1;
	} else {
		pdst = mic4_comb + 1024;
		memcpy((void *)pdst, (void *)mic4_buf, 2048);
		pdst = ref_comb + 256;
		memcpy((void *)pdst, (void *)ref_buf, 512);

		is_ready = false;
	}

#ifdef FILE_DUMP
	/* 512 samples * 2 bytes * 4 channels*/
	fwrite(mic4_buf, 1, 4096, mic0_file);
	/* 512 samples * 2 bytes * 1 channels*/
	fwrite(ref_buf, 1, 1024, ref_file);
#endif
	Mwsr_Process(m_hMWInst, mic4_comb, ref_comb, out_buf);

	return 0;
}

int ECNR_Process_2ch(short *mic0_buf, short *mic1_buf,
		     short *ref_buf, short *out_buf, int mode __unused)
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
	/* 512 samples * 2 bytes * 1 channels*/
	fwrite(mic0_comb, 1, 1024, mic0_file);
	/* 512 samples * 2 bytes * 1 channels*/
	fwrite(mic1_comb, 1, 1024, mic1_file);
	/* 512 samples * 2 bytes * 1 channels*/
	fwrite(ref_comb, 1, 1024, ref_file);
#endif

	//Mwsr_Process(m_hMWInst, mic0_comb, mic1_comb, ref_comb, out_buf);

	return 0;
}

int	ECNR_PostProcess(int len __unused, short *in __unused, int mode __unused)
{
	return 0;
}

void ECNR_DeInit(void)
{
	Mwsr_Free(m_hMWInst);

#ifdef FILE_DUMP
	fclose(mic1_file);
	fclose(mic0_file);
	fclose(ref_file);
#endif
}

