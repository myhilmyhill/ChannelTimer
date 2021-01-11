#include <windows.h>
#include <tchar.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <Powrprof.h>
#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "TVTestPlugin.h"
#include "resource.h"
#include <vector>
#include <string>
#include <windowsx.h>

#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"shlwapi.lib")
#pragma comment(lib,"powrprof.lib")


// FILETIME の単位
static const LONGLONG FILETIME_MS   = 10000LL;
static const LONGLONG FILETIME_SEC  = 1000LL * FILETIME_MS;
static const LONGLONG FILETIME_MIN  = 60LL * FILETIME_SEC;
static const LONGLONG FILETIME_HOUR = 60LL * FILETIME_MIN;

// FILETIME の時間差を求める
static LONGLONG DiffFileTime(const FILETIME &ft1, const FILETIME &ft2)
{
	LARGE_INTEGER li1, li2;

	li1.LowPart = ft1.dwLowDateTime;
	li1.HighPart = ft1.dwHighDateTime;
	li2.LowPart = ft2.dwLowDateTime;
	li2.HighPart = ft2.dwHighDateTime;

	return li1.QuadPart - li2.QuadPart;
}

// SYSTEMTIME の時間差を求める(ms単位)
static LONGLONG DiffSystemTime(const SYSTEMTIME &st1, const SYSTEMTIME &st2)
{
	FILETIME ft1, ft2;

	::SystemTimeToFileTime(&st1, &ft1);
	::SystemTimeToFileTime(&st2, &ft2);

	return DiffFileTime(ft1, ft2) / FILETIME_MS;
}


// ウィンドウクラス名
#define SLEEPTIMER_WINDOW_CLASS TEXT("TVTest Timer Window")

struct Timer
{
	// スリープ条件
	enum SleepCondition {
		CONDITION_DURATION,	// 時間経過
		CONDITION_DATETIME,	// 指定時刻
		CONDITION_EVENTEND	// 番組終了
	};

	SleepCondition condition;			// スリープする条件
	SYSTEMTIME dateToChange;
	DWORD durationToChange;				// スリープまでの時間(秒単位)
	WORD eventID;						// 現在の番組の event_id
	TVTest::ChannelSelectInfo channelInfo = {};
	Timer() noexcept {
		channelInfo.Size = sizeof(channelInfo);
		channelInfo.Flags = 0;
		channelInfo.Channel = -1;
		channelInfo.Space = -1;
	}
};

struct CServiceInfo {
	TCHAR szServiceName[64];
	WORD NetworkID;
	WORD ServiceID;
	int channel;
	CServiceInfo(const TVTest::ChannelInfo& ChInfo)
		: NetworkID(ChInfo.NetworkID)
		, ServiceID(ChInfo.ServiceID)
		, channel(ChInfo.Channel)
	{
		::lstrcpy(szServiceName, ChInfo.szChannelName);
	}
	bool operator==(const CServiceInfo& rhs) const {
		return this->ServiceID == rhs.ServiceID && this->NetworkID == rhs.NetworkID;
	}
	void toTchar(TCHAR* destination, size_t size) const {
		_stprintf_s(destination, size, _T("%d %d %s"), ServiceID, NetworkID, szServiceName);
	}
};

struct TuningSpaceInfo
{
	WCHAR Name[MAX_PATH];
};

// プラグインクラス
class CChannelTimer : public TVTest::CTVTestPlugin
{
	enum {
		TIMER_ID_SLEEP = 1,
		TIMER_ID_QUERY
	};

	static const int DEFAULT_POS = INT_MIN;

	bool m_fInitialized = false;				// 初期化済みか?
	TCHAR m_szIniFileName[MAX_PATH];	// INIファイルのパス
	Timer m_timer;
	bool m_fIgnoreRecStatus = true;			// 録画中でもスリープする
	int m_ConfirmTimeout = 10;				// 確認のタイムアウト時間(秒単位)
	bool m_fShowSettings = true;				// プラグイン有効時に設定表示
	bool m_fConfirm = true;					// 確認を取る
	POINT m_SettingsDialogPos;			// 設定ダイアログの位置
	HWND m_hwnd = nullptr;						// ウィンドウハンドル
	bool m_fEnabled = false;					// プラグインが有効か?
	int m_ConfirmTimerCount;			// 確認のタイマー
	std::vector<std::wstring> m_tuningSpaces;
	std::vector<CServiceInfo> m_channels;

	bool InitializePlugin();
	bool OnEnablePlugin(bool fEnable);
	bool BeginSleep();
	bool DoSleep();
	bool BeginTimer();
	void EndTimer();
	bool ShowSettingsDialog(HWND hwndOwner);

	static LRESULT CALLBACK EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void *pClientData);
	static CChannelTimer *GetThis(HWND hwnd);
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void *pClientData);
	static INT_PTR CALLBACK ConfirmDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void *pClientData);

public:
	CChannelTimer();
	virtual bool GetPluginInfo(TVTest::PluginInfo *pInfo);
	virtual bool Initialize();
	virtual bool Finalize();
};

CChannelTimer::CChannelTimer()
	: m_SettingsDialogPos(POINT{ DEFAULT_POS, DEFAULT_POS })
{}


bool CChannelTimer::GetPluginInfo(TVTest::PluginInfo *pInfo)
{
	// プラグインの情報を返す
	pInfo->Type           = TVTest::PLUGIN_TYPE_NORMAL;
	pInfo->Flags          = TVTest::PLUGIN_FLAG_HASSETTINGS		// 設定あり
	                      | TVTest::PLUGIN_FLAG_DISABLEONSTART;	// 起動時は常に無効
	pInfo->pszPluginName  = L"タイマー";
	pInfo->pszCopyright   = L"Public Domain";
	pInfo->pszDescription = L"";
	return true;
}


bool CChannelTimer::Initialize()
{
	// 初期化処理

	// アイコンを登録
	m_pApp->RegisterPluginIconFromResource(g_hinstDLL, MAKEINTRESOURCE(IDB_ICON));

	// イベントコールバック関数を登録
	m_pApp->SetEventCallback(EventCallback, this);

	return true;
}


// プラグインが有効にされた時の初期化処理
bool CChannelTimer::InitializePlugin()
{
	if (m_fInitialized)
		return true;

	// ウィンドウクラスの登録
	WNDCLASS wc;
	wc.style = 0;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = g_hinstDLL;
	wc.hIcon = nullptr;
	wc.hCursor = nullptr;
	wc.hbrBackground = nullptr;
	wc.lpszMenuName = nullptr;
	wc.lpszClassName = SLEEPTIMER_WINDOW_CLASS;
	if (::RegisterClass(&wc) == 0)
		return false;

	// ウィンドウの作成
	m_hwnd = ::CreateWindowEx(
		0, SLEEPTIMER_WINDOW_CLASS, nullptr, WS_POPUP,
		0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hinstDLL, this);
	if (m_hwnd == nullptr)
		return false;

	m_fInitialized = true;
	return true;
}


bool CChannelTimer::Finalize()
{
	// 終了処理

	// ウィンドウの破棄
	if (m_hwnd)
		::DestroyWindow(m_hwnd);

	return true;
}


// プラグインの有効/無効が切り替わった時の処理
bool CChannelTimer::OnEnablePlugin(bool fEnable)
{
	InitializePlugin();

	if (fEnable && m_fShowSettings) {
		if (!ShowSettingsDialog(m_pApp->GetAppWindow()))
			return false;
	}

	m_fEnabled = fEnable;

	if (m_fEnabled)
		BeginTimer();
	else
		EndTimer();

	return true;
}

// スリープ開始
bool CChannelTimer::BeginSleep()
{
	m_pApp->AddLog(L"スリープを開始します。");

	m_pApp->EnablePlugin(false);	// タイマーは一回限り有効
	EndTimer();		// EventCallbackで呼ばれるはずだが、念のため

	if (m_fConfirm) {
		// 確認ダイアログを表示
		TVTest::ShowDialogInfo Info;

		Info.Flags = 0;
		Info.hinst = g_hinstDLL;
		Info.pszTemplate = MAKEINTRESOURCE(IDD_CONFIRM);
		Info.pMessageFunc = ConfirmDlgProc;
		Info.pClientData = this;
		Info.hwndOwner = m_pApp->GetAppWindow();

		if (m_pApp->ShowDialog(&Info) != IDOK) {
			m_pApp->AddLog(L"ユーザーによってキャンセルされました。");
			return false;
		}
	}

	if (!m_fIgnoreRecStatus) {
		// 録画中はスリープ実行しない
		TVTest::RecordStatusInfo RecStat;
		if (!m_pApp->GetRecordStatus(&RecStat)) {
			m_pApp->AddLog(L"録画状態を取得できないのでキャンセルされました。",
				TVTest::LOG_TYPE_WARNING);
			return false;
		}
		if (RecStat.Status != TVTest::RECORD_STATUS_NOTRECORDING) {
			m_pApp->AddLog(L"録画中なのでキャンセルされました。");
			return false;
		}
	}

	// スリープ実行
	return DoSleep();
}


// スリープ実行
bool CChannelTimer::DoSleep()
{
	return m_pApp->SelectChannel(&m_timer.channelInfo);
}


// タイマー開始
bool CChannelTimer::BeginTimer()
{
	UINT_PTR Result;
	WCHAR szLog[256];
	const Timer::SleepCondition& condition = this->m_timer.condition;

	if (condition == Timer::SleepCondition::CONDITION_DURATION) {
		::wsprintfW(szLog, L"%lu 秒後にスリープします。", (unsigned long)this->m_timer.durationToChange);
		m_pApp->AddLog(szLog);
		Result = ::SetTimer(m_hwnd, TIMER_ID_SLEEP, this->m_timer.durationToChange * 1000, nullptr);
	}
	else if (condition == Timer::SleepCondition::CONDITION_DATETIME || condition == Timer::SleepCondition::CONDITION_EVENTEND) {
		if (condition == Timer::SleepCondition::CONDITION_DATETIME) {
			::wsprintfW(szLog, L"%d/%d/%d %02d:%02d:%02d (UTC) にスリープします。",
				this->m_timer.dateToChange.wYear, this->m_timer.dateToChange.wMonth, this->m_timer.dateToChange.wDay,
				this->m_timer.dateToChange.wHour, this->m_timer.dateToChange.wMinute, this->m_timer.dateToChange.wSecond);
			m_pApp->AddLog(szLog);
		}
		else {
			this->m_timer.eventID = 0;
		}
		Result = ::SetTimer(m_hwnd, TIMER_ID_QUERY, 3000, nullptr);
	}
	else {
		return false;
	}

	return Result != 0;
}


// タイマー停止
void CChannelTimer::EndTimer()
{
	::KillTimer(m_hwnd, TIMER_ID_SLEEP);
	::KillTimer(m_hwnd, TIMER_ID_QUERY);
}

// 設定ダイアログを表示
bool CChannelTimer::ShowSettingsDialog(HWND hwndOwner)
{
	TVTest::ShowDialogInfo Info;

	Info.Flags = 0;
	Info.hinst = g_hinstDLL;
	Info.pszTemplate = MAKEINTRESOURCE(IDD_SETTINGS);
	Info.pMessageFunc = SettingsDlgProc;
	Info.pClientData = this;
	Info.hwndOwner = hwndOwner;
	if (m_SettingsDialogPos.x != DEFAULT_POS && m_SettingsDialogPos.y != DEFAULT_POS) {
		Info.Flags |= TVTest::SHOW_DIALOG_FLAG_POSITION;
		Info.Position = m_SettingsDialogPos;
	}

	return m_pApp->ShowDialog(&Info) == IDOK;
}


// イベントコールバック関数
// 何かイベントが起きると呼ばれる
LRESULT CALLBACK CChannelTimer::EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void *pClientData)
{
	CChannelTimer *pThis = static_cast<CChannelTimer*>(pClientData);

	switch (Event) {
	case TVTest::EVENT_PLUGINENABLE:
		// プラグインの有効状態が変化した
		return pThis->OnEnablePlugin(lParam1 != 0);

	case TVTest::EVENT_PLUGINSETTINGS:
		// プラグインの設定を行う
		pThis->InitializePlugin();
		return pThis->ShowSettingsDialog(reinterpret_cast<HWND>(lParam1));
	}

	return 0;
}


// ウィンドウハンドルからthisを取得する
CChannelTimer *CChannelTimer::GetThis(HWND hwnd)
{
	return reinterpret_cast<CChannelTimer*>(::GetWindowLongPtr(hwnd, GWLP_USERDATA));
}


// ウィンドウプロシージャ
// 単にタイマーを処理するだけ
LRESULT CALLBACK CChannelTimer::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_CREATE:
		{
			LPCREATESTRUCT pcs = reinterpret_cast<LPCREATESTRUCT>(lParam);
			CChannelTimer *pThis = static_cast<CChannelTimer*>(pcs->lpCreateParams);

			::SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
		}
		return TRUE;

	case WM_TIMER:
		{
			CChannelTimer *pThis = GetThis(hwnd);
			Timer &timer = pThis->m_timer;

			if (wParam == TIMER_ID_SLEEP) {
				// 指定時間が経過したのでスリープ開始
				pThis->BeginSleep();
			} else if (wParam == TIMER_ID_QUERY) {
				if (timer.condition == Timer::SleepCondition::CONDITION_DATETIME) {
					SYSTEMTIME st;

					::GetSystemTime(&st);
					if (DiffSystemTime(st, timer.dateToChange) >= 0) {
						// 指定時刻が来たのでスリープ開始
						pThis->BeginSleep();
					}
				} else if (timer.condition == Timer::SleepCondition::CONDITION_EVENTEND) {
					TVTest::ProgramInfo Info = {};
					WCHAR szEventName[128];

					// 現在の番組の情報を取得
					Info.pszEventName = szEventName;
					Info.MaxEventName = _countof(szEventName);
					if (pThis->m_pApp->GetCurrentProgramInfo(&Info)) {
						if (timer.eventID == 0) {
							bool fSet = false;

							if (Info.Duration == 0) {
								// 終了時刻未定
								fSet = true;
							} else {
								FILETIME ft;
								::SystemTimeToFileTime(&Info.StartTime, &ft);
								LARGE_INTEGER li;
								li.LowPart = ft.dwLowDateTime;
								li.HighPart = ft.dwHighDateTime;
								li.QuadPart -= 9LL * FILETIME_HOUR;				// EPG日時(UTC+9) -> UTC
								li.QuadPart += Info.Duration * FILETIME_SEC;	// 終了時刻
								ft.dwLowDateTime = li.LowPart;
								ft.dwHighDateTime = li.HighPart;
								FILETIME CurrentTime;
								::GetSystemTimeAsFileTime(&CurrentTime);
								// 番組終了が2分以内の場合は、次の番組を対象にする
								if (DiffFileTime(ft, CurrentTime) > 2LL * FILETIME_MIN)
									fSet = true;
							}

							if (fSet) {
								timer.eventID = Info.EventID;
								pThis->m_pApp->AddLog(L"この番組が終了したらします。");
								pThis->m_pApp->AddLog(szEventName);
							}
						} else if (timer.eventID != Info.EventID) {
							// 番組が変わったのでスリープ開始
							pThis->BeginSleep();
						}
					}
				}
			}
		}
		return 0;
	}

	return ::DefWindowProc(hwnd,uMsg,wParam,lParam);
}


static void EnableDlgItem(HWND hDlg, int ID, BOOL fEnable)
{
	::EnableWindow(::GetDlgItem(hDlg, ID), fEnable);
}

static void EnableDlgItems(HWND hDlg, int FirstID, int LastID, BOOL fEnable)
{
	for (int i = FirstID; i <= LastID; i++)
		EnableDlgItem(hDlg, i, fEnable);
}

// 設定ダイアログプロシージャ
INT_PTR CALLBACK CChannelTimer::SettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void *pClientData)
{
	switch (uMsg) {
	case WM_INITDIALOG:
		{
			CChannelTimer *pThis = static_cast<CChannelTimer*>(pClientData);
			const Timer &timer = pThis->m_timer;

			::CheckRadioButton(
				hDlg, IDC_SETTINGS_CONDITION_DURATION, IDC_SETTINGS_CONDITION_EVENTEND,
				IDC_SETTINGS_CONDITION_DURATION + (int)timer.condition);
			EnableDlgItems(
				hDlg, IDC_SETTINGS_DURATION_HOURS, IDC_SETTINGS_DURATION_SECONDS_UD,
				timer.condition == Timer::SleepCondition::CONDITION_DURATION);
			EnableDlgItem(
				hDlg, IDC_SETTINGS_DATETIME, timer.condition == Timer::SleepCondition::CONDITION_DATETIME);

			::SetDlgItemInt(hDlg, IDC_SETTINGS_DURATION_HOURS, timer.durationToChange / (60 * 60), FALSE);
			::SendDlgItemMessage(hDlg, IDC_SETTINGS_DURATION_HOURS_UD, UDM_SETRANGE32, 0, 24 * 24);
			::SetDlgItemInt(hDlg, IDC_SETTINGS_DURATION_MINUTES, timer.durationToChange / 60 % 60, FALSE);
			::SendDlgItemMessage(hDlg, IDC_SETTINGS_DURATION_MINUTES_UD, UDM_SETRANGE32, 0, 59);
			::SetDlgItemInt(hDlg, IDC_SETTINGS_DURATION_SECONDS, timer.durationToChange % 60, FALSE);
			::SendDlgItemMessage(hDlg, IDC_SETTINGS_DURATION_SECONDS_UD, UDM_SETRANGE32, 0, 59);

			HWND hwndDateTime = ::GetDlgItem(hDlg, IDC_SETTINGS_DATETIME);
			DateTime_SetFormat(hwndDateTime, TEXT("yyyy'/'MM'/'dd' 'HH':'mm':'ss"));
			SYSTEMTIME st;
			::GetLocalTime(&st);
			DateTime_SetSystemtime(hwndDateTime, GDT_VALID, &st);

			// サービスのリストを取得する
			std::vector<CServiceInfo> services;

			// チューニング空間
			TVTest::CTVTestApp *pApp = pThis->m_pApp;
			int NumSpaces = 0;
			const int curTuningSpace = pApp->GetTuningSpace(&NumSpaces);

			HWND hwndTuningSpaces = ::GetDlgItem(hDlg, IDC_SETTINGS_TUNING_SPACE);
			pThis->m_tuningSpaces.clear();
			for (int i = 0; i < NumSpaces; i++) {
				TCHAR name[MAX_PATH];
				pApp->GetTuningSpaceName(i, name, _countof(name));
				pThis->m_tuningSpaces.push_back(name);
				ComboBox_AddString(hwndTuningSpaces, name);
			}
			ComboBox_SetCurSel(hwndTuningSpaces, curTuningSpace);

			// チャンネル
			HWND hwndChannels = ::GetDlgItem(hDlg, IDC_SETTINGS_CHANNELS);
			pThis->m_channels.clear();
			if (curTuningSpace >= 0) {
				TVTest::ChannelInfo curChInfo;
				pApp->GetCurrentChannelInfo(&curChInfo);
				const CServiceInfo pCurServiceInfo = CServiceInfo(curChInfo);
				TCHAR curName[MAX_PATH];
				pCurServiceInfo.toTchar(curName, _countof(curName));

				// 現在のチューニング空間のチャンネルを取得する
				TVTest::ChannelInfo ChInfo;
				for (int Channel = 0; pApp->GetChannelInfo(curTuningSpace, Channel, &ChInfo); Channel++) {
					if (ChInfo.Flags & TVTest::CHANNEL_FLAG_DISABLED > 0)
						continue;

					const CServiceInfo pServiceInfo = CServiceInfo(ChInfo);

					TCHAR name[MAX_PATH];
					pThis->m_channels.push_back(pServiceInfo);
					pServiceInfo.toTchar(name, _countof(name));
					ComboBox_AddString(hwndChannels, name);
				}
				ComboBox_SelectItemData(hwndChannels, -1, curName);
			}
			else {
				// 全てのチューニング空間のチャンネルを取得する
				TVTest::ChannelInfo ChInfo;
				for (int Space = 0; Space < NumSpaces; Space++) {
					for (int Channel = 0; pApp->GetChannelInfo(Space, Channel, &ChInfo); Channel++) {
						const CServiceInfo pServiceInfo = CServiceInfo(ChInfo);
						ComboBox_AddString(hwndChannels, pServiceInfo.szServiceName);
					}
				}
			}
		}
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDC_SETTINGS_CONDITION_DURATION:
		case IDC_SETTINGS_CONDITION_DATETIME:
		case IDC_SETTINGS_CONDITION_EVENTEND:
			{
				EnableDlgItems(
					hDlg, IDC_SETTINGS_DURATION_HOURS, IDC_SETTINGS_DURATION_SECONDS_UD,
					::IsDlgButtonChecked(hDlg, IDC_SETTINGS_CONDITION_DURATION) == BST_CHECKED);
				EnableDlgItem(
					hDlg, IDC_SETTINGS_DATETIME,
					::IsDlgButtonChecked(hDlg, IDC_SETTINGS_CONDITION_DATETIME) == BST_CHECKED);
			}
			return TRUE;

		case IDOK:
			{
				CChannelTimer *pThis = static_cast<CChannelTimer*>(pClientData);
				Timer *timer = &pThis->m_timer;
				Timer::SleepCondition Condition;

				if (::IsDlgButtonChecked(hDlg, IDC_SETTINGS_CONDITION_DURATION)) {
					Condition = Timer::SleepCondition::CONDITION_DURATION;
				} else if (::IsDlgButtonChecked(hDlg, IDC_SETTINGS_CONDITION_DATETIME)) {
					Condition = Timer::SleepCondition::CONDITION_DATETIME;
				} else if (::IsDlgButtonChecked(hDlg, IDC_SETTINGS_CONDITION_EVENTEND)) {
					Condition = Timer::SleepCondition::CONDITION_EVENTEND;
				} else {
					::MessageBox(hDlg, TEXT("スリープする条件を選択してください。"), nullptr, MB_OK | MB_ICONEXCLAMATION);
					return TRUE;
				}

				ULONGLONG Duration =
					(ULONGLONG)::GetDlgItemInt(hDlg, IDC_SETTINGS_DURATION_HOURS, nullptr, FALSE) * (60 * 60) +
					(ULONGLONG)::GetDlgItemInt(hDlg, IDC_SETTINGS_DURATION_MINUTES, nullptr, FALSE) * 60 +
					(ULONGLONG)::GetDlgItemInt(hDlg, IDC_SETTINGS_DURATION_SECONDS, nullptr, FALSE);
				if (Condition == Timer::SleepCondition::CONDITION_DURATION) {
					if (Duration * 1000 > USER_TIMER_MAXIMUM) {
						::MessageBox(hDlg, TEXT("スリープまでの時間が長すぎます。"), nullptr, MB_OK | MB_ICONEXCLAMATION);
						return TRUE;
					}
					if (Duration == 0) {
						::MessageBox(hDlg, TEXT("時間の指定が正しくありません。"), nullptr, MB_OK | MB_ICONEXCLAMATION);
						return TRUE;
					}
				}

				SYSTEMTIME DateTime;
				DWORD Result = DateTime_GetSystemtime(::GetDlgItem(hDlg, IDC_SETTINGS_DATETIME), &DateTime);
				if (Condition == Timer::SleepCondition::CONDITION_DATETIME) {
					if (Result != GDT_VALID) {
						::MessageBox(hDlg, TEXT("時刻を指定してください。"), nullptr, MB_OK | MB_ICONEXCLAMATION);
						return TRUE;
					}
					SYSTEMTIME UTCTime;
					::TzSpecificLocalTimeToSystemTime(nullptr, &DateTime, &UTCTime);
					DateTime = UTCTime;
					SYSTEMTIME CurTime;
					::GetSystemTime(&CurTime);
					if (DiffSystemTime(DateTime, CurTime) <= 0) {
						::MessageBox(hDlg, TEXT("指定された時刻を既に過ぎています。"), nullptr, MB_OK | MB_ICONEXCLAMATION);
						return TRUE;
					}
				}

				timer->condition = Condition;
				timer->durationToChange = (DWORD)Duration;
				timer->dateToChange = DateTime;

				HWND hwndTuningSpaces = ::GetDlgItem(hDlg, IDC_SETTINGS_TUNING_SPACE);
				if (ComboBox_GetCurSel(hwndTuningSpaces) < 0) {
					::MessageBox(hDlg, TEXT("ng"), nullptr, MB_OK | MB_ICONEXCLAMATION);
					return TRUE;
				}
				timer->channelInfo.Space = ComboBox_GetCurSel(hwndTuningSpaces);

				HWND hwndChannels = ::GetDlgItem(hDlg, IDC_SETTINGS_CHANNELS);
				if (ComboBox_GetCurSel(hwndChannels) < 0) {
					::MessageBox(hDlg, TEXT("ng"), nullptr, MB_OK | MB_ICONEXCLAMATION);
					return TRUE;
				}
				const auto& ch = pThis->m_channels.at(ComboBox_GetCurSel(hwndChannels));
				timer->channelInfo.NetworkID = ch.NetworkID;
				timer->channelInfo.ServiceID = ch.ServiceID;

				// タイマー設定
				if (pThis->m_fEnabled)
					pThis->BeginTimer();
			}
		case IDCANCEL:
			{
				CChannelTimer *pThis = static_cast<CChannelTimer*>(pClientData);
				RECT rc;

				::GetWindowRect(hDlg, &rc);
				pThis->m_SettingsDialogPos.x = rc.left;
				pThis->m_SettingsDialogPos.y = rc.top;

				::EndDialog(hDlg, LOWORD(wParam));
			}
			return TRUE;
		}
		return TRUE;
	}

	return FALSE;
}




// 確認ダイアログプロシージャ
INT_PTR CALLBACK CChannelTimer::ConfirmDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam, void *pClientData)
{
	switch (uMsg) {
	case WM_INITDIALOG:
		{
			CChannelTimer *pThis = static_cast<CChannelTimer*>(pClientData);

			TCHAR szText[64];
			::wsprintf(szText, TEXT("しますか？"));
			::SetDlgItemText(hDlg, IDC_CONFIRM_MODE, szText);

			if (pThis->m_ConfirmTimeout > 0) {
				::SetDlgItemInt(hDlg, IDC_CONFIRM_TIMEOUT, pThis->m_ConfirmTimeout, TRUE);
				pThis->m_ConfirmTimerCount = 0;
				::SetTimer(hDlg, 1, 1000, nullptr);
			} else {
				::SetDlgItemText(hDlg, IDC_CONFIRM_TIMEOUT, TEXT("∞"));
			}
		}
		return TRUE;

	case WM_TIMER:
		{
			CChannelTimer *pThis = static_cast<CChannelTimer*>(pClientData);

			pThis->m_ConfirmTimerCount++;
			if (pThis->m_ConfirmTimerCount < pThis->m_ConfirmTimeout) {
				::SetDlgItemInt(hDlg, IDC_CONFIRM_TIMEOUT, pThis->m_ConfirmTimeout - pThis->m_ConfirmTimerCount, TRUE);
			} else {
				::EndDialog(hDlg, IDOK);
			}
		}
		return TRUE;

	case WM_COMMAND:
		switch (LOWORD(wParam)) {
		case IDOK:
		case IDCANCEL:
			::EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		}
		return TRUE;
	}

	return FALSE;
}




// プラグインクラスのインスタンスを生成する
TVTest::CTVTestPlugin *CreatePluginClass()
{
	return new CChannelTimer;
}