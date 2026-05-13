#include "eink_mode_manager.h"

static constexpr const char* kNamespace = "eink_mode";
static constexpr const char* kKey = "mode";

EinkBoardMode EinkModeManager::LoadMode() {
    Settings settings(kNamespace, false);
    int value = settings.GetInt(kKey, 0);
    return value == 1 ? EinkBoardMode::EpaperBle : EinkBoardMode::XiaoZhi;
}

void EinkModeManager::SaveMode(EinkBoardMode mode) {
    Settings settings(kNamespace, true);
    settings.SetInt(kKey, mode == EinkBoardMode::EpaperBle ? 1 : 0);
}

EinkBoardMode EinkModeManager::ToggleMode() {
    EinkBoardMode current = LoadMode();
    EinkBoardMode next = current == EinkBoardMode::XiaoZhi ? EinkBoardMode::EpaperBle : EinkBoardMode::XiaoZhi;
    SaveMode(next);
    return next;
}
