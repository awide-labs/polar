/*-------------------------------------------------------------------------
 *
 * polar_masking.h
 *	  Header for polar_masking.
 *
 * IDENTIFICATION
 *	  external/polar_masking/polar_masking.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _POLAR_MASKING_H_
#define _POLAR_MASKING_H_

#define MASKING_SCHEMA_NAME "polar_masking"

/*
 * Some definiton for polar_masking_label_tab.
 * The relation is used for saving masking table labels.
 */
#define MASKING_LABEL_TAB_RELNAME "polar_masking_label_tab"
#define Anum_polar_masking_label_tab_labelid 1
#define Anum_polar_masking_label_tab_relid 2
#define Natts_polar_masking_label_tab 2
#define MASKING_LABEL_TAB_RELID_IDXNAME "polar_masking_label_tab_relid_idx"

/*
 * Some definiton for polar_masking_label_col.
 * The relation is used for saving masking column labels.
 */
#define MASKING_LABEL_COL_RELNAME "polar_masking_label_col"
#define Anum_polar_masking_label_col_labelid 1
#define Anum_polar_masking_label_col_relid 2
#define Anum_polar_masking_label_col_colid 3
#define Natts_polar_masking_label_col 3
#define MASKING_LABEL_COL_RELID_COLID_IDXNAME "polar_masking_label_col_relid_colid_idx"

/*
 * Some definiton for polar_masking_policy.
 * The relation is used for saving masking labels and their binding relationships with masking operators.
 */
#define MASKING_POLICY_RELNAME "polar_masking_policy"
#define Anum_polar_masking_policy_labelid 1
#define Anum_polar_masking_policy_name 2
#define Anum_polar_masking_policy_operator 3
#define Natts_polar_masking_policy 3
#define MASKING_POLICY_LABELID_IDXNAME "polar_masking_policy_labelid_idx"

#endif
