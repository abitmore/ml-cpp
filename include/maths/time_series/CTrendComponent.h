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

#ifndef INCLUDED_ml_maths_time_series_CTrendComponent_h
#define INCLUDED_ml_maths_time_series_CTrendComponent_h

#include <core/CoreTypes.h>

#include <maths/common/CBasicStatistics.h>
#include <maths/common/CLeastSquaresOnlineRegression.h>
#include <maths/common/CLinearAlgebraFwd.h>
#include <maths/common/CNaiveBayes.h>
#include <maths/common/CNormalMeanPrecConjugate.h>
#include <maths/common/CPRNG.h>
#include <maths/common/MathsTypes.h>

#include <maths/time_series/ImportExport.h>

#include <vector>

namespace ml {
namespace maths {
namespace common {
struct SDistributionRestoreParams;
}
namespace time_series {

//! \brief Models the trend component of a time series.
//!
//! DESCRIPTION:\n
//! This is an ensemble of trend models fitted over different time scales.
//! In particular, we age data at different rates from each of the models.
//! A prediction is then a weighted average of the different models. We
//! adjust the weighting of components based on the difference in their
//! decay rate and the target decay rate.
//!
//! The key advantage of this approach is that we can also adjust the
//! weighting over a forecast based on how far ahead we are predicting.
//! This means at each time scale we can revert to the trend for that time
//! scale. It also allows us to accurately estimate confidence intervals
//! (since these can be estimated from the variation of observed values
//! we see w.r.t. the predictions from the next longer time scale component).
//! This produces plausible looking forecasts and this sort of mean reversion
//! is common in many real world time series.
class MATHS_TIME_SERIES_EXPORT CTrendComponent {
public:
    using TDoubleVec = std::vector<double>;
    using TDouble3Vec = core::CSmallVector<double, 3>;
    using TSizeVec = std::vector<std::size_t>;
    using TVector2x1 = common::CVectorNx1<double, 2>;
    using TVector3x1 = common::CVectorNx1<double, 3>;
    using TMatrix3x3 = common::CSymmetricMatrixNxN<double, 3>;
    using TMatrix3x3Vec = std::vector<TMatrix3x3>;
    using TFloatMeanAccumulator =
        common::CBasicStatistics::SSampleMean<common::CFloatStorage>::TAccumulator;
    using TFloatMeanAccumulatorVec = std::vector<TFloatMeanAccumulator>;
    using TPredictor = std::function<double(core_t::TTime)>;
    using TSeasonalForecast = std::function<TDouble3Vec(core_t::TTime)>;
    using TWriteForecastResult = std::function<void(core_t::TTime, const TDouble3Vec&)>;

public:
    explicit CTrendComponent(double decayRate);

    //! Efficiently swap the state of this and \p other.
    void swap(CTrendComponent& other);

    //! Persist state by passing information to \p inserter.
    void acceptPersistInserter(core::CStatePersistInserter& inserter) const;

    //! Initialize by reading state from \p traverser.
    bool acceptRestoreTraverser(const common::SDistributionRestoreParams& params,
                                core::CStateRestoreTraverser& traverser);

    //!  Check if the trend has been estimated.
    bool initialized() const;

    //! Clear all data.
    void clear();

    //! Shift the regression models' time origins to \p time.
    void shiftOrigin(core_t::TTime time);

    //! Shift the slope of the regression models' keeping the prediction
    //! at \p time fixed.
    void shiftSlope(core_t::TTime time, double shift);

    //! Apply a level shift of \p shift.
    //!
    //! \note This bypasses the shift model.
    void shiftLevel(double shift);

    //! Apply a level shift of \p shift.
    //!
    //! \param[in] shift The shift to apply.
    //! \param[in] valuesStartTime The start time of \p values.
    //! \param[in] bucketLength The bucket length of \p values.
    //! \param[in] values The values used to test for a shift.
    //! \param[in] segments The constant shift segments endpoints of \p values.
    //! \param[in] shifts The shifts for each segment of \p values.
    void shiftLevel(double shift,
                    core_t::TTime valuesStartTime,
                    core_t::TTime bucketLength,
                    const TFloatMeanAccumulatorVec& values,
                    const TSizeVec& segments,
                    const TDoubleVec& shifts);

    //! Apply no level shift at \p time and \p value.
    //!
    //! This updates the model for the probability of a level shift.
    void dontShiftLevel(core_t::TTime time, double value);

    //! Apply a linear scale by \p scale.
    void linearScale(core_t::TTime time, double scale);

    //! Adds a value \f$(t, f(t))\f$ to this component.
    //!
    //! \param[in] time The time of the point.
    //! \param[in] value The value at \p time.
    //! \param[in] weight The weight of \p value. The smaller this is the
    //! less influence it has on the component.
    void add(core_t::TTime time, double value, double weight = 1.0);

    //! Set the data type.
    void dataType(maths_t::EDataType dataType);

    //! Get the base rate at which models lose information.
    double defaultDecayRate() const;

    //! Set the rate base rate at which models lose information.
    void decayRate(double decayRate);

    //! Age the trend to account for \p interval elapsed time.
    void propagateForwardsByTime(core_t::TTime interval);

    //! Get the predicted value at \p time.
    //!
    //! \param[in] time The time of interest.
    //! \param[in] confidence The symmetric confidence interval for the variance
    //! as a percentage.
    TVector2x1 value(core_t::TTime time, double confidence) const;

    //! Get a function which returns the trend value as a function of time.
    //!
    //! This caches the expensive part of the calculation and so is much faster
    //! than repeatedly calling value.
    //!
    //! \warning This can only be used as long as the trend component isn't updated.
    TPredictor predictor() const;

    //! Get the variance of the residual about the predicted value at \p time.
    //!
    //! \param[in] confidence The symmetric confidence interval for the
    //! variance as a percentage.
    TVector2x1 variance(double confidence) const;

    //! Get the maximum interval for which the trend model can be forecast.
    core_t::TTime maximumForecastInterval() const;

    //! Forecast the trend model from \p startTime to \p endTime.
    //!
    //! \param[in] startTime The start time of the forecast interval.
    //! \param[in] endTime The end time of the forecast interval.
    //! \param[in] step The time step.
    //! \param[in] confidence The confidence interval to calculate.
    //! \param[in] isNonNegative True if the data being modelled are known to be
    //! non-negative.
    //! \param[in] seasonal Forecasts seasonal components.
    //! \param[in] writer Writes out forecast results.
    void forecast(core_t::TTime startTime,
                  core_t::TTime endTime,
                  core_t::TTime step,
                  double confidence,
                  bool isNonNegative,
                  const TSeasonalForecast& seasonal,
                  const TWriteForecastResult& writer) const;

    //! Get the interval which has been observed so far.
    core_t::TTime observedInterval() const;

    //! Get the number of parameters used to describe the trend.
    double parameters() const;

    //! Get a checksum for this object.
    std::uint64_t checksum(std::uint64_t seed = 0) const;

    //! Get a debug description of this object.
    std::string print() const;

private:
    using TRegression = common::CLeastSquaresOnlineRegression<2, double>;
    using TRegressionArray = TRegression::TArray;
    using TRegressionArrayVec = std::vector<TRegressionArray>;
    using TMeanAccumulator = common::CBasicStatistics::SSampleMean<double>::TAccumulator;
    using TVector3x1MeanAccumulator =
        common::CBasicStatistics::SSampleMean<TVector3x1>::TAccumulator;
    using TMeanVarAccumulator = common::CBasicStatistics::SSampleMeanVar<double>::TAccumulator;

    //! \brief A model of the trend at a specific time scale.
    struct SModel {
        explicit SModel(double weight);
        void acceptPersistInserter(core::CStatePersistInserter& inserter) const;
        bool acceptRestoreTraverser(core::CStateRestoreTraverser& traverser);
        std::uint64_t checksum(std::uint64_t seed) const;
        TMeanAccumulator s_Weight;
        TRegression s_Regression;
        TVector3x1MeanAccumulator s_Mse;
    };
    using TModelVec = std::vector<SModel>;

    //! \brief Forecasts the level model by path roll out.
    class CForecastLevel : private core::CNonCopyable {
    public:
        //! The default number of roll out paths to use.
        static const std::size_t DEFAULT_NUMBER_PATHS{100};

    public:
        CForecastLevel(const common::CNaiveBayes& probability,
                       const common::CNormalMeanPrecConjugate& magnitude,
                       core_t::TTime timeOfLastChange,
                       std::size_t numberPaths = DEFAULT_NUMBER_PATHS);

        //! Forecast the time series level at \p time.
        TDouble3Vec forecast(core_t::TTime time, double prediction, double confidence);

    private:
        using TTimeVec = std::vector<core_t::TTime>;

    private:
        //! The model of the change probability.
        const common::CNaiveBayes& m_Probability;
        //! The model of the change magnitude.
        const common::CNormalMeanPrecConjugate& m_Magnitude;
        //! A random number generator for generating roll outs.
        common::CPRNG::CXorOShiro128Plus m_Rng;
        //! The current roll outs forecasted levels.
        TDoubleVec m_Levels;
        //! The current roll outs times of last change.
        TTimeVec m_TimesOfLastChange;
        //! Maintains the current bucket probability of change.
        TDoubleVec m_ProbabilitiesOfChange;
        //! Place holder for sampling.
        TDoubleVec m_Uniform01;
    };

private:
    //! Get the smoothing factors for the different regression models.
    void smoothingFactors(core_t::TTime interval, TDoubleVec& result) const;

    //! Select the most complex model for which there is significant evidence.
    TSizeVec selectModelOrdersForForecasting() const;

    //! Get the initial model weights to use for forecasting.
    TDoubleVec initialForecastModelWeights(std::size_t n) const;

    //! Get the mean count of samples in the prediction.
    double count() const;

    //! Get the predicted value at \p time.
    double value(const TDoubleVec& weights, const TRegressionArrayVec& models, double time) const;

    //! Get the weight to assign to the prediction verses the long term mean.
    double weightOfPrediction(core_t::TTime time) const;

    //! Check the state invariants after restoration
    //! Abort on failure.
    void checkRestoredInvariants() const;

private:
    //! The default rate at which information is aged out of the trend models.
    double m_DefaultDecayRate;

    //! The target rate at which information is aged out of the ensemble.
    double m_TargetDecayRate;

    //! The time the model was first updated.
    core_t::TTime m_FirstUpdate;
    //! The time the model was last updated.
    core_t::TTime m_LastUpdate;

    //! The start time of the regression models.
    core_t::TTime m_RegressionOrigin;
    //! The regression models (we have them for multiple time scales).
    TModelVec m_TrendModels;
    //! The variance of the prediction errors.
    double m_PredictionErrorVariance;
    //! The mean and variance of the values added to the trend component.
    TMeanVarAccumulator m_ValueMoments;

    //! The time of the last level change.
    core_t::TTime m_TimeOfLastLevelChange;
    //! A model of probability of level changes for the trend.
    common::CNaiveBayes m_ProbabilityOfLevelChangeModel;
    //! A model of magnitude of level changes for the trend.
    common::CNormalMeanPrecConjugate m_MagnitudeOfLevelChangeModel;
};
}
}
}

#endif // INCLUDED_ml_maths_time_series_CTrendComponent_h
