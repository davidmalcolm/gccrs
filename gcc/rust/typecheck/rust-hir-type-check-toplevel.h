// Copyright (C) 2020-2022 Free Software Foundation, Inc.

// This file is part of GCC.

// GCC is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3, or (at your option) any later
// version.

// GCC is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
// for more details.

// You should have received a copy of the GNU General Public License
// along with GCC; see the file COPYING3.  If not see
// <http://www.gnu.org/licenses/>.

#ifndef RUST_HIR_TYPE_CHECK_TOPLEVEL
#define RUST_HIR_TYPE_CHECK_TOPLEVEL

#include "rust-hir-type-check-base.h"
#include "rust-hir-full.h"
#include "rust-hir-type-check-implitem.h"
#include "rust-hir-type-check-type.h"
#include "rust-hir-type-check-expr.h"
#include "rust-hir-type-check-enumitem.h"
#include "rust-tyty.h"

namespace Rust {
namespace Resolver {

class TypeCheckTopLevel : public TypeCheckBase
{
  using Rust::Resolver::TypeCheckBase::visit;

public:
  static void Resolve (HIR::Item *item);

  void visit (HIR::TypeAlias &alias) override;
  void visit (HIR::TupleStruct &struct_decl) override;
  void visit (HIR::Module &module) override;
  void visit (HIR::StructStruct &struct_decl) override;
  void visit (HIR::Enum &enum_decl) override;
  void visit (HIR::Union &union_decl) override;
  void visit (HIR::StaticItem &var) override;
  void visit (HIR::ConstantItem &constant) override;
  void visit (HIR::Function &function) override;
  void visit (HIR::ImplBlock &impl_block) override;
  void visit (HIR::ExternBlock &extern_block) override;

private:
  TypeCheckTopLevel () : TypeCheckBase () {}

  void resolve_generic_params (
    const std::vector<std::unique_ptr<HIR::GenericParam>> &generic_params,
    std::vector<TyTy::SubstitutionParamMapping> &substitutions);
};

} // namespace Resolver
} // namespace Rust

#endif // RUST_HIR_TYPE_CHECK_TOPLEVEL
