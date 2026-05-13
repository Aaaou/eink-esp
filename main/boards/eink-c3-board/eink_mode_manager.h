#ifndef _EINK_MODE_MANAGER_H_
#define _EINK_MODE_MANAGER_H_

#include "settings.h"

enum class EinkBoardMode {
    XiaoZhi = 0,
    EpaperBle = 1,
};

class EinkModeManager {
public:
    static EinkBoardMode LoadMode();
    static void SaveMode(EinkBoardMode mode);
    static EinkBoardMode ToggleMode();
};

EinkBoardMode GetEinkBoardMode();
bool IsEpaperBleBoardMode();

#endif // _EINK_MODE_MANAGER_H_
