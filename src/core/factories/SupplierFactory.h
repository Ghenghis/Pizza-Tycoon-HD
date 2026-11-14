/*
 * SPDX-FileCopyrightText: 2025 Cowomaly <cowomaly@nullium.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "core/supply/Supplier.h"

#include <memory>
#include <unordered_map>
#include <vector>

// The table is filled whole the first time it is needed and never changes
// after, so a game being built on a worker reads it without locks.
class SupplierFactory {
public:
  static const Supplier* create_supplier(SupplierID);
  static std::vector<const Supplier*> all_suppliers();
  static std::vector<const Supplier*> all_suppliers_by_type(SupplierType,
                                                            int current_year);
};
