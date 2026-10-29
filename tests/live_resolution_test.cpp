#include "live_resolution.h"

#include <utility>

int main() {
    if (liveOutputSize(3840, 2160, 1280) != std::pair<int, int>{1280, 720}) return 1;
    if (liveOutputSize(1080, 1920, 1280) != std::pair<int, int>{720, 1280}) return 2;
    if (liveOutputSize(640, 480, 1280) != std::pair<int, int>{640, 480}) return 3;
    if (liveOutputSize(640, 480, 0) != std::pair<int, int>{640, 480}) return 4;
    if (liveOutputSize(1, 10000, 1280) != std::pair<int, int>{1, 1280}) return 5;
    if (liveOutputSize(0, 100, 1280) != std::pair<int, int>{0, 0}) return 6;
    return 0;
}
