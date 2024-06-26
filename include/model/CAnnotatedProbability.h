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

#ifndef INCLUDED_ml_model_CAnnotatedProbability_h
#define INCLUDED_ml_model_CAnnotatedProbability_h

#include <core/CSmallVector.h>

#include <maths/common/CModel.h>

#include <model/ImportExport.h>
#include <model/ModelTypes.h>

#include <optional>
#include <utility>
#include <vector>

namespace ml {
namespace core {
class CStatePersistInserter;
class CStateRestoreTraverser;
}

namespace model {

//! \brief A collection of data describing an attribute's probability.
struct MODEL_EXPORT SAttributeProbability {
    using TDouble1Vec = core::CSmallVector<double, 1>;
    using TSizeDoublePr = std::pair<std::size_t, double>;
    using TSizeDoublePr1Vec = core::CSmallVector<TSizeDoublePr, 1>;
    using TOptionalStr = std::optional<std::string>;
    using TOptionalStr1Vec = core::CSmallVector<TOptionalStr, 1>;

    SAttributeProbability();
    SAttributeProbability(std::size_t cid,
                          const TOptionalStr& attribute,
                          double probability,
                          model_t::CResultType type,
                          model_t::EFeature feature,
                          const TOptionalStr1Vec& correlatedAttributes,
                          const TSizeDoublePr1Vec& correlated);

    //! Total ordering of attribute probabilities by probability
    //! breaking ties using the attribute and finally the feature.
    bool operator<(const SAttributeProbability& other) const;

    //! Persist the probability passing information to \p inserter.
    void acceptPersistInserter(core::CStatePersistInserter& inserter) const;

    //! Restore the probability reading state from \p traverser.
    bool acceptRestoreTraverser(core::CStateRestoreTraverser& traverser);

    //! The attribute identifier.
    std::size_t s_Cid;
    //! The attribute.
    TOptionalStr s_Attribute;
    //! The attribute probability.
    double s_Probability;
    //! The type of result (see CResultType for details).
    model_t::CResultType s_Type;
    //! The most unusual feature of the attribute.
    model_t::EFeature s_Feature;
    //! The correlated attributes.
    TOptionalStr1Vec s_CorrelatedAttributes;
    //! The correlated attribute identifiers (if any).
    TSizeDoublePr1Vec s_Correlated;
    //! The current bucket value of the attribute (cached from the model).
    mutable TDouble1Vec s_CurrentBucketValue;
    //! The population mean (cached from the model).
    mutable TDouble1Vec s_BaselineBucketMean;
};

//! \brief A collection of data describing the result of a model
//! probability calculation.
//!
//! DESCRIPTION:\n
//! This includes all associated data such as a set of the smallest
//! attribute probabilities, the influences, extra descriptive data
//! and so on.
struct MODEL_EXPORT SAnnotatedProbability {
    using TAttributeProbability1Vec = core::CSmallVector<SAttributeProbability, 1>;
    using TOptionalStr = std::optional<std::string>;
    using TOptionalStrOptionalStrPr = std::pair<TOptionalStr, TOptionalStr>;
    using TOptionalStrOptionalStrPrDoublePr = std::pair<TOptionalStrOptionalStrPr, double>;
    using TOptionalStrOptionalStrPrDoublePrVec = std::vector<TOptionalStrOptionalStrPrDoublePr>;
    using TOptionalDouble = std::optional<double>;
    using TOptionalUInt64 = std::optional<std::uint64_t>;
    using TAnomalyScoreExplanation = maths::common::SAnomalyScoreExplanation;

    SAnnotatedProbability();
    SAnnotatedProbability(double p);

    //! Efficiently swap the contents of this and \p other.
    void swap(SAnnotatedProbability& other) noexcept;

    //! Is the result type interim?
    bool isInterim() const;

    //! Persist the probability passing information to \p inserter.
    void acceptPersistInserter(core::CStatePersistInserter& inserter) const;

    //! Restore the probability reading state from \p traverser.
    bool acceptRestoreTraverser(core::CStateRestoreTraverser& traverser);

    //! The probability of seeing the series' sample in a time interval.
    double s_Probability;

    //! The impact of multi/single bucket analysis on the probability
    double s_MultiBucketImpact;

    //! The smallest attribute probabilities and associated data describing
    //! the calculation.
    TAttributeProbability1Vec s_AttributeProbabilities;

    //! The field values which influence this probability.
    TOptionalStrOptionalStrPrDoublePrVec s_Influences;

    //! The result type (interim or final)
    //!
    //! This field is transient - does not get persisted because interim results
    //! never get persisted.
    model_t::CResultType s_ResultType;

    //! The current bucket count for this probability (cached from the model).
    TOptionalUInt64 s_CurrentBucketCount;

    //! The baseline bucket count for this probability (cached from the model).
    TOptionalDouble s_BaselineBucketCount;

    //! Should the quantiles be updated?
    bool s_ShouldUpdateQuantiles{true};

    //! Factors impacting the anomaly score.
    TAnomalyScoreExplanation s_AnomalyScoreExplanation;
};
}
}

#endif // INCLUDED_ml_model_CAnnotatedProbability_h
