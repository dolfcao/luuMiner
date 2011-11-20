#include "Global.h"
#include "Curl.h"

#include "curl/curl.h"
#include "Util.h"

//为了实现输出添加的内容
#include "stdafx.h"
#include "luuMiner.h"
#include "luuMinerDlg.h"
//


void Curl::GlobalInit()
{
	curl_global_init(CURL_GLOBAL_ALL);
}

void Curl::Init()
{
	curl = curl_easy_init();
	if (curl == NULL)
	{
		throw string("libcurl initialization failure");
	}
}

void Curl::Quit()
{
	if (curl != NULL)
	{
		curl_easy_cleanup(curl);
	}
	curl_global_cleanup();
}

size_t ResponseCallback(void *ptr, size_t size, size_t nmemb, void *data)
{
	string* getworksentdata = (string*)data;
	try 	
	{ 	
		for(uint i=0; i<size*nmemb; ++i) 	
		{ 		
			if(ptr!=NULL && data!=NULL) 		
			{ 			
				char c = ((char*)ptr)[i]; 			
				getworksentdata->push_back(c); 		
			} 	
		}
	} 	
	catch(std::exception s) 	
	{ 		
		//cout << "(1) Error: " << s.what() << endl; 	
	}
	return size*nmemb; 
}

string longpoll_url;
bool longpoll_active=false;
size_t HeaderCallback( void *ptr, size_t size, size_t nmemb, void *userdata)
{
	string hdr;
	for(uint i=0; i<size*nmemb; ++i)
	{
		char c = ((char*)ptr)[i];
		hdr.push_back(c);
	}
	if (!longpoll_active && hdr.length() >= 0x10 && hdr.substr(0,0xF) == "X-Long-Polling:")
	{
		longpoll_url = hdr.substr(0x10);
		longpoll_url = longpoll_url.substr(0, longpoll_url.length()-2);
		//cout << "Longpoll url -->" << longpoll_url << "<-- " << endl;
		longpoll_active = true;
	}
	return size*nmemb;
}

string Curl::GetWork_LP(string path, uint timeout)
{
	return Execute(GETWORK_LP, "", path, timeout);
}

string Curl::GetWork(string path, uint timeout)
{
	return Execute(GETWORK, "", path, timeout);
}

string Curl::TestWork(string work)
{
	return Execute(TESTWORK, work, "", 1);
}

string Curl::Execute(Curl::EXEC_TYPE type, string work, string path, uint timeout)
{

	string responsedata;
	string strTmp;
	try
	{
		strTmp="http://" + host + path;
		if(curl_easy_setopt(curl, CURLOPT_URL, strTmp.c_str())!=CURLE_OK){
			responsedata="";
			return responsedata;
		}
		strTmp=username + ":" + password;
		if(curl_easy_setopt(curl, CURLOPT_USERPWD, strTmp.c_str())!=CURLE_OK){
			responsedata="";
			return responsedata;
		}
		if(curl_easy_setopt(curl, CURLOPT_PORT, port)!=CURLE_OK){
			responsedata="";
			return responsedata;
		}
		if(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, ResponseCallback)!=CURLE_OK){
			responsedata="";
			return responsedata;
		}
		if(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responsedata)!=CURLE_OK){
			responsedata="";
			return responsedata;
		}
		if(curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback)!=CURLE_OK){
			responsedata="";
			return responsedata;
		}
		if (proxy != ""){
			if(curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str())!=CURLE_OK){
				responsedata="";
				return responsedata;
			}
		}

		curl_slist* headerlist = NULL;
		headerlist = curl_slist_append(headerlist, "Content-Type: application/json");
		headerlist = curl_slist_append(headerlist, "Accept: application/json");
		headerlist = curl_slist_append(headerlist, "User-Agent: reaper/" REAPER_VERSION);

		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerlist);

		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout);

		if (type == GETWORK_LP){			
			if(curl_easy_setopt(curl, CURLOPT_POSTFIELDS, NULL)!=CURLE_OK){
				responsedata="";
				return responsedata;
			}
			if(curl_easy_setopt(curl, CURLOPT_POST, 0)!=CURLE_OK){
				responsedata="";
				return responsedata;
			}
		}
		else if (type == GETWORK){
			if(curl_easy_setopt(curl, CURLOPT_POST, 1)!=CURLE_OK){
				responsedata="";
				return responsedata;
			}
			if(curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, "{\"method\":\"sc_getwork\",\"params\":[],\"id\":1}")!=CURLE_OK){
				responsedata="";
				return responsedata;
			}
		}
		else if (type == TESTWORK)
		{
			if(curl_easy_setopt(curl, CURLOPT_POST, 1)!=CURLE_OK){
				responsedata="";
				return responsedata;
			}
			strTmp="{\"method\":\"sc_testwork\",\"id\":\"1\",\"params\":[\"" + work + "\"]}";
			if(curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, strTmp.c_str())!=CURLE_OK){
				responsedata="";
				return responsedata;
			}			
		}
		else
		{
			//cout << "ERMYYR" << endl;
		}

		CURLcode code = curl_easy_perform(curl);
		if(code != CURLE_OK)
		{
			if (code == CURLE_COULDNT_CONNECT)
			{
				strGlobalUpdateInfo=_T("Could not connect. Server down?\r\n");
				AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,1,0);
			}
			else
			{
				strGlobalUpdateInfo=_T("Error submitting work. See http://curl.haxx.se/libcurl/c/libcurl-errors.html for error code explanations.\r\n");
				AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,1,0);
			}
		}
		curl_slist_free_all(headerlist);
	}
	catch(std::exception s)
	{
		strGlobalUpdateInfo=_T("");
		strGlobalUpdateInfo+=_T("Error: ");
		strGlobalUpdateInfo+=CString(s.what());
		strGlobalUpdateInfo+=_T("\r\n");

		AfxGetApp()->GetMainWnd()->SendMessage(WM_UPDATE_INFO,1,0);

	}
	catch(...)
	{
	}
	return responsedata;
}

#undef SetPort
void Curl::SetPort(string port_) 
{ 
	port = FromString<unsigned short>(port_); 
}

string Curl::GetPort() 
{ 
	return ToString(port); 
}
