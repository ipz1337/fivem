/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"

#include <CfxLocale.h>
#include <CefOverlay.h>
#include <NetLibrary.h>
#include <strsafe.h>
#include <GlobalEvents.h>

#include <nutsnbolts.h>
#include <ConsoleHost.h>
#include <CoreConsole.h>
#include <ICoreGameInit.h>
#include <GameInit.h>
#include <ScriptEngine.h>
//New libs needed for saveSettings
#include <fstream>
#include <sstream>
#include "KnownFolders.h"
#include <ShlObj.h>
#include <Shellapi.h>
#include <HttpClient.h>
#include <InputHook.h>

#include <json.hpp>

#include <CfxState.h>
#include <HostSharedData.h>

#include <skyr/url.hpp>

#include <se/Security.h>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>

#include <LauncherIPC.h>

#include <CrossBuildRuntime.h>

#include <SteamComponentAPI.h>

#include <MinMode.h>

#include "GameInit.h"

std::string g_lastConn;

extern bool XBR_InterceptCardResponse(const nlohmann::json& j);
extern bool XBR_InterceptCancelDefer();

static LONG WINAPI TerminateInstantly(LPEXCEPTION_POINTERS pointers)
{
	if (pointers->ExceptionRecord->ExceptionCode == STATUS_BREAKPOINT)
	{
		TerminateProcess(GetCurrentProcess(), 0xDEADCAFE);
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

static void SaveBuildNumber(uint32_t build)
{
	std::wstring fpath = MakeRelativeCitPath(L"CitizenFX.ini");

	if (GetFileAttributes(fpath.c_str()) != INVALID_FILE_ATTRIBUTES)
	{
		WritePrivateProfileString(L"Game", L"SavedBuildNumber", fmt::sprintf(L"%d", build).c_str(), fpath.c_str());
	}
}

void RestartGameToOtherBuild(int build = 0)
{
#if defined(GTA_FIVE) || defined(IS_RDR3)
	static HostSharedData<CfxState> hostData("CfxInitState");
	const wchar_t* cli;

	if (!build)
	{
		cli = va(L"\"%s\" %s -switchcl \"fivem://connect/%s\"",
		hostData->gameExePath,
		Is2060() ? L"" : L"-b2060",
		ToWide(g_lastConn));

		build = (Is2060()) ? 1604 : 2060;
	}
	else
	{
		cli = va(L"\"%s\" %s -switchcl \"fivem://connect/%s\"",
		hostData->gameExePath,
		build == 1604 ? L"" : fmt::sprintf(L"-b%d", build),
		ToWide(g_lastConn));
	}

	uint32_t defaultBuild =
#ifdef GTA_FIVE
	1604
#elif defined(IS_RDR3)
	1311
#else
	0
#endif
	;

	// we won't launch the default build if we don't do this
	if (build == defaultBuild)
	{
		SaveBuildNumber(defaultBuild);
	}

	STARTUPINFOW si = { 0 };
	si.cb = sizeof(si);

	PROCESS_INFORMATION pi;

	if (!CreateProcessW(NULL, const_cast<wchar_t*>(cli), NULL, NULL, FALSE, CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi))
	{
		trace("failed to exit: %d\n", GetLastError());
	}

	ExitProcess(0x69);
#endif
}

extern void InitializeBuildSwitch(int build);

void saveSettings(const wchar_t *json) {
	PWSTR appDataPath;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
		// create the directory if not existent
		std::wstring cfxPath = std::wstring(appDataPath) + L"\\CitizenFX";
		CreateDirectory(cfxPath.c_str(), nullptr);
		// open and read the profile file
		std::wstring settingsPath = cfxPath + L"\\settings.json";
		std::ofstream settingsFile(settingsPath);
		//trace(va("Saving settings data %s\n", json));
		settingsFile << ToNarrow(json);
		settingsFile.close();
		CoTaskMemFree(appDataPath);
	}
}

void loadSettings() {
	PWSTR appDataPath;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath))) {
		// create the directory if not existent
		std::wstring cfxPath = std::wstring(appDataPath) + L"\\CitizenFX";
		CreateDirectory(cfxPath.c_str(), nullptr);

		// open and read the profile file
		std::wstring settingsPath = cfxPath + L"\\settings.json";
		if (FILE* profileFile = _wfopen(settingsPath.c_str(), L"rb"))
		{
			std::ifstream settingsFile(settingsPath);

			std::stringstream settingsStream;
			settingsStream << settingsFile.rdbuf();
			settingsFile.close();

			//trace(va("Loaded JSON settings %s\n", json.c_str()));
			nui::PostFrameMessage("mpMenu", fmt::sprintf(R"({ "type": "loadedSettings", "json": %s })", settingsStream.str()));
		}
		
		CoTaskMemFree(appDataPath);
	}
}

inline ISteamComponent* GetSteam()
{
	auto steamComponent = Instance<ISteamComponent>::Get();

	// if Steam isn't running, return an error
	if (!steamComponent->IsSteamRunning())
	{
		steamComponent->Initialize();

		if (!steamComponent->IsSteamRunning())
		{
			return nullptr;
		}
	}

	return steamComponent;
}

inline bool HasDefaultName()
{
	auto steamComponent = GetSteam();

	if (steamComponent)
	{
		IClientEngine* steamClient = steamComponent->GetPrivateClient();

		if (steamClient)
		{
			InterfaceMapper steamFriends(steamClient->GetIClientFriends(steamComponent->GetHSteamUser(), steamComponent->GetHSteamPipe(), "CLIENTFRIENDS_INTERFACE_VERSION001"));

			if (steamFriends.IsValid())
			{
				return true;
			}
		}
	}

	return false;
}

NetLibrary* netLibrary;
static bool g_connected;

static void ConnectTo(const std::string& hostnameStr, bool fromUI = false, const std::string& connectParams = "")
{
	auto connectParamsReal = connectParams;
	static bool switched;

	if (wcsstr(GetCommandLineW(), L"-switchcl") && !switched)
	{
		connectParamsReal = "switchcl=true&" + connectParamsReal;
		switched = true;
	}

	if (!fromUI)
	{
		if (nui::HasMainUI())
		{
			auto j = nlohmann::json::object({
				{ "type", "connectTo" },
				{ "hostnameStr", hostnameStr },
				{ "connectParams", connectParamsReal }
			});
			nui::PostFrameMessage("mpMenu", j.dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace));

			return;
		}
	}

	if (g_connected)
	{
		trace("Ignoring ConnectTo because we're already connecting/connected.\n");
		return;
	}

	g_connected = true;

	nui::PostFrameMessage("mpMenu", R"({ "type": "connecting" })");

	g_lastConn = hostnameStr;

	if (!hostnameStr.empty() && hostnameStr[0] == '-')
	{
		netLibrary->ConnectToServer("cfx.re/join/" + hostnameStr.substr(1));
	}
	else
	{
		netLibrary->ConnectToServer(hostnameStr);
	}
}

static std::string g_pendingAuthPayload;

static void HandleAuthPayload(const std::string& payloadStr)
{
	if (nui::HasMainUI())
	{
		auto payloadJson = nlohmann::json(payloadStr).dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace);

		nui::PostFrameMessage("mpMenu", fmt::sprintf(R"({ "type": "authPayload", "data": %s })", payloadJson));
	}
	else
	{
		g_pendingAuthPayload = payloadStr;
	}
}

#include <LegitimacyAPI.h>

static std::string g_discourseClientId;
static std::string g_discourseUserToken;

static std::string g_cardConnectionToken;

struct ServerLink
{
	std::string rawIcon;
	std::string hostname;
	std::string url;
};

#include <wrl.h>
#include <psapi.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <botan/base64.h>

namespace WRL = Microsoft::WRL;

static WRL::ComPtr<IShellLink> MakeShellLink(const ServerLink& link)
{
	WRL::ComPtr<IShellLink> psl;
	HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&psl));

	if (SUCCEEDED(hr))
	{
		static HostSharedData<CfxState> hostData("CfxInitState");

		wchar_t imageFileName[1024];

		auto hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, hostData->GetInitialPid());
		GetModuleFileNameEx(hProcess, NULL, imageFileName, std::size(imageFileName));

		psl->SetPath(imageFileName);
		psl->SetArguments(fmt::sprintf(L"fivem://connect/%s", ToWide(link.url)).c_str());

		WRL::ComPtr<IPropertyStore> pps;
		psl.As(&pps);

		PROPVARIANT propvar;
		hr = InitPropVariantFromString(ToWide(link.hostname).c_str(), &propvar);
		hr = pps->SetValue(PKEY_Title, propvar);
		hr = pps->Commit();
		PropVariantClear(&propvar);

		psl->SetIconLocation(imageFileName, -201);

		if (!link.rawIcon.empty())
		{
			auto iconPath = MakeRelativeCitPath(fmt::sprintf(L"cache/browser/%08x.ico", HashString(link.rawIcon.c_str())));
			
			FILE* f = _wfopen(iconPath.c_str(), L"wb");

			if (f)
			{
				auto data = Botan::base64_decode(link.rawIcon.substr(strlen("data:image/png;base64,")));

				uint8_t iconHeader[] = {
					0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x10, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
					0xff, 0xff, 0x16, 0x00, 0x00, 0x00
				};

				*(uint32_t*)&iconHeader[14] = data.size();

				fwrite(iconHeader, 1, sizeof(iconHeader), f);

				fwrite(data.data(), 1, data.size(), f);
				fclose(f);

				psl->SetIconLocation(iconPath.c_str(), 0);
			}
		}
	}

	return psl;
}

static void UpdateJumpList(const std::vector<ServerLink>& links)
{
	PWSTR aumid;
	GetCurrentProcessExplicitAppUserModelID(&aumid);

	WRL::ComPtr<ICustomDestinationList> pcdl;
	HRESULT hr = CoCreateInstance(CLSID_DestinationList, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pcdl));

	if (FAILED(hr))
	{
		CoTaskMemFree(aumid);
		return;
	}

	pcdl->SetAppID(aumid);
	CoTaskMemFree(aumid);

	UINT cMinSlots;
	WRL::ComPtr<IObjectArray> poaRemoved;

	hr = pcdl->BeginList(&cMinSlots, IID_PPV_ARGS(&poaRemoved));

	if (FAILED(hr))
	{
		return;
	}

	{
		WRL::ComPtr<IObjectCollection> poc;
		hr = CoCreateInstance(CLSID_EnumerableObjectCollection, NULL, CLSCTX_INPROC, IID_PPV_ARGS(&poc));

		if (FAILED(hr))
		{
			return;
		}

		for (int i = 0; i < std::min(links.size(), size_t(cMinSlots)); i++)
		{
			auto shellLink = MakeShellLink(links[i]);

			poc->AddObject(shellLink.Get());
		}

		pcdl->AppendCategory(L"History", poc.Get());
	}

	pcdl->CommitList();
}

void DLL_IMPORT UiDone();

static InitFunction initFunction([] ()
{
	static std::function<void()> g_onYesCallback;

	static ipc::Endpoint ep("launcherTalk", false);

	OnCriticalGameFrame.Connect([]()
	{
		ep.RunFrame();
	});

	OnGameFrame.Connect([]()
	{
		ep.RunFrame();
	});

	Instance<ICoreGameInit>::Get()->OnGameRequestLoad.Connect([]()
	{
		ep.Call("loading");
	});

	NetLibrary::OnNetLibraryCreate.Connect([] (NetLibrary* lib)
	{
		netLibrary = lib;

		netLibrary->OnConnectOKReceived.Connect([](NetAddress)
		{
			auto peerAddress = netLibrary->GetCurrentPeer().ToString();

			nui::PostRootMessage(fmt::sprintf(R"({ "type": "setServerAddress", "data": "%s" })", peerAddress));
		});

		netLibrary->OnRequestBuildSwitch.Connect([](int build)
		{
			InitializeBuildSwitch(build);
			g_connected = false;
		});

		netLibrary->OnConnectionError.Connect([] (const char* error)
		{
#ifdef GTA_FIVE
			if (strstr(error, "This server requires a different game build"))
			{
				RestartGameToOtherBuild();
			}
#endif

			console::Printf("no_console", "OnConnectionError: %s\n", error);

			g_connected = false;

			rapidjson::Document document;
			document.SetString(error, document.GetAllocator());

			rapidjson::StringBuffer sbuffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(sbuffer);

			document.Accept(writer);

			nui::PostFrameMessage("mpMenu", fmt::sprintf(R"({ "type": "connectFailed", "message": %s })", sbuffer.GetString()));

			ep.Call("connectionError", std::string(error));
		});

		netLibrary->OnConnectionProgress.Connect([] (const std::string& message, int progress, int totalProgress)
		{
			console::Printf("no_console", "OnConnectionProgress: %s\n", message);

			rapidjson::Document document;
			document.SetObject();
			document.AddMember("message", rapidjson::Value(message.c_str(), message.size(), document.GetAllocator()), document.GetAllocator());
			document.AddMember("count", progress, document.GetAllocator());
			document.AddMember("total", totalProgress, document.GetAllocator());

			rapidjson::StringBuffer sbuffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(sbuffer);

			document.Accept(writer);

			if (nui::HasMainUI())
			{
				nui::PostFrameMessage("mpMenu", fmt::sprintf(R"({ "type": "connectStatus", "data": %s })", sbuffer.GetString()));
			}

			ep.Call("connectionProgress", message, progress, totalProgress);
		});

		netLibrary->OnConnectionCardPresent.Connect([](const std::string& card, const std::string& token)
		{
			g_cardConnectionToken = token;

			rapidjson::Document document;
			document.SetObject();
			document.AddMember("card", rapidjson::Value(card.c_str(), card.size(), document.GetAllocator()), document.GetAllocator());

			rapidjson::StringBuffer sbuffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(sbuffer);

			document.Accept(writer);

			if (nui::HasMainUI())
			{
				nui::PostFrameMessage("mpMenu", fmt::sprintf(R"({ "type": "connectCard", "data": %s })", sbuffer.GetString()));
			}

			ep.Call("connectionError", std::string("Cards don't exist here yet!"));
		});

		static std::function<void()> finishConnectCb;
		static bool disconnected;

		netLibrary->OnInterceptConnection.Connect([](const std::string& url, const std::function<void()>& cb)
		{
			if (Instance<ICoreGameInit>::Get()->GetGameLoaded() || Instance<ICoreGameInit>::Get()->HasVariable("killedGameEarly"))
			{
				if (!disconnected)
				{
					netLibrary->OnConnectionProgress("Waiting for game to shut down...", 0, 100);

					finishConnectCb = cb;

					return false;
				}
			}
			else
			{
				disconnected = false;
			}

			return true;
		});

		Instance<ICoreGameInit>::Get()->OnGameFinalizeLoad.Connect([]()
		{
			disconnected = false;
		});

		Instance<ICoreGameInit>::Get()->OnShutdownSession.Connect([]()
		{
			if (finishConnectCb)
			{
				auto cb = std::move(finishConnectCb);
				cb();
			}
			else
			{
				disconnected = true;
			}
		}, 5000);

		lib->AddReliableHandler("msgPaymentRequest", [](const char* buf, size_t len)
		{
			try
			{
				auto json = nlohmann::json::parse(std::string(buf, len));

				se::ScopedPrincipal scope(se::Principal{ "system.console" });
				console::GetDefaultContext()->GetVariableManager()->FindEntryRaw("warningMessageResult")->SetValue("0");
				console::GetDefaultContext()->ExecuteSingleCommandDirect(ProgramArguments{ "warningmessage", "PURCHASE REQUEST", fmt::sprintf("The server is requesting a purchase of %s for %s.", json.value("sku_name", ""), json.value("sku_price", "")), "Do you want to purchase this item?", "20" });

				g_onYesCallback = [json]()
				{
					std::map<std::string, std::string> postMap;
					postMap["data"] = json.value<std::string>("data", "");
					postMap["sig"] = json.value<std::string>("sig", "");
					postMap["clientId"] = g_discourseClientId;
					postMap["userToken"] = g_discourseUserToken;

					Instance<HttpClient>::Get()->DoPostRequest("https://keymaster.fivem.net/api/paymentAssign", postMap, [](bool success, const char* data, size_t length)
					{
						if (success)
						{
							auto res = nlohmann::json::parse(std::string(data, length));
							auto url = res.value("url", "");

							if (!url.empty())
							{
								if (url.find("http://") == 0 || url.find("https://") == 0)
								{
									ShellExecute(nullptr, L"open", ToWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
								}
							}
						}
					});
				};
			}
			catch (const std::exception& e)
			{

			}
		}, true);
	});

	OnMainGameFrame.Connect([]()
	{
		if (g_onYesCallback)
		{
			int result = atoi(console::GetDefaultContext()->GetVariableManager()->FindEntryRaw("warningMessageResult")->GetValue().c_str());

			if (result != 0)
			{
				if (result == 4)
				{
					g_onYesCallback();
				}

				g_onYesCallback = {};
			}
		}
	});

	OnKillNetwork.Connect([](const char*)
	{
		g_connected = false;
	});

	fx::ScriptEngine::RegisterNativeHandler("SEND_SDK_MESSAGE", [](fx::ScriptContext& context)
	{
		ep.Call("sdk:message", std::string(context.GetArgument<const char*>(0)));
	});

	static ConsoleCommand connectCommand("connect", [](const std::string& server)
	{
		ConnectTo(server);
	});

	ep.Bind("connectTo", [](const std::string& url)
	{
		ConnectTo(url);
	});

	ep.Bind("charInput", [](uint32_t ch)
	{
		bool p = true;
		LRESULT r;

		InputHook::DeprecatedOnWndProc(NULL, WM_CHAR, ch, 0, p, r);
	});

	ep.Bind("imeCommitText", [](const std::string& u8str, int rS, int rE, int p)
	{
		auto b = nui::GetFocusBrowser();

		if (b)
		{
			b->GetHost()->ImeCommitText(ToWide(u8str), CefRange(rS, rE), p);
		}
	});

	ep.Bind("imeSetComposition", [](const std::string& u8str, const std::vector<std::string>& underlines, int rS, int rE, int cS, int cE)
	{
		auto b = nui::GetFocusBrowser();

		if (b)
		{
			std::vector<CefCompositionUnderline> uls;

			for (auto& ul : underlines)
			{
				uls.push_back(*reinterpret_cast<const CefCompositionUnderline*>(ul.c_str()));
			}

			b->GetHost()->ImeSetComposition(ToWide(u8str), uls, CefRange(rS, rE), CefRange(cS, cE));
		}
	});

	ep.Bind("imeCancelComposition", []()
	{
		auto b = nui::GetFocusBrowser();

		if (b)
		{
			b->GetHost()->ImeCancelComposition();
		}
	});

	ep.Bind("resizeWindow", [](int w, int h)
	{
		auto wnd = FindWindow(L"grcWindow", NULL);

		SetWindowPos(wnd, NULL, 0, 0, w, h, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
	});

	ep.Bind("sdk:invokeNative", [](const std::string& nativeType, const std::string& argumentData)
	{
		if (nativeType == "sendCommand")
		{
			se::ScopedPrincipal ps{ se::Principal{"system.console"} };
			console::GetDefaultContext()->ExecuteSingleCommand(argumentData);
		}
	});

	static ConsoleCommand disconnectCommand("disconnect", []()
	{
		if (netLibrary->GetConnectionState() != 0)
		{
			OnKillNetwork("Disconnected.");
			OnMsgConfirm();
		}
	});

	static std::string curChannel;

	wchar_t resultPath[1024];

	static std::wstring fpath = MakeRelativeCitPath(L"CitizenFX.ini");
	GetPrivateProfileString(L"Game", L"UpdateChannel", L"production", resultPath, std::size(resultPath), fpath.c_str());

	curChannel = ToNarrow(resultPath);

	static ConVar<bool> uiPremium("ui_premium", ConVar_None, false);
	static ConVar<std::string> uiUpdateChannel("ui_updateChannel", ConVar_None, curChannel);

	OnGameFrame.Connect([]()
	{
		if (uiUpdateChannel.GetValue() != curChannel)
		{
			curChannel = uiUpdateChannel.GetValue();

			WritePrivateProfileString(L"Game", L"UpdateChannel", ToWide(curChannel).c_str(), fpath.c_str());

			rapidjson::Document document;
			document.SetString("Restart the game to apply the update channel change.", document.GetAllocator());

			rapidjson::StringBuffer sbuffer;
			rapidjson::Writer<rapidjson::StringBuffer> writer(sbuffer);

			document.Accept(writer);

			nui::PostFrameMessage("mpMenu", fmt::sprintf(R"({ "type": "setWarningMessage", "message": %s })", sbuffer.GetString()));
		}
	});
	
	ConHost::OnInvokeNative.Connect([](const char* type, const char* arg)
	{
		if (!_stricmp(type, "connectTo"))
		{
			ConnectTo(arg);
		}
	});

	nui::OnInvokeNative.Connect([](const wchar_t* type, const wchar_t* arg)
	{
		if (!_wcsicmp(type, L"getMinModeInfo"))
		{
#ifdef GTA_FIVE
			static bool done = ([]
			{
				UiDone();

				auto hWnd = FindWindowW(L"grcWindow", NULL);
				ShowWindow(hWnd, SW_SHOW);

				// game code locks it
				LockSetForegroundWindow(LSFW_UNLOCK);
				SetForegroundWindow(hWnd);

				return true;
			})();
#endif

			auto manifest = CoreGetMinModeManifest();

			nui::PostFrameMessage("mpMenu", fmt::sprintf(R"({ "type": "setMinModeInfo", "enabled": %s, "data": %s })", manifest->IsEnabled() ? "true" : "false", manifest->GetRaw()));

			static bool initSwitched;

			if (wcsstr(GetCommandLineW(), L"-switchcl") && !initSwitched)
			{
				nui::PostFrameMessage("mpMenu", fmt::sprintf(R"({ "type": "setSwitchCl", "enabled": %s })", true));
				initSwitched = true;
			}
		}
		else if (!_wcsicmp(type, L"connectTo"))
		{
			std::wstring hostnameStrW = arg;
			std::string hostnameStr(hostnameStrW.begin(), hostnameStrW.end());

			ConnectTo(hostnameStr, true);
		}
		else if (!_wcsicmp(type, L"cancelDefer"))
		{
			if (!XBR_InterceptCancelDefer())
			{
				netLibrary->CancelDeferredConnection();
			}

			g_connected = false;
		}
		else if (_wcsicmp(type, L"executeCommand") == 0)
		{
			if (!nui::HasMainUI())
			{
				return;
			}

			se::ScopedPrincipal principal{
				se::Principal{
				"system.console" }
			};
			console::GetDefaultContext()->ExecuteSingleCommand(ToNarrow(arg));
		}
		else if (!_wcsicmp(type, L"changeName"))
		{
			std::string newusername = ToNarrow(arg);
			if (!newusername.empty()) {
				if (newusername.c_str() != netLibrary->GetPlayerName()) {
					netLibrary->SetPlayerName(newusername.c_str());
					trace(va("Changed player name to %s\n", newusername.c_str()));
					nui::PostFrameMessage("mpMenu", fmt::sprintf(R"({ "type": "setSettingsNick", "nickname": "%s" })", newusername));
				}
			}
		}
		else if (!_wcsicmp(type, L"setLocale"))
		{
			if (nui::HasMainUI())
			{
				CoreGetLocalization()->SetLocale(ToNarrow(arg));
			}
		}
		else if (!_wcsicmp(type, L"loadSettings"))
		{
			loadSettings();
			trace("Settings loaded!\n");
		}
		else if (!_wcsicmp(type, L"saveSettings"))
		{
			saveSettings(arg);
			trace("Settings saved!\n");
		}
		else if (!_wcsicmp(type, L"loadWarning"))
		{
			std::string warningMessage;

			if (Instance<ICoreGameInit>::Get()->GetData("warningMessage", &warningMessage))
			{
				if (!warningMessage.empty())
				{
					rapidjson::Document document;
					document.SetString(warningMessage.c_str(), document.GetAllocator());

					rapidjson::StringBuffer sbuffer;
					rapidjson::Writer<rapidjson::StringBuffer> writer(sbuffer);

					document.Accept(writer);

					nui::PostFrameMessage("mpMenu", fmt::sprintf(R"({ "type": "setWarningMessage", "message": %s })", sbuffer.GetString()));
				}

				Instance<ICoreGameInit>::Get()->SetData("warningMessage", "");
			}

			wchar_t computerName[256] = { 0 };
			DWORD len = _countof(computerName);
			GetComputerNameW(computerName, &len);

			nui::PostFrameMessage("mpMenu", fmt::sprintf(R"({ "type": "setComputerName", "data": "%s" })", ToNarrow(computerName)));
		}
		else if (!_wcsicmp(type, L"checkNickname"))
		{
			if (!arg || !arg[0] || !netLibrary)
			{
				trace("Failed to set nickname\n");
				return;
			}

			const char* text = netLibrary->GetPlayerName();
			std::string newusername = ToNarrow(arg);

			if (text != newusername && !HasDefaultName()) // one's a string, two's a char, string meets char, string::operator== exists
			{
				trace("Loaded nickname: %s\n", newusername.c_str());
				netLibrary->SetPlayerName(newusername.c_str());
			}

			if (!g_pendingAuthPayload.empty())
			{
				auto pendingAuthPayload = g_pendingAuthPayload;

				g_pendingAuthPayload = "";

				HandleAuthPayload(pendingAuthPayload);
			}
		}
		else if (!_wcsicmp(type, L"exit"))
		{
			// queue an ExitProcess on the next game frame
			OnGameFrame.Connect([] ()
			{
				AddVectoredExceptionHandler(FALSE, TerminateInstantly);

				CefShutdown();

				TerminateProcess(GetCurrentProcess(), 0);
			});
		}
#ifdef GTA_FIVE
		else if (!_wcsicmp(type, L"setDiscourseIdentity"))
		{
			try
			{
				auto json = nlohmann::json::parse(ToNarrow(arg));

				g_discourseUserToken = json.value<std::string>("token", "");
				g_discourseClientId = json.value<std::string>("clientId", "");

				Instance<ICoreGameInit>::Get()->SetData("discourseUserToken", g_discourseUserToken);
				Instance<ICoreGameInit>::Get()->SetData("discourseClientId", g_discourseClientId);

				Instance<::HttpClient>::Get()->DoPostRequest(
					"https://lambda.fivem.net/api/validate/discourse",
					{
						{ "entitlementId", ros::GetEntitlementSource() },
						{ "authToken", g_discourseUserToken },
						{ "clientId", g_discourseClientId },
					},
					[](bool success, const char* data, size_t size)
				{
					if (success)
					{
						std::string response{ data, size };

						bool hasEndUserPremium = false;

						try
						{
							auto json = nlohmann::json::parse(response);

							for (const auto& group : json["user"]["groups"])
							{
								auto name = group.value<std::string>("name", "");

								if (name == "staff" || name == "patreon_enduser")
								{
									hasEndUserPremium = true;
									break;
								}
							}
						}
						catch (const std::exception& e)
						{

						}

						if (hasEndUserPremium)
						{
							uiPremium.GetHelper()->SetRawValue(true);
							Instance<ICoreGameInit>::Get()->SetVariable("endUserPremium");
						}
					}
				});
			}
			catch (const std::exception& e)
			{
				trace("failed to set discourse identity: %s\n", e.what());
			}
		}
#endif
		else if (!_wcsicmp(type, L"submitCardResponse"))
		{
			try
			{
				auto json = nlohmann::json::parse(ToNarrow(arg));

				if (!XBR_InterceptCardResponse(json))
				{
					if (!g_cardConnectionToken.empty())
					{
						netLibrary->SubmitCardResponse(json["data"].dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace), g_cardConnectionToken);
					}
				}
			}
			catch (const std::exception& e)
			{
				trace("failed to set card response: %s\n", e.what());
			}
		}
		else if (!_wcsicmp(type, L"setLastServers"))
		{
			try
			{
				auto json = nlohmann::json::parse(ToNarrow(arg));

				int start = json.size() > 15 ? json.size() - 15 : 0;
				int end = json.size();

				std::vector<ServerLink> links;

				for (int i = end - 1; i >= start; i--)
				{
					if (json[i].is_null() || json[i]["hostname"].is_null() || json[i]["address"].is_null())
					{
						continue;
					}

					ServerLink l;
					json[i]["hostname"].get_to(l.hostname);

					if (!json[i]["rawIcon"].is_null())
					{
						json[i]["rawIcon"].get_to(l.rawIcon);
					}

					json[i]["address"].get_to(l.url);

					if (l.url.find("cfx.re/join/") == 0)
					{
						l.url = "-" + l.url.substr(12);
					}

					links.push_back(std::move(l));
				}

				UpdateJumpList(links);
			}
			catch (const std::exception & e)
			{
				trace("failed to set last servers: %s\n", e.what());
			}
		}
	});

	OnGameFrame.Connect([]()
	{
		static bool hi;

		if (!hi)
		{
			ep.Call("hi");
			hi = true;
		}

		se::ScopedPrincipal scope(se::Principal{ "system.console" });
		Instance<console::Context>::Get()->ExecuteBuffer();
	});

	OnMsgConfirm.Connect([] ()
	{
		ep.Call("disconnected");

		nui::SetMainUI(true);

		nui::CreateFrame("mpMenu", console::GetDefaultContext()->GetVariableManager()->FindEntryRaw("ui_url")->GetValue());
	});
});

#include <gameSkeleton.h>
#include <shellapi.h>

#include <nng/nng.h>
#include <nng/protocol/pipeline0/pull.h>
#include <nng/protocol/pipeline0/push.h>

static void ProtocolRegister()
{
#ifdef GTA_FIVE
	LSTATUS result;

#define CHECK_STATUS(x) \
	result = (x); \
	if (result != ERROR_SUCCESS) { \
		trace("[Protocol Registration] " #x " failed: %x", result); \
		return; \
	}

	static HostSharedData<CfxState> hostData("CfxInitState");

	HKEY key;
	wchar_t command[1024];
	swprintf_s(command, L"\"%s\" \"%%1\"", hostData->gameExePath);

	CHECK_STATUS(RegCreateKeyW(HKEY_CURRENT_USER, L"SOFTWARE\\Classes\\fivem", &key));
	CHECK_STATUS(RegSetValueExW(key, NULL, 0, REG_SZ, (BYTE*)L"FiveM", 6 * 2));
	CHECK_STATUS(RegSetValueExW(key, L"URL Protocol", 0, REG_SZ, (BYTE*)L"", 1 * 2));
	CHECK_STATUS(RegCloseKey(key));

	CHECK_STATUS(RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\Classes\\FiveM.ProtocolHandler", &key));
	CHECK_STATUS(RegSetValueExW(key, NULL, 0, REG_SZ, (BYTE*)L"FiveM", 6 * 2));
	CHECK_STATUS(RegCloseKey(key));

	CHECK_STATUS(RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\FiveM", &key));
	CHECK_STATUS(RegCloseKey(key));

	CHECK_STATUS(RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\FiveM\\Capabilities", &key));
	CHECK_STATUS(RegSetValueExW(key, L"ApplicationName", 0, REG_SZ, (BYTE*)L"FiveM", 6 * 2));
	CHECK_STATUS(RegSetValueExW(key, L"ApplicationDescription", 0, REG_SZ, (BYTE*)L"FiveM", 6 * 2));
	CHECK_STATUS(RegCloseKey(key));

	CHECK_STATUS(RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\FiveM\\Capabilities\\URLAssociations", &key));
	CHECK_STATUS(RegSetValueExW(key, L"fivem", 0, REG_SZ, (BYTE*)L"FiveM.ProtocolHandler", 22 * 2));
	CHECK_STATUS(RegCloseKey(key));

	CHECK_STATUS(RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\RegisteredApplications", &key));
	CHECK_STATUS(RegSetValueExW(key, L"FiveM", 0, REG_SZ, (BYTE*)L"Software\\FiveM\\Capabilities", 28 * 2));
	CHECK_STATUS(RegCloseKey(key));

	CHECK_STATUS(RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\Classes\\FiveM.ProtocolHandler\\shell\\open\\command", &key));
	CHECK_STATUS(RegSetValueExW(key, NULL, 0, REG_SZ, (BYTE*)command, (wcslen(command) * sizeof(wchar_t)) + 2));
	CHECK_STATUS(RegCloseKey(key));

	if (!IsWindows8Point1OrGreater())
	{
		// these are for compatibility on downlevel Windows systems
		CHECK_STATUS(RegCreateKey(HKEY_CURRENT_USER, L"SOFTWARE\\Classes\\fivem\\shell\\open\\command", &key));
		CHECK_STATUS(RegSetValueExW(key, NULL, 0, REG_SZ, (BYTE*)command, (wcslen(command) * sizeof(wchar_t)) + 2));
		CHECK_STATUS(RegCloseKey(key));
	}
#endif
}

void Component_RunPreInit()
{
	static HostSharedData<CfxState> hostData("CfxInitState");

#ifndef _DEBUG
	if (hostData->IsGameProcess())
#else
	if (hostData->IsMasterProcess())
#endif
	{
		ProtocolRegister();
	}

	int argc;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLine(), &argc);

	static std::string connectHost;
	static std::string connectParams;
	static std::string authPayload;

	for (int i = 1; i < argc; i++)
	{
		std::string arg = ToNarrow(argv[i]);

		if (arg.find("fivem:") == 0)
		{
			auto parsed = skyr::make_url(arg);

			if (parsed)
			{
				if (!parsed->host().empty())
				{
					if (parsed->host() == "connect")
					{
						if (!parsed->pathname().empty())
						{
							connectHost = parsed->pathname().substr(1);
							const auto& search = parsed->search_parameters();
							if (!search.empty())
							{
								connectParams = search.to_string();
							}
						}
					}
					else if (parsed->host() == "accept-auth")
					{
						if (!parsed->search().empty())
						{
							authPayload = parsed->search().substr(1);
						}
					}
				}
			}

			break;
		}
	}

	LocalFree(argv);

	if (!connectHost.empty())
	{
		if (hostData->IsMasterProcess() || hostData->IsGameProcess())
		{
			rage::OnInitFunctionStart.Connect([](rage::InitFunctionType type)
			{
				if (type == rage::InitFunctionType::INIT_CORE)
				{
					ConnectTo(connectHost, false, connectParams);
					connectHost = "";
					connectParams = "";
				}
			}, 999999);
		}
		else
		{
			nng_socket socket;
			nng_dialer dialer;

			auto j = nlohmann::json::object({ { "host", connectHost }, { "params", connectParams } });
			std::string connectMsg = j.dump(-1, ' ', false, nlohmann::detail::error_handler_t::strict);

			nng_push0_open(&socket);
			nng_dial(socket, "ipc:///tmp/fivem_connect", &dialer, 0);
			nng_send(socket, const_cast<char*>(connectMsg.c_str()), connectMsg.size(), 0);

			if (!hostData->gamePid)
			{
				AllowSetForegroundWindow(hostData->GetInitialPid());
			}
			else
			{
				AllowSetForegroundWindow(hostData->gamePid);
			}

			TerminateProcess(GetCurrentProcess(), 0);
		}
	}

	if (!authPayload.empty())
	{
		if (hostData->IsMasterProcess() || hostData->IsGameProcess())
		{
			rage::OnInitFunctionStart.Connect([](rage::InitFunctionType type)
			{
				if (type == rage::InitFunctionType::INIT_CORE)
				{
					HandleAuthPayload(authPayload);
					authPayload = "";
				}
			}, 999999);
		}
		else
		{
			nng_socket socket;
			nng_dialer dialer;

			nng_push0_open(&socket);
			nng_dial(socket, "ipc:///tmp/fivem_auth", &dialer, 0);
			nng_send(socket, const_cast<char*>(authPayload.c_str()), authPayload.size(), 0);

			if (!hostData->gamePid)
			{
				AllowSetForegroundWindow(hostData->GetInitialPid());
			}
			else
			{
				AllowSetForegroundWindow(hostData->gamePid);
			}

			TerminateProcess(GetCurrentProcess(), 0);
		}
	}
}

static InitFunction connectInitFunction([]()
{
#if __has_include(<gameSkeleton.h>)
	rage::OnInitFunctionStart.Connect([](rage::InitFunctionType type)
	{
		if (type == rage::INIT_BEFORE_MAP_LOADED)
		{
			SaveBuildNumber(xbr::GetGameBuild());
		}
	});
#endif

	static nng_socket netSocket;
	static nng_listener listener;

	nng_pull0_open(&netSocket);
	nng_listen(netSocket, "ipc:///tmp/fivem_connect", &listener, 0);

	static nng_socket netAuthSocket;
	static nng_listener authListener;

	nng_pull0_open(&netAuthSocket);
	nng_listen(netAuthSocket, "ipc:///tmp/fivem_auth", &authListener, 0);

	OnGameFrame.Connect([]()
	{
		if (Instance<ICoreGameInit>::Get()->GetGameLoaded())
		{
			return;
		}

		char* buffer;
		size_t bufLen;

		int err;

		err = nng_recv(netSocket, &buffer, &bufLen, NNG_FLAG_NONBLOCK | NNG_FLAG_ALLOC);

		if (err == 0)
		{
			std::string connectMsg(buffer, buffer + bufLen);
			nng_free(buffer, bufLen);

			auto connectData = nlohmann::json::parse(connectMsg);
			ConnectTo(connectData["host"], false, connectData["params"]);

			SetForegroundWindow(FindWindow(L"grcWindow", nullptr));
		}

		err = nng_recv(netAuthSocket, &buffer, &bufLen, NNG_FLAG_NONBLOCK | NNG_FLAG_ALLOC);

		if (err == 0)
		{
			std::string msg(buffer, buffer + bufLen);
			nng_free(buffer, bufLen);

			HandleAuthPayload(msg);

			SetForegroundWindow(FindWindow(L"grcWindow", nullptr));
		}
	});
});

static bool displayingPermissionRequest;
static std::string requestedPermissionResource;
static std::string requestedPermissionUrl;
static std::string requestedPermissionOrigin;
static int requestedPermissionMask;
static std::function<void(bool, int)> requestedPermissionCallback;
static std::map<std::string, int> g_mediaSettings;

#include <json.hpp>

static void ParseMediaSettings(const std::string& jsonStr)
{
	try
	{
		auto j = nlohmann::json::parse(jsonStr);
		
		for (auto& pair : j["origins"])
		{
			g_mediaSettings.emplace(pair[0].get<std::string>(), pair[1].get<int>());
		}
	}
	catch (std::exception& e)
	{

	}
}

void LoadPermissionSettings()
{
	PWSTR appDataPath;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)))
	{
		// create the directory if not existent
		std::wstring cfxPath = std::wstring(appDataPath) + L"\\CitizenFX";
		CreateDirectory(cfxPath.c_str(), nullptr);

		// open and read the profile file
		std::wstring settingsPath = cfxPath + L"\\media_access.json";
		if (FILE* profileFile = _wfopen(settingsPath.c_str(), L"rb"))
		{
			std::ifstream settingsFile(settingsPath);

			std::stringstream settingsStream;
			settingsStream << settingsFile.rdbuf();
			settingsFile.close();

			ParseMediaSettings(settingsStream.str());
		}

		CoTaskMemFree(appDataPath);
	}
}

void SavePermissionSettings(const std::string& json)
{
	PWSTR appDataPath;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)))
	{
		// create the directory if not existent
		std::wstring cfxPath = std::wstring(appDataPath) + L"\\CitizenFX";
		CreateDirectory(cfxPath.c_str(), nullptr);
		// open and read the profile file
		std::wstring settingsPath = cfxPath + L"\\media_access.json";
		std::ofstream settingsFile(settingsPath);
		settingsFile << json;
		settingsFile.close();
		CoTaskMemFree(appDataPath);
	}
}

void SavePermissionSettings()
{
	auto a = nlohmann::json::array();

	for (auto& pair : g_mediaSettings)
	{
		a.push_back(nlohmann::json::array({ pair.first, pair.second }));
	}

	auto j = nlohmann::json::object();
	j["origins"] = a;

	try
	{
		SavePermissionSettings(j.dump());
	}
	catch (std::exception& e)
	{
	
	}
}

static bool permGrants[32];

bool HandleMediaRequest(const std::string& frameOrigin, const std::string& url, int permissions, const std::function<void(bool, int)>& onComplete)
{
	// if not connected to a server, we will allow media
	if (netLibrary->GetConnectionState() == NetLibrary::CS_IDLE)
	{
		onComplete(true, permissions);
		return true;
	}

	// if this server+resource already has permission, keep it
	auto origin = netLibrary->GetCurrentPeer().ToString() + ":" + frameOrigin;
	int hadPermissions = 0;

	if (auto it = g_mediaSettings.find(origin); it != g_mediaSettings.end())
	{
		hadPermissions = it->second;

		if ((it->second & permissions) == permissions)
		{
			onComplete(true, it->second & permissions);
			return true;
		}
	}

	// if we're already in a request, deny it
	if (displayingPermissionRequest)
	{
		return false;
	}

	// start a request cycle
	requestedPermissionResource = frameOrigin;
	requestedPermissionUrl = url;
	requestedPermissionCallback = [onComplete, hadPermissions, permissions](bool success, int mask)
	{
		displayingPermissionRequest = false;

		if (onComplete)
		{
			onComplete(success, (mask | hadPermissions) & permissions);

			if (success)
			{
				g_mediaSettings[requestedPermissionOrigin] |= mask;
				SavePermissionSettings();
			}
		}
	};
	requestedPermissionMask = permissions & ~hadPermissions;
	requestedPermissionOrigin = origin;
	
	permGrants[0] = true;
	permGrants[1] = false;
	permGrants[2] = false;
	permGrants[3] = false;

	displayingPermissionRequest = true;

	return true;
}

#include <imgui.h>

static InitFunction mediaRequestInit([]()
{
	LoadPermissionSettings();

	ConHost::OnShouldDrawGui.Connect([](bool* should)
	{
		*should = *should || displayingPermissionRequest;
	});

	ConHost::OnDrawGui.Connect([]()
	{
		if (displayingPermissionRequest)
		{
			const float DISTANCE = 10.0f;
			ImVec2 window_pos = ImVec2(ImGui::GetMainViewport()->Pos.x + ImGui::GetIO().DisplaySize.x - DISTANCE, ImGui::GetMainViewport()->Pos.y + DISTANCE);
			ImVec2 window_pos_pivot = ImVec2(1.0f, 0.0f);

			if (ConHost::IsConsoleOpen())
			{
				window_pos = ImVec2(ImGui::GetMainViewport()->Pos.x + ImGui::GetIO().DisplaySize.x - DISTANCE, ImGui::GetMainViewport()->Pos.y + ImGui::GetIO().DisplaySize.y - DISTANCE);
				window_pos_pivot = ImVec2(1.0f, 1.0f);
			}

			ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always, window_pos_pivot);
			ImGui::SetNextWindowFocus();
			ImGui::SetNextWindowBgAlpha(0.3f); // Transparent background
			if (ImGui::Begin("Permission Request", NULL, ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav))
			{
				ImGui::Text("Permission request: %s", requestedPermissionResource.c_str());
				ImGui::Separator();
				
				if (ConHost::IsConsoleOpen())
				{
					ImGui::Text("The resource %s at %s wants access to the following:", requestedPermissionResource.c_str(), requestedPermissionUrl.c_str());

					static std::map<int, std::string> permTypes{
						{ nui::NUI_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE, "Capture your desktop sound" },
						{ nui::NUI_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE, "Capture your full screen ^1/!\\ ALERT! This may expose personal information!" },
						{ nui::NUI_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE, "Capture your microphone" },
						{ nui::NUI_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE, "Capture your webcam" },
					};

					int i = 0;

					for (auto& type : permTypes)
					{
						if (requestedPermissionMask & type.first)
						{
							ImGui::Checkbox(type.second.c_str(), &permGrants[i]);
						}

						i++;
					}

					if (requestedPermissionMask & nui::NUI_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE)
					{
						if (permGrants[2] && !permGrants[3])
						{
							permGrants[3] = permGrants[2];
						}
					}

					// "accepted must match requested" dumb stuff
					if (!(requestedPermissionMask & nui::NUI_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE))
					{
						bool allowedAny = permGrants[0] || permGrants[1];
						permGrants[0] = allowedAny;
						permGrants[1] = allowedAny;
					}

					ImGui::Separator();
					if (ImGui::Button("Allow"))
					{
						int outcomeMask = 0;
						int i = 0;

						for (auto& type : permTypes)
						{
							if (permGrants[i] && (requestedPermissionMask & type.first))
							{
								outcomeMask |= type.first;
							}

							i++;
						}

						requestedPermissionCallback(true, outcomeMask);
						requestedPermissionCallback = {};
					}
					
					ImGui::SameLine();

					if (ImGui::Button("Deny"))
					{
						requestedPermissionCallback(false, 0);
						requestedPermissionCallback = {};
					}
				}
				else
				{
					ImGui::Text("The resource at %s is requesting your permission to access media devices.", requestedPermissionUrl.c_str());
					ImGui::Separator();
					ImGui::Text("Press ^2F8^7 to accept/deny.");
				}

				ImGui::End();
			}
		}
	});
});
