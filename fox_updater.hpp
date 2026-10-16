#pragma once

#include <string>

namespace FoxUpdater {
	int Refresh();
	int Init();
	int Check();
	int VariantPrev();
	int VariantNext();
	int SelectVariant();
	int PrepareAvailableReleases();
	int SelectAvailableRelease();
	int Download();
	int Install();
	int SetTestOptions(const std::string& fox_id, const std::string& type_filter);
}
