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

#include <api/CDataFrameTrainBoostedTreeRegressionRunner.h>

#include <core/CBoostJsonConcurrentLineWriter.h>
#include <core/CLogger.h>

#include <maths/analytics/CBoostedTree.h>
#include <maths/analytics/CBoostedTreeFactory.h>
#include <maths/analytics/CBoostedTreeHyperparameters.h>
#include <maths/analytics/CBoostedTreeLoss.h>
#include <maths/analytics/CDataFrameUtils.h>
#include <maths/analytics/CTreeShapFeatureImportance.h>

#include <api/CBoostedTreeInferenceModelBuilder.h>
#include <api/CDataFrameAnalysisConfigReader.h>
#include <api/CDataFrameAnalysisSpecification.h>
#include <api/ElasticsearchStateIndex.h>

#include <cmath>
#include <memory>
#include <set>
#include <string>

namespace ml {
namespace api {
namespace {
// Output
const std::string IS_TRAINING_FIELD_NAME{"is_training"};

const std::set<std::string> PREDICTION_FIELD_NAME_BLACKLIST{IS_TRAINING_FIELD_NAME};
}

const CDataFrameAnalysisConfigReader&
CDataFrameTrainBoostedTreeRegressionRunner::parameterReader() {
    static const CDataFrameAnalysisConfigReader PARAMETER_READER{[] {
        auto theReader = CDataFrameTrainBoostedTreeRunner::parameterReader();
        theReader.addParameter(STRATIFIED_CROSS_VALIDATION,
                               CDataFrameAnalysisConfigReader::E_OptionalParameter);
        theReader.addParameter(
            LOSS_FUNCTION, CDataFrameAnalysisConfigReader::E_OptionalParameter,
            {{MSE, int{TLossFunctionType::E_MseRegression}},
             {MSLE, int{TLossFunctionType::E_MsleRegression}},
             {PSEUDO_HUBER, int{TLossFunctionType::E_HuberRegression}}});
        theReader.addParameter(LOSS_FUNCTION_PARAMETER,
                               CDataFrameAnalysisConfigReader::E_OptionalParameter);
        return theReader;
    }()};
    return PARAMETER_READER;
}

CDataFrameTrainBoostedTreeRegressionRunner::TLossFunctionUPtr
CDataFrameTrainBoostedTreeRegressionRunner::lossFunction(const CDataFrameAnalysisParameters& parameters) {
    TLossFunctionType lossFunctionType{
        parameters[LOSS_FUNCTION].fallback(TLossFunctionType::E_MseRegression)};
    switch (lossFunctionType) {
    case TLossFunctionType::E_MsleRegression:
        return std::make_unique<maths::analytics::boosted_tree::CMsle>(
            parameters[LOSS_FUNCTION_PARAMETER].fallback(1.0));
    case TLossFunctionType::E_MseRegression:
        return std::make_unique<maths::analytics::boosted_tree::CMse>();
    case TLossFunctionType::E_HuberRegression:
        return std::make_unique<maths::analytics::boosted_tree::CPseudoHuber>(
            parameters[LOSS_FUNCTION_PARAMETER].fallback(1.0));
    case TLossFunctionType::E_BinaryClassification:
    case TLossFunctionType::E_MulticlassClassification:
        LOG_ERROR(<< "Input error: regression loss type is expected but classification type is provided. Defaulting to MSE instead.");
        return std::make_unique<maths::analytics::boosted_tree::CMse>();
    }
    return nullptr;
}

CDataFrameTrainBoostedTreeRegressionRunner::CDataFrameTrainBoostedTreeRegressionRunner(
    const CDataFrameAnalysisSpecification& spec,
    const CDataFrameAnalysisParameters& parameters,
    TDataFrameUPtrTemporaryDirectoryPtrPr* frameAndDirectory)
    : CDataFrameTrainBoostedTreeRunner{spec, parameters, lossFunction(parameters),
                                       frameAndDirectory} {

    this->boostedTreeFactory().stratifyRegressionCrossValidation(
        parameters[STRATIFIED_CROSS_VALIDATION].fallback(true));

    const TStrVec& categoricalFieldNames{spec.categoricalFieldNames()};
    if (std::find(categoricalFieldNames.begin(), categoricalFieldNames.end(),
                  this->dependentVariableFieldName()) != categoricalFieldNames.end()) {
        HANDLE_FATAL(<< "Input error: trying to perform regression with categorical target.");
    }
    if (PREDICTION_FIELD_NAME_BLACKLIST.count(this->predictionFieldName()) > 0) {
        HANDLE_FATAL(<< "Input error: " << PREDICTION_FIELD_NAME << " must not be equal to any of "
                     << PREDICTION_FIELD_NAME_BLACKLIST << ".");
    }
}

void CDataFrameTrainBoostedTreeRegressionRunner::writeOneRow(
    const core::CDataFrame&,
    const TRowRef& row,
    core::CBoostJsonConcurrentLineWriter& writer) const {

    const auto& tree = this->boostedTree();
    const std::size_t columnHoldingDependentVariable{tree.columnHoldingDependentVariable()};

    writer.onObjectBegin();
    writer.onKey(this->predictionFieldName());
    writer.onDouble(tree.prediction(row)[0]);
    writer.onKey(IS_TRAINING_FIELD_NAME);
    writer.onBool(maths::analytics::CDataFrameUtils::isMissing(
                      row[columnHoldingDependentVariable]) == false);
    auto* featureImportance = tree.shap();
    if (featureImportance != nullptr) {
        m_InferenceModelMetadata.columnNames(featureImportance->columnNames());
        featureImportance->shap(
            row, [&writer, this](
                     const maths::analytics::CTreeShapFeatureImportance::TSizeVec& indices,
                     const TStrVec& featureNames,
                     const maths::analytics::CTreeShapFeatureImportance::TVectorVec& shap) {
                writer.onKey(FEATURE_IMPORTANCE_FIELD_NAME);
                writer.onArrayBegin();
                for (auto i : indices) {
                    if (shap[i].norm() != 0.0) {
                        writer.onObjectBegin();
                        writer.onKey(FEATURE_NAME_FIELD_NAME);
                        writer.onString(featureNames[i]);
                        writer.onKey(IMPORTANCE_FIELD_NAME);
                        writer.onDouble(shap[i](0));
                        writer.onObjectEnd();
                    }
                }
                writer.onArrayEnd();

                for (int i = 0; i < static_cast<int>(shap.size()); ++i) {
                    if (shap[i].lpNorm<1>() != 0) {
                        const_cast<CDataFrameTrainBoostedTreeRegressionRunner*>(this)
                            ->m_InferenceModelMetadata.addToFeatureImportance(i, shap[i]);
                    }
                }
            });
    }
    writer.onObjectEnd();
}

void CDataFrameTrainBoostedTreeRegressionRunner::validate(const core::CDataFrame&,
                                                          std::size_t) const {
}

CDataFrameAnalysisRunner::TInferenceModelDefinitionUPtr
CDataFrameTrainBoostedTreeRegressionRunner::inferenceModelDefinition(
    const CDataFrameAnalysisRunner::TStrVec& fieldNames,
    const CDataFrameAnalysisRunner::TStrVecVec& categoryNames) const {
    CRegressionInferenceModelBuilder builder(
        fieldNames, this->boostedTree().columnHoldingDependentVariable(), categoryNames);
    this->accept(builder);
    return std::make_unique<CInferenceModelDefinition>(builder.build());
}

const CInferenceModelMetadata*
CDataFrameTrainBoostedTreeRegressionRunner::inferenceModelMetadata() const {
    auto* featureImportance = this->boostedTree().shap();
    if (featureImportance != nullptr) {
        m_InferenceModelMetadata.featureImportanceBaseline(featureImportance->baseline());
    }

    switch (this->task()) {
    case api_t::E_Encode:
    case api_t::E_Predict:
        break;
    case api_t::E_Train:
    case api_t::E_Update:
        m_InferenceModelMetadata.hyperparameterImportance(
            this->boostedTree().hyperparameterImportance());
        break;
    }

    m_InferenceModelMetadata.numTrainRows(this->boostedTree().numberTrainRows());
    m_InferenceModelMetadata.lossGap(this->boostedTree().lossGap());
    m_InferenceModelMetadata.numDataSummarizationRows(static_cast<std::size_t>(
        this->boostedTree().dataSummarization().manhattan()));
    return &m_InferenceModelMetadata;
}

// clang-format off
const std::string CDataFrameTrainBoostedTreeRegressionRunner::STRATIFIED_CROSS_VALIDATION{"stratified_cross_validation"};
const std::string CDataFrameTrainBoostedTreeRegressionRunner::LOSS_FUNCTION{"loss_function"};
const std::string CDataFrameTrainBoostedTreeRegressionRunner::LOSS_FUNCTION_PARAMETER{"loss_function_parameter"};
const std::string CDataFrameTrainBoostedTreeRegressionRunner::MSE{"mse"};
const std::string CDataFrameTrainBoostedTreeRegressionRunner::MSLE{"msle"};
const std::string CDataFrameTrainBoostedTreeRegressionRunner::PSEUDO_HUBER{"huber"};
// clang-format on

const std::string& CDataFrameTrainBoostedTreeRegressionRunnerFactory::name() const {
    return NAME;
}

CDataFrameTrainBoostedTreeRegressionRunnerFactory::TRunnerUPtr
CDataFrameTrainBoostedTreeRegressionRunnerFactory::makeImpl(
    const CDataFrameAnalysisSpecification&,
    TDataFrameUPtrTemporaryDirectoryPtrPr*) const {
    HANDLE_FATAL(<< "Input error: regression has a non-optional parameter '"
                 << CDataFrameTrainBoostedTreeRunner::DEPENDENT_VARIABLE_NAME << "'.");
    return nullptr;
}

CDataFrameTrainBoostedTreeRegressionRunnerFactory::TRunnerUPtr
CDataFrameTrainBoostedTreeRegressionRunnerFactory::makeImpl(
    const CDataFrameAnalysisSpecification& spec,
    const json::value& jsonParameters,
    TDataFrameUPtrTemporaryDirectoryPtrPr* frameAndDirectory) const {
    const CDataFrameAnalysisConfigReader& parameterReader{
        CDataFrameTrainBoostedTreeRegressionRunner::parameterReader()};
    auto parameters = parameterReader.read(jsonParameters);
    return std::make_unique<CDataFrameTrainBoostedTreeRegressionRunner>(
        spec, parameters, frameAndDirectory);
}

const std::string CDataFrameTrainBoostedTreeRegressionRunnerFactory::NAME{"regression"};
}
}
