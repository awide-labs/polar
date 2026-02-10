/*-------------------------------------------------------------------------
 *
 * lsyscache_px.c
 *	  Convenience routines for common queries in the system catalog cache.
 *
 * Portions Copyright (c) 2021, Alibaba Group Holding Limited
 * Portions Copyright (c) 2007-2009, Greenplum inc
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/lsyscache_px.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "bootstrap/bootstrap.h"
#include "catalog/namespace.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_language.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_range.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* POLAR px */
#include "catalog/pg_aggregate.h"
#include "funcapi.h"
#include "px/px_hash.h"
#include "catalog/pg_inherits.h"
/* POLAR end */

/*
 * px_get_agg_transtype
 *		Given aggregate id, return the aggregate transition function's result type.
 */
Oid
px_get_agg_transtype(Oid aggid)
{
	HeapTuple	tp;
	Oid			result;

	tp = SearchSysCache1(AGGFNOID, ObjectIdGetDatum(aggid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for aggregate %u", aggid);

	result = ((Form_pg_aggregate) GETSTRUCT(tp))->aggtranstype;
	ReleaseSysCache(tp);
	return result;
}

/*
 * is_ordered_agg
 *		Given aggregate id, check if it is an ordered aggregate
 */
bool
px_is_agg_ordered(Oid aggid)
{
	HeapTuple	aggTuple;
	char		aggkind;
	bool		isnull = false;

	aggTuple = SearchSysCache1(AGGFNOID,
							   ObjectIdGetDatum(aggid));
	if (!HeapTupleIsValid(aggTuple))
		elog(ERROR, "cache lookup failed for aggregate %u", aggid);

	aggkind = DatumGetChar(SysCacheGetAttr(AGGFNOID, aggTuple,
										   Anum_pg_aggregate_aggkind, &isnull));
	Assert(!isnull);

	ReleaseSysCache(aggTuple);

	return AGGKIND_IS_ORDERED_SET(aggkind);
}

/*
 * px_is_agg_partial_capable
 *		Given aggregate id, check if it can be used in 2-phase aggregation.
 *
 * It must have a combine function, and if the transition type is 'internal',
 * also serial/deserial functions.
 */
bool
px_is_agg_partial_capable(Oid aggid)
{
	HeapTuple	aggTuple;
	Form_pg_aggregate aggform;
	bool		result = true;

	aggTuple = SearchSysCache1(AGGFNOID,
							   ObjectIdGetDatum(aggid));
	if (!HeapTupleIsValid(aggTuple))
		elog(ERROR, "cache lookup failed for aggregate %u", aggid);
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);

	if (aggform->aggcombinefn == InvalidOid)
		result = false;
	else if (aggform->aggtranstype == INTERNALOID)
	{
		if (aggform->aggserialfn == InvalidOid ||
			aggform->aggdeserialfn == InvalidOid)
		{
			result = false;
		}
	}

	ReleaseSysCache(aggTuple);

	return result;
}

/*
 * px_pfree_ptr_array
 * 		Free an array of pointers, after freeing each individual element
 */
void
px_pfree_ptr_array(char **ptrarray, int nelements)
{
	int			i;

	if (NULL == ptrarray)
		return;

	for (i = 0; i < nelements; i++)
	{
		if (NULL != ptrarray[i])
		{
			pfree(ptrarray[i]);
		}
	}
	pfree(ptrarray);
}

/*
 * px_get_att_stats
 *		Get attribute statistics. Return a copy of the HeapTuple object, or NULL
 *		if no stats found for attribute
 *
 */
HeapTuple
px_get_att_stats(Oid relid, AttrNumber attrnum)
{
	HeapTuple	result;

	/*
	 * This is used by PXOPT, and PXOPT doesn't know that there are two
	 * different kinds of stats, the inherited stats and the non-inherited.
	 * Use the inherited stats, i.e. stats that cover all the child tables,
	 * too, if available.
	 */
	result = SearchSysCacheCopy3(STATRELATTINH,
								 ObjectIdGetDatum(relid),
								 Int16GetDatum(attrnum),
								 BoolGetDatum(true));
	if (!result)
		result = SearchSysCacheCopy3(STATRELATTINH,
									 ObjectIdGetDatum(relid),
									 Int16GetDatum(attrnum),
									 BoolGetDatum(false));

	return result;
}

/*
 * px_get_func_output_arg_types
 *		Given procedure id, return the function's output argument types
 */
List *
px_get_func_output_arg_types(Oid funcid)
{
	HeapTuple	tp;
	int			numargs;
	Oid		   *argtypes = NULL;
	char	  **argnames = NULL;
	char	   *argmodes = NULL;
	List	   *l_argtypes = NIL;
	int			i;

	tp = SearchSysCache1(PROCOID,
						 ObjectIdGetDatum(funcid));
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	numargs = get_func_arg_info(tp, &argtypes, &argnames, &argmodes);

	if (NULL == argmodes)
	{
		px_pfree_ptr_array(argnames, numargs);
		if (NULL != argtypes)
		{
			pfree(argtypes);
		}
		ReleaseSysCache(tp);
		return NULL;
	}

	for (i = 0; i < numargs; i++)
	{
		Oid			argtype = argtypes[i];
		char		argmode = argmodes[i];

		if (PROARGMODE_INOUT == argmode || PROARGMODE_OUT == argmode || PROARGMODE_TABLE == argmode)
		{
			l_argtypes = lappend_oid(l_argtypes, argtype);
		}
	}

	px_pfree_ptr_array(argnames, numargs);
	pfree(argtypes);
	pfree(argmodes);

	ReleaseSysCache(tp);
	return l_argtypes;
}

/*
 * px_get_func_arg_types
 *		Given procedure id, return all the function's argument types
 */
List *
px_get_func_arg_types(Oid funcid)
{
	HeapTuple	tp;
	Form_pg_proc procstruct;
	oidvector  *args;
	List	   *result = NIL;
	int			i;

	tp = SearchSysCache(PROCOID,
						ObjectIdGetDatum(funcid),
						0, 0, 0);
	if (!HeapTupleIsValid(tp))
		elog(ERROR, "cache lookup failed for function %u", funcid);

	procstruct = (Form_pg_proc) GETSTRUCT(tp);
	args = &procstruct->proargtypes;
	for (i = 0; i < args->dim1; i++)
	{
		result = lappend_oid(result, args->values[i]);
	}

	ReleaseSysCache(tp);
	return result;
}

/*
 * px_get_type_name
 *	  returns the name of the type with the given oid
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such type.
 */
char *
px_get_type_name(Oid oid)
{
	HeapTuple	tp;

	tp = SearchSysCache(TYPEOID,
						ObjectIdGetDatum(oid),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_type typtup = (Form_pg_type) GETSTRUCT(tp);
		char	   *result;

		result = pstrdup(NameStr(typtup->typname));
		ReleaseSysCache(tp);
		return result;
	}
	else
		return NULL;
}

/*
 * px_func_data_access
 *		Given procedure id, return the function's data access flag.
 */
char
px_func_data_access(Oid funcid)
{
	return PRODATAACCESS_NONE;
}

/*
 * px_func_exec_location
 *		Given procedure id, return the function's proexeclocation field
 */
char
px_func_exec_location(Oid funcid)
{
	return PRODATAACCESS_ANY;
}

/*
 * px_relation_exists
 *	  Is there a relation with the given oid
 */
bool
px_relation_exists(Oid oid)
{
	return SearchSysCacheExists(RELOID, oid, 0, 0, 0);
}

/*
 * px_index_exists
 *	  Is there an index with the given oid
 */
bool
px_index_exists(Oid oid)
{
	return SearchSysCacheExists(INDEXRELID, oid, 0, 0, 0);
}

/*
 * px_type_exists
 *	  Is there a type with the given oid
 */
bool
px_type_exists(Oid oid)
{
	return SearchSysCacheExists(TYPEOID, oid, 0, 0, 0);
}

/*
 * px_operator_exists
 *	  Is there an operator with the given oid
 */
bool
px_operator_exists(Oid oid)
{
	return SearchSysCacheExists(OPEROID, oid, 0, 0, 0);
}

/*
 * px_function_exists
 *	  Is there a function with the given oid
 */
bool
px_function_exists(Oid oid)
{
	return SearchSysCacheExists(PROCOID, oid, 0, 0, 0);
}

/*
 * px_aggregate_exists
 *	  Is there an aggregate with the given oid
 */
bool
px_aggregate_exists(Oid oid)
{
	return SearchSysCacheExists(AGGFNOID, oid, 0, 0, 0);
}

/*  Get oid of aggregate with given name and argument type */
Oid
px_get_aggregate(const char *aggname, Oid oidType)
{
	CatCList   *catlist;
	int			i;
	Oid			oidResult;

	/* lookup pg_proc for functions with the given name and arg type */
	catlist = SearchSysCacheList1(PROCNAMEARGSNSP,
								  CStringGetDatum((char *) aggname));

	oidResult = InvalidOid;
	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	htup = &catlist->members[i]->tuple;
		Form_pg_proc proctuple = (Form_pg_proc) GETSTRUCT(htup);
		Oid			oidProc = proctuple->oid;

		/* skip functions with the wrong number of type of arguments */
		if (1 != proctuple->pronargs || oidType != proctuple->proargtypes.values[0])
		{
			continue;
		}

		if (SearchSysCacheExists(AGGFNOID, ObjectIdGetDatum(oidProc), 0, 0, 0))
		{
			oidResult = oidProc;
			break;
		}
	}

	ReleaseSysCacheList(catlist);

	return oidResult;
}

/*
 * px_check_constraint_exists
 *	  Is there a check constraint with the given oid
 */
bool
px_check_constraint_exists(Oid oidCheckconstraint)
{
	return SearchSysCacheExists1(CONSTROID, ObjectIdGetDatum(oidCheckconstraint));
}

/*
 * px_get_check_constraint_relid
 *		Given check constraint id, return the check constraint's relation oid
 */
Oid
px_get_check_constraint_relid(Oid oidCheckconstraint)
{
	HeapTuple	tp;

	tp = SearchSysCache(CONSTROID,
						ObjectIdGetDatum(oidCheckconstraint),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Form_pg_constraint contup = (Form_pg_constraint) GETSTRUCT(tp);
		Oid			result;

		result = contup->conrelid;
		ReleaseSysCache(tp);
		return result;
	}
	else
		return InvalidOid;
}

/*
 * px_get_check_constraint_oids
 *	 Extract all check constraint oid for a given relation.
 */
List *
px_get_check_constraint_oids(Oid oidRel)
{
	List	   *plConstraints = NIL;
	HeapTuple	htup;
	Relation	conrel;
	ScanKeyData scankey;
	SysScanDesc sscan;

	/*
	 * lookup constraints for relation from the catalog table
	 *
	 * SELECT * FROM pg_constraint WHERE conrelid = :1
	 */
	conrel = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&scankey,
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oidRel));
	sscan = systable_beginscan(conrel, InvalidOid, false,
							   NULL, 1, &scankey);
	while (HeapTupleIsValid(htup = systable_getnext(sscan)))
	{
		Form_pg_constraint contuple = (Form_pg_constraint) GETSTRUCT(htup);

		/* only consider check constraints */
		if (CONSTRAINT_CHECK != contuple->contype || !contuple->convalidated)
		{
			continue;
		}

		plConstraints = lappend_oid(plConstraints, contuple->oid);
	}

	systable_endscan(sscan);
	table_close(conrel, AccessShareLock);

	return plConstraints;
}

/*
 * px_get_check_constraint_name
 *        returns the name of the check constraint with the given oidConstraint.
 *
 * Note: returns a palloc'd copy of the string, or NULL if no such constraint.
 */
char *
px_get_check_constraint_name(Oid oidCheckconstraint)
{
	return get_constraint_name(oidCheckconstraint);
}

/*
 * px_get_check_constraint_expr_tree
 *        returns the expression node tree representing the check constraint
 *        with the given oidConstraint.
 *
 * Note: returns a palloc'd expression node tree, or NULL if no such constraint.
 */
Node *
px_get_check_constraint_expr_tree(Oid oidCheckconstraint)
{
	HeapTuple	tp;
	Node	   *result = NULL;

	tp = SearchSysCache(CONSTROID,
						ObjectIdGetDatum(oidCheckconstraint),
						0, 0, 0);
	if (HeapTupleIsValid(tp))
	{
		Datum		conbin;
		bool		isnull;

		conbin = SysCacheGetAttr(CONSTROID, tp,
								 Anum_pg_constraint_conbin, &isnull);
		if (!isnull)
			result = stringToNode(TextDatumGetCString(conbin));

		ReleaseSysCache(tp);
	}
	return result;
}

/*
 * get_cast_func
 *        finds the cast function between the given source and destination type,
 *        and records its oid and properties in the output parameters.
 *        Returns true if a cast exists, false otherwise.
 */
bool
px_get_cast_func_external(Oid oidSrc, Oid oidDest, bool *is_binary_coercible, Oid *oidCastFunc, CoercionPathType *pathtype)
{
	if (IsBinaryCoercible(oidSrc, oidDest))
	{
		*is_binary_coercible = true;
		*oidCastFunc = 0;

		/* POLAR px */
		*pathtype = COERCION_PATH_NONE;
		return true;
	}

	*is_binary_coercible = false;

	*pathtype = find_coercion_pathway(oidDest, oidSrc, COERCION_IMPLICIT, oidCastFunc);
	if (*pathtype != COERCION_PATH_NONE)
		return true;
	return false;
}

/*
 * px_get_comparison_type
 *      Retrieve comparison type
 */
CmpType
px_get_comparison_type(Oid oidOp)
{
	OpBtreeInterpretation *opBti;
	List	   *opBtis;

	opBtis = get_op_btree_interpretation(oidOp);

	if (opBtis == NIL)
	{
		/* The operator does not belong to any B-tree operator family */
		return CmptOther;
	}

	/*
	 * XXX: Arbitrarily use the first found operator family. Usually there is
	 * only one, but e.g. if someone has created a reverse ordering family
	 * that sorts in descending order, it is ambiguous whether a < operator
	 * stands for the less than operator of the ascending opfamily, or the
	 * greater than operator for the descending opfamily.
	 */
	opBti = (OpBtreeInterpretation *) linitial(opBtis);

	switch (opBti->strategy)
	{
		case BTLessStrategyNumber:
			return CmptLT;
		case BTLessEqualStrategyNumber:
			return CmptLEq;
		case BTEqualStrategyNumber:
			return CmptEq;
		case BTGreaterEqualStrategyNumber:
			return CmptGEq;
		case BTGreaterStrategyNumber:
			return CmptGT;
		case ROWCOMPARE_NE:
			return CmptNEq;
		default:
			elog(ERROR, "unknown B-tree strategy: %d", opBti->strategy);
			return CmptOther;
	}
}

/*
 * px_get_comparison_operator
 *      Retrieve comparison operator between given types
 */
Oid
px_get_comparison_operator(Oid oidLeft, Oid oidRight, CmpType cmpt)
{
	int16		opstrat;
	HeapTuple	ht;
	Oid			result = InvalidOid;
	Relation	pg_amop;
	ScanKeyData scankey[4];
	SysScanDesc sscan;

	switch (cmpt)
	{
		case CmptLT:
			opstrat = BTLessStrategyNumber;
			break;
		case CmptLEq:
			opstrat = BTLessEqualStrategyNumber;
			break;
		case CmptEq:
			opstrat = BTEqualStrategyNumber;
			break;
		case CmptGEq:
			opstrat = BTGreaterEqualStrategyNumber;
			break;
		case CmptGT:
			opstrat = BTGreaterStrategyNumber;
			break;
		default:
			return InvalidOid;
	}

	pg_amop = table_open(AccessMethodOperatorRelationId, AccessShareLock);

	/*
	 * SELECT * FROM pg_amop WHERE amoplefttype = :1 and amoprighttype = :2
	 * and amopmethod = :3 and amopstrategy = :4
	 */
	ScanKeyInit(&scankey[0],
				Anum_pg_amop_amoplefttype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oidLeft));
	ScanKeyInit(&scankey[1],
				Anum_pg_amop_amoprighttype,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(oidRight));
	ScanKeyInit(&scankey[2],
				Anum_pg_amop_amopmethod,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(BTREE_AM_OID));
	ScanKeyInit(&scankey[3],
				Anum_pg_amop_amopstrategy,
				BTEqualStrategyNumber, F_INT2EQ,
				Int16GetDatum(opstrat));

	/* XXX: There is no index for this, so this is slow! */
	sscan = systable_beginscan(pg_amop, InvalidOid, false,
							   NULL, 4, scankey);

	/* XXX: There can be multiple results. Arbitrarily use the first one */
	while (HeapTupleIsValid(ht = systable_getnext(sscan)))
	{
		Form_pg_amop amoptup = (Form_pg_amop) GETSTRUCT(ht);

		result = amoptup->amopopr;
		break;
	}

	systable_endscan(sscan);
	table_close(pg_amop, AccessShareLock);

	return result;
}

/*
 * px_get_operator_opfamilies
 *		Get the oid of operator families the given operator belongs to
 *
 * PX calls this.
 */
List *
px_get_operator_opfamilies(Oid opno)
{
	List	   *opfam_oids;
	CatCList   *catlist;
	int			i;

	opfam_oids = NIL;

	catlist = SearchSysCacheList(AMOPOPID, 1,
								 ObjectIdGetDatum(opno),
								 0, 0);
	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	htup = &catlist->members[i]->tuple;
		Form_pg_amop amop_tuple = (Form_pg_amop) GETSTRUCT(htup);

		opfam_oids = lappend_oid(opfam_oids, amop_tuple->amopfamily);
	}

	ReleaseSysCacheList(catlist);

	return opfam_oids;
}

/*
 * px_get_index_opfamilies
 *		Get the oid of operator families for the index keys
 */
List *
px_get_index_opfamilies(Oid oidIndex)
{
	HeapTuple	htup;
	List	   *opfam_oids;
	bool		isnull = false;
	int			indnatts;
	Datum		indclassDatum;
	oidvector  *indclass;
	int			i;

	htup = SearchSysCache1(INDEXRELID,
						   ObjectIdGetDatum(oidIndex));
	if (!HeapTupleIsValid(htup))
		elog(ERROR, "Index %u not found", oidIndex);

	/*
	 * use SysCacheGetAttr() to retrieve number of index attributes, and the
	 * oid vector of indclass
	 */
	indnatts = DatumGetInt16(SysCacheGetAttr(INDEXRELID, htup, Anum_pg_index_indnatts, &isnull));
	Assert(!isnull);

	indclassDatum = SysCacheGetAttr(INDEXRELID, htup, Anum_pg_index_indclass, &isnull);
	if (isnull)
		return NIL;
	indclass = (oidvector *) DatumGetPointer(indclassDatum);

	opfam_oids = NIL;
	for (i = 0; i < indnatts; i++)
	{
		Oid			oidOpClass = indclass->values[i];
		Oid			opfam = get_opclass_family(oidOpClass);

		opfam_oids = lappend_oid(opfam_oids, opfam);
	}

	ReleaseSysCache(htup);
	return opfam_oids;
}


Oid
polar_get_compatible_hash_opfamily(Oid opno)
{
	Oid			result = InvalidOid;
	CatCList   *catlist;
	int			i;

	/*
	 * Search pg_amop to see if the target operator is registered as the "="
	 * operator of any hash opfamily.  If the operator is registered in
	 * multiple opfamilies, assume we can use any one.
	 */
	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		if (aform->amopmethod == HASH_AM_OID &&
			aform->amopstrategy == HTEqualStrategyNumber)
		{
			result = aform->amopfamily;
			break;
		}
	}

	ReleaseSysCacheList(catlist);

	return result;
}

Oid
polar_get_compatible_legacy_hash_opfamily(Oid opno)
{
	Oid			result = InvalidOid;

/* 	CatCList   *catlist;
	int			i; */

	/*
	 * Search pg_amop to see if the target operator is registered as the "="
	 * operator of any hash opfamily.  If the operator is registered in
	 * multiple opfamilies, assume we can use any one.
	 */
/* 	catlist = SearchSysCacheList1(AMOPOPID, ObjectIdGetDatum(opno));

	for (i = 0; i < catlist->n_members; i++)
	{
		HeapTuple	tuple = &catlist->members[i]->tuple;
		Form_pg_amop aform = (Form_pg_amop) GETSTRUCT(tuple);

		if (aform->amopmethod == HASH_AM_OID &&
			aform->amopstrategy == HTEqualStrategyNumber)
		{
			Oid hashfunc = px_hashproc_in_opfamily(aform->amopfamily, aform->amoplefttype);
			if (IsLegacyPxHashFunction(hashfunc))
			if (false)
			{
				result = aform->amopfamily;
				break;
			}
		}
	}

	ReleaseSysCacheList(catlist); */

	return result;
}

/* GPDB_12_MERGE_FIXME: only used by ORCA. Fix the callers to check
 * Relation->relkind == RELKIND_PARTITIONED_TABLE instead. They should
 * have the relcache entry at hand anyway.
 */
bool
px_relation_is_partitioned(Oid relid)
{
	HeapTuple	tuple;

	tuple = SearchSysCache1(PARTRELID, ObjectIdGetDatum(relid));

	if (HeapTupleIsValid(tuple))
	{
		ReleaseSysCache(tuple);
		return true;
	}
	else
		return false;
}

bool
px_index_is_partitioned(Oid relid)
{
	HeapTuple	tuple;
	Form_pg_class pg_class_tuple;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for relation %u", relid);
	pg_class_tuple = (Form_pg_class) GETSTRUCT(tuple);
	ReleaseSysCache(tuple);
	return pg_class_tuple->relkind == RELKIND_PARTITIONED_INDEX;
}


/*
 * px_has_subclass_slow
 *
 * Performs the exhaustive check whether a relation has a subclass. This is
 * different from has_subclass(), in that the latter can return true if a relation.
 * *might* have a subclass. See comments in has_subclass() for more details.
 */
bool
px_has_subclass_slow(Oid relationId)
{
	ScanKeyData scankey;
	Relation	rel;
	SysScanDesc sscan;
	bool		result;

	if (!has_subclass(relationId))
		return false;

	/* works in some concurrency scenario */
	rel = table_open(InheritsRelationId, AccessShareLock);

	ScanKeyInit(&scankey, Anum_pg_inherits_inhparent,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relationId));

	/* no index on inhparent */
	sscan = systable_beginscan(rel, InvalidOid, false,
							   NULL, 1, &scankey);

	result = (systable_getnext(sscan) != NULL);

	systable_endscan(sscan);

	table_close(rel, AccessShareLock);

	return result;
}

/** POLARDB end */
