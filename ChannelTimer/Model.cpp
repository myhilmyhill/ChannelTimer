#include "Model.h"

namespace ChannelTimer {
	std::wstring CServiceInfo::toString() const {
		const std::wstring ws = L" ";
		return std::to_wstring(ServiceID)
			+ ws
			+ std::to_wstring(NetworkID)
			+ ws
			+ channelName;
	}

	std::vector<std::wstring> GetDrivers(
		/* const */ TVTest::CTVTestApp* pApp,
		std::function<void(std::wstring name, int index)> func)
	{
		std::vector<std::wstring> drivers;
		WCHAR driverName[MAX_PATH];
		for (int i = 0; pApp->EnumDriver(i, driverName, _countof(driverName)); i++) {
			drivers.push_back(driverName);
			if (func) {
				func(driverName, i);
			}
		}
		return drivers;
	}

	std::vector<std::wstring> GetTuningSpaces(
		/* const */ TVTest::CTVTestApp* pApp,
		const std::function<void(const std::wstring& name, int index)> action)
	{
		std::vector<std::wstring> spaces;
		int NumSpaces = 0;
		const int _ = pApp->GetTuningSpace(&NumSpaces);

		for (int i = 0; i < NumSpaces; i++) {
			WCHAR name[MAX_PATH];
			pApp->GetTuningSpaceName(i, name, _countof(name));
			spaces.push_back(name);
			if (action) {
				action(name, i);
			}
		}
		return spaces;
	}

	std::vector<std::wstring> GetTuningSpaces(
		/* const */ TVTest::CTVTestApp* pApp,
		const std::wstring driverNameString,
		const std::function<void(const std::wstring& name, int index)> action)
	{
		std::vector<std::wstring> spaces;
		TVTest::DriverTuningSpaceList tuningList;
		pApp->GetDriverTuningSpaceList(driverNameString.c_str(), &tuningList);

		for (int i = 0; i < tuningList.NumSpaces; i++) {
			const WCHAR* name = tuningList.SpaceList[i]->pInfo->szName;
			spaces.push_back(name);
			if (action) {
				action(name, i);
			}
		}
		pApp->FreeDriverTuningSpaceList(&tuningList);

		return spaces;
	}

	std::vector<CServiceInfo> GetChannels(
		/* const */ TVTest::CTVTestApp* pApp,
		int curTuningSpace,
		const std::function<void(const CServiceInfo& chInfo, int index)> action)
	{
		std::vector<CServiceInfo> channels;

		// 現在のチューニング空間のチャンネルを取得する
		TVTest::ChannelInfo ChInfo;
		for (int Channel = 0; pApp->GetChannelInfo(curTuningSpace, Channel, &ChInfo); Channel++) {
			if (ChInfo.Flags & TVTest::CHANNEL_FLAG_DISABLED)
				continue;

			const CServiceInfo pServiceInfo = CServiceInfo(ChInfo);
			channels.push_back(pServiceInfo);
			if (action) {
				action(pServiceInfo, Channel);
			}
		}
		return channels;
	}

	std::vector<CServiceInfo> GetChannels(
		/* const */ TVTest::CTVTestApp* pApp,
		const std::wstring driverNameString,
		int curTuningSpace,
		const std::function<void(const CServiceInfo& chInfo, int index)> action)
	{
		std::vector<CServiceInfo> channels;

		TVTest::DriverTuningSpaceList tuningList;
		pApp->GetDriverTuningSpaceList(driverNameString.c_str(), &tuningList);

		// チャンネル
		if (tuningList.NumSpaces >= 0) {
			const auto& spaceList = *tuningList.SpaceList[curTuningSpace];

			for (int Channel = 0; Channel < spaceList.NumChannels; Channel++) {
				const TVTest::ChannelInfo& ChInfo = *spaceList.ChannelList[Channel];
				if (ChInfo.Flags & TVTest::CHANNEL_FLAG_DISABLED)
					continue;

				const CServiceInfo pServiceInfo = CServiceInfo(ChInfo);
				channels.push_back(pServiceInfo);
				if (action) {
					action(pServiceInfo, Channel);
				}
			}
		}
		pApp->FreeDriverTuningSpaceList(&tuningList);
		return channels;
	}
}
