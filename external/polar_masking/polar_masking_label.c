/*-------------------------------------------------------------------------
*
* polar_masking_label.c
*       Process definition of masking label and policy.
*
* IDENTIFICATION
*    external/polar_masking/polar_masking_label.c
*
*-------------------------------------------------------------------------
*/

#include "postgres.h"
#include "polar_masking.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/indexing.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"

PG_FUNCTION_INFO_V1(polar_masking_create_label);
PG_FUNCTION_INFO_V1(polar_masking_drop_label);
PG_FUNCTION_INFO_V1(polar_masking_apply_label_to_table);
PG_FUNCTION_INFO_V1(polar_masking_remove_table_from_label);

static int	get_labelid_applied_on_table(Relation label_tab_rel, Oid relid);
static Oid	check_schema_table_exist(char *schemaname, char *tabname);
static int	get_masking_labelid_ifexist(Relation policy_rel, char *labelname);

/*
 * get the labelid if table is set a masking label
 */
static int
get_labelid_applied_on_table(Relation label_tab_rel, Oid relid)
{
	HeapTuple	tuple;
	ScanKeyData scankey;
	SysScanDesc sscan;
	bool		isNull;
	int			labelid = InvalidMaskingLabelId;
	Oid			label_tab_relid_idx_oid;

	ScanKeyInit(&scankey,
				Anum_polar_masking_label_tab_relid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	if (OidIsValid(label_tab_relid_idx_oid = GetPolarMaskingLabelTabRelidIdxid()))
	{
		sscan = systable_beginscan(label_tab_rel, label_tab_relid_idx_oid, true,
								   NULL, 1, &scankey);
	}
	else
	{
		sscan = systable_beginscan(label_tab_rel, InvalidOid, false,
								   NULL, 1, &scankey);
	}

	if (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		labelid = DatumGetInt32(heap_getattr(tuple, Anum_polar_masking_label_tab_labelid,
											 RelationGetDescr(label_tab_rel), &isNull));
		systable_endscan(sscan);
		return labelid;
	}

	systable_endscan(sscan);

	return labelid;
}

/*
 * check existence of schema and table. If exsits return Oid of the relation,
 * else return invalidOid
 */
static Oid
check_schema_table_exist(char *schemaname, char *tabname)
{
	Oid			schemaid = get_namespace_oid(schemaname, true);
	Oid			relid = InvalidOid;

	if (schemaid == InvalidOid)
	{
		return InvalidOid;
	}
	relid = get_relname_relid(tabname, schemaid);
	return relid;
}

/*
 * Get the masking labelid if labelname exsits in polar_masking_policy
 */
static int
get_masking_labelid_ifexist(Relation policy_rel, char *labelname)
{
	HeapTuple	tuple;
	ScanKeyData scankey;
	SysScanDesc sscan;
	bool		isNull;
	int			labelid = InvalidMaskingLabelId;

	ScanKeyInit(&scankey,
				Anum_polar_masking_policy_name,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(labelname));

	sscan = systable_beginscan(policy_rel, InvalidOid, false,
							   NULL, 1, &scankey);

	if (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		labelid = DatumGetInt32(heap_getattr(tuple, Anum_polar_masking_policy_labelid,
											 RelationGetDescr(policy_rel), &isNull));
	}

	systable_endscan(sscan);
	return labelid;
}

/*
 * create masking label
 */
Datum
polar_masking_create_label(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	HeapTuple	tuple;
	Datum		new_record[Natts_polar_masking_policy];
	bool		new_record_nulls[Natts_polar_masking_policy];
	char	   *labelname;
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();

	if (!OidIsValid(policy_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);

	if (InvalidMaskingLabelId != get_masking_labelid_ifexist(policy_rel, labelname))
	{
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("masking label name: \"%s\" already exists",
						labelname)));
	}
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, false, sizeof(new_record_nulls));
	new_record[Anum_polar_masking_policy_labelid - 1] = Int32GetDatum(GetNextMaskingLabelid());
	new_record[Anum_polar_masking_policy_name - 1] = CStringGetTextDatum(labelname);

	tuple = heap_form_tuple(RelationGetDescr(policy_rel), new_record, new_record_nulls);
	CatalogTupleInsert(policy_rel, tuple);
	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}

/*
 * drop masking label
 */
Datum
polar_masking_drop_label(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	Relation	label_tab_rel;
	Relation	label_col_rel;
	HeapTuple	tuple;
	ScanKeyData scankey;
	SysScanDesc sscan;
	char	   *labelname;
	int			labelid;
	bool		isNull;
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();
	Oid			label_col_rel_oid = GetPolarMaskingLabelColRelid();
	Oid			label_tab_rel_oid = GetPolarMaskingLabelTabRelid();

	if (!OidIsValid(policy_rel_oid) || !OidIsValid(label_col_rel_oid) ||
		!OidIsValid(label_tab_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);
	label_tab_rel = table_open(label_tab_rel_oid, ExclusiveLock);
	label_col_rel = table_open(label_col_rel_oid, ExclusiveLock);

	/*
	 * remove label in polar_masking_policy, only one
	 */
	ScanKeyInit(&scankey,
				Anum_polar_masking_policy_name,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(labelname));

	sscan = systable_beginscan(policy_rel, InvalidOid, false,
							   NULL, 1, &scankey);

	if (!HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		systable_endscan(sscan);
		table_close(label_col_rel, ExclusiveLock);
		table_close(label_tab_rel, ExclusiveLock);
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("masking label name: \"%s\" does not exist",
						labelname)));
	}
	labelid = DatumGetInt32(heap_getattr(tuple, Anum_polar_masking_policy_labelid,
										 RelationGetDescr(policy_rel), &isNull));
	Assert(!isNull);
	CatalogTupleDelete(policy_rel, &tuple->t_self);
	systable_endscan(sscan);

	/*
	 * remove labels in polar_masking_label_tab
	 */
	ScanKeyInit(&scankey,
				Anum_polar_masking_label_tab_labelid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(labelid));
	sscan = systable_beginscan(label_tab_rel, InvalidOid, false,
							   NULL, 1, &scankey);
	while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		CatalogTupleDelete(label_tab_rel, &tuple->t_self);
	}
	systable_endscan(sscan);

	/*
	 * remove labels in polar_masking_label_col
	 */
	ScanKeyInit(&scankey,
				Anum_polar_masking_label_col_labelid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(labelid));
	sscan = systable_beginscan(label_col_rel, InvalidOid, false,
							   NULL, 1, &scankey);
	while (HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		CatalogTupleDelete(label_col_rel, &tuple->t_self);
	}
	systable_endscan(sscan);

	table_close(label_col_rel, ExclusiveLock);
	table_close(label_tab_rel, ExclusiveLock);
	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}

/*
 * bind one table to a masking label
 */
Datum
polar_masking_apply_label_to_table(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	Relation	label_tab_rel;
	HeapTuple	tuple;
	char	   *labelname;
	char	   *schemaname;
	char	   *tablename;
	Oid			relid;
	int			labelid;
	Datum		new_record[Natts_polar_masking_label_tab];
	bool		new_record_nulls[Natts_polar_masking_label_tab];
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();
	Oid			label_tab_rel_oid = GetPolarMaskingLabelTabRelid();

	if (!OidIsValid(policy_rel_oid) || !OidIsValid(label_tab_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	if (PG_ARGISNULL(1))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the schema name can not be null")));
	}
	else
	{
		schemaname = TextDatumGetCString(PG_GETARG_DATUM(1));
	}

	if (PG_ARGISNULL(2))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the table name can not be null")));
	}
	else
	{
		tablename = TextDatumGetCString(PG_GETARG_DATUM(2));
	}

	relid = check_schema_table_exist(schemaname, tablename);
	if (InvalidOid == relid)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the relation \"%s.%s\" does not exist", schemaname, tablename)));
	}

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);

	labelid = get_masking_labelid_ifexist(policy_rel, labelname);
	if (InvalidMaskingLabelId == labelid)
	{
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("masking label name: \"%s\" does not exist",
						labelname)));
	}

	label_tab_rel = table_open(label_tab_rel_oid, ExclusiveLock);

	/*
	 * if a masking label is applied to the table, cannot apply a label again.
	 */
	if (get_labelid_applied_on_table(label_tab_rel, relid) != InvalidMaskingLabelId)
	{
		table_close(label_tab_rel, ExclusiveLock);
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("masking label already be applied on the table, cannot apply the label now")));
	}

	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, false, sizeof(new_record_nulls));
	new_record[Anum_polar_masking_label_tab_labelid - 1] = Int32GetDatum(labelid);
	new_record[Anum_polar_masking_label_tab_relid - 1] = ObjectIdGetDatum(relid);

	tuple = heap_form_tuple(RelationGetDescr(label_tab_rel), new_record, new_record_nulls);
	CatalogTupleInsert(label_tab_rel, tuple);

	table_close(label_tab_rel, ExclusiveLock);
	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}

/*
 * unbind one table from a masking label
 */
Datum
polar_masking_remove_table_from_label(PG_FUNCTION_ARGS)
{
	Relation	policy_rel;
	Relation	label_tab_rel;
	HeapTuple	tuple;
	ScanKeyData scankey;
	SysScanDesc sscan;
	char	   *labelname;
	char	   *schemaname;
	char	   *tablename;
	Oid			relid;
	int			labelid;
	Oid			policy_rel_oid = GetPolarMaskingPolicyRelid();
	Oid			label_tab_rel_oid = GetPolarMaskingLabelTabRelid();
	Oid			label_tab_relid_idxid;

	if (!OidIsValid(policy_rel_oid) || !OidIsValid(label_tab_rel_oid))
	{
		elog(ERROR, "cannot find masking relations");
	}

	if (PG_ARGISNULL(0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the label name can not be null")));
	}
	else
	{
		labelname = TextDatumGetCString(PG_GETARG_DATUM(0));
	}

	if (PG_ARGISNULL(1))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the schema name can not be null")));
	}
	else
	{
		schemaname = TextDatumGetCString(PG_GETARG_DATUM(1));
	}

	if (PG_ARGISNULL(2))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the table name can not be null")));
	}
	else
	{
		tablename = TextDatumGetCString(PG_GETARG_DATUM(2));
	}

	relid = check_schema_table_exist(schemaname, tablename);
	if (InvalidOid == relid)
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("the relation \"%s.%s\" does not exist", schemaname, tablename)));
	}

	policy_rel = table_open(policy_rel_oid, ExclusiveLock);

	labelid = get_masking_labelid_ifexist(policy_rel, labelname);
	if (InvalidMaskingLabelId == labelid)
	{
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("masking label name: \"%s\" does not exist",
						labelname)));
	}
	label_tab_rel = table_open(label_tab_rel_oid, ExclusiveLock);
	label_tab_relid_idxid = GetPolarMaskingLabelTabRelidIdxid();

	ScanKeyInit(&scankey,
				Anum_polar_masking_label_tab_relid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));

	if (OidIsValid(label_tab_relid_idxid))
	{
		sscan = systable_beginscan(label_tab_rel, label_tab_relid_idxid, true,
								   NULL, 1, &scankey);
	}
	else
	{
		sscan = systable_beginscan(label_tab_rel, InvalidOid, false,
								   NULL, 1, &scankey);
	}

	if (!HeapTupleIsValid(tuple = systable_getnext(sscan)))
	{
		systable_endscan(sscan);
		table_close(label_tab_rel, ExclusiveLock);
		table_close(policy_rel, ExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("masking label is not be applied on the table")));
	}

	CatalogTupleDelete(label_tab_rel, &tuple->t_self);

	systable_endscan(sscan);
	table_close(label_tab_rel, ExclusiveLock);
	table_close(policy_rel, ExclusiveLock);
	PG_RETURN_NULL();
}
