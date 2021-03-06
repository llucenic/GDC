// d-decls.cc -- D frontend for GCC.
// Copyright (C) 2011, 2012 Free Software Foundation, Inc.

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


#include "d-system.h"
#include "d-lang.h"
#include "d-codegen.h"

#include "mars.h"
#include "aggregate.h"
#include "attrib.h"
#include "enum.h"
#include "id.h"
#include "init.h"
#include "module.h"
#include "statement.h"
#include "ctfe.h"

#include "dfrontend/target.h"

// Create the symbol with tree for struct initialisers.

Symbol *
SymbolDeclaration::toSymbol (void)
{
  return dsym->toInitializer();
}


// Helper for toSymbol.  Generate a mangled identifier for Symbol.
// We don't bother using sclass and t.

Symbol *
Dsymbol::toSymbolX (const char *prefix, int, type *, const char *suffix)
{
  const char *n = mangle();
  unsigned nlen = strlen (n);
  size_t sz = (2 + nlen + sizeof (size_t) * 3 + strlen (prefix) + strlen (suffix) + 1);
  Symbol *s = new Symbol();

  s->Sident = XNEWVEC (const char, sz);
  snprintf (CONST_CAST (char *, s->Sident), sz, "_D%s%zu%s%s",
	    n, strlen (prefix), prefix, suffix);
  return s;
}


Symbol *
Dsymbol::toSymbol (void)
{
  fprintf (stderr, "Dsymbol::toSymbol() '%s', kind = '%s'\n", toChars(), kind());
  gcc_unreachable();          // BUG: implement
  return NULL;
}

// Generate an import symbol from symbol.

Symbol *
Dsymbol::toImport (void)
{
  if (!isym)
    {
      if (!csym)
	csym = toSymbol();
      isym = toImport (csym);
    }
  return isym;
}

Symbol *
Dsymbol::toImport (Symbol *)
{
  // This is not used in GDC (yet?)
  return NULL;
}


// Create the symbol with VAR_DECL tree for static variables.

Symbol *
VarDeclaration::toSymbol (void)
{
  if (!csym)
    {
      tree var_decl;
      enum tree_code decl_kind;

      // For field declaration, it is possible for toSymbol to be called
      // before the parent's toCtype()
      if (isField())
	{
	  AggregateDeclaration *parent_decl = toParent()->isAggregateDeclaration();
	  gcc_assert (parent_decl);
	  parent_decl->type->toCtype();
	  gcc_assert (csym);
	  return csym;
	}

      csym = new Symbol();
      csym->Salignment = alignment;

      if (isDataseg())
	{
	  csym->Sident = mangle();
	  csym->prettyIdent = toPrettyChars();
	}
      else
	csym->Sident = ident->string;

      if (storage_class & STCparameter)
	decl_kind = PARM_DECL;
      else if (storage_class & STCmanifest)
	decl_kind = CONST_DECL;
      else
	decl_kind = VAR_DECL;

      var_decl = build_decl (UNKNOWN_LOCATION, decl_kind, get_identifier (csym->Sident),
			     declaration_type (this));

      csym->Stree = var_decl;

      if (decl_kind != CONST_DECL)
	{
	  if (isDataseg())
	    {
	      tree id = get_identifier (csym->Sident);
	      if (protection == PROTpublic || storage_class & (STCstatic | STCextern))
		id = targetm.mangle_decl_assembler_name (var_decl, id);
	      SET_DECL_ASSEMBLER_NAME (var_decl, id);
	    }
	}

      DECL_LANG_SPECIFIC (var_decl) = build_d_decl_lang_specific (this);
      d_keep (var_decl);
      set_decl_location (var_decl, this);

      if (decl_kind == VAR_DECL)
	setup_symbol_storage (this, var_decl, false);
      else if (decl_kind == PARM_DECL)
	{
	  /* from gcc code: Some languages have different nominal and real types.  */
	  // %% What about DECL_ORIGINAL_TYPE, DECL_ARG_TYPE_AS_WRITTEN, DECL_ARG_TYPE ?
	  DECL_ARG_TYPE (var_decl) = TREE_TYPE (var_decl);
	  DECL_CONTEXT (var_decl) = d_decl_context (this);
	  gcc_assert (TREE_CODE (DECL_CONTEXT (var_decl)) == FUNCTION_DECL);
	}
      else if (decl_kind == CONST_DECL)
	{
	  /* Not sure how much of an optimization this is... It is needed
	     for foreach loops on tuples which 'declare' the index variable
	     as a constant for each iteration. */
	  Expression *e = NULL;

	  if (init)
	    {
	      if (!init->isVoidInitializer())
		{
		  e = init->toExpression();
		  gcc_assert (e != NULL);
		}
	    }
	  else
	    e = type->defaultInit();

	  if (e)
	    DECL_INITIAL (var_decl) = e->toElem (cirstate);
	}

      // Can't set TREE_STATIC, etc. until we get to toObjFile as this could be
      // called from a varaible in an imported module
      // %% (out const X x) doesn't mean the reference is const...
      if ((isConst() || isImmutable()) && (storage_class & STCinit)
	  && !decl_reference_p (this))
	{
	  // %% CONST_DECLS don't have storage, so we can't use those,
	  // but it would be nice to get the benefit of them (could handle in
	  // VarExp -- makeAddressOf could switch back to the VAR_DECL

	  if (!TREE_STATIC (var_decl))
	    TREE_READONLY (var_decl) = 1;
	  else
	    csym->Sreadonly = true;

	  // can at least do this...
	  //  const doesn't seem to matter for aggregates, so prevent problems..
	  if (isConst() && isDataseg())
	    TREE_CONSTANT (var_decl) = 1;
	}

      // Propagate volatile.
      if (TYPE_VOLATILE (TREE_TYPE (var_decl)))
	TREE_THIS_VOLATILE (var_decl) = 1;

#if TARGET_DLLIMPORT_DECL_ATTRIBUTES
      // Have to test for import first
      if (isImportedSymbol())
	{
	  insert_decl_attributes (var_decl, "dllimport");
	  DECL_DLLIMPORT_P (var_decl) = 1;
	}
      else if (isExport())
	insert_decl_attributes (var_decl, "dllexport");
#endif

      if (global.params.vtls && isDataseg() && isThreadlocal())
	{
	  char *p = loc.toChars();
	  fprintf (stderr, "%s: %s is thread local\n", p ? p : "", toChars());
	  if (p)
	    free (p);
	}
    }

  return csym;
}

// Create the symbol with tree for classinfo decls.

Symbol *
ClassInfoDeclaration::toSymbol (void)
{
  return cd->toSymbol();
}

// Create the symbol with tree for moduleinfo decls.

Symbol *
ModuleInfoDeclaration::toSymbol (void)
{
  return mod->toSymbol();
}

// Create the symbol with tree for typeinfo decls.

Symbol *
TypeInfoDeclaration::toSymbol (void)
{
  if (!csym)
    {
      VarDeclaration::toSymbol();

      // This variable is the static initialization for the
      // given TypeInfo.  It is the actual data, not a reference
      gcc_assert (TREE_CODE (TREE_TYPE (csym->Stree)) == REFERENCE_TYPE);
      TREE_TYPE (csym->Stree) = TREE_TYPE (TREE_TYPE (csym->Stree));
      TREE_USED (csym->Stree) = 1;

      // Built-in typeinfo will be referenced as one-only.
      D_DECL_ONE_ONLY (csym->Stree) = 1;
      d_comdat_linkage (csym->Stree);
    }

  return csym;
}

// Create the symbol with tree for typeinfoclass decls.

Symbol *
TypeInfoClassDeclaration::toSymbol (void)
{
  gcc_assert (tinfo->ty == Tclass);
  TypeClass *tc = (TypeClass *) tinfo;
  return tc->sym->toSymbol();
}


// Create the symbol with tree for function aliases.

Symbol *
FuncAliasDeclaration::toSymbol (void)
{
  return funcalias->toSymbol();
}

// Create the symbol with FUNCTION_DECL tree for functions.

Symbol *
FuncDeclaration::toSymbol (void)
{
  if (!csym)
    {
      csym = new Symbol();

      if (!isym)
	{
	  tree id;
	  TypeFunction *ftype = (TypeFunction *) (tintro ? tintro : type);
	  tree fndecl = build_decl (UNKNOWN_LOCATION, FUNCTION_DECL,
				    NULL_TREE, NULL_TREE);
	  tree fntype = NULL_TREE;
	  tree vindex = NULL_TREE;

	  csym->Stree = fndecl;

	  if (ident)
	    id = get_identifier (ident->string);
	  else
	    {
	      static unsigned unamed_seq = 0;
	      char buf[64];
	      snprintf (buf, sizeof(buf), "___unamed_%u", ++unamed_seq);
	      id = get_identifier (buf);
	    }
	  DECL_NAME (fndecl) = id;
	  DECL_CONTEXT (fndecl) = d_decl_context (this);

	  if (needs_static_chain (this))
	    {
	      D_DECL_STATIC_CHAIN (fndecl) = 1;
	      // Save context and set decl_function_context for cgraph.
	      csym->ScontextDecl = DECL_CONTEXT (fndecl);
	      DECL_CONTEXT (fndecl) = decl_function_context (fndecl);
	    }


	  /* Nested functions may not have its toObjFile called before the outer
	     function is finished.  GCC requires that nested functions be finished
	     first so we need to arrange for toObjFile to be called earlier.  */
	  Dsymbol *outer = toParent2();
	  if (outer && outer->isFuncDeclaration())
	    {
	      Symbol *osym = outer->toSymbol();

	      if (osym->outputStage != Finished)
		((FuncDeclaration *) outer)->deferred.push (this);
	    }

	  TREE_TYPE (fndecl) = ftype->toCtype();
	  DECL_LANG_SPECIFIC (fndecl) = build_d_decl_lang_specific (this);
	  d_keep (fndecl);

	  if (isNested())
	    {
	      /* Even if D-style nested functions are not implemented, add an
		 extra argument to be compatible with delegates. */
	      fntype = build_method_type (void_type_node, TREE_TYPE (fndecl));
	    }
	  else if (isThis())
	    {
	      // Do this even if there is no debug info.  It is needed to make
	      // sure member functions are not called statically
	      AggregateDeclaration *agg_decl = isMember2();
	      tree handle = agg_decl->handle->toCtype();

	      // If handle is a pointer type, get record type.
	      if (!agg_decl->isStructDeclaration())
		handle = TREE_TYPE (handle);

	      fntype = build_method_type (handle, TREE_TYPE (fndecl));

	      if (isVirtual())
		vindex = size_int (vtblIndex);
	    }
	  else if (isMain() && ftype->nextOf()->toBasetype()->ty == Tvoid)
	    {
	      // void main() implicitly converted to int main().
  	      fntype = build_function_type (integer_type_node, TYPE_ARG_TYPES (TREE_TYPE (fndecl)));
	    }

	  if (fntype != NULL_TREE)
	    {
	      TYPE_ATTRIBUTES (fntype) = TYPE_ATTRIBUTES (TREE_TYPE (fndecl));
	      TYPE_LANG_SPECIFIC (fntype) = TYPE_LANG_SPECIFIC (TREE_TYPE (fndecl));
	      TREE_ADDRESSABLE (fntype) = TREE_ADDRESSABLE (TREE_TYPE (fndecl));
	      TREE_TYPE (fndecl) = fntype;
	      d_keep (fntype);
	    }

	  if (ident)
	    {
	      csym->Sident = mangle(); // save for making thunks
	      csym->prettyIdent = toPrettyChars();
	      tree id = get_identifier (csym->Sident);
	      id = targetm.mangle_decl_assembler_name (fndecl, id);
	      SET_DECL_ASSEMBLER_NAME (fndecl, id);
	    }

	  if (vindex)
	    {
	      DECL_VINDEX (fndecl) = vindex;
	      DECL_VIRTUAL_P (fndecl) = 1;
	    }

	  if (isMember2() || isFuncLiteralDeclaration())
	    {
	      // See grokmethod in cp/decl.c
	      DECL_DECLARED_INLINE_P (fndecl) = 1;
	      DECL_NO_INLINE_WARNING_P (fndecl) = 1;
	    }
	  // Don't know what to do with this.
	  else if (flag_inline_functions && canInline (0, 1, 0))
	    {
	      DECL_DECLARED_INLINE_P (fndecl) = 1;
	      DECL_NO_INLINE_WARNING_P (fndecl) = 1;
	    }

	  if (naked)
	    {
	      DECL_NO_INSTRUMENT_FUNCTION_ENTRY_EXIT (fndecl) = 1;
	      DECL_UNINLINABLE (fndecl) = 1;
	    }

	  // These are always compiler generated.
	  if (isArrayOp)
	    {
	      DECL_ARTIFICIAL (fndecl) = 1;
	      D_DECL_ONE_ONLY (fndecl) = 1;
	    }
	  // So are ensure and require contracts.
	  if (ident == Id::ensure || ident == Id::require)
	    {
	      DECL_ARTIFICIAL (fndecl) = 1;
	      TREE_PUBLIC (fndecl) = 1;
	    }

	  if (isStatic())
	    TREE_STATIC (fndecl) = 1;

	  // Assert contracts in functions cause implicit side effects that could
	  // cause wrong codegen if pure/nothrow is thrown in the equation.
	  if (!global.params.useAssert)
	    {
	      // Cannot mark as pure as in 'no side effects' if the function either
	      // returns by ref, or has an internal state 'this'.
	      // Note, pure D functions don't imply nothrow.
	      if (isPure() == PUREstrong && vthis == NULL
		  && ftype->isnothrow && ftype->retStyle() == RETstack)
		DECL_PURE_P (fndecl) = 1;

	      if (ftype->isnothrow)
		TREE_NOTHROW (fndecl) = 1;
	    }

#if TARGET_DLLIMPORT_DECL_ATTRIBUTES
	  // Have to test for import first
	  if (isImportedSymbol())
	    {
	      insert_decl_attributes (fndecl, "dllimport");
	      DECL_DLLIMPORT_P (fndecl) = 1;
	    }
	  else if (isExport())
	    insert_decl_attributes (fndecl, "dllexport");
#endif
	  set_decl_location (fndecl, this);
	  setup_symbol_storage (this, fndecl, false);
	  if (!ident)
	    TREE_PUBLIC (fndecl) = 0;

	  TREE_USED (fndecl) = 1; // %% Probably should be a little more intelligent about this

	  maybe_set_builtin_frontend (this);
	}
      else
	{
	  csym->Stree = isym->Stree;
	}
    }

  return csym;
}


// Create the thunk symbol functions.
// Thunk is added to class at OFFSET.

Symbol *
FuncDeclaration::toThunkSymbol (int offset)
{
  Symbol *sthunk;
  Thunk *thunk;

  toSymbol();

  /* If the thunk is to be static (that is, it is being emitted in this
     module, there can only be one FUNCTION_DECL for it.   Thus, there
     is a list of all thunks for a given function. */
  bool found = false;

  for (size_t i = 0; i < csym->thunks.dim; i++)
    {
      thunk = csym->thunks[i];
      if (thunk->offset == offset)
	{
	  found = true;
	  break;
	}
    }

  if (!found)
    {
      thunk = new Thunk();
      thunk->offset = offset;
      csym->thunks.push (thunk);
    }

  if (!thunk->symbol)
    {
      unsigned sz = strlen (csym->Sident) + 14;
      sthunk = new Symbol();
      sthunk->Sident = XNEWVEC (const char, sz);
      snprintf (CONST_CAST (char *, sthunk->Sident), sz, "_DT%u%s",
		offset, csym->Sident);

      tree target_func_decl = csym->Stree;
      tree thunk_decl = build_decl (DECL_SOURCE_LOCATION (target_func_decl),
				    FUNCTION_DECL, NULL_TREE, TREE_TYPE (target_func_decl));
      DECL_LANG_SPECIFIC (thunk_decl) = DECL_LANG_SPECIFIC (target_func_decl);
      DECL_CONTEXT (thunk_decl) = d_decl_context (this); // from c++...
      TREE_READONLY (thunk_decl) = TREE_READONLY (target_func_decl);
      TREE_THIS_VOLATILE (thunk_decl) = TREE_THIS_VOLATILE (target_func_decl);
      TREE_NOTHROW (thunk_decl) = TREE_NOTHROW (target_func_decl);

      /* Thunks inherit the public/private access of the function they are targetting.  */
      TREE_PUBLIC (thunk_decl) = TREE_PUBLIC (target_func_decl);
      TREE_PRIVATE (thunk_decl) = TREE_PRIVATE (target_func_decl);
      DECL_EXTERNAL (thunk_decl) = 0;

      /* Thunks are always addressable.  */
      TREE_ADDRESSABLE (thunk_decl) = 1;
      TREE_USED (thunk_decl) = 1;
      DECL_ARTIFICIAL (thunk_decl) = 1;
      DECL_DECLARED_INLINE_P (thunk_decl) = 0;

      DECL_VISIBILITY (thunk_decl) = DECL_VISIBILITY (target_func_decl);
      /* Needed on some targets to avoid "causes a section type conflict".  */
      D_DECL_ONE_ONLY (thunk_decl) = D_DECL_ONE_ONLY (target_func_decl);
      DECL_COMDAT (thunk_decl) = DECL_COMDAT (target_func_decl);
      DECL_COMDAT_GROUP (thunk_decl) = DECL_COMDAT_GROUP (target_func_decl);
      DECL_WEAK (thunk_decl) = DECL_WEAK (target_func_decl);

      DECL_NAME (thunk_decl) = get_identifier (sthunk->Sident);
      SET_DECL_ASSEMBLER_NAME (thunk_decl, DECL_NAME (thunk_decl));

      d_keep (thunk_decl);
      sthunk->Stree = thunk_decl;

      use_thunk (thunk_decl, target_func_decl, offset);

      thunk->symbol = sthunk;
    }

  return thunk->symbol;
}


// Create the "ClassInfo" symbol for classes.

Symbol *
ClassDeclaration::toSymbol (void)
{
  if (!csym)
    {
      csym = toSymbolX ("__Class", 0, 0, "Z");

      tree decl = build_decl (BUILTINS_LOCATION, VAR_DECL,
			      get_identifier (csym->Sident), d_unknown_type_node);
      csym->Stree = decl;
      d_keep (decl);

      setup_symbol_storage (this, decl, true);
      set_decl_location (decl, this);

      // ClassInfo cannot be const data, because we use the monitor on it.
      TREE_CONSTANT (decl) = 0;
    }

  return csym;
}

// Create the "InterfaceInfo" symbol for interfaces.

Symbol *
InterfaceDeclaration::toSymbol (void)
{
  if (!csym)
    {
      csym = toSymbolX ("__Interface", 0, 0, "Z");

      tree decl = build_decl (BUILTINS_LOCATION, VAR_DECL,
			      get_identifier (csym->Sident), d_unknown_type_node);
      csym->Stree = decl;
      d_keep (decl);

      setup_symbol_storage (this, decl, true);
      set_decl_location (decl, this);

      TREE_CONSTANT (decl) = 1;
    }

  return csym;
}

// Create the "ModuleInfo" symbol for given module.

Symbol *
Module::toSymbol (void)
{
  if (!csym)
    {
      csym = toSymbolX ("__ModuleInfo", 0, 0, "Z");

      tree decl = build_decl (BUILTINS_LOCATION, VAR_DECL,
			      get_identifier (csym->Sident), d_unknown_type_node);
      csym->Stree = decl;
      d_keep (decl);

      setup_symbol_storage (this, decl, true);
      set_decl_location (decl, this);

      // Not readonly, moduleinit depends on this.
      TREE_CONSTANT (decl) = 0;
      TREE_READONLY (decl) = 0;
    }

  return csym;
}

Symbol *
StructLiteralExp::toSymbol (void)
{
  if (!sym)
    {
      sym = new Symbol();

      // Build reference symbol.
      tree ctype = type->toCtype();
      tree decl = build_decl (UNKNOWN_LOCATION, VAR_DECL, NULL_TREE, ctype);
      get_unique_name (decl, "*");
      set_decl_location (decl, loc);

      TREE_STATIC (decl) = 1;
      TREE_READONLY (decl) = 1;
      TREE_USED (decl) = 1;
      TREE_PRIVATE (decl) = 1;
      DECL_ARTIFICIAL (decl) = 1;

      sym->Stree = decl;
      this->sinit = sym;

      toDt (&sym->Sdt);
      d_finish_symbol (sym);
    }
  
  return sym;
}

Symbol *
ClassReferenceExp::toSymbol (void)
{
  if (!value->sym)
    {
      value->sym = new Symbol();

      // Build reference symbol.
      tree decl = build_decl (UNKNOWN_LOCATION, VAR_DECL, NULL_TREE, d_unknown_type_node);
      char *ident;

      ASM_FORMAT_PRIVATE_NAME (ident, "*", DECL_UID (decl));
      DECL_NAME (decl) = get_identifier (ident);
      set_decl_location (decl, loc);

      TREE_STATIC (decl) = 1;
      TREE_READONLY (decl) = 1;
      TREE_USED (decl) = 1;
      TREE_PRIVATE (decl) = 1;
      DECL_ARTIFICIAL (decl) = 1;

      value->sym->Stree = decl;
      value->sym->Sident = ident;

      toInstanceDt (&value->sym->Sdt);
      d_finish_symbol (value->sym);

      value->sinit = value->sym;
    }

  return value->sym;
}

// Create the "vtbl" symbol for ClassDeclaration.
// This is accessible via the ClassData, but since it is frequently
// needed directly (like for rtti comparisons), make it directly accessible.

Symbol *
ClassDeclaration::toVtblSymbol (void)
{
  if (!vtblsym)
    {
      vtblsym = toSymbolX ("__vtbl", 0, 0, "Z");

      /* The DECL_INITIAL value will have a different type object from the
	 VAR_DECL.  The back end seems to accept this. */
      TypeSArray *vtbl_type = new TypeSArray (Type::tvoidptr,
					       new IntegerExp (loc, vtbl.dim, Type::tindex));

      tree decl = build_decl (UNKNOWN_LOCATION, VAR_DECL,
			      get_identifier (vtblsym->Sident), vtbl_type->toCtype());
      vtblsym->Stree = decl;
      d_keep (decl);

      setup_symbol_storage (this, decl, true);
      set_decl_location (decl, this);

      TREE_READONLY (decl) = 1;
      TREE_CONSTANT (decl) = 1;
      TREE_ADDRESSABLE (decl) = 1;
      // from cp/class.c
      DECL_CONTEXT (decl) = d_decl_context (this);
      DECL_ARTIFICIAL (decl) = 1;
      DECL_VIRTUAL_P (decl) = 1;
      DECL_ALIGN (decl) = TARGET_VTABLE_ENTRY_ALIGN;
    }
  return vtblsym;
}

// Create the static initializer for the struct/class.

// Because this is called from the front end (mtype.cc:TypeStruct::defaultInit()),
// we need to hold off using back-end stuff until the toobjfile phase.

// Specifically, it is not safe create a VAR_DECL with a type from toCtype()
// because there may be unresolved recursive references.
// StructDeclaration::toObjFile calls toInitializer without ever calling
// SymbolDeclaration::toSymbol, so we just need to keep checking if we
// are in the toObjFile phase.

Symbol *
AggregateDeclaration::toInitializer (void)
{
  if (!sinit)
    {
      StructDeclaration *sd = isStructDeclaration();
      sinit = toSymbolX ("__init", 0, 0, "Z");

      if (sd)
	sinit->Salignment = sd->alignment;
    }

  if (!sinit->Stree && current_module_decl)
    {
      tree stype;
      if (isStructDeclaration())
	stype = type->toCtype();
      else
	stype = TREE_TYPE (type->toCtype());

      sinit->Stree = build_decl (UNKNOWN_LOCATION, VAR_DECL,
				 get_identifier (sinit->Sident), stype);
      d_keep (sinit->Stree);

      setup_symbol_storage (this, sinit->Stree, true);
      set_decl_location (sinit->Stree, this);

      TREE_ADDRESSABLE (sinit->Stree) = 1;
      TREE_READONLY (sinit->Stree) = 1;
      TREE_CONSTANT (sinit->Stree) = 1;
      // These initialisers are always global.
      DECL_CONTEXT (sinit->Stree) = 0;
    }

  return sinit;
}

// Create the static initializer for the typedef variable.

Symbol *
TypedefDeclaration::toInitializer (void)
{
  if (!sinit)
    sinit = toSymbolX ("__init", 0, 0, "Z");

  if (!sinit->Stree && current_module_decl)
    {
      sinit->Stree = build_decl (UNKNOWN_LOCATION, VAR_DECL,
				 get_identifier (sinit->Sident), type->toCtype());
      d_keep (sinit->Stree);

      setup_symbol_storage (this, sinit->Stree, true);
      set_decl_location (sinit->Stree, this);

      TREE_CONSTANT (sinit->Stree) = 1;
      TREE_READONLY (sinit->Stree) = 1;
      DECL_CONTEXT (sinit->Stree) = 0;
    }

  return sinit;
}

// Create the static initializer for the enum.

Symbol *
EnumDeclaration::toInitializer (void)
{
  if (!sinit)
    {
      Identifier *ident_save = ident;
      if (!ident)
	ident = Lexer::uniqueId("__enum");
      sinit = toSymbolX ("__init", 0, 0, "Z");
      ident = ident_save;
    }

  if (!sinit->Stree && current_module_decl)
    {
      sinit->Stree = build_decl (UNKNOWN_LOCATION, VAR_DECL,
				 get_identifier (sinit->Sident), type->toCtype());
      d_keep (sinit->Stree);

      setup_symbol_storage (this, sinit->Stree, true);
      set_decl_location (sinit->Stree, this);

      TREE_CONSTANT (sinit->Stree) = 1;
      TREE_READONLY (sinit->Stree) = 1;
      DECL_CONTEXT (sinit->Stree) = 0;
    }

  return sinit;
}


/* Create debug information for a ClassDeclaration's inheritance tree.
   Interfaces are not included. */
static tree
binfo_for (tree tgt_binfo, ClassDeclaration *cls)
{
  tree binfo = make_tree_binfo (1);
  // Want RECORD_TYPE, not REFERENCE_TYPE
  TREE_TYPE (binfo) = TREE_TYPE (cls->type->toCtype());
  BINFO_INHERITANCE_CHAIN (binfo) = tgt_binfo;
  BINFO_OFFSET (binfo) = integer_zero_node;

  if (cls->baseClass)
    BINFO_BASE_APPEND (binfo, binfo_for (binfo, cls->baseClass));

  return binfo;
}

/* Create debug information for an InterfaceDeclaration's inheritance
   tree.  In order to access all inherited methods in the debugger,
   the entire tree must be described.

   This function makes assumptions about inherface layout. */
static tree
intfc_binfo_for (tree tgt_binfo, ClassDeclaration *iface, unsigned& inout_offset)
{
  tree binfo = make_tree_binfo (iface->baseclasses->dim);

  // Want RECORD_TYPE, not REFERENCE_TYPE
  TREE_TYPE (binfo) = TREE_TYPE (iface->type->toCtype());
  BINFO_INHERITANCE_CHAIN (binfo) = tgt_binfo;
  BINFO_OFFSET (binfo) = size_int (inout_offset * Target::ptrsize);

  for (size_t i = 0; i < iface->baseclasses->dim; i++, inout_offset++)
    {
      BaseClass *bc = iface->baseclasses->tdata()[i];
      BINFO_BASE_APPEND (binfo, intfc_binfo_for (binfo, bc->base, inout_offset));
    }

  return binfo;
}

void
ClassDeclaration::toDebug (void)
{
  /* Used to create BINFO even if debugging was off.  This was needed to keep
     references to inherited types. */
  tree rec_type = TREE_TYPE (type->toCtype());

  if (!isInterfaceDeclaration())
    TYPE_BINFO (rec_type) = binfo_for (NULL_TREE, this);
  else
    {
      unsigned offset = 0;
      TYPE_BINFO (rec_type) = intfc_binfo_for (NULL_TREE, this, offset);
    }

  build_type_decl (rec_type, this);
}

void
EnumDeclaration::toDebug (void)
{
  TypeEnum *tc = (TypeEnum *) type;
  if (!tc->sym->defaultval || type->isZeroInit())
    return;

  tree ctype = type->toCtype();

  // The ctype is not necessarily enum, which doesn't sit well with
  // rest_of_type_compilation.  Can call this on structs though.
  if (AGGREGATE_TYPE_P (ctype) || TREE_CODE (ctype) == ENUMERAL_TYPE)
    rest_of_type_compilation (ctype, 1);
}

void
TypedefDeclaration::toDebug (void)
{
}

void
StructDeclaration::toDebug (void)
{
  tree ctype = type->toCtype();
  build_type_decl (ctype, this);
  rest_of_type_compilation (ctype, 1);
}


// Stubs unused in GDC, but required for D front-end.

Symbol *
Module::toModuleAssert (void)
{
  return NULL;
}

Symbol *
Module::toModuleUnittest (void)
{
  return NULL;
}

Symbol *
Module::toModuleArray (void)
{
  return NULL;
}

Symbol *
TypeAArray::aaGetSymbol (const char *, int)
{
  return 0;
}

int
Dsymbol::cvMember (unsigned char *)
{
  return 0;
}

int
EnumDeclaration::cvMember (unsigned char *)
{
  return 0;
}

int
FuncDeclaration::cvMember (unsigned char *)
{
  return 0;
}

int
VarDeclaration::cvMember (unsigned char *)
{
  return 0;
}

int
TypedefDeclaration::cvMember (unsigned char *)
{
  return 0;
}

