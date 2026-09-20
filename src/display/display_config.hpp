#pragma once
/**
 * display_config.hpp — 显示配置 (V3)
 *
 * 800×800 不散落 magic number，全部集中于此。
 */

#include <string>
#include <stdexcept>

namespace holopet {

struct DisplayConfig {
    int width = 800;
    int height = 800;

    bool fullscreen = false;

    std::string title = "HoloPet";

    /** 验证配置, 无效抛异常 */
    void validate() const {
        if (width <= 0) {
            throw std::invalid_argument(
                "DisplayConfig: width must be > 0, got " + std::to_string(width));
        }
        if (height <= 0) {
            throw std::invalid_argument(
                "DisplayConfig: height must be > 0, got " + std::to_string(height));
        }
    }

    /** 是否有效 (不抛异常) */
    bool valid() const noexcept {
        return width > 0 && height > 0;
    }
};

} // namespace holopet
