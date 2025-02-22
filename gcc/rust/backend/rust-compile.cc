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

#include "rust-hir-trait-resolve.h"
#include "rust-hir-path-probe.h"
#include "rust-hir-type-bounds.h"
#include "rust-hir-dot-operator.h"
#include "rust-compile.h"
#include "rust-compile-item.h"
#include "rust-compile-implitem.h"
#include "rust-compile-expr.h"
#include "rust-compile-struct-field-expr.h"
#include "rust-compile-stmt.h"

namespace Rust {
namespace Compile {

CompileCrate::CompileCrate (HIR::Crate &crate, Context *ctx)
  : crate (crate), ctx (ctx)
{}

CompileCrate::~CompileCrate () {}

void
CompileCrate::Compile (HIR::Crate &crate, Context *ctx)
{
  CompileCrate c (crate, ctx);
  c.go ();
}

void
CompileCrate::go ()
{
  for (auto &item : crate.items)
    CompileItem::compile (item.get (), ctx);
}

// rust-compile-block.h

void
CompileBlock::visit (HIR::BlockExpr &expr)
{
  fncontext fnctx = ctx->peek_fn ();
  tree fndecl = fnctx.fndecl;
  Location start_location = expr.get_locus ();
  Location end_location = expr.get_end_locus ();
  auto body_mappings = expr.get_mappings ();

  Resolver::Rib *rib = nullptr;
  if (!ctx->get_resolver ()->find_name_rib (body_mappings.get_nodeid (), &rib))
    {
      rust_fatal_error (expr.get_locus (), "failed to setup locals per block");
      return;
    }

  std::vector<Bvariable *> locals
    = compile_locals_for_block (ctx, *rib, fndecl);

  tree enclosing_scope = ctx->peek_enclosing_scope ();
  tree new_block = ctx->get_backend ()->block (fndecl, enclosing_scope, locals,
					       start_location, end_location);
  ctx->push_block (new_block);

  for (auto &s : expr.get_statements ())
    {
      auto compiled_expr = CompileStmt::Compile (s.get (), ctx);
      if (compiled_expr != nullptr)
	{
	  tree s = convert_to_void (compiled_expr, ICV_STATEMENT);
	  ctx->add_statement (s);
	}
    }

  if (expr.has_expr ())
    {
      // the previous passes will ensure this is a valid return or
      // a valid trailing expression
      tree compiled_expr = CompileExpr::Compile (expr.expr.get (), ctx);
      if (compiled_expr != nullptr)
	{
	  if (result == nullptr)
	    {
	      ctx->add_statement (compiled_expr);
	    }
	  else
	    {
	      tree result_reference = ctx->get_backend ()->var_expression (
		result, expr.get_final_expr ()->get_locus ());

	      tree assignment
		= ctx->get_backend ()->assignment_statement (result_reference,
							     compiled_expr,
							     expr.get_locus ());
	      ctx->add_statement (assignment);
	    }
	}
    }

  ctx->pop_block ();
  translated = new_block;
}

void
CompileConditionalBlocks::visit (HIR::IfExpr &expr)
{
  fncontext fnctx = ctx->peek_fn ();
  tree fndecl = fnctx.fndecl;
  tree condition_expr = CompileExpr::Compile (expr.get_if_condition (), ctx);
  tree then_block = CompileBlock::compile (expr.get_if_block (), ctx, result);

  translated
    = ctx->get_backend ()->if_statement (fndecl, condition_expr, then_block,
					 NULL, expr.get_locus ());
}

void
CompileConditionalBlocks::visit (HIR::IfExprConseqElse &expr)
{
  fncontext fnctx = ctx->peek_fn ();
  tree fndecl = fnctx.fndecl;
  tree condition_expr = CompileExpr::Compile (expr.get_if_condition (), ctx);
  tree then_block = CompileBlock::compile (expr.get_if_block (), ctx, result);
  tree else_block = CompileBlock::compile (expr.get_else_block (), ctx, result);

  translated
    = ctx->get_backend ()->if_statement (fndecl, condition_expr, then_block,
					 else_block, expr.get_locus ());
}

void
CompileConditionalBlocks::visit (HIR::IfExprConseqIf &expr)
{
  fncontext fnctx = ctx->peek_fn ();
  tree fndecl = fnctx.fndecl;
  tree condition_expr = CompileExpr::Compile (expr.get_if_condition (), ctx);
  tree then_block = CompileBlock::compile (expr.get_if_block (), ctx, result);

  // else block
  std::vector<Bvariable *> locals;
  Location start_location = expr.get_conseq_if_expr ()->get_locus ();
  Location end_location = expr.get_conseq_if_expr ()->get_locus (); // FIXME
  tree enclosing_scope = ctx->peek_enclosing_scope ();
  tree else_block = ctx->get_backend ()->block (fndecl, enclosing_scope, locals,
						start_location, end_location);
  ctx->push_block (else_block);

  tree else_stmt_decl
    = CompileConditionalBlocks::compile (expr.get_conseq_if_expr (), ctx,
					 result);
  ctx->add_statement (else_stmt_decl);

  ctx->pop_block ();

  translated
    = ctx->get_backend ()->if_statement (fndecl, condition_expr, then_block,
					 else_block, expr.get_locus ());
}

// rust-compile-struct-field-expr.h

void
CompileStructExprField::visit (HIR::StructExprFieldIdentifierValue &field)
{
  translated = CompileExpr::Compile (field.get_value (), ctx);
}

void
CompileStructExprField::visit (HIR::StructExprFieldIndexValue &field)
{
  translated = CompileExpr::Compile (field.get_value (), ctx);
}

void
CompileStructExprField::visit (HIR::StructExprFieldIdentifier &field)
{
  // we can make the field look like an identifier expr to take advantage of
  // existing code
  HIR::IdentifierExpr expr (field.get_mappings (), field.get_field_name (),
			    field.get_locus ());
  translated = CompileExpr::Compile (&expr, ctx);
}

// Shared methods in compilation

tree
HIRCompileBase::coercion_site (tree rvalue, const TyTy::BaseType *rval,
			       const TyTy::BaseType *lval,
			       Location lvalue_locus, Location rvalue_locus)
{
  if (rvalue == error_mark_node)
    return error_mark_node;

  const TyTy::BaseType *actual = rval->destructure ();
  const TyTy::BaseType *expected = lval->destructure ();

  if (expected->get_kind () == TyTy::TypeKind::REF)
    {
      // this is a dyn object
      if (SLICE_TYPE_P (TREE_TYPE (rvalue)))
	{
	  return rvalue;
	}

      // bad coercion... of something to a reference
      if (actual->get_kind () != TyTy::TypeKind::REF)
	return error_mark_node;

      const TyTy::ReferenceType *exp
	= static_cast<const TyTy::ReferenceType *> (expected);
      const TyTy::ReferenceType *act
	= static_cast<const TyTy::ReferenceType *> (actual);

      tree expected_type = TyTyResolveCompile::compile (ctx, act->get_base ());
      tree deref_rvalue
	= ctx->get_backend ()->indirect_expression (expected_type, rvalue,
						    false /*known_valid*/,
						    rvalue_locus);
      tree coerced
	= coercion_site (deref_rvalue, act->get_base (), exp->get_base (),
			 lvalue_locus, rvalue_locus);
      if (exp->is_dyn_object () && SLICE_TYPE_P (TREE_TYPE (coerced)))
	return coerced;

      return address_expression (coerced,
				 build_reference_type (TREE_TYPE (coerced)),
				 rvalue_locus);
    }
  else if (expected->get_kind () == TyTy::TypeKind::POINTER)
    {
      // this is a dyn object
      if (SLICE_TYPE_P (TREE_TYPE (rvalue)))
	{
	  return rvalue;
	}

      // bad coercion... of something to a reference
      bool valid_coercion = actual->get_kind () == TyTy::TypeKind::REF
			    || actual->get_kind () == TyTy::TypeKind::POINTER;
      if (!valid_coercion)
	return error_mark_node;

      const TyTy::ReferenceType *exp
	= static_cast<const TyTy::ReferenceType *> (expected);

      TyTy::BaseType *actual_base = nullptr;
      tree expected_type = error_mark_node;
      if (actual->get_kind () == TyTy::TypeKind::REF)
	{
	  const TyTy::ReferenceType *act
	    = static_cast<const TyTy::ReferenceType *> (actual);

	  actual_base = act->get_base ();
	  expected_type = TyTyResolveCompile::compile (ctx, act->get_base ());
	}
      else if (actual->get_kind () == TyTy::TypeKind::POINTER)
	{
	  const TyTy::PointerType *act
	    = static_cast<const TyTy::PointerType *> (actual);

	  actual_base = act->get_base ();
	  expected_type = TyTyResolveCompile::compile (ctx, act->get_base ());
	}
      rust_assert (actual_base != nullptr);

      tree deref_rvalue
	= ctx->get_backend ()->indirect_expression (expected_type, rvalue,
						    false /*known_valid*/,
						    rvalue_locus);
      tree coerced = coercion_site (deref_rvalue, actual_base, exp->get_base (),
				    lvalue_locus, rvalue_locus);
      if (exp->is_dyn_object () && SLICE_TYPE_P (TREE_TYPE (coerced)))
	return coerced;

      return address_expression (coerced,
				 build_pointer_type (TREE_TYPE (coerced)),
				 rvalue_locus);
    }
  else if (expected->get_kind () == TyTy::TypeKind::ARRAY)
    {
      if (actual->get_kind () != TyTy::TypeKind::ARRAY)
	return error_mark_node;

      tree tree_rval_type = TyTyResolveCompile::compile (ctx, actual);
      tree tree_lval_type = TyTyResolveCompile::compile (ctx, expected);
      if (!verify_array_capacities (tree_lval_type, tree_rval_type,
				    lvalue_locus, rvalue_locus))
	return error_mark_node;
    }
  else if (expected->get_kind () == TyTy::TypeKind::DYNAMIC
	   && actual->get_kind () != TyTy::TypeKind::DYNAMIC)
    {
      const TyTy::DynamicObjectType *dyn
	= static_cast<const TyTy::DynamicObjectType *> (expected);
      return coerce_to_dyn_object (rvalue, actual, expected, dyn, rvalue_locus);
    }
  else if (expected->get_kind () == TyTy::TypeKind::SLICE)
    {
      // bad coercion
      bool valid_coercion = actual->get_kind () == TyTy::TypeKind::SLICE
			    || actual->get_kind () == TyTy::TypeKind::ARRAY;
      if (!valid_coercion)
	return error_mark_node;

      // nothing to do here
      if (actual->get_kind () == TyTy::TypeKind::SLICE)
	return rvalue;

      // return an unsized coercion
      Resolver::Adjustment unsize_adj (
	Resolver::Adjustment::AdjustmentType::UNSIZE, expected);
      return resolve_unsized_adjustment (unsize_adj, rvalue, rvalue_locus);
    }

  return rvalue;
}

tree
HIRCompileBase::coerce_to_dyn_object (tree compiled_ref,
				      const TyTy::BaseType *actual,
				      const TyTy::BaseType *expected,
				      const TyTy::DynamicObjectType *ty,
				      Location locus)
{
  tree dynamic_object = TyTyResolveCompile::compile (ctx, ty);
  tree dynamic_object_fields = TYPE_FIELDS (dynamic_object);
  tree vtable_field = DECL_CHAIN (dynamic_object_fields);
  rust_assert (TREE_CODE (TREE_TYPE (vtable_field)) == ARRAY_TYPE);

  //' this assumes ordering and current the structure is
  // __trait_object_ptr
  // [list of function ptrs]

  std::vector<std::pair<Resolver::TraitReference *, HIR::ImplBlock *>>
    probed_bounds_for_receiver = Resolver::TypeBoundsProbe::Probe (actual);

  tree address_of_compiled_ref = null_pointer_node;
  if (!actual->is_unit ())
    address_of_compiled_ref
      = address_expression (compiled_ref,
			    build_pointer_type (TREE_TYPE (compiled_ref)),
			    locus);

  std::vector<tree> vtable_ctor_elems;
  std::vector<unsigned long> vtable_ctor_idx;
  unsigned long i = 0;
  for (auto &bound : ty->get_object_items ())
    {
      const Resolver::TraitItemReference *item = bound.first;
      const TyTy::TypeBoundPredicate *predicate = bound.second;

      auto address = compute_address_for_trait_item (item, predicate,
						     probed_bounds_for_receiver,
						     actual, actual, locus);
      vtable_ctor_elems.push_back (address);
      vtable_ctor_idx.push_back (i++);
    }

  tree vtable_ctor = ctx->get_backend ()->array_constructor_expression (
    TREE_TYPE (vtable_field), vtable_ctor_idx, vtable_ctor_elems, locus);

  std::vector<tree> dyn_ctor = {address_of_compiled_ref, vtable_ctor};
  return ctx->get_backend ()->constructor_expression (dynamic_object, false,
						      dyn_ctor, -1, locus);
}

tree
HIRCompileBase::compute_address_for_trait_item (
  const Resolver::TraitItemReference *ref,
  const TyTy::TypeBoundPredicate *predicate,
  std::vector<std::pair<Resolver::TraitReference *, HIR::ImplBlock *>>
    &receiver_bounds,
  const TyTy::BaseType *receiver, const TyTy::BaseType *root, Location locus)
{
  // There are two cases here one where its an item which has an implementation
  // within a trait-impl-block. Then there is the case where there is a default
  // implementation for this within the trait.
  //
  // The awkward part here is that this might be a generic trait and we need to
  // figure out the correct monomorphized type for this so we can resolve the
  // address of the function , this is stored as part of the
  // type-bound-predicate
  //
  // Algo:
  // check if there is an impl-item for this trait-item-ref first
  // else assert that the trait-item-ref has an implementation

  TyTy::TypeBoundPredicateItem predicate_item
    = predicate->lookup_associated_item (ref->get_identifier ());
  rust_assert (!predicate_item.is_error ());

  // this is the expected end type
  TyTy::BaseType *trait_item_type = predicate_item.get_tyty_for_receiver (root);
  rust_assert (trait_item_type->get_kind () == TyTy::TypeKind::FNDEF);
  TyTy::FnType *trait_item_fntype
    = static_cast<TyTy::FnType *> (trait_item_type);

  // find impl-block for this trait-item-ref
  HIR::ImplBlock *associated_impl_block = nullptr;
  const Resolver::TraitReference *predicate_trait_ref = predicate->get ();
  for (auto &item : receiver_bounds)
    {
      Resolver::TraitReference *trait_ref = item.first;
      HIR::ImplBlock *impl_block = item.second;
      if (predicate_trait_ref->is_equal (*trait_ref))
	{
	  associated_impl_block = impl_block;
	  break;
	}
    }

  // FIXME this probably should just return error_mark_node but this helps
  // debug for now since we are wrongly returning early on type-resolution
  // failures, until we take advantage of more error types and error_mark_node
  rust_assert (associated_impl_block != nullptr);

  // lookup self for the associated impl
  std::unique_ptr<HIR::Type> &self_type_path
    = associated_impl_block->get_type ();
  TyTy::BaseType *self = nullptr;
  bool ok = ctx->get_tyctx ()->lookup_type (
    self_type_path->get_mappings ().get_hirid (), &self);
  rust_assert (ok);

  // lookup the predicate item from the self
  TyTy::TypeBoundPredicate *self_bound = nullptr;
  for (auto &bound : self->get_specified_bounds ())
    {
      const Resolver::TraitReference *bound_ref = bound.get ();
      const Resolver::TraitReference *specified_ref = predicate->get ();
      if (bound_ref->is_equal (*specified_ref))
	{
	  self_bound = &bound;
	  break;
	}
    }
  rust_assert (self_bound != nullptr);

  // lookup the associated item from the associated impl block
  TyTy::TypeBoundPredicateItem associated_self_item
    = self_bound->lookup_associated_item (ref->get_identifier ());
  rust_assert (!associated_self_item.is_error ());

  TyTy::BaseType *mono1 = associated_self_item.get_tyty_for_receiver (self);
  rust_assert (mono1 != nullptr);
  rust_assert (mono1->get_kind () == TyTy::TypeKind::FNDEF);
  TyTy::FnType *assocated_item_ty1 = static_cast<TyTy::FnType *> (mono1);

  // Lookup the impl-block for the associated impl_item if it exists
  HIR::Function *associated_function = nullptr;
  for (auto &impl_item : associated_impl_block->get_impl_items ())
    {
      bool is_function = impl_item->get_impl_item_type ()
			 == HIR::ImplItem::ImplItemType::FUNCTION;
      if (!is_function)
	continue;

      HIR::Function *fn = static_cast<HIR::Function *> (impl_item.get ());
      bool found_associated_item
	= fn->get_function_name ().compare (ref->get_identifier ()) == 0;
      if (found_associated_item)
	associated_function = fn;
    }

  // we found an impl_item for this
  if (associated_function != nullptr)
    {
      // lookup the associated type for this item
      TyTy::BaseType *lookup = nullptr;
      bool ok = ctx->get_tyctx ()->lookup_type (
	associated_function->get_mappings ().get_hirid (), &lookup);
      rust_assert (ok);
      rust_assert (lookup->get_kind () == TyTy::TypeKind::FNDEF);
      TyTy::FnType *lookup_fntype = static_cast<TyTy::FnType *> (lookup);

      if (lookup_fntype->needs_substitution ())
	{
	  TyTy::SubstitutionArgumentMappings mappings
	    = assocated_item_ty1->solve_missing_mappings_from_this (
	      *trait_item_fntype, *lookup_fntype);
	  lookup_fntype = lookup_fntype->handle_substitions (mappings);
	}

      return CompileInherentImplItem::Compile (associated_function, ctx,
					       lookup_fntype, true, locus);
    }

  // we can only compile trait-items with a body
  bool trait_item_has_definition = ref->is_optional ();
  rust_assert (trait_item_has_definition);

  HIR::TraitItem *trait_item = ref->get_hir_trait_item ();
  return CompileTraitItem::Compile (trait_item, ctx, trait_item_fntype, true,
				    locus);
}

bool
HIRCompileBase::verify_array_capacities (tree ltype, tree rtype,
					 Location lvalue_locus,
					 Location rvalue_locus)
{
  rust_assert (ltype != NULL_TREE);
  rust_assert (rtype != NULL_TREE);

  // lets just return ok as other errors have already occurred
  if (ltype == error_mark_node || rtype == error_mark_node)
    return true;

  tree ltype_domain = TYPE_DOMAIN (ltype);
  if (!ltype_domain)
    return false;

  if (!TREE_CONSTANT (TYPE_MAX_VALUE (ltype_domain)))
    return false;

  unsigned HOST_WIDE_INT ltype_length
    = wi::ext (wi::to_offset (TYPE_MAX_VALUE (ltype_domain))
		 - wi::to_offset (TYPE_MIN_VALUE (ltype_domain)) + 1,
	       TYPE_PRECISION (TREE_TYPE (ltype_domain)),
	       TYPE_SIGN (TREE_TYPE (ltype_domain)))
	.to_uhwi ();

  tree rtype_domain = TYPE_DOMAIN (rtype);
  if (!rtype_domain)
    return false;

  if (!TREE_CONSTANT (TYPE_MAX_VALUE (rtype_domain)))
    return false;

  unsigned HOST_WIDE_INT rtype_length
    = wi::ext (wi::to_offset (TYPE_MAX_VALUE (rtype_domain))
		 - wi::to_offset (TYPE_MIN_VALUE (rtype_domain)) + 1,
	       TYPE_PRECISION (TREE_TYPE (rtype_domain)),
	       TYPE_SIGN (TREE_TYPE (rtype_domain)))
	.to_uhwi ();

  if (ltype_length != rtype_length)
    {
      rust_error_at (
	rvalue_locus,
	"expected an array with a fixed size of " HOST_WIDE_INT_PRINT_UNSIGNED
	" elements, found one with " HOST_WIDE_INT_PRINT_UNSIGNED " elements",
	ltype_length, rtype_length);
      return false;
    }

  return true;
}

} // namespace Compile
} // namespace Rust
