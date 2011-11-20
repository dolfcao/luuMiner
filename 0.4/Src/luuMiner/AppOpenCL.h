#ifndef MTRLTINOPENCL_H
#define MTRLTINOPENCL_H

class OpenCL
{
private:
public:
	int Init();
	void Quit();
	void CreateGPUThread();

	uint GetVectorSize();
};

#include "pthread.h"

//#ifndef CPU_MINING_ONLY
#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif

#include "Util.h"

//#ifndef CPU_MINING_ONLY
struct _clState
{
	cl_context context;
	cl_kernel kernel;
	cl_command_queue commandQueue;
	cl_program program;
	cl_mem CLbuffer[2];
	cl_mem padbuffer32;

	uint vectors;
	uint thread_id;

	pthread_t thread;

	bool shares_available;
	deque<vector<uchar> > shares;
	pthread_mutex_t share_mutex;

	ullint hashes;
};

#endif
