/*-------------------------------------------------------------------------
*	Greenplum Database
*
*	Copyright (C) 2012 EMC Corp.
*	Copyright (C) 2021, Alibaba Group Holding Limiteds
*
*	@filename:
*		px_wrappers.cpp
*
*	@doc:
*		Implementation of GPDB function wrappers. Note that we should never
* 		return directly from inside the PG_TRY() block, in order to restore
*		the long jump stack. That is why we save the return value of the GPDB
*		function to a local variable and return it after the PG_END_TRY().
*		./README file contains the sources (caches and catalog tables) of metadata
*		requested by the optimizer and retrieved using GPDB function wrappers. Any
*		change to optimizer's requested metadata should also be recorded in ./README file.
*
*
*	@test:
*
*-------------------------------------------------------------------------
*/

#include "px_optimizer_util/utils/px_defs.h"

#include "gpos/base.h"
#include "gpos/error/CException.h"
#include "gpos/error/CAutoExceptionStack.h"

#include "naucrates/exception.h"

#include "px_optimizer_util/px_wrappers.h"
#include "catalog/pg_collation.h"
#include "utils/guc.h"
#include "utils/rel.h"

#include "px_optimizer_util/utils/RelationWrapper.h"

extern "C" {
	#include "utils/memutils.h"
	#include "catalog/pg_inherits.h"
	#include "parser/parse_agg.h"
	#include "optimizer/clauses.h"
	#include "access/htup_details.h"
	#include "px/px_mutate.h"
	#include "px/px_vars.h"
	#include "optimizer/optimizer.h"
	#include "optimizer/plancat.h"
	#include "partitioning/partdesc.h"
	#include "utils/relcache.h"
	#include "miscadmin.h"
	#include "utils/memutils.h"
	#include "catalog/partition.h"
	#include "storage/lmgr.h"
	#include "utils/lsyscache.h"
	#include "utils/partcache.h"
}
#define PX_WRAP_START	\
	sigjmp_buf local_sigjmp_buf;	\
	{	\
		CAutoExceptionStack aes((void **) &PG_exception_stack, (void**) &error_context_stack);	\
		if (0 == sigsetjmp(local_sigjmp_buf, 0))	\
		{	\
			aes.SetLocalJmp(&local_sigjmp_buf)

#define PX_WRAP_END	\
		}	\
		else \
		{ \
			GPOS_RAISE(gpdxl::ExmaGPDB, gpdxl::ExmiGPDBError); \
		} \
	}	\

using namespace gpos;

bool
px::BoolFromDatum
	(
	Datum d
	)
{
	PX_WRAP_START;
	{
		return DatumGetBool(d);
	}
	PX_WRAP_END;
	return false;
}

Datum
px::DatumFromBool
	(
	bool b
	)
{
	PX_WRAP_START;
	{
		return BoolGetDatum(b);
	}
	PX_WRAP_END;
	return 0;
}

Datum
px::DatumFromUint8
	(
	uint8 ui8
	)
{
	PX_WRAP_START;
	{
		return UInt8GetDatum(ui8);
	}
	PX_WRAP_END;
	return 0;
}

int16
px::Int16FromDatum
	(
	Datum d
	)
{
	PX_WRAP_START;
	{
		return DatumGetInt16(d);
	}
	PX_WRAP_END;
	return 0;
}

Datum
px::DatumFromInt16
	(
	int16 i16
	)
{
	PX_WRAP_START;
	{
		return Int16GetDatum(i16);
	}
	PX_WRAP_END;
	return 0;
}

uint16
px::Uint16FromDatum
	(
	Datum d
	)
{
	PX_WRAP_START;
	{
		return DatumGetUInt16(d);
	}
	PX_WRAP_END;
	return 0;
}

Datum
px::DatumFromUint16
	(
	uint16 ui16
	)
{
	PX_WRAP_START;
	{
		return UInt16GetDatum(ui16);
	}
	PX_WRAP_END;
	return 0;
}

int32
px::Int32FromDatum
	(
	Datum d
	)
{
	PX_WRAP_START;
	{
		return DatumGetInt32(d);
	}
	PX_WRAP_END;
	return 0;
}

Datum
px::DatumFromInt32
	(
	int32 i32
	)
{
	PX_WRAP_START;
	{
		return Int32GetDatum(i32);
	}
	PX_WRAP_END;
	return 0;
}

uint32
px::lUint32FromDatum
	(
	Datum d
	)
{
	PX_WRAP_START;
	{
		return DatumGetUInt32(d);
	}
	PX_WRAP_END;
	return 0;
}

Datum
px::DatumFromUint32
	(
	uint32 ui32
	)
{
	PX_WRAP_START;
	{
		return UInt32GetDatum(ui32);
	}
	PX_WRAP_END;
	return 0;
}

int64
px::Int64FromDatum
	(
	Datum d
	)
{
	Datum d2 = d;
	PX_WRAP_START;
	{
		return DatumGetInt64(d2);
	}
	PX_WRAP_END;
	return 0;
}

Datum
px::DatumFromInt64
	(
	int64 i64
	)
{
	int64 ii64 = i64;
	PX_WRAP_START;
	{
		return Int64GetDatum(ii64);
	}
	PX_WRAP_END;
	return 0;
}

Oid
px::OidFromDatum
	(
	Datum d
	)
{
	PX_WRAP_START;
	{
		return DatumGetObjectId(d);
	}
	PX_WRAP_END;
	return 0;
}

void *
px::PointerFromDatum
	(
	Datum d
	)
{
	PX_WRAP_START;
	{
		return DatumGetPointer(d);
	}
	PX_WRAP_END;
	return NULL;
}

float4
px::Float4FromDatum
	(
	Datum d
	)
{
	PX_WRAP_START;
	{
		return DatumGetFloat4(d);
	}
	PX_WRAP_END;
	return 0;
}

float8
px::Float8FromDatum
	(
	Datum d
	)
{
	PX_WRAP_START;
	{
		return DatumGetFloat8(d);
	}
	PX_WRAP_END;
	return 0;
}

Datum
px::DatumFromPointer
	(
	const void *p
	)
{
	PX_WRAP_START;
	{
		return PointerGetDatum(p);
	}
	PX_WRAP_END;
	return 0;
}

bool
px::AggregateExists
	(
	Oid oid
	)
{
	PX_WRAP_START;
	{
		return px_aggregate_exists(oid);
	}
	PX_WRAP_END;
	return false;
}

Bitmapset *
px::BmsAddMember
	(
	Bitmapset *a,
	int x
	)
{
	PX_WRAP_START;
	{
		return bms_add_member(a, x);
	}
	PX_WRAP_END;
	return NULL;
}

void *
px::CopyObject
	(
	void *from
	)
{
	PX_WRAP_START;
	{
		return copyObjectImpl(from);
	}
	PX_WRAP_END;
	return NULL;
}

Size
px::DatumSize
	(
	Datum value,
	bool type_by_val,
	int iTypLen
	)
{
	PX_WRAP_START;
	{
		return datumGetSize(value, type_by_val, iTypLen);
	}
	PX_WRAP_END;
	return 0;
}

Node *
px::MutateExpressionTree
	(
	Node *node,
	Node *(*mutator) (),
	void *context,
	int flags
	)
{
	PX_WRAP_START;
	{
		return expression_tree_mutator(node, mutator, context);
	}
	PX_WRAP_END;
	return NULL;
}

bool
px::WalkExpressionTree
	(
	Node *node,
	bool (*walker) (),
	void *context
	)
{
	PX_WRAP_START;
	{
		return expression_tree_walker(node, walker, context);
	}
	PX_WRAP_END;
	return false;
}

Oid
px::ExprType
	(
	Node *expr
	)
{
	PX_WRAP_START;
	{
		return exprType(expr);
	}
	PX_WRAP_END;
	return 0;
}

int32
px::ExprTypeMod
	(
	Node *expr
	)
{
	PX_WRAP_START;
	{
		return exprTypmod(expr);
	}
	PX_WRAP_END;
	return 0;
}

Oid
px::ExprCollation
	(
	Node *expr
	)
{
	PX_WRAP_START;
	{
		if (expr && IsA(expr, List))
		{
			// GPDB_91_MERGE_FIXME: collation
			List *exprlist = (List *) expr;
			ListCell   *lc;

			Oid collation = InvalidOid;
			foreach(lc, exprlist)
			{
				Node *expr = (Node *) lfirst(lc);
				if ((collation = exprCollation(expr)) != InvalidOid)
				{
					break;
				}
			}
			return collation;
		}
		else
		{
			return exprCollation(expr);
		}
	}
	PX_WRAP_END;
	return 0;
}

Oid
px::TypeCollation
(
 Oid type
 )
{
	PX_WRAP_START;
	{
		Oid collation = InvalidOid;
		if (type_is_collatable(type))
		{
			collation = DEFAULT_COLLATION_OID;
		}
		return collation;
	}
	PX_WRAP_END;
	return 0;
}


List *
px::ExtractNodesPlan
	(
	Plan *pl,
	int node_tag,
	bool descend_into_subqueries
	)
{
	PX_WRAP_START;
	{
		return extract_nodes_plan(pl, node_tag, descend_into_subqueries);
	}
	PX_WRAP_END;
	return NIL;
}

List *
px::ExtractNodesExpression
	(
	Node *node,
	int node_tag,
	bool descend_into_subqueries
	)
{
	PX_WRAP_START;
	{
		return extract_nodes_expression(node, node_tag, descend_into_subqueries);
	}
	PX_WRAP_END;
	return NIL;
}

void
px::FreeAttrStatsSlot
	(
	AttStatsSlot *sslot
	)
{
	PX_WRAP_START;
	{
		free_attstatsslot(sslot);
		return;
	}
	PX_WRAP_END;
}

bool
px::FuncStrict
	(
	Oid funcid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return func_strict(funcid);
	}
	PX_WRAP_END;
	return false;
}

char
px::FuncStability
	(
	Oid funcid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return func_volatile(funcid);
	}
	PX_WRAP_END;
	return '\0';
}

char
px::FuncDataAccess
	(
	Oid funcid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return px_func_data_access(funcid);
	}
	PX_WRAP_END;
	return '\0';
}

char
px::FuncExecLocation
	(
	Oid funcid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return px_func_exec_location(funcid);
	}
	PX_WRAP_END;
	return '\0';
}

bool
px::FunctionExists
	(
	Oid oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return px_function_exists(oid);
	}
	PX_WRAP_END;
	return false;
}

Oid
px::GetAggIntermediateResultType
	(
	Oid aggid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_aggregate */
		return px_get_agg_transtype(aggid);
	}
	PX_WRAP_END;
	return 0;
}

int
px::GetAggregateArgTypes
	(
	Aggref *aggref,
	Oid *inputTypes
	)
{
	PX_WRAP_START;
	{
		return get_aggregate_argtypes(aggref, inputTypes);
	}
	PX_WRAP_END;
	return 0;
}

Oid
px::ResolveAggregateTransType
	(
	Oid aggfnoid,
	Oid aggtranstype,
	Oid *inputTypes,
	int numArguments
	)
{
	PX_WRAP_START;
	{
		return resolve_aggregate_transtype(aggfnoid, aggtranstype, inputTypes, numArguments);
	}
	PX_WRAP_END;
	return 0;
}

Query *
px::FlattenJoinAliasVar
	(
	Query *query,
	gpos::ULONG query_level
	)
{
	PX_WRAP_START;
	{
		return flatten_join_alias_var_optimizer(query, query_level);
	}
	PX_WRAP_END;

	return NULL;
}

bool
px::IsOrderedAgg
	(
	Oid aggid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_aggregate */
		return px_is_agg_ordered(aggid);
	}
	PX_WRAP_END;
	return false;
}

bool
px::IsAggPartialCapable
	(
	Oid aggid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_aggregate */
		return px_is_agg_partial_capable(aggid);
	}
	PX_WRAP_END;
	return false;
}

Oid
px::GetAggregate
	(
	const char *agg,
	Oid type_oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_aggregate */
		return px_get_aggregate(agg, type_oid);
	}
	PX_WRAP_END;
	return 0;
}

Oid
px::GetArrayType
	(
	Oid typid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type */
		return get_array_type(typid);
	}
	PX_WRAP_END;
	return 0;
}

bool
px::GetAttrStatsSlot
	(
	AttStatsSlot *sslot,
	HeapTuple statstuple,
	int reqkind,
	Oid reqop,
	int flags
	)
{
	PX_WRAP_START;
	{
		return get_attstatsslot(sslot, statstuple, reqkind, reqop, flags);
	}
	PX_WRAP_END;
	return false;
}

HeapTuple
px::GetAttStats
	(
	Oid relid,
	AttrNumber attnum
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_statistic */
		return px_get_att_stats(relid, attnum);
	}
	PX_WRAP_END;
	return NULL;
}

Oid
px::GetCommutatorOp
	(
	Oid opno
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return get_commutator(opno);
	}
	PX_WRAP_END;
	return 0;
}

bool
px::CheckConstraintExists
	(
	Oid check_constraint_oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_constraint */
		return px_check_constraint_exists(check_constraint_oid);
	}
	PX_WRAP_END;
	return false;
}

// check that a table doesn't have UPDATE triggers.
bool
px::HasUpdateTriggers(Oid relid)
{
	PX_WRAP_START;
	{
		return px_has_update_triggers(relid);
	}
	PX_WRAP_END;
	return false;
}

char *
px::GetCheckConstraintName
	(
	Oid check_constraint_oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_constraint */
		return px_get_check_constraint_name(check_constraint_oid);
	}
	PX_WRAP_END;
	return NULL;
}

Oid
px::GetCheckConstraintRelid
	(
	Oid check_constraint_oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_constraint */
		return px_get_check_constraint_relid(check_constraint_oid);
	}
	PX_WRAP_END;
	return 0;
}

Node *
px::PnodeCheckConstraint
	(
	Oid check_constraint_oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_constraint */
		return px_get_check_constraint_expr_tree(check_constraint_oid);
	}
	PX_WRAP_END;
	return NULL;
}

List *
px::GetCheckConstraintOids
	(
	Oid rel_oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_constraint */
		return px_get_check_constraint_oids(rel_oid);
	}
	PX_WRAP_END;
	return NULL;
}


Node *
px::GetRelationPartConstraints(Relation rel)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_partition, pg_partition_rule, pg_constraint */
		List *part_quals = RelationGetPartitionQual(rel);
		if (part_quals)
		{
			return (Node *) make_ands_explicit(part_quals);
		}
	}
	PX_WRAP_END;
	return nullptr;
}

bool
px::GetCastFunc
	(
	Oid src_oid,
	Oid dest_oid,
	bool *is_binary_coercible,
	Oid *cast_fn_oid,
	CoercionPathType *pathtype
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_cast */
		return px_get_cast_func_external(src_oid, dest_oid, is_binary_coercible, cast_fn_oid, pathtype);
	}
	PX_WRAP_END;
	return false;
}

unsigned int
px::GetComparisonType
	(
	Oid op_oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_amop */
		return px_get_comparison_type(op_oid);
	}
	PX_WRAP_END;
	return CmptOther;
}

Oid
px::GetComparisonOperator
	(
	Oid left_oid,
	Oid right_oid,
	unsigned int cmpt
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_amop */
		return px_get_comparison_operator(left_oid, right_oid, (CmpType) cmpt);
	}
	PX_WRAP_END;
	return InvalidOid;
}

Oid
px::GetEqualityOp
	(
	Oid type_oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type */
		Oid eq_opr;

		get_sort_group_operators(type_oid,
					 false, true, false,
					 NULL, &eq_opr, NULL, NULL);

		return eq_opr;
	}
	PX_WRAP_END;
	return InvalidOid;
}

Oid
px::GetEqualityOpForOrderingOp
	(
	Oid opno,
	bool *reverse
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_amop */
		return get_equality_op_for_ordering_op(opno, reverse);
	}
	PX_WRAP_END;
	return InvalidOid;
}

Oid
px::GetOrderingOpForEqualityOp
(
	Oid opno,
	bool *reverse
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_amop */
		return get_ordering_op_for_equality_op(opno, reverse);
	}
	PX_WRAP_END;
	return InvalidOid;
}

char *
px::GetFuncName
	(
	Oid funcid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return get_func_name(funcid);
	}
	PX_WRAP_END;
	return NULL;
}

List *
px::GetFuncOutputArgTypes
	(
	Oid funcid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return px_get_func_output_arg_types(funcid);
	}
	PX_WRAP_END;
	return NIL;
}

List *
px::GetFuncArgTypes
	(
	Oid funcid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return px_get_func_arg_types(funcid);
	}
	PX_WRAP_END;
	return NIL;
}

bool
px::GetFuncRetset
	(
	Oid funcid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return get_func_retset(funcid);
	}
	PX_WRAP_END;
	return false;
}

Oid
px::GetFuncRetType
	(
	Oid funcid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return get_func_rettype(funcid);
	}
	PX_WRAP_END;
	return 0;
}

Oid
px::GetInverseOp
	(
	Oid opno
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return get_negator(opno);
	}
	PX_WRAP_END;
	return 0;
}

RegProcedure
px::GetOpFunc
	(
	Oid opno
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return get_opcode(opno);
	}
	PX_WRAP_END;
	return 0;
}

char *
px::GetOpName
	(
	Oid opno
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return get_opname(opno);
	}
	PX_WRAP_END;
	return NULL;
}

Oid
px::GetTypeRelid
	(
	Oid typid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type */
		return get_typ_typrelid(typid);
	}
	PX_WRAP_END;
	return 0;
}

char *
px::GetTypeName
	(
	Oid typid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type */
		return px_get_type_name(typid);
	}
	PX_WRAP_END;
	return NULL;
}

int
px::GetPxWorkerCount(void)
{
	PX_WRAP_START;
	{
      return getPxWorkerCount();
	}
	PX_WRAP_END;
	return 0;
}

bool
px::IsPxUseGlobalFunction(void)
{
	PX_WRAP_START;
	{
      return px_use_global_function;
	}
	PX_WRAP_END;
	return false;
}

bool
px::HeapAttIsNull
	(
	HeapTuple tup,
	int attno
	)
{
	PX_WRAP_START;
	{ /* TODO remote NULL */
      return heap_attisnull(tup, attno, NULL);
	}
	PX_WRAP_END;
	return false;
}

void
px::FreeHeapTuple
	(
	HeapTuple htup
	)
{
	PX_WRAP_START;
	{
		heap_freetuple(htup);
		return;
	}
	PX_WRAP_END;
}

bool
px::IndexExists
	(
	Oid oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_index */
		return px_index_exists(oid);
	}
	PX_WRAP_END;
	return false;
}

Oid
px::GetDefaultDistributionOpclassForType
	(
	Oid typid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type, pg_opclass */
		return px_default_distribution_opclass_for_type(typid);
	}
	PX_WRAP_END;
	return false;
}

Oid
px::GetDefaultDistributionOpfamilyForType
	(
	Oid typid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type, pg_opclass */
		return px_default_distribution_opfamily_for_type(typid);
	}
	PX_WRAP_END;
	return false;
}

Oid
px::GetHashProcInOpfamily
	(
	Oid opfamily,
	Oid typid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_amproc, pg_type, pg_opclass */
		return px_hashproc_in_opfamily(opfamily, typid);
	}
	PX_WRAP_END;
	return false;
}

Oid
px::GetOpclassFamily
	(
	Oid opclass
	)
{
	PX_WRAP_START;
	{
		return get_opclass_family(opclass);
	}
	PX_WRAP_END;
	return false;
}

List *
px::LAppend
	(
	List *list,
	void *datum
	)
{
	PX_WRAP_START;
	{
		return lappend(list, datum);
	}
	PX_WRAP_END;
	return NIL;
}

List *
px::LAppendInt
	(
	List *list,
	int iDatum
	)
{
	PX_WRAP_START;
	{
		return lappend_int(list, iDatum);
	}
	PX_WRAP_END;
	return NIL;
}

List *
px::LAppendOid
	(
	List *list,
	Oid datum
	)
{
	PX_WRAP_START;
	{
		return lappend_oid(list, datum);
	}
	PX_WRAP_END;
	return NIL;
}

List *
px::LPrepend
	(
	void *datum,
	List *list
	)
{
	PX_WRAP_START;
	{
		return lcons(datum, list);
	}
	PX_WRAP_END;
	return NIL;
}

List *
px::LPrependInt
	(
	int datum,
	List *list
	)
{
	PX_WRAP_START;
	{
		return lcons_int(datum, list);
	}
	PX_WRAP_END;
	return NIL;
}

List *
px::LPrependOid
	(
	Oid datum,
	List *list
	)
{
	PX_WRAP_START;
	{
		return lcons_oid(datum, list);
	}
	PX_WRAP_END;
	return NIL;
}

List *
px::ListConcat
	(
	List *list1,
	List *list2
	)
{
	PX_WRAP_START;
	{
		return list_concat(list1, list2);
	}
	PX_WRAP_END;
	return NIL;
}

List *
px::ListCopy
	(
	List *list
	)
{
	PX_WRAP_START;
	{
		return list_copy(list);
	}
	PX_WRAP_END;
	return NIL;
}

ListCell *
px::ListHead
	(
	List *l
	)
{
	PX_WRAP_START;
	{
		return list_head(l);
	}
	PX_WRAP_END;
	return NULL;
}

ListCell *
px::ListTail
	(
	List *l
	)
{
	PX_WRAP_START;
	{
		return list_tail(l);
	}
	PX_WRAP_END;
	return NULL;
}

uint32
px::ListLength
	(
	List *l
	)
{
	PX_WRAP_START;
	{
		return list_length(l);
	}
	PX_WRAP_END;
	return 0;
}

void *
px::ListNth
	(
	List *list,
	int n
	)
{
	PX_WRAP_START;
	{
		return list_nth(list, n);
	}
	PX_WRAP_END;
	return NULL;
}

int
px::ListNthInt
	(
	List *list,
	int n
	)
{
	PX_WRAP_START;
	{
		return list_nth_int(list, n);
	}
	PX_WRAP_END;
	return 0;
}

Oid
px::ListNthOid
	(
	List *list,
	int n
	)
{
	PX_WRAP_START;
	{
		return list_nth_oid(list, n);
	}
	PX_WRAP_END;
	return 0;
}

bool
px::ListMemberOid
	(
	List *list,
	Oid oid
	)
{
	PX_WRAP_START;
	{
		return list_member_oid(list, oid);
	}
	PX_WRAP_END;
	return false;
}

void
px::ListFree
	(
	List *list
	)
{
	PX_WRAP_START;
	{
		list_free(list);
		return;
	}
	PX_WRAP_END;
}

void
px::ListFreeDeep
	(
	List *list
	)
{
	PX_WRAP_START;
	{
		list_free_deep(list);
		return;
	}
	PX_WRAP_END;
}

TypeCacheEntry *
px::LookupTypeCache
	(
	Oid type_id,
	int flags
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type, pg_operator, pg_opclass, pg_opfamily, pg_amop */
		return lookup_type_cache(type_id, flags);
	}
	PX_WRAP_END;
	return NULL;
}

String *
px::MakeStringValue
	(
	char *str
	)
{
	PX_WRAP_START;
	{
		return makeString(str);
	}
	PX_WRAP_END;
	return NULL;
}

Integer *
px::MakeIntegerValue
	(
	long i
	)
{
	PX_WRAP_START;
	{
		return makeInteger(i);
	}
	PX_WRAP_END;
	return NULL;
}

Node *
px::MakeIntConst(int32 intValue)
{
	PX_WRAP_START;
	{
		return (Node *) makeConst(INT4OID, -1, InvalidOid, sizeof(int32),
								  Int32GetDatum(intValue), false, true);
	}
	PX_WRAP_END;
	return NULL;
}

Node *
px::MakeBoolConst
	(
	bool value,
	bool isnull
	)
{
	PX_WRAP_START;
	{
		return makeBoolConst(value, isnull);
	}
	PX_WRAP_END;
	return NULL;
}

Node *
px::MakeNULLConst
	(
	Oid type_oid
	)
{
	PX_WRAP_START;
	{
		return (Node *) makeNullConst(type_oid, -1 /*consttypmod*/, InvalidOid);
	}
	PX_WRAP_END;
	return NULL;
}

Node *
px::MakePXWorkerIndexFilterExpr(int segid)
{
	PX_WRAP_START;
	{
		return (Node *) makePXWorkerIndexFilterExpr(segid);
	}
	PX_WRAP_END;
}

TargetEntry *
px::MakeTargetEntry
	(
	Expr *expr,
	AttrNumber resno,
	char *resname,
	bool resjunk
	)
{
	PX_WRAP_START;
	{
		return makeTargetEntry(expr, resno, resname, resjunk);
	}
	PX_WRAP_END;
	return NULL;
}

Var *
px::MakeVar
	(
	Index varno,
	AttrNumber varattno,
	Oid vartype,
	int32 vartypmod,
	Index varlevelsup
	)
{
	PX_WRAP_START;
	{
		// GPDB_91_MERGE_FIXME: collation
		Oid collation = TypeCollation(vartype);
		return makeVar(varno, varattno, vartype, vartypmod, collation, varlevelsup);
	}
	PX_WRAP_END;
	return NULL;
}

void *
px::MemCtxtAllocZeroAligned
	(
	MemoryContext context,
	Size size
	)
{
	PX_WRAP_START;
	{
		return MemoryContextAllocZeroAligned(context, size);
	}
	PX_WRAP_END;
	return NULL;
}

void *
px::MemCtxtAllocZero
	(
	MemoryContext context,
	Size size
	)
{
	PX_WRAP_START;
	{
		return MemoryContextAllocZero(context, size);
	}
	PX_WRAP_END;
	return NULL;
}

void *
px::MemCtxtRealloc
	(
	void *pointer,
	Size size
	)
{
	PX_WRAP_START;
	{
		return repalloc(pointer, size);
	}
	PX_WRAP_END;
	return NULL;
}

char *
px::MemCtxtStrdup
	(
	MemoryContext context,
	const char *string
	)
{
	PX_WRAP_START;
	{
		return MemoryContextStrdup(context, string);
	}
	PX_WRAP_END;
	return NULL;
}

// Helper function to throw an error with errcode, message and hint, like you
// would with ereport(...) in the backend. This could be extended for other
// fields, but this is all we need at the moment.
void
px::PxEreportImpl
	(
	int xerrcode,
	int severitylevel,
	const char *xerrmsg,
	const char *xerrhint,
	const char *filename,
	int lineno,
	const char *funcname
	)
{
	PX_WRAP_START;
	{
		// We cannot use the ereport() macro here, because we want to pass on
		// the caller's filename and line number. This is essentially an
		// expanded version of ereport(). It will be caught by the
		// PX_WRAP_END, and propagated up as a C++ exception, to be
		// re-thrown as a Postgres error once we leave the C++ land.
		if (errstart(severitylevel, TEXTDOMAIN))
		{
			errcode(xerrcode);
			errmsg("%s", xerrmsg);
			if (xerrhint)
				errhint("%s", xerrhint);
			errfinish (filename, lineno, funcname);
		}
	}
	PX_WRAP_END;
}

char *
px::NodeToString
	(
	void *obj
	)
{
	PX_WRAP_START;
	{
		return nodeToString(obj);
	}
	PX_WRAP_END;
	return NULL;
}

Node *
px::StringToNode
	(
	char *string
	)
{
	PX_WRAP_START;
	{
		return (Node*) stringToNode(string);
	}
	PX_WRAP_END;
	return NULL;
}


Node *
px::GetTypeDefault
	(
	Oid typid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type */
		return get_typdefault(typid);
	}
	PX_WRAP_END;
	return NULL;
}


double
px::NumericToDoubleNoOverflow
	(
	Numeric num
	)
{
	PX_WRAP_START;
	{
		return numeric_to_double_no_overflow(num);
	}
	PX_WRAP_END;
	return 0.0;
}

bool
px::NumericIsNan
	(
	Numeric num
	)
{
	PX_WRAP_START;
	{
		return numeric_is_nan(num);
	}
	PX_WRAP_END;
	return false;
}

double
px::ConvertTimeValueToScalar
	(
	Datum datum,
	Oid typid
	)
{
	bool failure = false;
	PX_WRAP_START;
	{
		return convert_timevalue_to_scalar(datum, typid, &failure);
	}
	PX_WRAP_END;
	return 0.0;
}

double
px::ConvertNetworkToScalar
	(
	Datum datum,
	Oid typid
	)
{
	bool failure = false;
	PX_WRAP_START;
	{
		return convert_network_to_scalar(datum, typid, &failure);
	}
	PX_WRAP_END;
	return 0.0;
}

bool
px::IsOpHashJoinable
	(
	Oid opno,
	Oid inputtype
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return op_hashjoinable(opno, inputtype);
	}
	PX_WRAP_END;
	return false;
}

bool
px::IsOpMergeJoinable
	(
	Oid opno,
	Oid inputtype
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return op_mergejoinable(opno, inputtype);
	}
	PX_WRAP_END;
	return false;
}

bool
px::IsOpStrict
	(
	Oid opno
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_operator, pg_proc */
		return op_strict(opno);
	}
	PX_WRAP_END;
	return false;
}

bool
px::IsOpNDVPreserving(Oid opno)
{
	switch (opno)
	{
		// for now, we consider only the concatenation op as NDV-preserving
		// (note that we do additional checks later, e.g. col || 'const' is
		// NDV-preserving, while col1 || col2 is not)
		case 654:
			return true;
		default:
			return false;
	}
}

void
px::GetOpInputTypes
	(
	Oid opno,
	Oid *lefttype,
	Oid *righttype
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_operator */
		op_input_types(opno, lefttype, righttype);
		return;
	}
	PX_WRAP_END;
}

bool
px::OperatorExists
	(
	Oid oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_operator */
		return px_operator_exists(oid);
	}
	PX_WRAP_END;
	return false;
}

void *
px::GPDBAlloc
	(
	Size size
	)
{
	PX_WRAP_START;
	{
		return palloc(size);
	}
	PX_WRAP_END;
	return NULL;
}

void
px::GPDBFree
	(
	void *ptr
	)
{
	PX_WRAP_START;
	{
		pfree(ptr);
		return;
	}
	PX_WRAP_END;
}

bool
px::WalkQueryOrExpressionTree
	(
	Node *node,
	bool (*walker) (),
	void *context,
	int flags
	)
{
	PX_WRAP_START;
	{
		return query_or_expression_tree_walker(node, walker, context, flags);
	}
	PX_WRAP_END;
	return false;
}

Node *
px::MutateQueryOrExpressionTree
	(
	Node *node,
	Node *(*mutator) (),
	void *context,
	int flags
	)
{
	PX_WRAP_START;
	{
		return query_or_expression_tree_mutator(node, mutator, context, flags);
	}
	PX_WRAP_END;
	return NULL;
}

Query *
px::MutateQueryTree
	(
	Query *query,
	Node *(*mutator) (),
	void *context,
	int flags
	)
{
	PX_WRAP_START;
	{
		return query_tree_mutator(query, mutator, context, flags);
	}
	PX_WRAP_END;
	return NULL;
}

bool
px::WalkQueryTree
	(
	Query *query,
	bool (*walker) (),
	void *context,
	int flags
	)
{
	PX_WRAP_START;
	{
		return query_tree_walker(query, walker, context, flags);
	}
	PX_WRAP_END;
	return false;
}

PxPolicy *
px::GetDistributionPolicy
	(
	Relation rel
	)
{
    PX_WRAP_START;
    {
        /* catalog tables: pg_class */
		return createRandomPartitionedPolicy(getPxWorkerCount());
    }
    PX_WRAP_END;
    return NULL;
}


bool
px::RelationExists
	(
	Oid oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_class */
		return px_relation_exists(oid);
	}
	PX_WRAP_END;
	return false;
}

double
px::PxEstimatePartitionedNumTuples
	(
	Relation rel
	)
{
	PX_WRAP_START;
	{
		return px_estimate_partitioned_numtuples(rel);
	}
	PX_WRAP_END;
}

void
px::CloseRelation
	(
	Relation rel
	)
{
	PX_WRAP_START;
	{
		RelationClose(rel);
		return;
	}
	PX_WRAP_END;
}

List *
px::GetRelationIndexes
	(
	Relation relation
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: from relcache */
		return RelationGetIndexList(relation);
	}
	PX_WRAP_END;
	return NIL;
}

px::RelationWrapper
px::GetRelation(Oid rel_oid)
{
	PX_WRAP_START;
	{
		/* catalog tables: relcache */
		return RelationWrapper{RelationIdGetRelation(rel_oid)};
	}
	PX_WRAP_END;
}

TargetEntry *
px::FindFirstMatchingMemberInTargetList
	(
	Node *node,
	List *targetlist
	)
{
	PX_WRAP_START;
	{
      return tlist_member((Expr*)node, targetlist);
	}
	PX_WRAP_END;
	return NULL;
}

List *
px::FindMatchingMembersInTargetList
	(
	Node *node,
	List *targetlist
	)
{
	PX_WRAP_START;
	{
		return tlist_members(node, targetlist);
	}
	PX_WRAP_END;

	return NIL;
}

bool
px::Equals
	(
	void *p1,
	void *p2
	)
{
	PX_WRAP_START;
	{
		return equal(p1, p2);
	}
	PX_WRAP_END;
	return false;
}

bool
px::TypeExists
	(
	Oid oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type */
		return px_type_exists(oid);
	}
	PX_WRAP_END;
	return false;
}

bool
px::IsCompositeType
	(
	Oid typid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type */
		return type_is_rowtype(typid);
	}
	PX_WRAP_END;
	return false;
}

bool
px::IsTextRelatedType
	(
	Oid typid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type */
		char typcategory;
		bool typispreferred;
		get_type_category_preferred(typid, &typcategory, &typispreferred);

		return typcategory == TYPCATEGORY_STRING;
	}
	PX_WRAP_END;
	return false;
}

int
px::GetIntFromValue
	(
	Node *node
	)
{
	PX_WRAP_START;
	{
		return intVal(node);
	}
	PX_WRAP_END;
	return 0;
}


int
px::StrCmpIgnoreCase
	(
	const char *s1,
	const char *s2
	)
{
	PX_WRAP_START;
	{
		return pg_strcasecmp(s1, s2);
	}
	PX_WRAP_END;
	return 0;
}

bool *
px::ConstructRandomSegMap
	(
	int total_primaries,
	int total_to_skip
	)
{
	PX_WRAP_START;
	{
		return makeRandomSegMap(total_primaries, total_to_skip);
	}
	PX_WRAP_END;
	return NULL;
}

StringInfo
px::MakeStringInfo(void)
{
	PX_WRAP_START;
	{
		return makeStringInfo();
	}
	PX_WRAP_END;
	return NULL;
}

void
px::AppendStringInfo
	(
	StringInfo str,
	const char *str1,
	const char *str2
	)
{
	PX_WRAP_START;
	{
		appendStringInfo(str, "%s%s", str1, str2);
		return;
	}
	PX_WRAP_END;
}

int
px::FindNodes
	(
	Node *node,
	List *nodeTags
	)
{
	PX_WRAP_START;
	{
		return find_nodes(node, nodeTags);
	}
	PX_WRAP_END;
	return -1;
}

int
px::CheckCollation
	(
	Node *node
	)
{
	PX_WRAP_START;
	{
		return check_collation(node);
	}
	PX_WRAP_END;
	return -1;
}

Node *
px::CoerceToCommonType
	(
	ParseState *pstate,
	Node *node,
	Oid target_type,
	const char *context
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_type, pg_cast */
		return coerce_to_common_type
					(
					pstate,
					node,
					target_type,
					context
					);
	}
	PX_WRAP_END;
	return NULL;
}

bool
px::ResolvePolymorphicArgType
	(
	int numargs,
	Oid *argtypes,
	char *argmodes,
	FuncExpr *call_expr
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_proc */
		return resolve_polymorphic_argtypes(numargs, argtypes, argmodes, (Node *)call_expr);
	}
	PX_WRAP_END;
	return false;
}

// hash a list of const values with GPDB's hash function
int32 
px::PxHashConstList
	(
	List *constants,
	int num_segments,
	Oid *hashfuncs
	)
{
	PX_WRAP_START;
	{
		return pxhash_const_list(constants, num_segments, hashfuncs);
	}
	PX_WRAP_END;
	return 0;
}

unsigned int
px::PxHashRandomSeg(int num_segments)
{
	PX_WRAP_START;
	{
		return pxhashrandomseg(num_segments);
	}
	PX_WRAP_END;
	return 0;
}

// check permissions on range table
void
px::CheckRTPermissions
	(
	List *rtable
	)
{
	PX_WRAP_START;
	{
		ExecCheckRTPerms(rtable, true);
		return;
	}
	PX_WRAP_END;
}

// check permissions on range table list
bool
px::CheckRTPermissionsWithRet
	(
	List *rtable
	)
{
	PX_WRAP_START;
	{
		return ExecCheckRTPerms(rtable, false);
	}
	PX_WRAP_END;
}

// get index op family properties
void
px::IndexOpProperties
	(
	Oid opno,
	Oid opfamily,
	int *strategy,
	Oid *righttype
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_amop */

		// Only the right type is returned to the caller, the left
		// type is simply ignored.
		Oid	lefttype;

		get_op_opfamily_properties(opno, opfamily, false, strategy, &lefttype, righttype);
		return;
	}
	PX_WRAP_END;
}

// get oids of opfamilies for the index keys
List *
px::GetIndexOpFamilies
	(
	Oid index_oid
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_index */

		// We return the operator families of the index keys.
		return px_get_index_opfamilies(index_oid);
	}
	PX_WRAP_END;

	return NIL;
}

// get oids of families this operator belongs to
List *
px::GetOpFamiliesForScOp
	(
	Oid opno
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_amop */

		// We return the operator families this operator
		// belongs to.
		return px_get_operator_opfamilies(opno);
	}
	PX_WRAP_END;

	return NIL;
}


// get the OID of hash equality operator(s) compatible with the given op
Oid
px::GetCompatibleHashOpFamily(Oid opno)
{
	PX_WRAP_START;
	{
		return polar_get_compatible_hash_opfamily(opno);
	}
	PX_WRAP_END;
	return InvalidOid;
}

// get the OID of hash equality operator(s) compatible with the given op
Oid
px::GetCompatibleLegacyHashOpFamily(Oid opno)
{
	PX_WRAP_START;
	{
		return polar_get_compatible_legacy_hash_opfamily(opno);
	}
	PX_WRAP_END;
	return InvalidOid;
}

List *
px::GetMergeJoinOpFamilies
	(
	Oid opno
	)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_amop */

		return get_mergejoin_opfamilies(opno);
	}
	PX_WRAP_END;
	return NIL;
}


// Evaluates 'expr' and returns the result as an Expr.
// Caller keeps ownership of 'expr' and takes ownership of the result
Expr *
px::EvaluateExpr
	(
	Expr *expr,
	Oid result_type,
	int32 typmod
	)
{
	PX_WRAP_START;
	{
		return evaluate_expr(expr, result_type, typmod, InvalidOid);
	}
	PX_WRAP_END;
	return NULL;
}

char *
px::DefGetString
	(
	DefElem *defelem
	)
{
	PX_WRAP_START;
	{
		return defGetString(defelem);
	}
	PX_WRAP_END;
	return NULL;
}

Expr *
px::TransformArrayConstToArrayExpr
	(
	Const *c
	)
{
	PX_WRAP_START;
	{
		return transform_array_Const_to_ArrayExpr(c);
	}
	PX_WRAP_END;
	return NULL;
}

Node *
px::EvalConstExpressions
	(
	Node *node
	)
{
	PX_WRAP_START;
	{
		return eval_const_expressions(NULL, node);
	}
	PX_WRAP_END;
	return NULL;
}


/*
 * To detect changes to catalog tables that require resetting the Metadata
 * Cache, we use the normal PostgreSQL catalog cache invalidation mechanism.
 * We register a callback to a cache on all the catalog tables that contain
 * information that's contained in the PXOPT metadata cache.

 * There is no fine-grained mechanism in the metadata cache for invalidating
 * individual entries ATM, so we just blow the whole cache whenever anything
 * changes. The callback simply increments a counter. Whenever we start
 * planning a query, we check the counter to see if it has changed since the
 * last planned query, and reset the whole cache if it has.
 *
 * To make sure we've covered all catalog tables that contain information
 * that's stored in the metadata cache, there are "catalog tables: xxx"
 * comments in all the calls to backend functions in this file. They indicate
 * which catalog tables each function uses. We conservatively assume that
 * anything fetched via the wrapper functions in this file can end up in the
 * metadata cache and hence need to have an invalidation callback registered.
 */
static bool mdcache_invalidation_counter_registered = false;
static int64 mdcache_invalidation_counter = 0;
static int64 last_mdcache_invalidation_counter = 0;

static void
mdsyscache_invalidation_counter_callback(Datum arg, int cacheid, uint32 hashvalue)
{
	mdcache_invalidation_counter++;
}

static void
mdrelcache_invalidation_counter_callback(Datum arg, Oid relid)
{
	mdcache_invalidation_counter++;
}

static void
register_mdcache_invalidation_callbacks(void)
{
	/* These are all the catalog tables that we care about. */
	int			metadata_caches[] = {
		AGGFNOID,			/* pg_aggregate */
		AMOPOPID,			/* pg_amop */
		CASTSOURCETARGET,	/* pg_cast */
		CONSTROID,			/* pg_constraint */
		OPEROID,			/* pg_operator */
		OPFAMILYOID,		/* pg_opfamily */
		//PARTOID,			/* pg_partition */
		//PARTRULEOID,		/* pg_partition_rule */
		STATRELATTINH,			/* pg_statistics */
		TYPEOID,			/* pg_type */
		PROCOID,			/* pg_proc */

		/*
		 * lookup_type_cache() will also access pg_opclass, via GetDefaultOpClass(),
		 * but there is no syscache for it. Postgres doesn't seem to worry about
		 * invalidating the type cache on updates to pg_opclass, so we don't
		 * worry about that either.
		 */
		/* pg_opclass */

		/*
		 * Information from the following catalogs are included in the
		 * relcache, and any updates will generate relcache invalidation
		 * event. We'll catch the relcache invalidation event and don't need
		 * to register a catcache callback for them.
		 */
		/* pg_class */
		/* pg_index */
		/* pg_trigger */

		/*
		 * pg_exttable is only updated when a new external table is dropped/created,
		 * which will trigger a relcache invalidation event.
		 */
		/* pg_exttable */

		/*
		 * XXX: no syscache on pg_inherits. Is that OK? For any partitioning
		 * changes, I think there will also be updates on pg_partition and/or
		 * pg_partition_rules.
		 */
		/* pg_inherits */
		/*
		 * We assume that px_node_config will not change on the fly in a way that
		 * would affect PXOPT
		 */
		/* px_node_config */
	};
	unsigned int i;

	for (i = 0; i < lengthof(metadata_caches); i++)
	{
		CacheRegisterSyscacheCallback(metadata_caches[i],
									  &mdsyscache_invalidation_counter_callback,
									  (Datum) 0);
	}

	/* also register the relcache callback */
	CacheRegisterRelcacheCallback(&mdrelcache_invalidation_counter_callback,
								  (Datum) 0);
}

// Has there been any catalog changes since last call?
bool
px::MDCacheNeedsReset
		(
			void
		)
{
	PX_WRAP_START;
	{
		if (!mdcache_invalidation_counter_registered)
		{
			register_mdcache_invalidation_callbacks();
			mdcache_invalidation_counter_registered = true;
		}
		if (last_mdcache_invalidation_counter == mdcache_invalidation_counter)
			return false;
		else
		{
			last_mdcache_invalidation_counter = mdcache_invalidation_counter;
			return true;
		}
	}
	PX_WRAP_END;

	return true;
}

// returns true if a query cancel is requested in GPDB
bool
px::IsAbortRequested
	(
	void
	)
{
	// No PX_WRAP_START/END needed here. We just check these global flags,
	// it cannot throw an ereport().
	return (QueryCancelPending || ProcDiePending);
}

// Given the type OID, get the typelem (InvalidOid if not an array type).
Oid
px::GetElementType(Oid array_type_oid)
{
	PX_WRAP_START;
	{
		return get_element_type(array_type_oid);
	}
	PX_WRAP_END;
	return -1;
}

PxPolicy *
px::MakePxPolicy
		(
			PxPolicyType ptype,
			int nattrs,
			int numsegments
		)
{
	PX_WRAP_START;
	{
		/*
		 * FIXME_TABLE_EXPAND: it used by PXOPT, help...
		 */
		return makePxPolicy(ptype, nattrs, numsegments);
	}
	PX_WRAP_END;
	return NULL;
}

uint32
px::HashChar(Datum d)
{
	PX_WRAP_START;
	{
		return DatumGetUInt32(DirectFunctionCall1(hashchar, d));
	}
	PX_WRAP_END;
	return 0;
}

uint32
px::HashBpChar(Datum d)
{
	PX_WRAP_START;
	{
		return DatumGetUInt32(
			DirectFunctionCall1Coll(hashbpchar, C_COLLATION_OID, d));
	}
	PX_WRAP_END;
	return 0;
}

uint32
px::HashText(Datum d)
{
	PX_WRAP_START;
	{
		return DatumGetUInt32(
			DirectFunctionCall1Coll(hashtext, C_COLLATION_OID, d));
	}
	PX_WRAP_END;
	return 0;
}

uint32
px::HashName(Datum d)
{
	PX_WRAP_START;
	{
		return DatumGetUInt32(DirectFunctionCall1(hashname, d));
	}
	PX_WRAP_END;
	return 0;
}

uint32
px::UUIDHash(Datum d)
{
	PX_WRAP_START;
	{
		return DatumGetUInt32(DirectFunctionCall1(uuid_hash, d));
	}
	PX_WRAP_END;
	return 0;
}

void *
px::GPDBMemoryContextAlloc
	(
	MemoryContext context,
	Size size
	)
{
	PX_WRAP_START;
	{
		return MemoryContextAlloc(context, size);
	}
	PX_WRAP_END;
	return NULL;
}

void
px::GPDBMemoryContextDelete(MemoryContext context)
{
	PX_WRAP_START;
	{
		MemoryContextDelete(context);
	}
	PX_WRAP_END;
}

MemoryContext
px::GPDBAllocSetContextCreate()
{
	PX_WRAP_START;
	{
		MemoryContext cxt;
		cxt = AllocSetContextCreate(px_OptimizerMemoryContext,
		"PXOPT memory pool",
		ALLOCSET_DEFAULT_MINSIZE,
		ALLOCSET_DEFAULT_INITSIZE,
		ALLOCSET_DEFAULT_MAXSIZE);

		/*
		 * Declare it as accounting root so that we can call
		 * MemoryContextGetCurrentSpace() on it.
		 */
		MemoryContextDeclareAccountingRoot(cxt);
		return cxt;
	}
	PX_WRAP_END;
	return NULL;
}

bool
px::ExpressionReturnsSet(Node *clause)
{
	PX_WRAP_START;
	{
		return expression_returns_set(clause);
	}
	PX_WRAP_END;
}

bool
px::RelIsPartitioned(Oid relid)
{
	PX_WRAP_START;
	{
		return px_relation_is_partitioned(relid);
	}
	PX_WRAP_END;
	return false;
}

bool
px::IndexIsPartitioned(Oid relid)
{
	PX_WRAP_START;
	{
		return px_index_is_partitioned(relid);
	}
	PX_WRAP_END;
	return false;
}

List *
px::GetRelChildIndexes(Oid reloid)
{
	List *partoids = NIL;
	PX_WRAP_START;
	{
		if (InvalidOid == reloid)
		{
			return NIL;
		}
		partoids = find_inheritance_children(reloid, NoLock);
	}
	PX_WRAP_END;
	return partoids;
}

/* POLAR px */
PartitionDesc
px::GetPartitionDesc(Relation rel, bool omit_detached)
{
	PX_WRAP_START;
	{
		return RelationGetPartitionDesc(rel, omit_detached);
	}
	PX_WRAP_END;
}

PartitionKey
px::GetPartitionKey(Relation rel)
{
	PX_WRAP_START;
	{
		return RelationGetPartitionKey(rel);
	}
	PX_WRAP_END;
}

void
px::GetRelLeafPartTables(Oid oid, List **oids)
{
	int i;
	PX_WRAP_START;
	{
		px::RelationWrapper rel = px::GetRelation(oid);
		*oids = lappend(*oids,&(rel->rd_id));
		if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			return;
		}

		PartitionDesc pd = px::GetPartitionDesc(rel.get(), true);
		for (i = 0; i < pd->nparts; i++)
		{
			Oid childOID = pd->oids[i];
			GetRelLeafPartTables(childOID, oids);
		}
	}
	PX_WRAP_END;
}

void
px::GetPartTableHegiht(Oid oid, int *height, int currentHeight)
{
	PX_WRAP_START;
	{
		int i;
		px::RelationWrapper rel = px::GetRelation(oid);
		if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			return ;
		}

		currentHeight++;
		if (*height < currentHeight)
		{
			*height = currentHeight;
		}

		PartitionDesc pd = px::GetPartitionDesc(rel.get(), true);
		for (i = 0; i < pd->nparts; i++)
		{
			Oid	childOID = pd->oids[i];
			GetPartTableHegiht(childOID, height, currentHeight);
		}
	}
	PX_WRAP_END;
}

void
px::GetRelLeafPartIndexes(Oid oid, List **oids)
{
	int i;
	PX_WRAP_START;
	{
		px::RelationWrapper rel = px::GetRelation(oid);
		List *indexoidlist;
		indexoidlist = RelationGetIndexList(rel.get());

		ListCell *lc = NULL;
		foreach(lc, indexoidlist)
		{
			Oid *oid = (Oid *) lfirst(lc);
			*oids = lappend(*oids, oid);
		}
		if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		{
			return;
		}

		PartitionDesc pd = px::GetPartitionDesc(rel.get(), true);
		for (i = 0; i < pd->nparts; i++)
		{
			Oid	childOID = pd->oids[i];
			GetRelLeafPartIndexes(childOID, oids);
		}
	}
	PX_WRAP_END;
}

List *
px::GetAllChildPartIndexes(Oid indexOid)
{
	List *partoids = NIL;
	PX_WRAP_START;
	{
		if (InvalidOid == indexOid)
		{
			return NIL;
		}
		partoids = find_all_inheritors(indexOid, NoLock, NULL);
	}
	PX_WRAP_END;
	return partoids;
}
/* POLAR end */

/*
 * it's necessary for partitioned tables
 */
void
px::GPDBLockRelationOid(Oid reloid, int lockmode)
{
	PX_WRAP_START;
	{
		// FIXME: pass a correct lockmode
		LockRelationOid(reloid, lockmode);
		return;
	}
	PX_WRAP_END;
}

bool
px::HasSubclassSlow(Oid rel_oid)
{
	PX_WRAP_START;
	{
		/* catalog tables: pg_inherits */
		return px_has_subclass_slow(rel_oid);
	}
	PX_WRAP_END;
	return false;
}

// EOF
