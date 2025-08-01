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

#include <core/CLogger.h>
#include <core/CMemoryDef.h>
#include <core/CWordDictionary.h>
#include <core/Constants.h>

#include <model/CAnomalyDetector.h>
#include <model/CAnomalyDetectorModelConfig.h>
#include <model/CHierarchicalResults.h>
#include <model/CLimits.h>
#include <model/CResourceMonitor.h>
#include <model/CTokenListDataCategorizer.h>

#include <boost/test/unit_test.hpp>

#include <string>

BOOST_AUTO_TEST_SUITE(CResourceMonitorTest)

using namespace ml;
using namespace model;

class CTestFixture {
public:
    CTestFixture() {}

    void reportCallback(const CResourceMonitor::SModelSizeStats& modelSizeStats) {
        m_ReportedModelSizeStats = modelSizeStats;
    }

    void addTestData(core_t::TTime& firstTime,
                     const core_t::TTime bucketLength,
                     const std::size_t buckets,
                     const std::size_t newPeoplePerBucket,
                     std::size_t& startOffset,
                     CAnomalyDetector& detector,
                     CResourceMonitor& monitor) {
        std::string numberValue("100");
        core_t::TTime bucketStart = firstTime;
        CHierarchicalResults results;
        std::string pervasive("IShouldNotBeRemoved");

        for (core_t::TTime time = firstTime;
             time < static_cast<core_t::TTime>(firstTime + bucketLength * buckets);
             time += (bucketLength / std::max(std::size_t(1), newPeoplePerBucket))) {
            bool newBucket = false;
            for (; bucketStart + bucketLength <= time; bucketStart += bucketLength) {
                detector.buildResults(bucketStart, bucketStart + bucketLength, results);
                monitor.pruneIfRequired(bucketStart);
                monitor.updateMoments(monitor.totalMemory(), bucketStart, bucketLength);
                newBucket = true;
            }

            if (newBucket) {
                CAnomalyDetector::TStrCPtrVec fieldValues;
                fieldValues.push_back(&pervasive);
                fieldValues.push_back(&numberValue);
                detector.addRecord(time, fieldValues);
            }

            if (newPeoplePerBucket > 0) {
                CAnomalyDetector::TStrCPtrVec fieldValues;
                std::ostringstream ss1;
                ss1 << "person" << startOffset++;
                std::string value(ss1.str());

                fieldValues.push_back(&value);
                fieldValues.push_back(&numberValue);
                detector.addRecord(time, fieldValues);
            }
        }
        firstTime = bucketStart;
    }

protected:
    CResourceMonitor::SModelSizeStats m_ReportedModelSizeStats;
};

BOOST_FIXTURE_TEST_CASE(testMonitor, CTestFixture) {
    static const std::string EMPTY_STRING;
    static const core_t::TTime FIRST_TIME{358556400};
    static const core_t::TTime BUCKET_LENGTH{3600};

    CAnomalyDetectorModelConfig modelConfig =
        CAnomalyDetectorModelConfig::defaultConfig(BUCKET_LENGTH);
    CLimits limits;

    model::CTokenListDataCategorizer<> categorizer(limits, nullptr, 0.7, "whatever");

    CSearchKey key1(1, // detectorIndex
                    function_t::E_IndividualMetric, false, model_t::E_XF_None,
                    "value", "colour");

    CAnomalyDetector detector1(limits, modelConfig, EMPTY_STRING, FIRST_TIME,
                               modelConfig.factory(key1));

    CSearchKey key2(2, // detectorIndex
                    function_t::E_IndividualMetric, false, model_t::E_XF_None,
                    "value", "colour");

    CAnomalyDetector detector2(limits, modelConfig, EMPTY_STRING, FIRST_TIME,
                               modelConfig.factory(key2));

    std::size_t mem = core::memory::dynamicSize(&categorizer) +
                      core::memory::dynamicSize(&detector1) +
                      core::memory::dynamicSize(&detector2);

    {
        // Test default constructor
        CResourceMonitor mon;
        BOOST_TEST_REQUIRE(mon.m_ByteLimitHigh > 0);
        BOOST_REQUIRE_EQUAL((49 * mon.m_ByteLimitHigh) / 50, mon.m_ByteLimitLow);
        BOOST_TEST_REQUIRE(mon.m_ByteLimitHigh > mon.m_ByteLimitLow);
        BOOST_TEST_REQUIRE(mon.m_AllowAllocations);
        LOG_DEBUG(<< "Resource limit is: " << mon.m_ByteLimitHigh);
        if (sizeof(std::size_t) == 8) {
            // 64-bit platform
            BOOST_REQUIRE_EQUAL(std::size_t(4 * core::constants::BYTES_IN_GIGABYTES / 2),
                                mon.m_ByteLimitHigh);
        } else {
            // Unexpected platform
            BOOST_TEST_REQUIRE(false);
        }
    }
    {
        // Test foreground persistence constructor
        CResourceMonitor mon(true);
        BOOST_TEST_REQUIRE(mon.m_ByteLimitHigh > 0);
        BOOST_REQUIRE_EQUAL((49 * mon.m_ByteLimitHigh) / 50, mon.m_ByteLimitLow);
        BOOST_TEST_REQUIRE(mon.m_ByteLimitHigh > mon.m_ByteLimitLow);
        BOOST_TEST_REQUIRE(mon.m_AllowAllocations);
        LOG_DEBUG(<< "Resource limit is: " << mon.m_ByteLimitHigh);
        if (sizeof(std::size_t) == 8) {
            // 64-bit platform
            BOOST_REQUIRE_EQUAL(std::size_t(4 * core::constants::BYTES_IN_GIGABYTES),
                                mon.m_ByteLimitHigh);
        } else {
            // Unexpected platform
            BOOST_TEST_REQUIRE(false);
        }
    }
    {
        // Test size constructor
        CResourceMonitor mon;
        mon.memoryLimit(543);
        BOOST_REQUIRE_EQUAL(std::size_t(569376768 / 2), mon.m_ByteLimitHigh);
        BOOST_REQUIRE_EQUAL(std::size_t((49 * 569376768ull / 2) / 50), mon.m_ByteLimitLow);
        BOOST_TEST_REQUIRE(mon.m_AllowAllocations);

        // Test memoryLimit
        mon.memoryLimit(987);
        BOOST_REQUIRE_EQUAL(std::size_t(1034944512ull / 2), mon.m_ByteLimitHigh);
        BOOST_REQUIRE_EQUAL(std::size_t((49 * 1034944512ull / 2) / 50), mon.m_ByteLimitLow);
    }
    {
        // Test adding and removing a CAnomalyDetector
        CResourceMonitor mon;

        BOOST_REQUIRE_EQUAL(0, mon.m_Resources.size());
        BOOST_REQUIRE_EQUAL(0, mon.m_MonitoredResourceCurrentMemory);
        BOOST_REQUIRE_EQUAL(0, mon.m_PreviousTotal);

        mon.registerComponent(categorizer);
        BOOST_REQUIRE_EQUAL(1, mon.m_Resources.size());

        mon.registerComponent(detector1);
        BOOST_REQUIRE_EQUAL(2, mon.m_Resources.size());

        mon.registerComponent(detector2);
        BOOST_REQUIRE_EQUAL(3, mon.m_Resources.size());

        mon.refresh(categorizer);
        mon.refresh(detector1);
        mon.refresh(detector2);
        mon.sendMemoryUsageReportIfSignificantlyChanged(0, 1);
        BOOST_REQUIRE_EQUAL(mem, mon.totalMemory());
        BOOST_REQUIRE_EQUAL(mem, mon.m_PreviousTotal);

        mon.unRegisterComponent(detector2);
        BOOST_REQUIRE_EQUAL(2, mon.m_Resources.size());

        mon.unRegisterComponent(detector1);
        BOOST_REQUIRE_EQUAL(1, mon.m_Resources.size());

        mon.unRegisterComponent(categorizer);
        BOOST_REQUIRE_EQUAL(0, mon.m_Resources.size());
    }
    {
        // Check that High limit can be breached and then gone back
        CResourceMonitor mon(false, 1.0);
        BOOST_TEST_REQUIRE(mem > 5); // This SHOULD be OK

        // Let's go above the low but below the high limit
        mon.m_ByteLimitHigh = mem + 1;
        mon.m_ByteLimitLow = mem - 1;

        mon.registerComponent(categorizer);
        mon.registerComponent(detector1);
        mon.registerComponent(detector2);
        mon.refresh(categorizer);
        mon.refresh(detector1);
        mon.refresh(detector2);
        BOOST_REQUIRE_EQUAL(mem, mon.totalMemory());
        BOOST_REQUIRE_EQUAL(true, mon.areAllocationsAllowed());
        BOOST_TEST_REQUIRE(mon.allocationLimit() > 0);

        // Let's go above the high and low limit
        mon.m_ByteLimitHigh = mem - 1;
        mon.m_ByteLimitLow = mem - 2;

        mon.refresh(categorizer);
        mon.refresh(detector1);
        mon.refresh(detector2);
        BOOST_REQUIRE_EQUAL(mem, mon.totalMemory());
        BOOST_REQUIRE_EQUAL(false, mon.areAllocationsAllowed());
        BOOST_REQUIRE_EQUAL(0, mon.allocationLimit());

        // Let's go above the low limit but above the high limit
        mon.m_ByteLimitHigh = mem + 1;
        mon.m_ByteLimitLow = mem - 1;

        mon.refresh(categorizer);
        mon.refresh(detector1);
        mon.refresh(detector2);
        BOOST_REQUIRE_EQUAL(mem, mon.totalMemory());
        BOOST_REQUIRE_EQUAL(false, mon.areAllocationsAllowed());

        // Let's go below the high and low limit
        mon.m_ByteLimitHigh = mem + 2;
        mon.m_ByteLimitLow = mem + 1;

        mon.refresh(categorizer);
        mon.refresh(detector1);
        mon.refresh(detector2);
        BOOST_REQUIRE_EQUAL(mem, mon.totalMemory());
        BOOST_REQUIRE_EQUAL(true, mon.areAllocationsAllowed());
    }
    {
        // Test the report callback
        CResourceMonitor mon;
        mon.registerComponent(categorizer);
        mon.registerComponent(detector1);
        mon.registerComponent(detector2);

        mon.memoryUsageReporter(std::bind_front(&CTestFixture::reportCallback, this));
        m_ReportedModelSizeStats.s_Usage = 0;
        BOOST_REQUIRE_EQUAL(0, m_ReportedModelSizeStats.s_Usage);

        mon.refresh(categorizer);
        mon.refresh(detector1);
        mon.refresh(detector2);
        mon.sendMemoryUsageReportIfSignificantlyChanged(0, 1);
        BOOST_REQUIRE_EQUAL(mem, m_ReportedModelSizeStats.s_Usage);
        BOOST_REQUIRE_EQUAL(model_t::E_MemoryStatusOk, m_ReportedModelSizeStats.s_MemoryStatus);
    }
    {
        // As above but refreshing all resources in one call
        CResourceMonitor mon;
        mon.registerComponent(categorizer);
        mon.registerComponent(detector1);
        mon.registerComponent(detector2);

        mon.memoryUsageReporter(std::bind_front(&CTestFixture::reportCallback, this));
        m_ReportedModelSizeStats.s_Usage = 0;
        BOOST_REQUIRE_EQUAL(0, m_ReportedModelSizeStats.s_Usage);

        mon.forceRefreshAll();
        mon.sendMemoryUsageReportIfSignificantlyChanged(0, 1);
        BOOST_REQUIRE_EQUAL(mem, m_ReportedModelSizeStats.s_Usage);
        BOOST_REQUIRE_EQUAL(model_t::E_MemoryStatusOk, m_ReportedModelSizeStats.s_MemoryStatus);
    }
    {
        // Test the report callback for allocation failures
        CResourceMonitor mon;
        mon.registerComponent(categorizer);
        mon.registerComponent(detector1);
        mon.registerComponent(detector2);

        mon.memoryUsageReporter(std::bind_front(&CTestFixture::reportCallback, this));
        m_ReportedModelSizeStats.s_AllocationFailures = 0;
        BOOST_REQUIRE_EQUAL(0, m_ReportedModelSizeStats.s_AllocationFailures);

        // Set a soft-limit degraded status
        mon.startPruning();

        // This refresh should trigger a report
        mon.refresh(categorizer);
        mon.refresh(detector1);
        mon.refresh(detector2);
        mon.sendMemoryUsageReportIfSignificantlyChanged(0, 1);
        BOOST_REQUIRE_EQUAL(0, m_ReportedModelSizeStats.s_AllocationFailures);
        BOOST_REQUIRE_EQUAL(mem, m_ReportedModelSizeStats.s_Usage);
        BOOST_REQUIRE_EQUAL(model_t::E_MemoryStatusSoftLimit,
                            m_ReportedModelSizeStats.s_MemoryStatus);

        // Set some canary values
        m_ReportedModelSizeStats.s_AllocationFailures = 12345;
        m_ReportedModelSizeStats.s_ByFields = 54321;
        m_ReportedModelSizeStats.s_OverFields = 23456;
        m_ReportedModelSizeStats.s_PartitionFields = 65432;
        m_ReportedModelSizeStats.s_Usage = 1357924;

        // This should not trigger a report
        mon.refresh(categorizer);
        mon.refresh(detector1);
        mon.refresh(detector2);
        mon.sendMemoryUsageReportIfSignificantlyChanged(0, 1);
        BOOST_REQUIRE_EQUAL(12345, m_ReportedModelSizeStats.s_AllocationFailures);
        BOOST_REQUIRE_EQUAL(54321, m_ReportedModelSizeStats.s_ByFields);
        BOOST_REQUIRE_EQUAL(23456, m_ReportedModelSizeStats.s_OverFields);
        BOOST_REQUIRE_EQUAL(65432, m_ReportedModelSizeStats.s_PartitionFields);
        BOOST_REQUIRE_EQUAL(1357924, m_ReportedModelSizeStats.s_Usage);

        // Add some memory failures, which should be reported
        mon.acceptAllocationFailureResult(14400000);
        mon.acceptAllocationFailureResult(14400000);
        mon.acceptAllocationFailureResult(14401000);
        mon.acceptAllocationFailureResult(14402000);

        BOOST_REQUIRE_EQUAL(3, mon.m_AllocationFailuresCount);
        BOOST_REQUIRE_EQUAL(core_t::TTime(0), mon.m_LastAllocationFailureReport);
        mon.refresh(categorizer);
        mon.refresh(detector1);
        mon.refresh(detector2);
        mon.sendMemoryUsageReportIfSignificantlyChanged(0, 1);
        BOOST_REQUIRE_EQUAL(3, m_ReportedModelSizeStats.s_AllocationFailures);
        BOOST_REQUIRE_EQUAL(mem, m_ReportedModelSizeStats.s_Usage);
        BOOST_REQUIRE_EQUAL(core_t::TTime(14402000), mon.m_LastAllocationFailureReport);
        BOOST_REQUIRE_EQUAL(model_t::E_MemoryStatusHardLimit,
                            m_ReportedModelSizeStats.s_MemoryStatus);

        // Set some canary values
        m_ReportedModelSizeStats.s_AllocationFailures = 12345;
        m_ReportedModelSizeStats.s_ByFields = 54321;
        m_ReportedModelSizeStats.s_OverFields = 23456;
        m_ReportedModelSizeStats.s_PartitionFields = 65432;
        m_ReportedModelSizeStats.s_Usage = 1357924;

        // As nothing has changed, nothing should be reported
        mon.refresh(categorizer);
        mon.refresh(detector1);
        mon.refresh(detector2);
        mon.sendMemoryUsageReportIfSignificantlyChanged(0, 1);
        BOOST_REQUIRE_EQUAL(12345, m_ReportedModelSizeStats.s_AllocationFailures);
        BOOST_REQUIRE_EQUAL(54321, m_ReportedModelSizeStats.s_ByFields);
        BOOST_REQUIRE_EQUAL(23456, m_ReportedModelSizeStats.s_OverFields);
        BOOST_REQUIRE_EQUAL(65432, m_ReportedModelSizeStats.s_PartitionFields);
        BOOST_REQUIRE_EQUAL(1357924, m_ReportedModelSizeStats.s_Usage);
    }
    {
        // Test the need to report usage based on a change in levels, up and down
        CResourceMonitor mon;
        mon.memoryUsageReporter(std::bind_front(&CTestFixture::reportCallback, this));
        BOOST_TEST_REQUIRE(!mon.needToSendReport(
            model_t::E_AssignmentBasisCurrentModelBytes, 0, 1));

        std::size_t origTotalMemory = mon.totalMemory();

        // Go up to 10 bytes, triggering a need
        mon.m_MonitoredResourceCurrentMemory = 10;
        BOOST_TEST_REQUIRE(
            mon.needToSendReport(model_t::E_AssignmentBasisCurrentModelBytes, 0, 1));
        mon.sendMemoryUsageReport(0, 1);
        BOOST_REQUIRE_EQUAL(origTotalMemory + 10, m_ReportedModelSizeStats.s_Usage);

        // Nothing new added, so no report
        BOOST_TEST_REQUIRE(!mon.needToSendReport(
            model_t::E_AssignmentBasisCurrentModelBytes, 0, 1));

        // 10% increase should trigger a need
        mon.m_MonitoredResourceCurrentMemory += 1 + (origTotalMemory + 9) / 10;
        BOOST_TEST_REQUIRE(
            mon.needToSendReport(model_t::E_AssignmentBasisCurrentModelBytes, 0, 1));
        mon.sendMemoryUsageReport(0, 1);
        BOOST_REQUIRE_EQUAL(origTotalMemory + 11 + (origTotalMemory + 9) / 10,
                            m_ReportedModelSizeStats.s_Usage);

        // Huge increase should trigger a need
        mon.m_MonitoredResourceCurrentMemory = 1000;
        BOOST_TEST_REQUIRE(
            mon.needToSendReport(model_t::E_AssignmentBasisCurrentModelBytes, 0, 1));
        mon.sendMemoryUsageReport(0, 1);
        BOOST_REQUIRE_EQUAL(origTotalMemory + 1000, m_ReportedModelSizeStats.s_Usage);

        // 0.1% increase should not trigger a need
        mon.m_MonitoredResourceCurrentMemory += 1 + (origTotalMemory + 999) / 1000;
        BOOST_TEST_REQUIRE(!mon.needToSendReport(
            model_t::E_AssignmentBasisCurrentModelBytes, 0, 1));

        // A decrease should trigger a need
        mon.m_MonitoredResourceCurrentMemory = 900;
        BOOST_TEST_REQUIRE(
            mon.needToSendReport(model_t::E_AssignmentBasisCurrentModelBytes, 0, 1));
        mon.sendMemoryUsageReport(0, 1);
        BOOST_REQUIRE_EQUAL(origTotalMemory + 900, m_ReportedModelSizeStats.s_Usage);

        // A tiny decrease should not trigger a need
        mon.m_MonitoredResourceCurrentMemory = 899;
        BOOST_TEST_REQUIRE(!mon.needToSendReport(
            model_t::E_AssignmentBasisCurrentModelBytes, 0, 1));
    }
}

BOOST_FIXTURE_TEST_CASE(testPruning, CTestFixture) {
    static const std::string EMPTY_STRING;
    static const core_t::TTime FIRST_TIME{358556400};
    static const core_t::TTime BUCKET_LENGTH{3600};

    CAnomalyDetectorModelConfig modelConfig =
        CAnomalyDetectorModelConfig::defaultConfig(BUCKET_LENGTH);
    CLimits limits(false, 1.0);

    CSearchKey key(1, // detectorIndex
                   function_t::E_IndividualMetric, false, model_t::E_XF_None,
                   "value", "colour");

    CResourceMonitor& monitor = limits.resourceMonitor();
    monitor.memoryLimit(140);

    CAnomalyDetector detector(limits, modelConfig, EMPTY_STRING, FIRST_TIME,
                              modelConfig.factory(key));

    core_t::TTime bucket = FIRST_TIME;
    std::size_t startOffset = 10;

    // Add a couple of buckets of data
    this->addTestData(bucket, BUCKET_LENGTH, 2, 5, startOffset, detector, monitor);
    BOOST_REQUIRE_EQUAL(false, monitor.pruneIfRequired(bucket));
    BOOST_REQUIRE_EQUAL(false, monitor.m_HasPruningStarted);
    BOOST_REQUIRE_EQUAL(model_t::E_MemoryStatusOk, monitor.m_MemoryStatus);
    // Memory should not be stable, as we've only seen 2 buckets
    BOOST_REQUIRE_EQUAL(false, monitor.isMemoryStable(BUCKET_LENGTH));

    LOG_DEBUG(<< "Saturating the pruner");
    // Add enough data to saturate the pruner
    this->addTestData(bucket, BUCKET_LENGTH, 1100, 3, startOffset, detector, monitor);

    LOG_DEBUG(<< "Window is now: " << monitor.m_PruneWindow);
    BOOST_REQUIRE_EQUAL(true, monitor.m_HasPruningStarted);
    BOOST_TEST_REQUIRE(monitor.m_PruneWindow < std::size_t(1000));
    BOOST_REQUIRE_EQUAL(model_t::E_MemoryStatusSoftLimit, monitor.m_MemoryStatus);
    // Memory should be stable now, after 1100 more buckets,
    // and with the pruner running
    BOOST_REQUIRE_EQUAL(true, monitor.isMemoryStable(BUCKET_LENGTH));
    BOOST_REQUIRE_EQUAL(true, monitor.m_AllowAllocations);

    LOG_DEBUG(<< "Allowing pruner to relax");
    // Add no new people and see that the window relaxes away from the minimum window
    this->addTestData(bucket, BUCKET_LENGTH, 100, 0, startOffset, detector, monitor);
    LOG_DEBUG(<< "Window is now: " << monitor.m_PruneWindow);
    std::size_t level = monitor.m_PruneWindow;
    BOOST_REQUIRE_EQUAL(model_t::E_MemoryStatusSoftLimit, monitor.m_MemoryStatus);
    BOOST_REQUIRE_EQUAL(true, monitor.m_AllowAllocations);
    BOOST_TEST_REQUIRE(monitor.totalMemory() < monitor.m_PruneThreshold);

    LOG_DEBUG(<< "Testing fine-grained control");
    // Check that the window keeps growing now
    this->addTestData(bucket, BUCKET_LENGTH, 50, 0, startOffset, detector, monitor);
    BOOST_TEST_REQUIRE(monitor.m_PruneWindow > level);
    level = monitor.m_PruneWindow;
    this->addTestData(bucket, BUCKET_LENGTH, 50, 0, startOffset, detector, monitor);
    BOOST_TEST_REQUIRE(monitor.m_PruneWindow > level);
    level = monitor.m_PruneWindow;

    // Check that adding a bunch of new data shrinks the window again
    this->addTestData(bucket, BUCKET_LENGTH, 200, 10, startOffset, detector, monitor);
    BOOST_TEST_REQUIRE(monitor.m_PruneWindow < level);
    level = monitor.m_PruneWindow;

    // And finally that it grows again
    this->addTestData(bucket, BUCKET_LENGTH, 700, 0, startOffset, detector, monitor);
    BOOST_TEST_REQUIRE(monitor.m_PruneWindow > level);

    // If it grows enough we will stop pruning and revert to memory status OK,
    // and 8000 completely empty buckets (even without the all pervasive person)
    // is the quickest way to achieve this.
    bucket += BUCKET_LENGTH * 8000;
    this->addTestData(bucket, BUCKET_LENGTH, 2, 0, startOffset, detector, monitor);
    LOG_DEBUG(<< "Window is now: " << monitor.m_PruneWindow);
    BOOST_REQUIRE_EQUAL(false, monitor.m_HasPruningStarted);
    BOOST_REQUIRE_EQUAL(monitor.m_PruneWindowMaximum, monitor.m_PruneWindow);
    BOOST_REQUIRE_EQUAL(model_t::E_MemoryStatusOk, monitor.m_MemoryStatus);
}

BOOST_FIXTURE_TEST_CASE(testExtraMemory, CTestFixture) {
    static const std::string EMPTY_STRING;
    static const core_t::TTime FIRST_TIME{358556400};
    static const core_t::TTime BUCKET_LENGTH{3600};

    CAnomalyDetectorModelConfig modelConfig =
        CAnomalyDetectorModelConfig::defaultConfig(BUCKET_LENGTH);
    CLimits limits;

    CSearchKey key(1, // detectorIndex
                   function_t::E_IndividualMetric, false, model_t::E_XF_None,
                   "value", "colour");

    CResourceMonitor& monitor = limits.resourceMonitor();
    // set the limit to 1 MB
    monitor.memoryLimit(1);

    CAnomalyDetector detector(limits, modelConfig, EMPTY_STRING, FIRST_TIME,
                              modelConfig.factory(key));

    monitor.forceRefresh(detector);
    std::size_t allocationLimit = monitor.allocationLimit();

    monitor.addExtraMemory(100);
    monitor.addExtraMemory(100);

    BOOST_TEST_REQUIRE(monitor.areAllocationsAllowed());
    BOOST_REQUIRE_EQUAL(allocationLimit - 200, monitor.allocationLimit());

    monitor.clearExtraMemory();
    BOOST_TEST_REQUIRE(monitor.areAllocationsAllowed());
    BOOST_REQUIRE_EQUAL(allocationLimit, monitor.allocationLimit());

    // Push over the limit by adding 1MB
    monitor.addExtraMemory(core::constants::BYTES_IN_MEGABYTES);
    BOOST_TEST_REQUIRE(monitor.areAllocationsAllowed() == false);
    BOOST_REQUIRE_EQUAL(0, monitor.allocationLimit());

    monitor.clearExtraMemory();
    BOOST_TEST_REQUIRE(monitor.areAllocationsAllowed());
    BOOST_REQUIRE_EQUAL(allocationLimit, monitor.allocationLimit());
}

BOOST_FIXTURE_TEST_CASE(testPeakUsage, CTestFixture) {
    // Clear the counters so that other test cases do not interfere.
    core::CProgramCounters::counter(counter_t::E_TSADPeakMemoryUsage) = 0;

    CLimits limits;
    CResourceMonitor& monitor = limits.resourceMonitor();
    monitor.memoryUsageReporter(std::bind_front(&CTestFixture::reportCallback, this));
    std::size_t baseTotalMemory = monitor.totalMemory();

    monitor.updateMoments(monitor.totalMemory(), 0, 1);
    monitor.sendMemoryUsageReport(0, 1);
    BOOST_REQUIRE_EQUAL(baseTotalMemory, m_ReportedModelSizeStats.s_Usage);
    BOOST_REQUIRE_EQUAL(baseTotalMemory, m_ReportedModelSizeStats.s_PeakUsage);

    monitor.addExtraMemory(100);

    monitor.updateMoments(monitor.totalMemory(), 0, 1);
    monitor.sendMemoryUsageReport(0, 1);
    BOOST_REQUIRE_EQUAL(baseTotalMemory + 100, m_ReportedModelSizeStats.s_Usage);
    BOOST_REQUIRE_EQUAL(baseTotalMemory + 100, m_ReportedModelSizeStats.s_PeakUsage);

    monitor.addExtraMemory(-50);

    monitor.updateMoments(monitor.totalMemory(), 0, 1);
    monitor.sendMemoryUsageReport(0, 1);
    BOOST_REQUIRE_EQUAL(baseTotalMemory + 50, m_ReportedModelSizeStats.s_Usage);
    BOOST_REQUIRE_EQUAL(baseTotalMemory + 100, m_ReportedModelSizeStats.s_PeakUsage);

    monitor.addExtraMemory(100);

    monitor.updateMoments(monitor.totalMemory(), 0, 1);
    monitor.sendMemoryUsageReport(0, 1);
    BOOST_REQUIRE_EQUAL(baseTotalMemory + 150, m_ReportedModelSizeStats.s_Usage);
    BOOST_REQUIRE_EQUAL(baseTotalMemory + 150, m_ReportedModelSizeStats.s_PeakUsage);
}

BOOST_FIXTURE_TEST_CASE(testUpdateMoments, CTestFixture) {

    static const core_t::TTime FIRST_TIME{358556400};
    static const core_t::TTime BUCKET_LENGTH{3600};

    // First a realistic case
    {
        CLimits limits;
        CResourceMonitor& monitor = limits.resourceMonitor();
        core_t::TTime time{FIRST_TIME};
        std::size_t totalMemory{1000000};

        // For the first 19 buckets memory is not stable due to the bucket count alone
        for (std::size_t count = 0; count < 19; ++count) {
            monitor.updateMoments(totalMemory, time, BUCKET_LENGTH);
            BOOST_REQUIRE_EQUAL(FIRST_TIME, monitor.m_FirstMomentsUpdateTime);
            BOOST_REQUIRE_EQUAL(time, monitor.m_LastMomentsUpdateTime);
            BOOST_REQUIRE_EQUAL(false, monitor.isMemoryStable(BUCKET_LENGTH));
            time += BUCKET_LENGTH;
            totalMemory += 200000;
        }

        // At bucket 20 the coefficient of variation comes into play - initially it's
        // too high, as we added 200000 bytes to the memory usage in every one of the
        // first 19 buckets
        for (std::size_t count = 20; count < 45; ++count) {
            monitor.updateMoments(totalMemory, time, BUCKET_LENGTH);
            BOOST_REQUIRE_EQUAL(FIRST_TIME, monitor.m_FirstMomentsUpdateTime);
            BOOST_REQUIRE_EQUAL(time, monitor.m_LastMomentsUpdateTime);
            BOOST_REQUIRE_EQUAL(false, monitor.isMemoryStable(BUCKET_LENGTH));
            time += BUCKET_LENGTH;
        }

        // After several buckets of flat memory use memory should be reported as
        // stable, and should continue to be reported as stable even when there
        // are small fluctuations
        for (std::size_t count = 46; count < 100; ++count) {
            monitor.updateMoments(totalMemory, time, BUCKET_LENGTH);
            BOOST_REQUIRE_EQUAL(FIRST_TIME, monitor.m_FirstMomentsUpdateTime);
            BOOST_REQUIRE_EQUAL(time, monitor.m_LastMomentsUpdateTime);
            BOOST_REQUIRE_EQUAL(true, monitor.isMemoryStable(BUCKET_LENGTH));
            time += BUCKET_LENGTH;
            totalMemory += (count % 5 - 2) * 1000;
        }
    }
    // Unexpected edge cases - mean and variance both always zero
    {
        CLimits limits;
        CResourceMonitor& monitor = limits.resourceMonitor();
        core_t::TTime time{FIRST_TIME};

        // Asking about stability before adding any measurements is wrong but
        // should not cause a crash
        BOOST_REQUIRE_EQUAL(false, monitor.isMemoryStable(BUCKET_LENGTH));

        // For the first 19 buckets memory is not stable due to the bucket count alone
        for (std::size_t count = 0; count < 19; ++count) {
            monitor.updateMoments(0, time, BUCKET_LENGTH);
            BOOST_REQUIRE_EQUAL(FIRST_TIME, monitor.m_FirstMomentsUpdateTime);
            BOOST_REQUIRE_EQUAL(time, monitor.m_LastMomentsUpdateTime);
            BOOST_REQUIRE_EQUAL(false, monitor.isMemoryStable(BUCKET_LENGTH));
            time += BUCKET_LENGTH;
        }

        // At bucket 20 the coefficient of variation comes into play, and by its
        // textbook definition it's 0/0.  However, the rearrangement of the
        // formula should avoid NaNs.
        monitor.updateMoments(0, time, BUCKET_LENGTH);
        BOOST_REQUIRE_EQUAL(FIRST_TIME, monitor.m_FirstMomentsUpdateTime);
        BOOST_REQUIRE_EQUAL(time, monitor.m_LastMomentsUpdateTime);
        BOOST_REQUIRE_EQUAL(true, monitor.isMemoryStable(BUCKET_LENGTH));
    }
}

BOOST_AUTO_TEST_SUITE_END()
