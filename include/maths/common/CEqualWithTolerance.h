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

#ifndef INCLUDED_ml_maths_common_CEqualWithTolerance_h
#define INCLUDED_ml_maths_common_CEqualWithTolerance_h

#include <core/CLogger.h>

#include <maths/common/CLinearAlgebraFwd.h>

namespace ml {
namespace maths {
namespace common {
namespace equal_with_tolerance_detail {

template<typename T>
struct SNorm {
    static T dispatch(const T& t) { return t; }
};

template<typename T, std::size_t N>
struct SNorm<CVectorNx1<T, N>> {
    static T dispatch(const CVectorNx1<T, N>& t) { return t.euclidean(); }
};

template<typename T>
struct SNorm<CVector<T>> {
    static T dispatch(const CVector<T>& t) { return t.euclidean(); }
};

template<typename T, std::size_t N>
struct SNorm<CSymmetricMatrixNxN<T, N>> {
    static T dispatch(const CSymmetricMatrixNxN<T, N>& t) {
        return t.frobenius();
    }
};

template<typename T>
struct SNorm<CSymmetricMatrix<T>> {
    static T dispatch(const CSymmetricMatrix<T>& t) { return t.frobenius(); }
};
}

//! \brief The tolerance types for equal with tolerance.
class CToleranceTypes {
public:
    enum EToleranceType { E_AbsoluteTolerance = 03, E_RelativeTolerance = 06 };
};

//! \brief Comparator that can be used for determining equality to
//! within a given tolerance.
//!
//! DESCRIPTION:\n
//! Tests if two values are equal to specified tolerances. This can
//! test if |a - b| <= eps and/or is |a - b| <= eps * |max(a, b)|,
//! i.e. absolute and relative tolerances. Note that this is *not*
//! an equivalence relation since E(a, b) and E(a, c) doesn't imply
//! E(b, c) and so shouldn't be used with algorithms that expect one.
//!
//! This uses <= to handle the case that a = b which should always
//! evaluate as equal.
//!
//! IMPLEMENTATION DECISIONS:\n
//! This requires T to have operator<= and operator- and operator*
//! defined. Since operator* is only used for the relative comparison
//! we should ideally disable this check at compile time for types
//! which don't have operator*. However, our version of boost doesn't
//! have has_multiplies and so, short of writing this functionality
//! ourselves, we can't implement this.
template<typename T>
class CEqualWithTolerance : public CToleranceTypes {
public:
    CEqualWithTolerance(unsigned int toleranceType, const T& eps)
        : m_ToleranceType(toleranceType), m_AbsoluteEps(abs(norm(eps))),
          m_RelativeEps(abs(norm(eps))) {}

    CEqualWithTolerance(unsigned int toleranceType, const T& absoluteEps, const T& relativeEps)
        : m_ToleranceType(toleranceType), m_AbsoluteEps(abs(norm(absoluteEps))),
          m_RelativeEps(abs(norm(relativeEps))) {}

    bool operator()(const T& lhs, const T& rhs) const {
        const T& max = norm(rhs) > norm(lhs) ? rhs : lhs;
        const T& min = norm(rhs) > norm(lhs) ? lhs : rhs;
        const T& maxAbs = abs(norm(rhs)) > abs(norm(lhs)) ? rhs : lhs;

        T difference = max - min;

        switch (m_ToleranceType) {
        case 2: // absolute & relative
            return (norm(difference) <= m_AbsoluteEps) &&
                   (norm(difference) <= m_RelativeEps * abs(norm(maxAbs)));
        case 3: // absolute
            return norm(difference) <= m_AbsoluteEps;
        case 6: // relative
            return norm(difference) <= m_RelativeEps * abs(norm(maxAbs));
        case 7: // absolute | relative
            return (norm(difference) <= m_AbsoluteEps) ||
                   (norm(difference) <= m_RelativeEps * abs(norm(maxAbs)));
        }
        LOG_ERROR(<< "Unexpected tolerance type " << m_ToleranceType);
        return false;
    }

private:
    using TNorm =
        decltype(equal_with_tolerance_detail::SNorm<T>::dispatch(std::declval<T>()));

private:
    //! A type agnostic implementation of fabs.
    template<typename U>
    static inline U abs(const U& x) {
        return x < U(0) ? -x : x;
    }

    //! Get the norm of the specified type.
    static auto norm(const T& t) {
        return equal_with_tolerance_detail::SNorm<T>::dispatch(t);
    }

private:
    unsigned int m_ToleranceType;
    TNorm m_AbsoluteEps;
    TNorm m_RelativeEps;
};
}
}
}

#endif // INCLUDED_ml_maths_common_CEqualWithTolerance_h
