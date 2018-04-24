#include <stdio.h>
#include <stdlib.h>

#include "ecnr_wrapper.h"
#include "pvo_wrapper.h"

void ECNR_Init(int val1, int val2, char *dummy)
{
	printf("%s: This is Powervoice version\n", __func__);

	PVPRE_Init(val1, val2, dummy);
}

int ECNR_Process_4ch(short *mic4_buf, short *ref_buf __unused, short *out_buf,
					  short *out_buf2 __unused, int mode __unused)
{
	int ret;

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
	PVPRE_Close();
}

