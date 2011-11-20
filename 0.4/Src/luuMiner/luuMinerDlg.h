#pragma once
#include "afxwin.h"

#include "Global.h"
#include "Curl.h"
#include "AppOpenCL.h"
#include "CPUMiner.h"
#include "Util.h"
#include "json/json.h"
#include "Config.h"
#include "RSHash.h"

//
#define WM_UPDATE_INFO WM_USER + 1999

const CString APPWINDOWTITLESTR=_T("Luu Miner");
const CString APPABOUTINFOSTR=_T("LuuMiner - SolidCoin GPU GUI MINER  ");
const CString APPINFOSTR=_T("LuuMiner - SolidCoin GPU GUI MINER For Windows  \r\n LuuMiner is based on reaper v12, Great Thanks to mtrlt! \r\n Donations are welcome! Thanks!\r\n sgChtTbUusT9W5grYVT1UqVr4ECWWaDYQj \r\n\r\n");
const CString SCADDRESS=_T("sgChtTbUusT9W5grYVT1UqVr4ECWWaDYQj");
const CString LUUMINERVERSION=_T("Version 0.4");
const CString LUUMINERWEBSITE=_T("https://github.com/dolfcao/luuMiner");

const CString FORCEINITMINERSTR=_T("Force to reinit miner during every start/stop, if no config changed, we recommand it unchecked! ");
const CString CPUONLYSTR=_T("Only use CPU to mine");
const CString GPUONLYSTR=_T("Only use GPU to mine");

// CluuMinerDlg dialog
class CluuMinerDlg : public CDialogEx
{
	// Construction
public:
	CluuMinerDlg(CWnd* pParent = NULL,CString strWindowTitle=APPWINDOWTITLESTR);	// standard constructor
	// Dialog Data
	enum { IDD = IDD_LUUMINER_DIALOG };
protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV support
	// Implementation
protected:
	HICON m_hIcon;
	// Generated message map functions
	//}AFX_MSG
	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	//自定义消息处理函数
	afx_msg LRESULT OnUpdateInfo(WPARAM wParam, LPARAM lParam);
	DECLARE_MESSAGE_MAP()

public:
	CString m_strWindowTitle;
	CMenu m_menu;
	//true started, can stop
	//false stopped, can start
	bool m_bStartOrStop;
	//Mining Pool Config
	CString m_strPoolAddress;
	int m_iPoolPort;
	CString m_strPoolUsername;
	CString m_strPoolPass;
	CString m_strMiningInfo,m_strMiningInfo1;

	BOOL m_bCPUMiningOnly;
	BOOL m_bGPUMiningOnly;
	CString m_strAppInformation;

	//interface option
	CWinThread* pThread;
	pthread_t MiningThread;
	//pthread_t InitMinerThread;
	CToolTipCtrl   m_tt;

	//First param indicates whether the miner need to be initialized or not during every start stop process
	//Second param indicated has the miner been initialized
	//optimizing
	BOOL m_bForceInitMiner, m_bGlobalMinerWasInitialized;

public:
	//Mining Functions
	Curl curl;
	OpenCL opencl;
	CPUMiner cpuminer;
	clock_t workupdate;
	uint getworks;
	void Parse(string data);

public:

	afx_msg void OnHelpAbout();
	afx_msg void OnFileStartmine();
	afx_msg void OnFileStopmine();
	afx_msg void OnBnClickedButtonStartStopMining();
	BOOL PreTranslateMessage(MSG* pMsg);

	//extra function
public:
	void ReadUserInfoToUserInterface();
	int InitSCMiner();
	afx_msg void OnAboutHelp();
	afx_msg void OnFileExit();
	afx_msg void OnClickedCheckCpuMiningOnly();
	afx_msg void OnClickedCheckGpuMiningOnly();
	afx_msg void OnBnClickedCheckForceInitMiner();
};