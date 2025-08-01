/*
 * Copyright Elasticsearch B.V. and/or licensed to Elasticsearch B.V. under one
 * or more contributor license agreements. Licensed under the Elastic License
 * 2.0 and the following additional limitation. Functionality enabled by the
 * files subject to the Elastic License 2.0 may only be used in production when
 * invoked by an Elasticsearch process with a license key installed that permits
 * use of machine learning features. You may not use this file except in
 * compliance with the Elastic License 2.0 and the foregoing additional
 * limitation.
 */

#include <model/CProcessMemoryUsage.h>

namespace ml {
namespace model {
// On platforms other than Linux the process memory usage is the estimated size of the models.
const CProcessMemoryUsage::EMemoryStrategy CProcessMemoryUsage::MEMORY_STRATEGY{
    EMemoryStrategy::E_Estimated};
}
}
