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

#include <api/CDataFrameAnalysisSpecificationJsonWriter.h>

#include <core/CDataFrame.h>

#include <api/CDataFrameAnalysisSpecification.h>

#include <iostream>

namespace ml {
namespace api {

void CDataFrameAnalysisSpecificationJsonWriter::write(const std::string& jobId,
                                                      std::size_t rows,
                                                      std::size_t cols,
                                                      std::size_t memoryLimit,
                                                      std::size_t numberThreads,
                                                      const std::string& temporaryDirectory,
                                                      const std::string& resultsField,
                                                      const std::string& missingFieldValue,
                                                      const TStrVec& categoricalFields,
                                                      bool diskUsageAllowed,
                                                      const std::string& analysisName,
                                                      const std::string& analysisParameters,
                                                      TBoostJsonLineWriter& writer) {
    json::value analysisParametersDoc;
    if (analysisParameters.empty() == false) {
        json::error_code ec;
        json::parser p;
        p.write(analysisParameters, ec);
        if (ec.failed()) {
            HANDLE_FATAL(<< "Input error: analysis parameters " << analysisParameters
                         << " cannot be parsed as json. Please report this problem.");
        }
        analysisParametersDoc = p.release();
    }
    LOG_DEBUG(<< "analysisParametersDoc: " << analysisParametersDoc);

    write(jobId, rows, cols, memoryLimit, numberThreads, temporaryDirectory,
          resultsField, missingFieldValue, categoricalFields, diskUsageAllowed,
          analysisName, analysisParametersDoc, writer);
}

void CDataFrameAnalysisSpecificationJsonWriter::write(const std::string& jobId,
                                                      std::size_t rows,
                                                      std::size_t cols,
                                                      std::size_t memoryLimit,
                                                      std::size_t numberThreads,
                                                      const std::string& temporaryDirectory,
                                                      const std::string& resultsField,
                                                      const std::string& missingFieldValue,
                                                      const TStrVec& categoricalFields,
                                                      bool diskUsageAllowed,
                                                      const std::string& analysisName,
                                                      const json::value& analysisParametersDocument,
                                                      TBoostJsonLineWriter& writer) {
    writer.StartObject();

    writer.Key(CDataFrameAnalysisSpecification::JOB_ID);
    writer.String(jobId);

    writer.Key(CDataFrameAnalysisSpecification::ROWS);
    writer.Uint64(rows);

    writer.Key(CDataFrameAnalysisSpecification::COLS);
    writer.Uint64(cols);

    writer.Key(CDataFrameAnalysisSpecification::MEMORY_LIMIT);
    writer.Uint64(memoryLimit);

    writer.Key(CDataFrameAnalysisSpecification::THREADS);
    writer.Uint64(numberThreads);

    writer.Key(CDataFrameAnalysisSpecification::TEMPORARY_DIRECTORY);
    writer.String(temporaryDirectory);

    writer.Key(CDataFrameAnalysisSpecification::RESULTS_FIELD);
    writer.String(resultsField);

    if (missingFieldValue != core::CDataFrame::DEFAULT_MISSING_STRING) {
        writer.Key(CDataFrameAnalysisSpecification::MISSING_FIELD_VALUE);
        writer.String(missingFieldValue);
    }

    json::array array;
    for (const auto& field : categoricalFields) {
        array.push_back(json::value(field));
    }
    writer.Key(CDataFrameAnalysisSpecification::CATEGORICAL_FIELD_NAMES);
    writer.write(array);

    writer.Key(CDataFrameAnalysisSpecification::DISK_USAGE_ALLOWED);
    writer.Bool(diskUsageAllowed);

    writer.Key(CDataFrameAnalysisSpecification::ANALYSIS);
    writer.StartObject();
    writer.Key(CDataFrameAnalysisSpecification::NAME);
    writer.String(analysisName);

    // if no parameters are specified, parameters document has Null as its root element
    if (analysisParametersDocument.is_null() == false) {
        if (analysisParametersDocument.is_object()) {
            writer.Key(CDataFrameAnalysisSpecification::PARAMETERS);
            writer.write(analysisParametersDocument);
        } else {
            HANDLE_FATAL(<< "Input error: analysis parameters suppose to "
                         << "contain an object as root node.");
        }
    }

    writer.EndObject();
    writer.EndObject();
    writer.Flush();
}

std::string CDataFrameAnalysisSpecificationJsonWriter::jsonString(
    const std::string& jobId,
    std::size_t rows,
    std::size_t cols,
    std::size_t memoryLimit,
    std::size_t numberThreads,
    const std::string& missingFieldValue,
    const TStrVec& categoricalFields,
    bool diskUsageAllowed,
    const std::string& tempDir,
    const std::string& resultField,
    const std::string& analysisName,
    const std::string& analysisParameters) {

    std::ostringstream os;
    TBoostJsonLineWriter writer(os);

    write(jobId, rows, cols, memoryLimit, numberThreads, tempDir, resultField,
          missingFieldValue, categoricalFields, diskUsageAllowed, analysisName,
          analysisParameters, writer);

    return os.str();
}
}
}
