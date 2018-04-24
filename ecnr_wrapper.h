//ecnr_wrapper.h is h file
// this file wrap the ecnr_wrapper.h into callable functions from h file.
// created 2018.04 by hsjung@nexell.co.kr
// Copyright (c) Nexell 2018.

#ifndef __ECNR_WRAPPER_H__
#define __ECNR_WRAPPER_H__

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------------
// Voice prev process
// ----------------------------------
void ECNR_Init(int, int, char *);

int ECNR_Process_4ch(short *, short *, short *, short *, int);
int ECNR_Process_2ch(short *, short *, short *, short *, int);

int ECNR_PostProcess(int, short *, int);

void ECNR_DeInit(void);

#ifdef __cplusplus
}
#endif


#endif
