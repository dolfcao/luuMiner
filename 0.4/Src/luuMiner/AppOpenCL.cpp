#include "Global.h"
#include "AppOpenCL.h"
#include "Util.h"
#include "Config.h"
#include <ctime>
#include <algorithm>
#include "stdafx.h"
#include "luuMinerDlg.h"
//#ifndef CPU_MINING_ONLY
vector<_clState> GPUstates;
//#endif
extern pthread_mutex_t current_work_mutex;
extern Work current_work;
pthread_mutex_t noncemutex = PTHREAD_MUTEX_INITIALIZER;
uint nonce = 0;
extern Config config;

uint OpenCL::GetVectorSize()
{
	return 2;
}

/*
struct BLOCK_DATA
{
0	int32 nVersion;
4	uint256 hashPrevBlock;
36	uint256 hashMerkleRoot;
68	int64 nBlockNum;
76	int64 nTime;
84	uint64 nNonce1;
92	uint64 nNonce2;
100	uint64 nNonce3;
108	uint32 nNonce4;
112 char miner_id[12];
124	uint32 dwBits;
};
*/

extern unsigned char *BlockHash_1_MemoryPAD8;
extern uint *BlockHash_1_MemoryPAD32;
extern ullint shares_hwinvalid;

#include "RSHash.h"

#include <deque>
using std::deque;

string VectorToHexString(vector<uchar> vec);

//#ifndef CPU_MINING_ONLY
void* Reap_GPU(void* param)
{
	_clState* state = (_clState*)param;
	state->hashes = 0;

	size_t globalsize = globalconfs.global_worksize;
	size_t localsize = globalconfs.local_worksize;

	Work tempwork;

	uchar tempdata[1024];
	memset(tempdata, 0, 1024);

	clEnqueueWriteBuffer(state->commandQueue, state->CLbuffer[0], true, 0, KERNEL_INPUT_SIZE, tempdata, 0, NULL, NULL);
	clEnqueueWriteBuffer(state->commandQueue, state->CLbuffer[1], true, 0, KERNEL_OUTPUT_SIZE*sizeof(uint), tempdata, 0, NULL, NULL);

	uint kernel_output[KERNEL_OUTPUT_SIZE] = {};

	bool write_kernel_output = true;
	bool write_kernel_input = true;

	size_t base = 0;
	clSetKernelArg(state->kernel, 2, sizeof(cl_mem), &state->padbuffer32);

	bool errorfree = true;
	deque<uint> runtimes;
	while((!shutdown_now)&&(!quitappnow))
	{
		if (globalconfs.max_aggression && !runtimes.empty())
		{
			uint avg_runtime=0;
			for(deque<uint>::iterator it = runtimes.begin(); it != runtimes.end(); ++it)
			{
				avg_runtime += *it;
			}
			avg_runtime /= (uint)runtimes.size();
			if (avg_runtime > TARGET_RUNTIME_MS+TARGET_RUNTIME_ALLOWANCE_MS)
			{
				globalsize -= localsize;
			}
			else if (avg_runtime*3 < TARGET_RUNTIME_MS-TARGET_RUNTIME_ALLOWANCE_MS)
			{
				globalsize = (globalsize+globalsize/2)/localsize*localsize;
			}
			else if (avg_runtime < TARGET_RUNTIME_MS-TARGET_RUNTIME_ALLOWANCE_MS)
			{
				globalsize += localsize;
			}
		}
		clock_t starttime = ticker();
		if (current_work.old)
		{
			Wait_ms(20);
			continue;
		}
		if (tempwork.time != current_work.time)
		{
			pthread_mutex_lock(&current_work_mutex);
			tempwork = current_work;
			pthread_mutex_unlock(&current_work_mutex);
			memcpy(tempdata, &tempwork.data[0], 128);
			*(uint*)&tempdata[100] = state->thread_id;
			base = 0;
			write_kernel_input = true;
		}

		ullint newtime = tempwork.ntime_at_getwork + (ticker()-tempwork.time)/1000;
		if (*(ullint*)&tempdata[76] != newtime)
		{
			*(ullint*)&tempdata[76] = newtime;
			write_kernel_input = true;
		}
		if (write_kernel_input)
			clEnqueueWriteBuffer(state->commandQueue, state->CLbuffer[0], true, 0, KERNEL_INPUT_SIZE, tempdata, 0, NULL, NULL);
		if (write_kernel_output)
			clEnqueueWriteBuffer(state->commandQueue, state->CLbuffer[1], true, 0, KERNEL_OUTPUT_SIZE*sizeof(uint), kernel_output, 0, NULL, NULL);

		clSetKernelArg(state->kernel, 0, sizeof(cl_mem), &state->CLbuffer[0]);
		clSetKernelArg(state->kernel, 1, sizeof(cl_mem), &state->CLbuffer[1]);

		cl_int returncode;
		returncode = clEnqueueNDRangeKernel(state->commandQueue, state->kernel, 1, &base, &globalsize, &localsize, 0, NULL, NULL);
		//OpenCL throws CL_INVALID_KERNEL_ARGS randomly, let's just ignore them.
		if (returncode != CL_SUCCESS && returncode != CL_INVALID_KERNEL_ARGS && errorfree)
		{
			//cout << humantime() << "Error " << returncode << " while trying to run OpenCL kernel" << endl;
			errorfree = false;
		}
		else if ((returncode == CL_SUCCESS || returncode == CL_INVALID_KERNEL_ARGS) && !errorfree)
		{
			//cout << humantime() << "Previous OpenCL error cleared" << endl;
			errorfree = true;
		}
		clEnqueueReadBuffer(state->commandQueue, state->CLbuffer[1], true, 0, KERNEL_OUTPUT_SIZE*sizeof(uint), kernel_output, 0, NULL, NULL);

		write_kernel_input = false;
		write_kernel_output = false;
		for(uint i=0; i<KERNEL_OUTPUT_SIZE; ++i)
		{
			if (kernel_output[i] == 0)
				continue;
			uint result = kernel_output[i];
			uchar testmem[512];
			uchar finalhash[32];
			memcpy(testmem, tempdata, 128);
			*((uint*)&testmem[108]) = result;
			BlockHash_1(testmem, finalhash);

			if (finalhash[31] != 0 || finalhash[30] != 0 || finalhash[29] >= 0x80)
			{
				++shares_hwinvalid;
			}
			bool below=true;
			for(int j=0; j<32; ++j)
			{
				if (finalhash[31-j] > tempwork.target_share[j])
				{
					below=false;
					break;
				}
				if (finalhash[31-j] < tempwork.target_share[j])
				{
					break;
				}
			}
			if (below)
			{
				vector<uchar> share(testmem, testmem+128);
				pthread_mutex_lock(&state->share_mutex);
				state->shares_available = true;
				state->shares.push_back(share);
				pthread_mutex_unlock(&state->share_mutex);
			}
			kernel_output[i] = 0;
			write_kernel_output = true;
		}
		if (errorfree)
		{
			state->hashes += globalsize;
		}
		base += globalsize;
		clock_t endtime = ticker();
		runtimes.push_back(uint(endtime-starttime));
		if (runtimes.size() > RUNTIMES_SIZE)
			runtimes.pop_front();
	}
	pthread_exit(NULL);
	return NULL;
}

_clState clState;

//#endif

//return 0 is correct, -1 is wrong
int OpenCL::Init()
{
	try
	{
		int builtsucc=0;//1 from source succ 1 from binary succ
		CluuMinerDlg* pDlg=(CluuMinerDlg*)(theApp.m_pMainWnd);
		CString out;

		GPUstates.clear();
		if (globalconfs.threads_per_gpu == 0)
		{
			strGlobalUpdateInfo=_T("No GPUs selected.\r\n");
			AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
			throw string("No GPUs selected.");
			return FUNCTION_ERROR;
		}

		cl_int status = 0;

		cl_uint numPlatforms;
		cl_platform_id platform = NULL;

		status = clGetPlatformIDs(0, NULL, &numPlatforms);
		if(status != CL_SUCCESS)
			throw string("Error getting OpenCL platforms");

		if(numPlatforms > 0)
		{
			cl_platform_id* platforms = new cl_platform_id[numPlatforms];

			status = clGetPlatformIDs(numPlatforms, platforms, NULL);
			if(status != CL_SUCCESS)
				throw string("Error getting OpenCL platform IDs");

			unsigned int i;
			strGlobalUpdateInfo=_T("List of platforms:");
			AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
			for(i=0; i < numPlatforms; ++i)
			{
				char pbuff[100];

				status = clGetPlatformInfo( platforms[i], CL_PLATFORM_NAME, sizeof(pbuff), pbuff, NULL);
				if(status != CL_SUCCESS)
				{
					delete [] platforms;
					throw string("Error getting OpenCL platform info");
				}

				strGlobalUpdateInfo.Format(_T("%s%d%s%s%s"),_T( "\t"),i,_T( "\t"),CString(pbuff),_T("\r\n"));
				AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
				if (globalconfs.platform == i)
				{
					platform = platforms[i];
				}
			}
			delete [] platforms;
		}
		else
		{
			throw string("No OpenCL platforms found");
		}

		if (platform == NULL)
		{
			throw string("Chosen platform number does not exist");
		}
		strGlobalUpdateInfo.Format(_T("%s%d"),_T( "Using platform number: "),globalconfs.platform);
		AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);

		cl_uint numDevices;
		status = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, 0, NULL, &numDevices);
		if(status != CL_SUCCESS)
		{
			throw string("Error getting OpenCL device IDs");
		}

		if (numDevices == 0)
			throw string("No OpenCL devices found");

		vector<cl_device_id> devices;
		cl_device_id* devicearray = new cl_device_id[numDevices];

		status = clGetDeviceIDs(platform, CL_DEVICE_TYPE_GPU, numDevices, devicearray, NULL);
		if(status != CL_SUCCESS)
			throw string("Error getting OpenCL device ID list");

		for(uint i=0; i<numDevices; ++i)
			devices.push_back(devicearray[i]);

		cl_context_properties cps[3] = { CL_CONTEXT_PLATFORM, (cl_context_properties)platform, 0 };

		clState.context = clCreateContextFromType(cps, CL_DEVICE_TYPE_GPU, NULL, NULL, &status);
		if(status != CL_SUCCESS)
			throw string("Error creating OpenCL context");

		strGlobalUpdateInfo=_T("\r\n");
		AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
		if (globalconfs.devices.empty())
		{
			strGlobalUpdateInfo=_T("Using all devices\r\n");
			AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
		}
		else
		{
			strGlobalUpdateInfo=_T( "Using device\r\n");
			CString strTmp;
			AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
			for(uint i=0; i<globalconfs.devices.size(); ++i)
			{
				strTmp.Format(_T("%d%s"),globalconfs.devices[i],_T("\r\n"));
				if (i+1 < globalconfs.devices.size())
				{
					strTmp+=_T( ", ");
				}
				strGlobalUpdateInfo+=strTmp;
			}
			strGlobalUpdateInfo+=_T( "\r\n");
			AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
		}

		for(uint device_id=0; device_id<numDevices; ++device_id)
		{
			string source;
			string sourcefilename;
			{
				sourcefilename = config.GetCombiValue<string>("device", device_id, "kernel");
				if (sourcefilename == "")
					sourcefilename = config.GetValue<string>("kernel");
				FILE* filu = fopen(sourcefilename.c_str(), "rb");
				if (filu == NULL)
				{
					throw string("Couldn't find kernel file ") + sourcefilename;
				}
				fseek(filu, 0, SEEK_END);
				uint size = ftell(filu);
				fseek(filu, 0, SEEK_SET);
				size_t readsize = 0;
				for(uint i=0; i<size; ++i)
				{
					char c;
					readsize += fread(&c, 1, 1, filu);
					source.push_back(c);
				}
				if (readsize != size)
				{
					//cout << "Read error while reading kernel source " << sourcefilename << endl;
				}
			}

			vector<size_t> sourcesizes;
			sourcesizes.push_back(source.length());

			const char* see = source.c_str();

			char pbuff[100];
			status = clGetDeviceInfo(devices[device_id], CL_DEVICE_NAME, sizeof(pbuff), pbuff, NULL);
			strGlobalUpdateInfo.Format(_T("%s%d%s"),_T("\t"),device_id,CString(pbuff));
			strGlobalUpdateInfo+=_T("\r\n");
			AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
			if(status != CL_SUCCESS)
				throw string("Error getting OpenCL device info");

			if (!globalconfs.devices.empty() && std::find(globalconfs.devices.begin(), globalconfs.devices.end(), device_id) == globalconfs.devices.end())
			{
				strGlobalUpdateInfo=_T(" (disabled)" );
				AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
				continue;
			}

			//cout << endl;

			uchar* filebinary = NULL;
			size_t filebinarysize=0;
			string filebinaryname;
			for(char*p = &pbuff[0]; *p != 0; ++p)
			{
				//get rid of unwanted characters in filenames
				if (*p >= 33 && *p < 127 && *p != '\\' && *p != ':' && *p != '/' && *p != '*' && *p != '<' && *p != '>' && *p != '"' && *p != '?' && *p != '|')
					filebinaryname += *p;
			}
			filebinaryname = sourcefilename.substr(0,sourcefilename.size()-3) + REAPER_VERSION + "." + filebinaryname + ".bin";
			if (globalconfs.save_binaries)
			{
				FILE* filu = fopen(filebinaryname.c_str(), "rb");
				if (filu != NULL)
				{
					fseek(filu, 0, SEEK_END);
					uint size = ftell(filu);
					fseek(filu, 0, SEEK_SET);
					if (size > 0)
					{
						filebinary = new uchar[size];
						filebinarysize = size;
						size_t readsize = fread(filebinary, size, 1, filu);
						if (readsize != 1)
						{
							//cout << "Read error while reading binary" << endl;
						}
					}
					fclose(filu);
				}
			}

			_clState GPUstate;

			if (filebinary == NULL)
			{
				strGlobalUpdateInfo=_T( "Compiling kernel... this could take up to 2 minutes.\r\nIf the application doesn't response, just wait a moment.\r\n");
				AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
				GPUstate.program = clCreateProgramWithSource(clState.context, 1, (const char **)&see, &sourcesizes[0], &status);
				if(status != CL_SUCCESS)
				{
					//delete [] see;
					throw string("Error creating OpenCL program from source");
				}

				string compile_options;

				status = clBuildProgram(GPUstate.program, 1, &devices[device_id], compile_options.c_str(), NULL, NULL);
				if(status != CL_SUCCESS)
				{
					size_t logSize;
					status = clGetProgramBuildInfo(GPUstate.program, devices[device_id], CL_PROGRAM_BUILD_LOG, 0, NULL, &logSize);

					char* log = new char[logSize];
					status = clGetProgramBuildInfo(GPUstate.program, devices[device_id], CL_PROGRAM_BUILD_LOG, logSize, log, NULL);
					//cout << log << endl;
					delete [] log;
					//delete [] see;
					throw string("Error building OpenCL program");
				}

				uint device_amount;
				clGetProgramInfo(GPUstate.program, CL_PROGRAM_NUM_DEVICES, sizeof(uint), &device_amount, NULL);

				size_t* binarysizes = new size_t[device_amount];
				uchar** binaries = new uchar*[device_amount];
				try
				{
					for(uint curr_binary = 0; curr_binary<device_amount; ++curr_binary)
					{
						clGetProgramInfo(GPUstate.program, CL_PROGRAM_BINARY_SIZES, device_amount*sizeof(size_t), binarysizes, NULL);
						binaries[curr_binary] = new uchar[binarysizes[curr_binary]];
					}
					clGetProgramInfo(GPUstate.program, CL_PROGRAM_BINARIES, sizeof(uchar*)*device_amount, binaries, NULL);

					for(uint binary_id = 0; binary_id < device_amount; ++binary_id)
					{
						if (binarysizes[binary_id] == 0)
							continue;

						strGlobalUpdateInfo.Format(_T("%s,%d,%s,%s"),_T("Binary size: "),binarysizes[binary_id],_T(" bytes"),_T("\r\n"));
						AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
					}

					if (globalconfs.save_binaries)
					{
						FILE* filu = fopen(filebinaryname.c_str(), "wb");
						fwrite(binaries[device_id], binarysizes[device_id], 1, filu);
						fclose(filu);
					}

					builtsucc=1;
				}
				catch(...)
				{
					//delete [] see;
					for(uint binary_id=0; binary_id < device_amount; ++binary_id)
					{
						if(binaries[binary_id]!=NULL) delete [] binaries[binary_id];
					}
					delete [] binarysizes;
					delete [] binaries;
				}
				//delete [] see;
				for(uint binary_id=0; binary_id < device_amount; ++binary_id)
					delete [] binaries[binary_id];
				delete [] binarysizes;
				delete [] binaries;
			}
			else
			{
				cl_int binary_status, errorcode_ret;
				GPUstate.program = clCreateProgramWithBinary(clState.context, 1, &devices[device_id], &filebinarysize, const_cast<const uchar**>(&filebinary), &binary_status, &errorcode_ret);
				if (binary_status != CL_SUCCESS)
				{
					strGlobalUpdateInfo.Format(_T("%s,%d,%s"),_T("Binary status error code: "),binary_status,_T("\r\n"));
					AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,1,0);
					//delete [] filebinary;
					throw string("Binary status error\r\n");
				}
				if (errorcode_ret != CL_SUCCESS)
				{
					strGlobalUpdateInfo.Format(_T("%s,%d,%s"),_T("Binary loading error code: "),errorcode_ret,_T("\r\n"));
					AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,1,0);
					//delete [] filebinary;
					throw string("Binary loading error\r\n");
				}
				status = clBuildProgram(GPUstate.program, 1, &devices[device_id], NULL, NULL, NULL);
				if (status != CL_SUCCESS)
				{
					strGlobalUpdateInfo.Format(_T("%s,%d,%s"),_T("Error while building from binary:"),status,_T("\r\n"));
					AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,1,0);
					//delete [] filebinary;
					throw string("Error while building from binary\r\n");
				}

				builtsucc=2;
			}
			delete [] filebinary;

			GPUstate.kernel = clCreateKernel(GPUstate.program, "search", &status);
			if(status != CL_SUCCESS)
			{
				strGlobalUpdateInfo.Format(_T("%s%d"),_T("\r\nKernel build not successful.\r\n"),status);
				AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,1,0);
				throw string("Error creating OpenCL kernel");
			}
			cl_mem padbuffer32 = clCreateBuffer(clState.context, CL_MEM_READ_ONLY, 1024*1024*4*sizeof(uint), NULL, &status);
			for(uint thread_id = 0; thread_id < globalconfs.threads_per_gpu; ++thread_id)
			{
				GPUstate.commandQueue = clCreateCommandQueue(clState.context, devices[device_id], 0, &status);
				if (thread_id == 0)
				{
					clEnqueueWriteBuffer(GPUstate.commandQueue, padbuffer32, true, 0, 1024*1024*4*sizeof(uint), BlockHash_1_MemoryPAD32, 0, NULL, NULL);
				}
				if(status != CL_SUCCESS)
				{
					//delete [] padbuffer32;
					//delete [] (GPUstate.commandQueue);
					throw string("Error creating OpenCL command queue");
				}

				GPUstate.CLbuffer[0] = clCreateBuffer(clState.context, CL_MEM_READ_ONLY, KERNEL_INPUT_SIZE, NULL, &status);
				GPUstate.CLbuffer[1] = clCreateBuffer(clState.context, CL_MEM_WRITE_ONLY, KERNEL_OUTPUT_SIZE*sizeof(uint), NULL, &status);
				GPUstate.padbuffer32 = padbuffer32;

				if(status != CL_SUCCESS)
				{
					//delete [] padbuffer32;
					//delete [] (GPUstate.commandQueue);
					//delete [] (GPUstate.CLbuffer[0]);
					//delete [] (GPUstate.CLbuffer[1]);
					//delete [] (GPUstate.padbuffer32);
					throw string("Error creating OpenCL buffer");
				}

				pthread_mutex_t initializer = PTHREAD_MUTEX_INITIALIZER;

				GPUstate.share_mutex = initializer;
				GPUstate.shares_available = false;

				GPUstate.vectors = GetVectorSize();
				GPUstate.thread_id = device_id*numDevices+thread_id;
				GPUstate.hashes=0;
				GPUstates.push_back(GPUstate);
			}
		}
		if (builtsucc==1) strGlobalUpdateInfo=_T( "Program built from source.\r\n");
		if (builtsucc==2) strGlobalUpdateInfo=_T("Program built from saved binary.\r\n");
		if((builtsucc!=1)&&(builtsucc!=2)) strGlobalUpdateInfo.Empty();
		AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
	}
	catch(string s)
	{
		strGlobalUpdateInfo=_T("\r\n");
		strGlobalUpdateInfo+=s.c_str();
		AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
		//delete [] (GPUstate.commandQueue);
		//delete [] (GPUstate.CLbuffer[0]);
		//delete [] (GPUstate.CLbuffer[1]);
		//delete [] (GPUstate.padbuffer32);
		return FUNCTION_ERROR;
	}
	catch(...)
	{
		//delete [] (GPUstate.commandQueue);
		//delete [] (GPUstate.CLbuffer[0]);
		//delete [] (GPUstate.CLbuffer[1]);
		//delete [] (GPUstate.padbuffer32);
		return FUNCTION_ERROR;
	}
	return FUNCTION_OK;
}

void OpenCL::Quit()
{
}

void OpenCL::CreateGPUThread()
{
	if (GPUstates.empty())
	{
		//cout << "No GPUs selected." << endl;
		return;
	}

	//cout << "Creating " << GPUstates.size() << " GPU threads" << endl;
	for(uint i=0; i<GPUstates.size(); ++i)
	{
		//cout << i+1 << "...";
		pthread_create(&(GPUstates[i].thread), NULL, Reap_GPU, (void*)&GPUstates[i]);
	}
}