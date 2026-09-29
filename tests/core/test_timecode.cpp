#include "core/timecode.h"

#include <cstdio>

int main() {
    using mvm::core::formatTimecode;
    using mvm::core::parseTimecode;
    if (formatTimecode(1800, 30000, 1001) != "00:01:00:00" ||
        parseTimecode("00:01:00:00", 30000, 1001) != 1800 ||
        parseTimecode("00:00:00:30", 30000, 1001) || parseTimecode("00:60:00:00", 30000, 1001) ||
        parseTimecode("00:00:00:-1", 30000, 1001) || parseTimecode("broken", 30000, 1001)) {
        std::fprintf(stderr, "timecode の往復または不正入力の拒否に失敗しました\n");
        return 1;
    }
    std::puts("timecode: PASS");
    return 0;
}
