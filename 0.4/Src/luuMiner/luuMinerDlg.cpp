#include "stdafx.h"
#include "luuMiner.h"
#include "luuMinerDlg.h"
#include "afxdialogex.h"

extern vector<_clState> GPUstates;
extern vector<Reap_CPU_param> CPUstates;

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#ifdef WIN32
void Wait_ms(uint n);
#undef SetPort
#else
void Wait_ms(uint n);
#endif

struct LongPollThreadParams
{
	Curl* curl;
	CluuMinerDlg* app;
};

const char* hextable[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b", "c", "d", "e", "f"};
ullint shares_valid = 0;
ullint shares_invalid = 0;
ullint shares_hwinvalid = 0;
clock_t current_work_time = 0;
bool getwork_now = false;
bool shutdown_now=false;
//bool initminerfailed=false;
bool quitappnow=false;
bool cpu_mining_only=false;
bool gpu_mining_only=false;

Config config;
GlobalConfs globalconfs;
bool sharethread_active;

void DoubleSHA256(uint* output2, uint* workdata, uint* midstate);
bool ShareTest(uint* workdata);

void* MiningThreadFunc(void* lpParam);
void* InitMinerThreadFunc(void* lpParam);
CString strGlobalUpdateInfo=_T("");

extern Work current_work;
extern string longpoll_url;
extern bool longpoll_active;

uchar HexToChar(char data)
{
	if (data <= '9')
		return data-'0';
	else if (data <= 'Z')
		return data-'7';
	else
		return data-'W';
}

uchar HexToChar(char h, char l)
{
	return HexToChar(h)*16+HexToChar(l);
}

string CharToHex(uchar c)
{
	return string(hextable[c/16]) + string(hextable[c%16]);
}

vector<uchar> HexStringToVector(string str)
{
	vector<uchar> ret;
	ret.assign(str.length()/2, 0);
	for(uint i=0; i<str.length(); i+=2)
	{
		ret[i/2] = HexToChar(str[i+0], str[i+1]);
	}
	return ret;
}
string VectorToHexString(vector<uchar> vec)
{
	string ret;
	for(uint i=0; i<vec.size(); i++)
	{
		ret += CharToHex(vec[i]);
	}
	return ret;
}

void SubmitShare(Curl& curl, vector<uchar>& w)
{
	if (w.size() != 128)
	{
		strGlobalUpdateInfo.Format(_T("%s%d%s"),_T("SubmitShare: Size of share is "),w.size(),_T(", should be 128\r\n"));

		AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
		return;
	}
	string ret = curl.TestWork(VectorToHexString(w));
	Json::Value root;
	Json::Reader reader;
	bool parse_success = reader.parse(ret, root);
	if (parse_success)
	{
		Json::Value result = root.get("result", "null");
		if (result.isObject())
		{
			Json::Value work = result.get("work", "null");
			if (work.isArray())
			{
				Json::Value innerobj = work.get(Json::Value::UInt(0), "");
				if (innerobj.isObject())
				{
					Json::Value share_valid = innerobj.get("share_valid", "null");
					if (share_valid.isBool())
					{
						if (share_valid.asBool())
						{
							++shares_valid;
						}
						else
						{
							getwork_now = true;
							++shares_invalid;
						}
					}
					//Json::Value block_valid = innerobj.get("block_valid");
				}
			}
		}
		else
		{
			strGlobalUpdateInfo=_T("Weird response from server.\r\n");
			AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,1,0);
		}
	}
}

void* ShareThread(void* param)
{
	try
	{
		Curl curl;
		curl.Init();
		Curl* parent_curl = (Curl*)param;
		curl.SetUsername(parent_curl->GetUsername());
		curl.SetPassword(parent_curl->GetPassword());
		curl.SetHost(parent_curl->GetHost());
		curl.SetPort(parent_curl->GetPort());

		while((!shutdown_now)&&(!quitappnow))
		{
			sharethread_active = true;
			Wait_ms(50);
			if(!cpu_mining_only){
				if(GPUstates.size()>0){
					foreachgpu(){
						if (!it->shares_available)
							continue;
						pthread_mutex_lock(&it->share_mutex);
						it->shares_available = false;
						deque<vector<uchar> > v;
						v.swap(it->shares);
						pthread_mutex_unlock(&it->share_mutex);
						while(!v.empty()){
							if((!shutdown_now)&&(!quitappnow))SubmitShare(curl, v.back());
							v.pop_back();
						}
					}
				}
			}
			if(!gpu_mining_only){
				foreachcpu(){
					if (!it->shares_available)
						continue;
					pthread_mutex_lock(&it->share_mutex);
					it->shares_available = false;
					deque<vector<uchar> > v;
					v.swap(it->shares);
					pthread_mutex_unlock(&it->share_mutex);
					while(!v.empty()){
						SubmitShare(curl, v.back());
						v.pop_back();
					}
				}
			}
		}
		curl.Quit();
	}
	catch(...)
	{
		pthread_exit(NULL);
		return NULL;
	}
	pthread_exit(NULL);
	return NULL;
}

void* LongPollThread(void* param)
{
	LongPollThreadParams* p = (LongPollThreadParams*)param;
	Curl curl;
	curl.Init();

	Curl* parent_curl = p->curl;

	curl.SetUsername(parent_curl->GetUsername());
	curl.SetPassword(parent_curl->GetPassword());
	curl.SetHost(parent_curl->GetHost());
	curl.SetPort(parent_curl->GetPort());

	string LP_url = longpoll_url;
	string LP_path;

	strGlobalUpdateInfo.Format(_T("%s%s%s"),_T("Long polling URL: ["),LP_url,_T("]. trying to parse.\r\n"));
	AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);

	clock_t lastcall = 0;

	vector<string> exploded = Explode(LP_url, '/');
	if (exploded.size() >= 2 && exploded[0] == "http:"){
		vector<string> exploded2 = Explode(exploded[1], ':');
		if (exploded2.size() != 2)
			goto couldnt_parse;
		curl.SetHost(exploded2[0]);
		curl.SetPort(exploded2[1]);
		if (exploded.size() <= 2)
			LP_path = '/';
		else
			LP_path = "/" + exploded[2];
	}
	else if (LP_url.length() > 0 && LP_url[0] == '/'){
		LP_path = LP_url;
	}
	else{
		goto couldnt_parse;
	}

	while((!shutdown_now)&&(!quitappnow)){
		clock_t ticks = ticker();
		if (ticks-lastcall < 5000){
			Wait_ms(ticks-lastcall);
		}
		lastcall = ticks;
		try
		{
			p->app->Parse(curl.GetWork_LP(LP_path, 60));
		}
		catch(...)
		{
			curl.Quit();
			pthread_exit(NULL);
			return NULL;}
	}
	curl.Quit();
	pthread_exit(NULL);
	return NULL;

couldnt_parse:
	strGlobalUpdateInfo.Format(_T("%s%s%s"),_T("Couldn't parse long polling URL: ["),LP_url,_T("]. trying to parse.\r\n"));
	AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
	curl.Quit();
	pthread_exit(NULL);
	return NULL;
}

// CAboutDlg dialog used for App About
class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();
	enum { IDD = IDD_ABOUTBOX };

protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	virtual BOOL OnInitDialog();
protected:
	DECLARE_MESSAGE_MAP()
public:
	CString m_strSCAddress;
	CString m_strInfo;
};

CAboutDlg::CAboutDlg() : CDialogEx(CAboutDlg::IDD)
	, m_strSCAddress(SCADDRESS)
	, m_strInfo(_T(""))
{
	m_strInfo=APPABOUTINFOSTR+LUUMINERVERSION;
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Text(pDX, IDC_EDIT_SC_ADDRESS, m_strSCAddress);
}
BOOL CAboutDlg::OnInitDialog()
{
	this->SetDlgItemText(IDC_STATIC_ABOUT_APP_INFO,m_strInfo);
	this->SetDlgItemText(IDC_EDIT_SC_ADDRESS,m_strSCAddress);
	CDialogEx::OnInitDialog();

	return TRUE;
}
BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()

// CluuMinerDlg dialog
CluuMinerDlg::CluuMinerDlg(CWnd* pParent /*=NULL*/,CString strWindowTitle)
	: CDialogEx(CluuMinerDlg::IDD, pParent)
	, m_strPoolAddress(_T(""))
	, m_iPoolPort(8322)
	, m_strPoolUsername(_T(""))
	, m_strPoolPass(_T(""))
	, m_strMiningInfo(_T(""))
	, m_bStartOrStop(TRUE)
	, m_bCPUMiningOnly(FALSE)
	, m_strAppInformation(APPINFOSTR)
	, m_bForceInitMiner(FALSE)
	, m_bGlobalMinerWasInitialized(FALSE)
	, m_bGPUMiningOnly(FALSE)
{
	m_strWindowTitle=strWindowTitle;
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CluuMinerDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Text(pDX, IDC_EDIT_MINING_INFO, m_strMiningInfo);
	DDX_Text(pDX, IDC_EDIT_POOL_ADDRESS, m_strPoolAddress);
	DDX_Text(pDX, IDC_EDIT_POOL_PORT, m_iPoolPort);
	DDV_MinMaxInt(pDX, m_iPoolPort, 1024, 65535);
	DDX_Text(pDX, IDC_EDIT_POOL_USERNAME, m_strPoolUsername);
	DDX_Text(pDX, IDC_EDIT_POOL_PASS, m_strPoolPass);
	DDX_Check(pDX, IDC_CHECK_CPU_MINING_ONLY, m_bCPUMiningOnly);
	DDX_Text(pDX, IDC_EDIT1, m_strAppInformation);
	DDX_Check(pDX, IDC_CHECK_FORCE_INIT_MINER, m_bForceInitMiner);
	DDX_Check(pDX, IDC_CHECK_GPU_MINING_ONLY, m_bGPUMiningOnly);
}

BEGIN_MESSAGE_MAP(CluuMinerDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_BUTTON_START_STOP_MINING, &CluuMinerDlg::OnBnClickedButtonStartStopMining)
	ON_MESSAGE(WM_UPDATE_INFO, OnUpdateInfo)
	ON_COMMAND(ID_ABOUT_ABOUT, &CluuMinerDlg::OnHelpAbout)
	ON_COMMAND(ID_ABOUT_HELP, &CluuMinerDlg::OnAboutHelp)
	ON_COMMAND(ID_FILE_EXIT, &CluuMinerDlg::OnFileExit)
	ON_BN_CLICKED(IDC_CHECK_CPU_MINING_ONLY, &CluuMinerDlg::OnClickedCheckCpuMiningOnly)
	ON_BN_CLICKED(IDC_CHECK_GPU_MINING_ONLY, &CluuMinerDlg::OnClickedCheckGpuMiningOnly)
	ON_BN_CLICKED(IDC_CHECK_FORCE_INIT_MINER, &CluuMinerDlg::OnBnClickedCheckForceInitMiner)
END_MESSAGE_MAP()

// CluuMinerDlg message handlers
BOOL CluuMinerDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// Add "About..." menu item to system menu.
	// IDM_ABOUTBOX must be in the system command range.
	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != NULL)
	{
		BOOL bNameValid;
		CString strAboutMenu;
		bNameValid = strAboutMenu.LoadString(IDS_ABOUTBOX);
		ASSERT(bNameValid);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
		//pSysMenu->EnableMenuItem( SC_CLOSE, MF_BYCOMMAND|MF_GRAYED);
	}

	// Set the icon for this dialog.  The framework does this automatically
	//  when the application's main window is not a dialog
	SetIcon(m_hIcon, TRUE);			// Set big icon
	SetIcon(m_hIcon, FALSE);		// Set small icon

	// TODO: Add extra initialization here
	this->SetWindowText(m_strWindowTitle);

	m_menu.LoadMenu(IDR_MENU_MAIN);
	SetMenu(&m_menu);

	//Tool Tip
	EnableToolTips(TRUE);
	if(!m_tt)
	{
		m_tt.Create(this);
		m_tt.Activate(TRUE);
		m_tt.AddTool(GetDlgItem(IDC_CHECK_FORCE_INIT_MINER), FORCEINITMINERSTR);//Ìí¼Ó
		m_tt.AddTool(GetDlgItem(IDC_CHECK_CPU_MINING_ONLY), CPUONLYSTR);//Ìí¼Ó
		m_tt.AddTool(GetDlgItem(IDC_CHECK_GPU_MINING_ONLY), GPUONLYSTR);//Ìí¼Ó
		m_tt.SetTipTextColor(RGB(255,0,0));
		m_tt.SetDelayTime(150);
	}

	//reader user inforamtion from config file to user interface
	ReadUserInfoToUserInterface();
	UpdateData(false);

	return TRUE;  // return TRUE  unless you set the focus to a control
}

BOOL CluuMinerDlg::PreTranslateMessage(MSG* pMsg)
{
	if(pMsg-> message   ==   WM_KEYDOWN)
	{
		switch(pMsg-> wParam)
		{
		case   VK_RETURN://no enter
			return   TRUE;
		case   VK_ESCAPE://no Esc
			return   TRUE;
		}
	}
	if(pMsg->hwnd == GetDlgItem(IDC_CHECK_CPU_MINING_ONLY)->GetSafeHwnd())
		m_tt.RelayEvent(pMsg);
	return   CDialog::PreTranslateMessage(pMsg);
}

void CluuMinerDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else if (nID == SC_CLOSE)
	{
		OnFileExit();
	}
	else
	{
		CDialogEx::OnSysCommand(nID, lParam);
	}
}

// If you add a minimize button to your dialog, you will need the code below
//  to draw the icon.  For MFC applications using the document/view model,
//  this is automatically done for you by the framework.

void CluuMinerDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // device context for painting

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// Center icon in client rectangle
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// Draw the icon
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

// The system calls this function to obtain the cursor to display while the user drags
//  the minimized window.
HCURSOR CluuMinerDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

void CluuMinerDlg::OnHelpAbout()
{
	OnSysCommand(IDM_ABOUTBOX,0);
}
//void* InitMinerThreadFunc(void* lpParam)
//{
//	try{
//		OpenCL* opencl=(OpenCL*)lpParam;
//		if(FUNCTION_OK!=opencl->Init())
//		{
//			CluuMinerDlg *pDlg = (CluuMinerDlg *)AfxGetApp()->GetMainWnd();
//			strGlobalUpdateInfo=_T("Init opencl failed.\r\n");
//			pDlg->SendMessage(WM_UPDATE_INFO,0,0);
//			initminerfailed=true;
//			pthread_exit(NULL);
//		}
//	}
//	catch(...)
//	{
//		pthread_exit(NULL);
//		return NULL;
//	}
//	pthread_exit(NULL);
//	return NULL;
//}
void* MiningThreadFunc(void* lpParam)
{
	try{
		if((!shutdown_now)&&(!quitappnow)){
			CluuMinerDlg *pDlg = (CluuMinerDlg *)AfxGetApp()->GetMainWnd();
			strGlobalUpdateInfo=_T("Starting right now, please wait....\r\n");
			pDlg->SendMessage(WM_UPDATE_INFO,0,0);

			if (!cpu_mining_only)pDlg->opencl.CreateGPUThread();
			if (!gpu_mining_only)pDlg->cpuminer.CreateCPUThread();

			pthread_t sharethread;
			pthread_create(&sharethread, NULL, ShareThread, &(pDlg->curl));

			pDlg->Parse(pDlg->curl.GetWork());

			const int work_update_period_ms = 2000;

			pthread_t longpollthread;
			LongPollThreadParams lp_params;
			if (config.GetValue<bool>("long_polling") && longpoll_active){
				strGlobalUpdateInfo=_T("Activating long polling.\r\n");
				pDlg->SendMessage(WM_UPDATE_INFO,0,0);
				lp_params.app = pDlg;
				lp_params.curl = &(pDlg->curl);
				pthread_create(&longpollthread, NULL, LongPollThread, &lp_params);
			}

			clock_t ticks = ticker();
			clock_t starttime = ticker();
			pDlg->workupdate = ticker();

			clock_t sharethread_update_time = ticker();

			while((!shutdown_now)&&(!quitappnow)){
				Wait_ms(50);
				clock_t timeclock = ticker();
				if (timeclock - current_work_time >= WORK_EXPIRE_TIME_SEC*1000){
					if (!current_work.old){
						strGlobalUpdateInfo.Format(_T("%s%s"),humantime().c_str(),_T("Work too old... waiting for getwork.\r\n"));
						pDlg->SendMessage(WM_UPDATE_INFO,1,0);
					}
					current_work.old = true;
				}
				if (sharethread_active){
					sharethread_active = false;
					sharethread_update_time = timeclock;
				}
				if (timeclock-sharethread_update_time >= SHARE_THREAD_RESTART_THRESHOLD_SEC*1000){
					strGlobalUpdateInfo=_T("Share thread messed up. Starting another one.\r\n");
					pDlg->SendMessage(WM_UPDATE_INFO,1,0);
					pthread_create(&sharethread, NULL, ShareThread, &(pDlg->curl));
				}
				if (getwork_now || timeclock - current_work_time >= work_update_period_ms){
					try{
						pDlg->Parse(pDlg->curl.GetWork());
						getwork_now = false;
					}
					catch(...){
					}
				}
				if (timeclock - ticks >= 1000){
					ullint totalhashesGPU=0;
					if(!cpu_mining_only){foreachgpu() totalhashesGPU += it->hashes;}
					ullint totalhashesCPU=0;
					if(!gpu_mining_only){foreachcpu()totalhashesCPU += it->hashes;}
					ticks += (timeclock-ticks)/1000*1000;
					float stalepercent = 0.0f;
					if (shares_valid+shares_invalid+shares_hwinvalid != 0)
						stalepercent = 100.0f*float(shares_invalid+shares_hwinvalid)/float(shares_invalid+shares_valid+shares_hwinvalid);

					if (ticks-starttime == 0){
						CString strOutInfo1,strOutInfo2,strOutInfo3,strOutInfo4;
						strOutInfo1.Format(_T("%s%d%s"),_T("??? kH/s, valid shares: "),shares_valid);
						strOutInfo2.Format(_T("%s%d"),_T(" ,invalid shares:"),shares_invalid);
						strOutInfo3.Format(_T("%s%f%s%d"),_T("\r\n invalid percent: "),stalepercent,_T("%, time used: "),(ticks-starttime)/1000);
						strOutInfo4=_T( "s    \r\n");
						strGlobalUpdateInfo=strOutInfo1+strOutInfo2+strOutInfo3+strOutInfo4;
						pDlg->SendMessage(WM_UPDATE_INFO,1,0);
					}
					else{
						strGlobalUpdateInfo=_T("");
						CString strGPU,strCPU;
						if (totalhashesGPU != 0){
							strGPU.Format(_T("%s%.2f%s"),_T("GPU "),double(totalhashesGPU)/(ticks-starttime),_T( "kH/s\r\n"));
							strGlobalUpdateInfo+=strGPU;
						}
						if (totalhashesCPU != 0){
							strCPU.Format(_T("%s%.2f%s"),_T("CPU "),double(totalhashesCPU)/(ticks-starttime),_T( "kH/s\r\n"));
							strGlobalUpdateInfo+=strCPU;
						}
						CString strOutInfo1,strOutInfo2,strOutInfo3,strOutInfo4;
						strOutInfo1.Format(_T("%s%d"),_T("valid shares: "),shares_valid);
						strOutInfo2.Format(_T("%s%d"),_T(",  invalid shares:"),shares_invalid);
						strOutInfo3.Format(_T("%s%f%s%d"),_T("\r\ninvalid percent: "),stalepercent,_T("%, time used: "),(ticks-starttime)/1000);
						strOutInfo4=_T( "s    \r\n");
						strGlobalUpdateInfo+=strOutInfo1+strOutInfo2+strOutInfo3+strOutInfo4;
						pDlg->SendMessage(WM_UPDATE_INFO,1,0);
					}
				}
			}
		}
		pthread_exit(NULL);
		return NULL;
	}
	catch(string s){
		pthread_exit(NULL);
		strGlobalUpdateInfo=_T("Error: ");
		strGlobalUpdateInfo+=CString(s.c_str());
		strGlobalUpdateInfo+=_T("\r\n");
		AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,1,0);
		return NULL;
	}
}

int CluuMinerDlg::InitSCMiner()
{
	CMenu* pSysMenu = AfxGetApp()->GetMainWnd()->GetSystemMenu(FALSE);;
	try{
		strGlobalUpdateInfo=_T("Init Miner right now, please wait....\r\n");
		SendMessage(WM_UPDATE_INFO,1,0);
		string config_name = "reaper.conf";
		getworks = 0;
		if(FUNCTION_OK!=config.Load(config_name))return FUNCTION_ERROR;
		globalconfs.local_worksize = config.GetValue<uint>("worksize");
		if (config.GetValue<string>("aggression") == "max"){
			globalconfs.global_worksize = 1<<11;
			globalconfs.max_aggression = true;
		}
		else{
			globalconfs.global_worksize = (1<<config.GetValue<uint>("aggression"));
			globalconfs.max_aggression = false;
		}
		globalconfs.threads_per_gpu = config.GetValue<uint>("threads_per_gpu");
		if (config.GetValue<string>("kernel") == "")
			config.SetValue("kernel", 0, "reaper.cl");
		globalconfs.save_binaries = config.GetValue<bool>("save_binaries");
		uint numdevices = config.GetValueCount("device");
		for(uint i=0; i<numdevices; ++i)
			globalconfs.devices.push_back(config.GetValue<uint>("device", i));

		globalconfs.cputhreads = config.GetValue<uint>("cpu_mining_threads");
		if (cpu_mining_only){
			if (globalconfs.cputhreads == 0){
				gpu_mining_only=true;
				strGlobalUpdateInfo=_T("cpu_mining_threads is zero. Nothing to do, quitting..\r\n");
				SendMessage(WM_UPDATE_INFO,1,0);
				return FUNCTION_ERROR;
			}
		}

		if (globalconfs.cputhreads == 0 && globalconfs.threads_per_gpu == 0){
			strGlobalUpdateInfo=_T("No CPU or GPU mining threads.. please set either cpu_mining_threads or threads_per_gpu to something other than 0.\r\n");
			SendMessage(WM_UPDATE_INFO,0,0);
			return FUNCTION_ERROR;
		}
		globalconfs.platform = config.GetValue<uint>("platform");

		if (globalconfs.local_worksize > globalconfs.global_worksize){
			strGlobalUpdateInfo=_T("Aggression is too low for the current worksize. Increasing.\r\n");
			SendMessage(WM_UPDATE_INFO,1,0);
			globalconfs.global_worksize = globalconfs.local_worksize;
		}

		BlockHash_Init();
		current_work.old = true;

		Curl::GlobalInit();
		curl.Init();
		CString cstrPort;
		cstrPort.Format(_T("%d"),m_iPoolPort);
		std::string strPoolAddress= (CStringA)(m_strPoolAddress);
		std::string strPort= (CStringA)cstrPort;
		std::string strPoolUsername= (CStringA)(m_strPoolUsername);
		std::string strPoolPass= (CStringA)(m_strPoolPass);
		curl.SetHost(strPoolAddress);
		curl.SetPort(strPort);
		curl.SetUsername(strPoolUsername);
		curl.SetPassword(strPoolPass);
		//disable close

		if (pSysMenu != NULL)pSysMenu->EnableMenuItem( SC_CLOSE, MF_BYCOMMAND|MF_GRAYED);

		if (!cpu_mining_only)
		{
			/*pthread_create(&MiningThread, NULL, &InitMinerThreadFunc, &opencl);
			if(initminerfailed)
			{
				if (pSysMenu != NULL)pSysMenu->EnableMenuItem( SC_CLOSE, MF_ENABLED);
				return FUNCTION_ERROR;
			}*/
			if(FUNCTION_OK!=opencl.Init())
			{
				SendMessage(WM_UPDATE_INFO,0,0);
				return FUNCTION_ERROR;
			}
		}
		if (!gpu_mining_only)cpuminer.Init();
		SendMessage(WM_UPDATE_INFO,0,0);
		//enable close
		if (pSysMenu != NULL)pSysMenu->EnableMenuItem( SC_CLOSE, MF_ENABLED);

		strGlobalUpdateInfo=_T("Miner Initialized!\r\n");
		SendMessage(WM_UPDATE_INFO,1,0);

		m_bGlobalMinerWasInitialized=TRUE;
	}
	catch(exception e)
	{
		if (pSysMenu != NULL)pSysMenu->EnableMenuItem( SC_CLOSE, MF_ENABLED);
		return FUNCTION_ERROR;
	}
	return FUNCTION_OK;
}
void CluuMinerDlg::OnFileStartmine()
{
	UpdateData(TRUE);
	shutdown_now=false;

	m_strMiningInfo=_T("");
	m_strMiningInfo1=_T("");
	UpdateData(FALSE);
	//pThread=AfxBeginThread(ThreadFunc, 0);
	//cpu only or not
	cpu_mining_only=false;
	if(m_bCPUMiningOnly==TRUE)cpu_mining_only=true;
	if(m_bCPUMiningOnly==FALSE)cpu_mining_only=false;
	gpu_mining_only=false;
	if(m_bGPUMiningOnly==TRUE)gpu_mining_only=true;
	if(m_bGPUMiningOnly==FALSE)gpu_mining_only=false;
	//Init Miner
	if((!m_bGlobalMinerWasInitialized)||((m_bGlobalMinerWasInitialized)&&(m_bForceInitMiner)))
		if(FUNCTION_OK!=InitSCMiner()){
			MessageBox(_T("Miner Initialization Failed, please check out!"),_T(""),MB_OKCANCEL);
			//((CButton*)this->GetDlgItem(IDC_BUTTON_START_STOP_MINING))->EnableWindow(FALSE);
			this->SetDlgItemText(IDC_BUTTON_START_STOP_MINING,_T("Start Mining"));
			this->m_bStartOrStop=TRUE;
			return;
		}
		//Create Mining thread
		pthread_create(&MiningThread, NULL, &MiningThreadFunc, &curl);
}

void CluuMinerDlg::OnFileStopmine()
{
	strGlobalUpdateInfo=_T("\r\n Stopping right now, please wait....\r\n");
	SendMessage(WM_UPDATE_INFO,0,0);

	getwork_now = false;
	Sleep(200);
	shutdown_now = true;
	Sleep(200);

	shares_valid = 0;
	shares_invalid = 0;
	shares_hwinvalid = 0;
	current_work_time = 0;

	if((m_bGlobalMinerWasInitialized)&&(m_bForceInitMiner)){
		try{
			cpuminer.Quit();
			opencl.Quit();
			curl.Quit();
			BlockHash_DeInit();
			globalconfs.devices.clear();
			config.Clear();
		}
		catch(...){
		}
	}
	strGlobalUpdateInfo=_T("Mining is now stopped! \r\n");
	SendMessage(WM_UPDATE_INFO,0,0);
}

bool targetprinted=false;
pthread_mutex_t current_work_mutex = PTHREAD_MUTEX_INITIALIZER;
Work current_work;

void CluuMinerDlg::Parse(string data)
{
	workupdate = ticker();
	if (data == ""){
		strGlobalUpdateInfo=_T("Couldn't connect to pool. Trying again in a few seconds...\r\n");
		SendMessage(WM_UPDATE_INFO,1,0);
		return;
	}
	Json::Value root, result, error;
	Json::Reader reader;
	bool parsing_successful = reader.parse( data, root );
	if (!parsing_successful){
		goto got_error;
	}

	result = root.get("result", "null");
	error = root.get("error", "null");

	if (result.isObject()){
		Json::Value::Members members = result.getMemberNames();
		uint neededmembers=0;
		for(Json::Value::Members::iterator it = members.begin(); it != members.end(); ++it){
			if (*it == "data")	++neededmembers;
		}
		if (neededmembers != 1 || !result["data"].isString()){
			goto got_error;
		}

		++getworks;
		Work newwork;
		newwork.data = HexStringToVector(result["data"].asString());
		newwork.old = false;
		newwork.time = ticker();
		current_work_time = ticker();

		if (!targetprinted){
			targetprinted = true;
			strGlobalUpdateInfo.Format(_T("%s%s%s"),_T("target_share: \r\n"),CString(result["target_share"].asString().c_str()),_T("\r\n\r\n"));
			AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,0,0);
		}
		newwork.target_share = HexStringToVector(result["target_share"].asString().substr(2));
		newwork.ntime_at_getwork = (*(ullint*)&newwork.data[76]) + 1;

		current_work.time = ticker();
		pthread_mutex_lock(&current_work_mutex);
		current_work = newwork;
		pthread_mutex_unlock(&current_work_mutex);
		return;
	}
	else if (!error.isNull())
	{
		strGlobalUpdateInfo=(CString)(error.asString().c_str());
		strGlobalUpdateInfo+=_T("\r\n Code ");
		SendMessage(WM_UPDATE_INFO,1,0);
	}
got_error:
	strGlobalUpdateInfo=_T("Error with pool: ");
	SendMessage(WM_UPDATE_INFO,1,0);
	return;
}

void CluuMinerDlg::OnBnClickedButtonStartStopMining()
{
	if(m_bStartOrStop==TRUE){
		m_bStartOrStop=false;
		this->SetDlgItemText(IDC_BUTTON_START_STOP_MINING,_T("Stop Mining"));
		OnFileStartmine();
	}
	else{
		m_bStartOrStop=true;
		this->SetDlgItemText(IDC_BUTTON_START_STOP_MINING,_T("Start Mining"));
		OnFileStopmine();
	}
}

LRESULT CluuMinerDlg::OnUpdateInfo(WPARAM wParam, LPARAM lParam)
{
	if((!shutdown_now)&&(!quitappnow)){
		int updateornot=wParam;
		if(wParam==0){
			m_strMiningInfo+=strGlobalUpdateInfo;
			m_strMiningInfo1=m_strMiningInfo;
		}
		if(wParam==1){
			m_strMiningInfo=m_strMiningInfo1;
			m_strMiningInfo+=strGlobalUpdateInfo;
		}
		UpdateData(FALSE);
		CEdit* pBoxUpdateInfo=(CEdit*)GetDlgItem(IDC_EDIT_MINING_INFO);
		pBoxUpdateInfo->LineScroll (pBoxUpdateInfo->GetLineCount(), 0);
	}
	return 0;
}

void CluuMinerDlg::ReadUserInfoToUserInterface()
{
	string config_name = "reaper.conf";
	if(FUNCTION_OK!=config.Load(config_name)){
		MessageBox(_T("config file reaper.conf doesn't exist, please check out! Application Quit Now!"),_T(""),MB_OKCANCEL);
		PostQuitMessage(0);
		return;
	}
	m_strPoolAddress=config.GetValue<string>("host").c_str();
	m_iPoolPort=FromString<unsigned short>(config.GetValue<string>("port"));
	m_strPoolUsername=config.GetValue<string>("user").c_str();
	m_strPoolPass=config.GetValue<string>("pass").c_str();
	UpdateData(FALSE);
	config.Clear();
}

void CluuMinerDlg::OnAboutHelp()
{
	ShellExecute(GetSafeHwnd(), NULL,  LUUMINERWEBSITE,   NULL,   NULL,   SW_SHOWNORMAL);
}

void CluuMinerDlg::OnFileExit()
{
	quitappnow=true;
	if(shutdown_now==false){
		shutdown_now = true;
		Sleep(1000);
	}
	if(m_bGlobalMinerWasInitialized)
	{
		m_bGlobalMinerWasInitialized=false;
		try{
			cpuminer.Quit();
			opencl.Quit();
			curl.Quit();
			BlockHash_DeInit();
			globalconfs.devices.clear();
			config.Clear();
		}
		catch(...){
		}
	}
	PostQuitMessage(0);
}

void CluuMinerDlg::OnClickedCheckCpuMiningOnly()
{
	if(((CButton *)GetDlgItem(IDC_CHECK_CPU_MINING_ONLY))->GetCheck()==BST_CHECKED)
	{
		((CButton *)GetDlgItem(IDC_CHECK_GPU_MINING_ONLY))->SetCheck(BST_UNCHECKED);
		((CButton *)GetDlgItem(IDC_CHECK_GPU_MINING_ONLY))->EnableWindow(FALSE);
		m_bGPUMiningOnly=FALSE;
		m_bCPUMiningOnly=TRUE;
	}
	if(((CButton *)GetDlgItem(IDC_CHECK_CPU_MINING_ONLY))->GetCheck()==BST_UNCHECKED)
	{
		((CButton *)GetDlgItem(IDC_CHECK_GPU_MINING_ONLY))->EnableWindow(TRUE);
		m_bCPUMiningOnly=FALSE;
	}
}

void CluuMinerDlg::OnClickedCheckGpuMiningOnly()
{
	if(((CButton *)GetDlgItem(IDC_CHECK_GPU_MINING_ONLY))->GetCheck()==BST_CHECKED)
	{
		((CButton *)GetDlgItem(IDC_CHECK_CPU_MINING_ONLY))->SetCheck(BST_UNCHECKED);
		((CButton *)GetDlgItem(IDC_CHECK_CPU_MINING_ONLY))->EnableWindow(FALSE);
		m_bCPUMiningOnly=FALSE;
		m_bGPUMiningOnly=TRUE;
	}
	if(((CButton *)GetDlgItem(IDC_CHECK_GPU_MINING_ONLY))->GetCheck()==BST_UNCHECKED)
	{
		((CButton *)GetDlgItem(IDC_CHECK_CPU_MINING_ONLY))->EnableWindow(TRUE);
		m_bGPUMiningOnly=FALSE;
	}
}

void CluuMinerDlg::OnBnClickedCheckForceInitMiner()
{
	UpdateData(TRUE);
}