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

#ifndef INCLUDED_ml_maths_common_CMultivariateOneOfNPrior_h
#define INCLUDED_ml_maths_common_CMultivariateOneOfNPrior_h

#include <core/CSmallVectorFwd.h>

#include <maths/common/CModelWeight.h>
#include <maths/common/CMultivariatePrior.h>
#include <maths/common/ImportExport.h>
#include <maths/common/MathsTypes.h>

#include <utility>
#include <vector>

namespace ml {
namespace core {
class CStatePersistInserter;
class CStateRestoreTraverser;
}

namespace maths {
namespace common {
struct SDistributionRestoreParams;

//! \brief Interface for a multivariate prior distribution which assumes data
//! are from one of N models.
//!
//! DESCRIPTION:\n
//! Implements a prior distribution which assumes all data is from one of N
//! models (subsequently referred to as component models).
//!
//! Each component model is assumed to have a prior which implements the
//! CMultivariatePrior interface. An object of this class thus comprises the
//! individual component prior objects plus a collection of weights, one per
//! component model (see addSamples for details). Each weight is proportional
//! to the probability that the sampled data comes from the corresponding
//! component models.
//!
//! IMPORTANT: Other than for testing, this class should not be constructed
//! directly. Creation of objects is managed by CMultivariateOneOfNPriorFactory.
//!
//! IMPLEMENTATION DECISIONS:\n
//! This is basically an implementation of the composite pattern for
//! CMultivariatePrior in a meaningful sense for the hierarchy. It holds
//! component models by pointer to the base class so that any prior in the
//! hierarchy can be mixed in. All component models are owned by the object
//! (it wouldn't make sense to share them) so this also defines the necessary
//! functions to support value semantics and manage the heap.
class MATHS_COMMON_EXPORT CMultivariateOneOfNPrior : public CMultivariatePrior {
public:
    using TDouble3Vec = core::CSmallVector<double, 3>;
    using TPriorPtrVec = std::vector<TPriorPtr>;
    using TDoublePriorPtrPr = std::pair<double, TPriorPtr>;
    using TDoublePriorPtrPrVec = std::vector<TDoublePriorPtrPr>;
    using TWeightPriorPtrPr = std::pair<CModelWeight, TPriorPtr>;
    using TWeightPriorPtrPrVec = std::vector<TWeightPriorPtrPr>;
    using TPriorCPtr3Vec = core::CSmallVector<const CMultivariatePrior*, 3>;
    using TMinAccumulator = CBasicStatistics::SMin<double>::TAccumulator;
    using TMaxAccumulator = CBasicStatistics::SMax<double>::TAccumulator;

    // Lift all overloads of into scope.
    //{
    using CMultivariatePrior::addSamples;
    using CMultivariatePrior::dataType;
    using CMultivariatePrior::decayRate;
    using CMultivariatePrior::print;
    using CMultivariatePrior::probabilityOfLessLikelySamples;
    //}

private:
    //! The maximum relative error we'll tolerate in c.d.f. and probability calculations.
    static const double MAXIMUM_RELATIVE_ERROR;
    //! The log of maximum relative error we'll tolerate in c.d.f. and probability
    //! calculations.
    static const double LOG_MAXIMUM_RELATIVE_ERROR;

public:
    //! \name Life-Cycle
    //@{
    //! Create with a collection of models.
    //!
    //! \param[in] dimension The model dimension.
    //! \param[in] models The simple models which comprise the mixed model.
    //! \param[in] dataType The type of data being modeled (see maths_t::EDataType
    //! for details).
    //! \param[in] decayRate The rate at which to revert to the non-informative prior.
    //! \warning This class takes ownership of \p models.
    CMultivariateOneOfNPrior(std::size_t dimension,
                             const TPriorPtrVec& models,
                             maths_t::EDataType dataType,
                             double decayRate = 0.0);

    //! Create with a weighted collection of models.
    //!
    //! \param[in] dimension The model dimension.
    //! \param[in] models The simple models and their weights which comprise
    //! the mixed model.
    //! \param[in] dataType The type of data being modeled (see maths_t::EDataType
    //! for details).
    //! \param[in] decayRate The rate at which we revert to the non-informative prior.
    //! \warning This class takes ownership of \p models.
    CMultivariateOneOfNPrior(std::size_t dimension,
                             const TDoublePriorPtrPrVec& models,
                             maths_t::EDataType dataType,
                             double decayRate = 0.0);

    //! Construct from part of a state document.
    CMultivariateOneOfNPrior(std::size_t dimension,
                             const SDistributionRestoreParams& params,
                             core::CStateRestoreTraverser& traverser);

    //! Implements value semantics for copy construction.
    CMultivariateOneOfNPrior(const CMultivariateOneOfNPrior& other);

    //! Implements value semantics for assignment.
    //!
    //! \param[in] rhs The mixed model to copy.
    //! \return The newly updated model.
    //! \note That this class has value semantics: this overwrites the current
    //! collection of models.
    CMultivariateOneOfNPrior& operator=(const CMultivariateOneOfNPrior& rhs);

    //! Efficient swap of the contents of this prior and \p other.
    void swap(CMultivariateOneOfNPrior& other);
    //@}

    //! \name Prior Contract
    //@{
    //! Create a copy of the prior.
    //!
    //! \return A pointer to a newly allocated clone of this model.
    //! \warning The caller owns the object returned.
    CMultivariateOneOfNPrior* clone() const override;

    //! Get the dimension of the prior.
    std::size_t dimension() const override;

    //! Set the data type.
    void dataType(maths_t::EDataType value) override;

    //! Set the rate at which the prior returns to non-informative.
    void decayRate(double value) override;

    //! Reset the prior to non-informative.
    void setToNonInformative(double offset = 0.0, double decayRate = 0.0) override;

    //! Forward the offset to the model priors.
    void adjustOffset(const TDouble10Vec1Vec& samples,
                      const TDouble10VecWeightsAry1Vec& weights) override;

    //! Update the model weights using the marginal likelihoods for
    //! the data. The component prior parameters are then updated.
    //!
    //! \param[in] samples A collection of samples of the process.
    //! \param[in] weights The weights of each sample in \p samples.
    void addSamples(const TDouble10Vec1Vec& samples,
                    const TDouble10VecWeightsAry1Vec& weights) override;

    //! Propagate the prior density function forwards by \p time.
    //!
    //! The prior distribution relaxes back to non-informative at a rate
    //! controlled by the decay rate parameter (optionally supplied to the
    //! constructor).
    //!
    //! \param[in] time The time increment to apply.
    //! \note \p time must be non negative.
    void propagateForwardsByTime(double time) override;

    //! Compute the univariate prior marginalizing over the variables
    //! \p marginalize and conditioning on the variables \p condition.
    //!
    //! \param[in] marginalize The variables to marginalize out.
    //! \param[in] condition The variables to condition on.
    //! \return The corresponding univariate prior or null if one couldn't
    //! be computed.
    //! \warning The caller owns the result.
    //! \note The variables are passed by the index of their dimension
    //! which must therefore be in range.
    //! \note The caller must specify dimension - 1 variables between
    //! \p marginalize and \p condition so the resulting distribution
    //! is univariate.
    TUnivariatePriorPtrDoublePr univariate(const TSize10Vec& marginalize,
                                           const TSizeDoublePr10Vec& condition) const override;

    //! Compute the bivariate prior marginalizing over the variables
    //! \p marginalize and conditioning on the variables \p condition.
    //!
    //! \param[in] marginalize The variables to marginalize out.
    //! \param[in] condition The variables to condition on.
    //! \warning The caller owns the result.
    //! \note The variables are passed by the index of their dimension
    //! which must therefore be in range.
    //! \note It is assumed that the variables are in sorted order.
    //! \note The caller must specify dimension - 2 variables between
    //! \p marginalize and \p condition so the resulting distribution
    //! is univariate.
    TPriorPtrDoublePr bivariate(const TSize10Vec& marginalize,
                                const TSizeDoublePr10Vec& condition) const override;

    //! Get the support for the marginal likelihood function.
    TDouble10VecDouble10VecPr marginalLikelihoodSupport() const override;

    //! Get the mean of the marginal likelihood function.
    TDouble10Vec marginalLikelihoodMean() const override;

    //! Get the weighted mean of the model nearest marginal likelihood means.
    TDouble10Vec nearestMarginalLikelihoodMean(const TDouble10Vec& value) const override;

    //! Get the covariance matrix for the marginal likelihood.
    TDouble10Vec10Vec marginalLikelihoodCovariance() const override;

    //! Get the diagonal of the covariance matrix for the marginal likelihood.
    TDouble10Vec marginalLikelihoodVariances() const override;

    //! Get the mode of the marginal likelihood function.
    TDouble10Vec marginalLikelihoodMode(const TDouble10VecWeightsAry& weights) const override;

    //! Compute the log marginal likelihood function at \p samples integrating
    //! over the prior density function for the distribution parameters.
    //!
    //! \param[in] samples A collection of samples of the process.
    //! \param[in] weights The weights of each sample in \p samples.
    //! \param[out] result Filled in with the joint likelihood of \p samples.
    //! \note The samples are assumed to be independent and identically
    //! distributed.
    maths_t::EFloatingPointErrorStatus
    jointLogMarginalLikelihood(const TDouble10Vec1Vec& samples,
                               const TDouble10VecWeightsAry1Vec& weights,
                               double& result) const override;

    //! Sample the marginal likelihood function.
    //!
    //! This samples each model in proportion to the probability the data
    //! come from that model. Since each model can only be sampled an integer
    //! number of times we find the sampling which minimizes the error from
    //! the ideal sampling.
    //!
    //! \param[in] numberSamples The number of samples required.
    //! \param[out] samples Filled in with samples from the prior.
    //! \note \p numberSamples is truncated to the number of samples received.
    void sampleMarginalLikelihood(std::size_t numberSamples,
                                  TDouble10Vec1Vec& samples) const override;

    //! Check if this is a non-informative prior.
    bool isNonInformative() const override;

    //! Get a human readable description of the prior.
    //!
    //! \param[in] separator String used to separate priors.
    //! \param[in,out] result Filled in with the description.
    void print(const std::string& separator, std::string& result) const override;

    //! Get a checksum for this object.
    std::uint64_t checksum(std::uint64_t seed = 0) const override;

    //! Debug the memory used by this component.
    void debugMemoryUsage(const core::CMemoryUsage::TMemoryUsagePtr& mem) const override;

    //! Get the memory used by this component.
    std::size_t memoryUsage() const override;

    //! Get the static size of this object - used for virtual hierarchies.
    std::size_t staticSize() const override;

    //! Get the tag name for this prior.
    std::string persistenceTag() const override;

    //! Persist state by passing information to the supplied inserter
    void acceptPersistInserter(core::CStatePersistInserter& inserter) const override;
    //@}

    //! \name Test Functions
    //@{
    //! Get the current values for the model weights.
    TDouble3Vec weights() const;

    //! Get the current values for the log model weights.
    TDouble3Vec logWeights() const;

    //! Get the current constituent models.
    TPriorCPtr3Vec models() const;
    //@}

private:
    //! Check that the model weights are valid.
    bool badWeights() const;

    //! Full debug dump of the model weights.
    std::string debugWeights() const;

private:
    //! The model dimension.
    std::size_t m_Dimension;

    //! A collection of component models and their probabilities.
    TWeightPriorPtrPrVec m_Models;
};
}
}
}

#endif // INCLUDED_ml_maths_common_CMultivariateOneOfNPrior_h
