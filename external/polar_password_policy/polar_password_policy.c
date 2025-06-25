/*-------------------------------------------------------------------------
 *
 * polar_password_policy.c
 *
 * IDENTIFICATION
 *    external/polar_password_policy/polar_password_policy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/transam.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "commands/extension.h"
#include "fmgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

PG_MODULE_MAGIC;


#define POLAR_PASSWORD_POLICY_SCHEMA		"polar_security"

/* Table for saving password policies */
#define POLAR_PASSWORD_POLICY				"polar_password_policy"
#define POLAR_PASSWORD_POLICY_OID_INDEX		"polar_password_policy_oid_index"
#define POLAR_PASSWORD_POLICY_NAME_INDEX	"polar_password_policy_name_index"
#define Anum_polar_password_policy_oid					1
#define Anum_polar_password_policy_name					2
#define Anum_polar_password_policy_password_complexity	3
#define Anum_polar_password_policy_password_min_length	4
#define Anum_polar_password_policy_password_max_length	5
#define Anum_polar_password_policy_password_valid_time	6
#define Anum_polar_password_policy_password_reuse_count	7
#define Anum_polar_password_policy_password_reuse_time	8
#define Anum_polar_password_policy_password_fail_count	9
#define Anum_polar_password_policy_password_lock_time	10
#define Natts_polar_password_policy						10

#define POLAR_PASSWORD_POLICY_BINDING				"polar_password_policy_binding"
#define POLAR_PASSWORD_POLICY_BINDING_ROLE_INDEX	"polar_password_policy_binding_role_index"
#define POLAR_PASSWORD_POLICY_BINDING_POLICY_INDEX	"polar_password_policy_binding_policy_index"
#define Anum_polar_password_policy_binding_role		1
#define Anum_polar_password_policy_binding_policy	2
#define Natts_polar_password_policy_binding			2

/* Options for password policies */
#define PASSWORD_POLICY_OPTION_NUM 8
typedef struct polar_password_policy_option_struct
{
	char	   *name;
	int			def;
	int			lower;
	int			upper;
}			polar_password_policy_option_struct;
static polar_password_policy_option_struct
polar_password_policy_option[PASSWORD_POLICY_OPTION_NUM] =
{
	{
		"password_complexity", 4, 3, 4
	},
	{
		"password_min_length", 8, 6, 999
	},
	{
		"password_max_length", 32, 6, 999
	},
	{
		"password_valid_time", 30, 1, 180
	},
	{
		"password_reuse_count", 5, 1, 20
	},
	{
		"password_reuse_time", 7, 1, 30
	},
	{
		"password_fail_count", 5, 1, 20
	},
	{
		"password_lock_time", 60, 1, 86400
	}
};

#define DEFAULT_PASSWORD_POLICY "default_password_policy"


static bool polar_password_policy_enable = false;

PG_FUNCTION_INFO_V1(polar_create_password_policy);
PG_FUNCTION_INFO_V1(polar_create_password_policy_option);
PG_FUNCTION_INFO_V1(polar_alter_password_policy);
PG_FUNCTION_INFO_V1(polar_rename_password_policy);
PG_FUNCTION_INFO_V1(polar_drop_password_policy);
PG_FUNCTION_INFO_V1(polar_bind_password_policy);
PG_FUNCTION_INFO_V1(polar_unbind_password_policy);

void		_PG_init(void);


/*
 * Create a password policy.
 */
static void
polar_password_policy_create(char *pname, int complexity, int min_length,
							 int max_length, int valid_time, int reuse_count,
							 int reuse_time, int fail_count, int lock_time)
{
	Oid			schid;
	Oid			relid;
	Oid			indid;
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	Datum		values[Natts_polar_password_policy];
	bool		nulls[Natts_polar_password_policy];
	NameData	pnamedata;

	schid = get_namespace_oid(POLAR_PASSWORD_POLICY_SCHEMA, false);
	relid = get_relname_relid(POLAR_PASSWORD_POLICY, schid);
	indid = get_relname_relid(POLAR_PASSWORD_POLICY_NAME_INDEX, schid);
	if (!OidIsValid(relid) || !OidIsValid(indid))
		return;

	rel = table_open(relid, RowExclusiveLock);

	/* Form a scan key and fetch a tuple */
	ScanKeyInit(&skey,
				Anum_polar_password_policy_name,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(pname));
	scan = systable_beginscan(rel, indid, true, NULL, 1, &skey);
	tup = systable_getnext(scan);

	/* Check whether the password policy exists */
	if (HeapTupleIsValid(tup))
	{
		systable_endscan(scan);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("policy \"%s\" already exists", pname)));
	}

	/* Form a tuple */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	values[Anum_polar_password_policy_oid - 1] = ObjectIdGetDatum(GetNewObjectId());
	namestrcpy(&pnamedata, pname);
	values[Anum_polar_password_policy_name - 1] = NameGetDatum(&pnamedata);
	values[Anum_polar_password_policy_password_complexity - 1] = Int32GetDatum(complexity);
	values[Anum_polar_password_policy_password_min_length - 1] = Int32GetDatum(min_length);
	values[Anum_polar_password_policy_password_max_length - 1] = Int32GetDatum(max_length);
	values[Anum_polar_password_policy_password_valid_time - 1] = Int32GetDatum(valid_time);
	values[Anum_polar_password_policy_password_reuse_count - 1] = Int32GetDatum(reuse_count);
	values[Anum_polar_password_policy_password_reuse_time - 1] = Int32GetDatum(reuse_time);
	values[Anum_polar_password_policy_password_fail_count - 1] = Int32GetDatum(fail_count);
	values[Anum_polar_password_policy_password_lock_time - 1] = Int32GetDatum(lock_time);

	/* Insert a new tuple */
	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);

	heap_freetuple(tup);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);
}

/*
 * Check whether the value of the password policy option is within the specified range.
 * And return the column number of this option in polar_password_policy.
 */
static int
polar_password_policy_option_check(char *name, int value)
{
	for (int i = 0; i < PASSWORD_POLICY_OPTION_NUM; i++)
	{
		if (strcmp(name, polar_password_policy_option[i].name) == 0)
		{
			if (value < polar_password_policy_option[i].lower
				|| value > polar_password_policy_option[i].upper)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("\"%s\" can only between %d and %d", name,
								polar_password_policy_option[i].lower,
								polar_password_policy_option[i].upper)));
			return i + 3;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unrecognized option: %s", name)));
}

/*
 * Create a password policy with default values.
 */
Datum
polar_create_password_policy(PG_FUNCTION_ARGS)
{
	char	   *pname;

	if (!polar_password_policy_enable && !creating_extension)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("polar_password_policy.enable is false")));

	pname = NameStr(*(PG_GETARG_NAME(0)));
	polar_password_policy_create(pname,
								 polar_password_policy_option[Anum_polar_password_policy_password_complexity - 3].def,
								 polar_password_policy_option[Anum_polar_password_policy_password_min_length - 3].def,
								 polar_password_policy_option[Anum_polar_password_policy_password_max_length - 3].def,
								 polar_password_policy_option[Anum_polar_password_policy_password_valid_time - 3].def,
								 polar_password_policy_option[Anum_polar_password_policy_password_reuse_count - 3].def,
								 polar_password_policy_option[Anum_polar_password_policy_password_reuse_time - 3].def,
								 polar_password_policy_option[Anum_polar_password_policy_password_fail_count - 3].def,
								 polar_password_policy_option[Anum_polar_password_policy_password_lock_time - 3].def);

	PG_RETURN_VOID();
}

/*
 * Create a password policy with user-specified values.
 */
Datum
polar_create_password_policy_option(PG_FUNCTION_ARGS)
{
	char	   *pname;
	int			complexity;
	int			min_length;
	int			max_length;
	int			valid_time;
	int			reuse_count;
	int			reuse_time;
	int			fail_count;
	int			lock_time;

	if (!polar_password_policy_enable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("polar_password_policy.enable is false")));

	/* Check parameter value */
	pname = NameStr(*(PG_GETARG_NAME(0)));
	complexity = PG_GETARG_INT32(1);
	polar_password_policy_option_check(polar_password_policy_option[Anum_polar_password_policy_password_complexity - 3].name, complexity);
	min_length = PG_GETARG_INT32(2);
	polar_password_policy_option_check(polar_password_policy_option[Anum_polar_password_policy_password_min_length - 3].name, min_length);
	max_length = PG_GETARG_INT32(3);
	polar_password_policy_option_check(polar_password_policy_option[Anum_polar_password_policy_password_max_length - 3].name, max_length);
	valid_time = PG_GETARG_INT32(4);
	polar_password_policy_option_check(polar_password_policy_option[Anum_polar_password_policy_password_valid_time - 3].name, valid_time);
	reuse_count = PG_GETARG_INT32(5);
	polar_password_policy_option_check(polar_password_policy_option[Anum_polar_password_policy_password_reuse_count - 3].name, reuse_count);
	reuse_time = PG_GETARG_INT32(6);
	polar_password_policy_option_check(polar_password_policy_option[Anum_polar_password_policy_password_reuse_time - 3].name, reuse_time);
	fail_count = PG_GETARG_INT32(7);
	polar_password_policy_option_check(polar_password_policy_option[Anum_polar_password_policy_password_fail_count - 3].name, fail_count);
	lock_time = PG_GETARG_INT32(8);
	polar_password_policy_option_check(polar_password_policy_option[Anum_polar_password_policy_password_lock_time - 3].name, lock_time);
	if (min_length > max_length)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"%s\" must be less than \"%s\"",
						polar_password_policy_option[Anum_polar_password_policy_password_min_length - 3].name,
						polar_password_policy_option[Anum_polar_password_policy_password_max_length - 3].name)));

	polar_password_policy_create(pname, complexity, min_length, max_length, valid_time,
								 reuse_count, reuse_time, fail_count, lock_time);

	PG_RETURN_VOID();
}

/*
 * Alter a password policy.
 */
Datum
polar_alter_password_policy(PG_FUNCTION_ARGS)
{
	char	   *pname;
	char	   *oname;
	int			ovalue;
	int			attnum;
	Oid			schid;
	Oid			relid;
	Oid			indid;
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	TupleDesc	tupdesc;
	Datum		values[Natts_polar_password_policy];
	bool		nulls[Natts_polar_password_policy];
	bool		replaces[Natts_polar_password_policy];

	if (!polar_password_policy_enable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("polar_password_policy.enable is false")));

	/* Check parameter value */
	pname = NameStr(*(PG_GETARG_NAME(0)));
	oname = NameStr(*(PG_GETARG_NAME(1)));
	ovalue = PG_GETARG_INT32(2);
	attnum = polar_password_policy_option_check(oname, ovalue);

	schid = get_namespace_oid(POLAR_PASSWORD_POLICY_SCHEMA, false);
	relid = get_relname_relid(POLAR_PASSWORD_POLICY, schid);
	indid = get_relname_relid(POLAR_PASSWORD_POLICY_NAME_INDEX, schid);
	if (!OidIsValid(relid) || !OidIsValid(indid))
		PG_RETURN_VOID();

	rel = table_open(relid, RowExclusiveLock);

	/* Form a scan key and fetch a tuple */
	ScanKeyInit(&skey,
				Anum_polar_password_policy_name,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(pname));
	scan = systable_beginscan(rel, indid, true, NULL, 1, &skey);
	tup = systable_getnext(scan);

	/* Check whether the password policy exists */
	if (!HeapTupleIsValid(tup))
	{
		systable_endscan(scan);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("policy \"%s\" does not exist", pname)));
	}

	tupdesc = RelationGetDescr(rel);

	/* Check parameter value */
	if (attnum == Anum_polar_password_policy_password_min_length)
	{
		bool		null;
		int			max_length;

		max_length = DatumGetInt32(heap_getattr(tup, Anum_polar_password_policy_password_max_length, tupdesc, &null));
		if (max_length < ovalue)
		{
			systable_endscan(scan);
			table_close(rel, RowExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" must be less than \"%s\"",
							polar_password_policy_option[Anum_polar_password_policy_password_min_length - 3].name,
							polar_password_policy_option[Anum_polar_password_policy_password_max_length - 3].name)));
		}
	}
	else if (attnum == Anum_polar_password_policy_password_max_length)
	{
		bool		null;
		int			min_length;

		min_length = DatumGetInt32(heap_getattr(tup, Anum_polar_password_policy_password_min_length, tupdesc, &null));
		if (min_length > ovalue)
		{
			systable_endscan(scan);
			table_close(rel, RowExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("\"%s\" must be less than \"%s\"",
							polar_password_policy_option[Anum_polar_password_policy_password_min_length - 3].name,
							polar_password_policy_option[Anum_polar_password_policy_password_max_length - 3].name)));
		}
	}

	/* Form a tuple */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	values[attnum - 1] = Int32GetDatum(ovalue);
	replaces[attnum - 1] = true;

	/* Update a tuple */
	tup = heap_modify_tuple(tup, tupdesc, values, nulls, replaces);
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	heap_freetuple(tup);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * Rename a password policy.
 */
Datum
polar_rename_password_policy(PG_FUNCTION_ARGS)
{
	char	   *pname;
	char	   *pname_n;
	Oid			schid;
	Oid			relid;
	Oid			indid;
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	Datum		values[Natts_polar_password_policy];
	bool		nulls[Natts_polar_password_policy];
	bool		replaces[Natts_polar_password_policy];
	NameData	pnamedata_n;

	if (!polar_password_policy_enable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("polar_password_policy.enable is false")));

	pname = NameStr(*(PG_GETARG_NAME(0)));
	pname_n = NameStr(*(PG_GETARG_NAME(1)));
	if (strcmp(pname, DEFAULT_PASSWORD_POLICY) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("this is the default password policy and cannot be renamed")));
	else if (strcmp(pname_n, DEFAULT_PASSWORD_POLICY) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot be renamed to the default password policy")));
	else if (strcmp(pname, pname_n) == 0)
		PG_RETURN_VOID();

	schid = get_namespace_oid(POLAR_PASSWORD_POLICY_SCHEMA, false);
	relid = get_relname_relid(POLAR_PASSWORD_POLICY, schid);
	indid = get_relname_relid(POLAR_PASSWORD_POLICY_NAME_INDEX, schid);
	if (!OidIsValid(relid) || !OidIsValid(indid))
		PG_RETURN_VOID();

	rel = table_open(relid, RowExclusiveLock);

	/* Form a scan key and fetch a tuple -- new password policy name */
	ScanKeyInit(&skey,
				Anum_polar_password_policy_name,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(pname_n));
	scan = systable_beginscan(rel, indid, true, NULL, 1, &skey);
	tup = systable_getnext(scan);

	/* Check whether the password policy exists */
	if (HeapTupleIsValid(tup))
	{
		systable_endscan(scan);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("policy \"%s\" already exists", pname_n)));
	}

	systable_endscan(scan);

	/* Form a scan key and fetch a tuple -- old password policy name */
	ScanKeyInit(&skey,
				Anum_polar_password_policy_name,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(pname));
	scan = systable_beginscan(rel, indid, true, NULL, 1, &skey);
	tup = systable_getnext(scan);

	/* Check whether the password policy exists */
	if (!HeapTupleIsValid(tup))
	{
		systable_endscan(scan);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("policy \"%s\" does not exist", pname)));
	}

	/* Form a tuple */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	namestrcpy(&pnamedata_n, pname_n);
	values[Anum_polar_password_policy_name - 1] = NameGetDatum(&pnamedata_n);
	replaces[Anum_polar_password_policy_name - 1] = true;

	/* Update a tuple */
	tup = heap_modify_tuple(tup, RelationGetDescr(rel), values, nulls, replaces);
	CatalogTupleUpdate(rel, &tup->t_self, tup);

	heap_freetuple(tup);
	systable_endscan(scan);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * Drop a password policy.
 */
Datum
polar_drop_password_policy(PG_FUNCTION_ARGS)
{
	char	   *pname;
	Oid			schid;
	Oid			relid;
	Oid			indid;
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			polid;
	bool		isnull;
	Relation	rel_binding;
	SysScanDesc scan_binding;

	if (!polar_password_policy_enable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("polar_password_policy.enable is false")));

	pname = NameStr(*(PG_GETARG_NAME(0)));
	if (strcmp(pname, DEFAULT_PASSWORD_POLICY) == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("this is the default password policy and cannot be deleted")));

	schid = get_namespace_oid(POLAR_PASSWORD_POLICY_SCHEMA, false);
	relid = get_relname_relid(POLAR_PASSWORD_POLICY, schid);
	indid = get_relname_relid(POLAR_PASSWORD_POLICY_NAME_INDEX, schid);
	if (!OidIsValid(relid) || !OidIsValid(indid))
		PG_RETURN_VOID();

	rel = table_open(relid, RowExclusiveLock);

	/* Form a scan key and fetch a tuple */
	ScanKeyInit(&skey,
				Anum_polar_password_policy_name,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(pname));
	scan = systable_beginscan(rel, indid, true, NULL, 1, &skey);
	tup = systable_getnext(scan);

	/* Check whether the password policy exists */
	if (!HeapTupleIsValid(tup))
	{
		systable_endscan(scan);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("policy \"%s\" does not exist", pname)));
	}

	polid = DatumGetObjectId(heap_getattr(tup, Anum_polar_password_policy_oid, RelationGetDescr(rel), &isnull));

	/* Check whether a password policy is bound to the specified user */
	relid = get_relname_relid(POLAR_PASSWORD_POLICY_BINDING, schid);
	indid = get_relname_relid(POLAR_PASSWORD_POLICY_BINDING_POLICY_INDEX, schid);
	if (!OidIsValid(relid) || !OidIsValid(indid))
	{
		systable_endscan(scan);
		table_close(rel, RowExclusiveLock);
		PG_RETURN_VOID();
	}

	rel_binding = table_open(relid, AccessShareLock);

	/* Form a scan key and fetch a tuple */
	ScanKeyInit(&skey,
				Anum_polar_password_policy_binding_policy,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(polid));
	scan_binding = systable_beginscan(rel_binding, indid, true, NULL, 1, &skey);
	if (HeapTupleIsValid(systable_getnext(scan_binding)))
	{
		systable_endscan(scan_binding);
		systable_endscan(scan);
		table_close(rel_binding, AccessShareLock);
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" has been bound to the user, please unbind it first", pname)));
	}

	/* Delete a tuple */
	CatalogTupleDelete(rel, &tup->t_self);

	systable_endscan(scan_binding);
	systable_endscan(scan);
	table_close(rel_binding, AccessShareLock);
	table_close(rel, RowExclusiveLock);

	PG_RETURN_VOID();
}

/*
 * Record the binding relationship between users and password policies.
 */
static void
polar_bind_password_policy_to_user(Oid rolid, char *pname)
{
	Oid			schid;
	Oid			relid;
	Oid			indid;
	Relation	rel;
	ScanKeyData skey;
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			polid;
	bool		isnull;
	Relation	rel_binding;
	TupleDesc	tupdesc;
	Datum		values[Natts_polar_password_policy_binding];
	bool		nulls[Natts_polar_password_policy_binding];

	schid = get_namespace_oid(POLAR_PASSWORD_POLICY_SCHEMA, false);
	relid = get_relname_relid(POLAR_PASSWORD_POLICY, schid);
	indid = get_relname_relid(POLAR_PASSWORD_POLICY_NAME_INDEX, schid);
	if (!OidIsValid(relid) || !OidIsValid(indid))
		return;

	rel = table_open(relid, AccessShareLock);

	/* Form a scan key and fetch a tuple */
	ScanKeyInit(&skey,
				Anum_polar_password_policy_name,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(pname));
	scan = systable_beginscan(rel, indid, true, NULL, 1, &skey);
	tup = systable_getnext(scan);

	/* Check whether the password policy exists */
	if (!HeapTupleIsValid(tup))
	{
		systable_endscan(scan);
		table_close(rel, AccessShareLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("policy \"%s\" does not exist", pname)));
	}

	polid = DatumGetObjectId(heap_getattr(tup, Anum_polar_password_policy_oid, RelationGetDescr(rel), &isnull));
	systable_endscan(scan);

	/* Check whether a password policy is bound to the specified user */
	relid = get_relname_relid(POLAR_PASSWORD_POLICY_BINDING, schid);
	indid = get_relname_relid(POLAR_PASSWORD_POLICY_BINDING_ROLE_INDEX, schid);
	if (!OidIsValid(relid) || !OidIsValid(indid))
	{
		table_close(rel, AccessShareLock);
		return;
	}

	rel_binding = table_open(relid, RowExclusiveLock);
	tupdesc = RelationGetDescr(rel_binding);

	/* Form a scan key and fetch a tuple */
	ScanKeyInit(&skey,
				Anum_polar_password_policy_binding_role,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(rolid));
	scan = systable_beginscan(rel_binding, indid, true, NULL, 1, &skey);
	tup = systable_getnext(scan);

	/* Form a tuple */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	if (!HeapTupleIsValid(tup))
	{
		values[Anum_polar_password_policy_binding_role - 1] = ObjectIdGetDatum(rolid);
		values[Anum_polar_password_policy_binding_policy - 1] = ObjectIdGetDatum(polid);

		/* Insert a new tuple */
		tup = heap_form_tuple(tupdesc, values, nulls);
		CatalogTupleInsert(rel_binding, tup);
		heap_freetuple(tup);
	}
	else
	{
		Oid			polid_o = DatumGetObjectId(heap_getattr(tup, Anum_polar_password_policy_binding_policy, tupdesc, &isnull));

		if (polid_o != polid)
		{
			bool		replaces[Natts_polar_password_policy_binding];

			memset(replaces, false, sizeof(nulls));

			values[Anum_polar_password_policy_binding_policy - 1] = ObjectIdGetDatum(polid);
			replaces[Anum_polar_password_policy_binding_policy - 1] = true;

			/* Update a tuple */
			tup = heap_modify_tuple(tup, tupdesc, values, nulls, replaces);
			CatalogTupleUpdate(rel_binding, &tup->t_self, tup);
			heap_freetuple(tup);
		}
	}

	systable_endscan(scan);
	table_close(rel_binding, RowExclusiveLock);
	table_close(rel, AccessShareLock);
}

/*
 * Bind a user to a password policy.
 */
Datum
polar_bind_password_policy(PG_FUNCTION_ARGS)
{
	char	   *rname;
	char	   *pname;
	Oid			rolid;

	if (!polar_password_policy_enable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("polar_password_policy.enable is false")));

	rname = NameStr(*(PG_GETARG_NAME(0)));
	pname = NameStr(*(PG_GETARG_NAME(1)));
	rolid = get_role_oid(rname, false);
	polar_bind_password_policy_to_user(rolid, pname);

	PG_RETURN_VOID();
}

/*
 * Unbind a user from a password policy.
 */
Datum
polar_unbind_password_policy(PG_FUNCTION_ARGS)
{
	char	   *rname;
	Oid			rolid;

	if (!polar_password_policy_enable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("polar_password_policy.enable is false")));

	rname = NameStr(*(PG_GETARG_NAME(0)));
	rolid = get_role_oid(rname, false);
	polar_bind_password_policy_to_user(rolid, DEFAULT_PASSWORD_POLICY);

	PG_RETURN_VOID();
}

void
_PG_init(void)
{
	/* Define custom GUC variables */
	DefineCustomBoolVariable("polar_password_policy.enable",
							 "Whether to enable the password policy function",
							 NULL,
							 &polar_password_policy_enable,
							 false,
							 PGC_SIGHUP,
							 POLAR_GUC_IS_VISIBLE | POLAR_GUC_IS_CHANGABLE,
							 NULL,
							 NULL,
							 NULL);
}
