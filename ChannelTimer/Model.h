#pragma once
#include <functional>
#include <string>
#include <vector>
#include <windows.h>
#include "TVTestPlugin.h"

namespace ChannelTimer {
	struct CServiceInfo {
		const WORD NetworkID;
		const WORD ServiceID;
		const int channel;
		const std::wstring channelName;

		CServiceInfo(const TVTest::ChannelInfo& ChInfo)
			: NetworkID(ChInfo.NetworkID)
			, ServiceID(ChInfo.ServiceID)
			, channel(ChInfo.Channel)
			, channelName(ChInfo.szChannelName)
		{}

		bool operator==(const CServiceInfo& rhs) const {
			return this->ServiceID == rhs.ServiceID && this->NetworkID == rhs.NetworkID;
		}

		std::wstring toString() const;
	};

	std::vector<std::wstring> GetDrivers(
		/* const */ TVTest::CTVTestApp* pApp,
		std::function<void(std::wstring name, int index)> func = nullptr);

	/**
	 * 現在のチューナーのチューニング空間
	 */
	std::vector<std::wstring> GetTuningSpaces(
		/* const */ TVTest::CTVTestApp* pApp,
		const std::function<void(const std::wstring& name, int index)> action = nullptr);

	/**
	 * チューナー名指定のチューニング空間
	 */
	std::vector<std::wstring> GetTuningSpaces(
		/* const */ TVTest::CTVTestApp* pApp,
		const std::wstring driverNameString,
		const std::function<void(const std::wstring& name, int index)> action = nullptr);

	/**
	 * 現在のチューニング空間のチャンネル
	 */
	std::vector<CServiceInfo> GetChannels(
		/* const */ TVTest::CTVTestApp* pApp,
		int curTuningSpace,
		const std::function<void(const CServiceInfo& chInfo, int index)> action = nullptr);

	/**
	 * チューナー名、チューニング空間指定のチューニング空間のチャンネル
	 */
	std::vector<CServiceInfo> GetChannels(
		/* const */ TVTest::CTVTestApp* pApp,
		const std::wstring driverNameString,
		int curTuningSpace,
		const std::function<void(const CServiceInfo& chInfo, int index)> action = nullptr);
}