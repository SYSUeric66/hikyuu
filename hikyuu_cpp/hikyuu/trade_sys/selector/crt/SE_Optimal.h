/*
 *  Copyright (c) 2024 hikyuu.org
 *
 *  Created on: 2024-09-14
 *      Author: fasiondog
 */

#pragma once

#include "../SelectorBase.h"

namespace hku {

/**
 * 测试账户资产最大寻优选择器
 * @return SEPtr
 */
SEPtr HKU_API SE_MaxFundsOptimal();

/**
 * 使用 Performance 统计结果进行寻优的选择器
 * @return SEPtr
 */
SEPtr HKU_API SE_PerformanceOptimal();

}  // namespace hku