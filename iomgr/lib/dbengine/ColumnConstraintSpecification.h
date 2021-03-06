// Copyright (C) 2019-2020 Siodb GmbH. All rights reserved.
// Use of this source code is governed by a license that can be found
// in the LICENSE file.

#pragma once

// Project headers
#include "ConstraintType.h"
#include "parser/expr/Expression.h"

namespace siodb::iomgr::dbengine {

/** Specification of the constraint that should be applied to a column. */
struct ColumnConstraintSpecification {
    /** Initializes object of class ColumnConstraintSpecification. */
    ColumnConstraintSpecification() noexcept
        : m_type(ConstraintType::kMax)
    {
    }

    /**
     * Initializes object of class ColumnConstraintSpecification.
     * @param name Constraint name.
     * @param type Constraint type.
     * @param expression Constraint expression.
     */
    ColumnConstraintSpecification(
            std::string&& name, ConstraintType type, requests::ExpressionPtr&& expression)
        : m_name(std::move(name))
        , m_type(type)
        , m_expression(std::move(expression))
    {
    }

    /**
     * Initializes object of class ColumnConstraintSpecification from existing object.
     * @param src Source object.
     */
    explicit ColumnConstraintSpecification(const ColumnConstraintSpecification& src)
        : m_name(src.m_name)
        , m_type(src.m_type)
        , m_expression(src.m_expression->clone())
    {
    }

    /** Desired constraint name. Empty name will cause automatic name generatrion. */
    std::string m_name;

    /** Constraint type */
    ConstraintType m_type;

    /** Constraint expression */
    requests::ExpressionPtr m_expression;
};

/** Column constraint specification list */
using ColumnConstraintSpecificationList = std::vector<ColumnConstraintSpecification>;

}  // namespace siodb::iomgr::dbengine
