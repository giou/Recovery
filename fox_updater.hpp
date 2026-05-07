#pragma once

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
}
