#ifndef GLOBAL_H
#define GLOBAL_H
#pragma once

#define _CRT_SECURE_NO_WARNINGS

#include "stdafx.h"
#include "luuMiner.h"

#include "CMakeConf.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <vector>
#include <iostream>
#include <string>
#include <deque>
using namespace std;

typedef unsigned long long int ullint;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;

//this struct has been deprecated, possibly.
struct GlobalConfs
{
	uint global_worksize;
	uint local_worksize;
	uint threads_per_gpu;
	vector<uint> devices;
	bool save_binaries;
	uint cputhreads;
	uint platform;
	bool max_aggression;
};

extern GlobalConfs globalconfs;
extern bool shutdown_now;
extern bool quitappnow;
//extern bool initminerfailed;
extern bool cpu_mining_only;
extern bool gpu_mining_only;

extern CString strGlobalUpdateInfo;

//judge funcion is ok or not
const int FUNCTION_ERROR = -1;
const int FUNCTION_OK = 0;

const uint KERNEL_INPUT_SIZE = 128;
const uint KERNEL_OUTPUT_SIZE = 256;

const uint WORK_EXPIRE_TIME_SEC = 120;
const uint SHARE_THREAD_RESTART_THRESHOLD_SEC = 20;

const uint TARGET_RUNTIME_MS = 320;
const uint TARGET_RUNTIME_ALLOWANCE_MS = 25;
const uint RUNTIMES_SIZE = 16;

const uint CPU_BATCH_SIZE = 1024;

#define foreachgpu() for(vector<_clState>::iterator it = GPUstates.begin(); it != GPUstates.end(); ++it)
#define foreachcpu() for(vector<Reap_CPU_param>::iterator it = CPUstates.begin(); it != CPUstates.end(); ++it)

#if defined(_M_X64) || defined(__x86_64__)
#define REAPER_PLATFORM "64-bit"
#else
#define REAPER_PLATFORM "32-bit"
#endif

#endif