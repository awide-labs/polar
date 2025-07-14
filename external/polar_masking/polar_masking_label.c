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

static int	get_masking_labelid_ifexist(Relation policy_rel, char *labelname);

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
