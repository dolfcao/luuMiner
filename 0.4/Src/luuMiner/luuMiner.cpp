
#include "stdafx.h"
#include "luuMiner.h"
#include "luuMinerDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// CluuMinerApp
BEGIN_MESSAGE_MAP(CluuMinerApp, CWinApp)
	ON_COMMAND(ID_HELP, &CWinApp::OnHelp)
END_MESSAGE_MAP()


// CluuMinerApp construction
CluuMinerApp::CluuMinerApp()
{
}


// The one and only CluuMinerApp object
CluuMinerApp theApp;


// CluuMinerApp initialization
BOOL CluuMinerApp::InitInstance()
{
	INITCOMMONCONTROLSEX InitCtrls;
	InitCtrls.dwSize = sizeof(InitCtrls);
	InitCtrls.dwICC = ICC_WIN95_CLASSES;
	InitCommonControlsEx(&InitCtrls);

	CWinApp::InitInstance();
	CShellManager *pShellManager = new CShellManager;
	SetRegistryKey(_T("luuMiner"));

	CluuMinerDlg dlg;
	m_pMainWnd = &dlg;
	INT_PTR nResponse = dlg.DoModal();

	if (pShellManager != NULL)
	{
		delete pShellManager;
	}
	return FALSE;
}

