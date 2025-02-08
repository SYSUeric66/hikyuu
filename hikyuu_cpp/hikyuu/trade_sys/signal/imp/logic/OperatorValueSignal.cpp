/*
 *  Copyright (c) 2025 hikyuu.org
 *
 *  Created on: 2025-02-08
 *      Author: fasiondog
 */

#include "OperatorValueSignal.h"

#if HKU_SUPPORT_SERIALIZATION
BOOST_CLASS_EXPORT(hku::OperatorValueSignal)
#endif

namespace hku {

OperatorValueSignal::OperatorValueSignal() : SignalBase("SG_OperatorValue") {}
OperatorValueSignal::OperatorValueSignal(const string& name) : SignalBase(name) {}

OperatorValueSignal::OperatorValueSignal(const string& name, const SignalPtr& sg, double value,
                                         int mode)
: SignalBase(name), m_value(value), m_mode(mode) {
    if (sg) {
        m_sg = sg->clone();
    }
}

OperatorValueSignal::~OperatorValueSignal() {}

void OperatorValueSignal::_reset() {
    m_value = 0.0;
    if (m_sg) {
        m_sg->reset();
    }
}

SignalPtr OperatorValueSignal::_clone() {
    return make_shared<OperatorValueSignal>(m_name, m_sg, m_value);
}

} /* namespace hku */
