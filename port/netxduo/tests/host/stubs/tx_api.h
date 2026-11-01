#ifndef NXSTUB_TX_API_H
#define NXSTUB_TX_API_H
#include "nx_api.h"
typedef struct TX_THREAD_STRUCT { UCHAR* tx_thread_name; } TX_THREAD;
#define TX_SUCCESS 0
#define TX_AUTO_START 1
#define TX_NO_TIME_SLICE 0
UINT tx_thread_create(TX_THREAD*, CHAR*, void(*)(ULONG), ULONG, void*, ULONG, UINT, UINT, ULONG, UINT);
void tx_thread_sleep(ULONG);
#endif
