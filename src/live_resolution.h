#pragma once

#include <algorithm>
#include <cmath>
#include <utility>

inline std::pair<int, int> liveOutputSize(int width, int height, int maxDimension) {
    if (width <= 0 || height <= 0) return {0, 0};
    if (maxDimension <= 0 || std::max(width, height) <= maxDimension)
        return {width, height};
    const double scale = double(maxDimension) / double(std::max(width, height));
    return {std::max(1, int(std::lround(double(width) * scale))),
            std::max(1, int(std::lround(double(height) * scale)))};
}
