#include <stdio.h>
#include <stdlib.h>

#include "ecnr_wrapper.h"
#include "pvo_wrapper.h"

#ifdef FILE_DUMP
static FILE *mic0_file = NULL;
static FILE *mic1_file = NULL;
static FILE *ref_file = NULL;
#endif

void ECNR_Init(int val1, int val2, char *dummy)
{
	printf("%s: This is Powervoice version\n", __func__);

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
	PVPRE_Init(val1, val2, dummy);
}

int ECNR_Process_4ch(short *mic4_buf, short *ref_buf __unused, short *out_buf,
					  short *out_buf2 __unused, int mode __unused)
{
	int ret;

#ifdef FILE_DUMP
	/* 256 samples * 2 bytes * 4 channels*/
	fwrite(mic4_buf, 1, 2048, mic0_file);
	/* 256 samples * 2 bytes * 1 channels*/
	fwrite(ref_buf, 1, 512, ref_file);
#endif

	ret = PVPRE_Process_4ch(mic4_buf, ref_buf, out_buf, out_buf2, mode);

	return ret;
}

int ECNR_Process_2ch(short *mic0_buf __unused, short *mic1_buf __unused,
		     short *ref_buf __unused, short *out_buf __unused, int mode __unused)
{
	int ret = 0;

	return ret;
}

int ECNR_PostProcess(int len __unused, short *in __unused, int mode __unused)
{
	int ret;

	ret = PoVoGateSource(len, in, mode);

	return ret;
}

void ECNR_DeInit(void)
{
#ifdef FILE_DUMP
	fclose(mic1_file);
	fclose(mic0_file);
	fclose(ref_file);
#endif
	PVPRE_Close();
}

