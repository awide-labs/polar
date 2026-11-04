//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2014 Pivotal Inc.
//	Copyright (C) 2021, Alibaba Group Holding Limited
//
//	@filename:
//		CCostModelParamsPolarDB.cpp
//
//	@doc:
//		Parameters of PolarDB cost model
//---------------------------------------------------------------------------

#include "gpos/base.h"
#include "gpos/string/CWStringConst.h"
#include "gpdbcost/CCostModelParamsPolarDB.h"

using namespace gpopt;

// sequential i/o bandwidth
const CDouble CCostModelParamsPolarDB::DSeqIOBandwidthVal = 1024.0;

// random i/o bandwidth
const CDouble CCostModelParamsPolarDB::DRandomIOBandwidthVal = 30.0;

// tuple processing bandwidth
const CDouble CCostModelParamsPolarDB::DTupProcBandwidthVal = 512.0;

// output bandwidth
const CDouble CCostModelParamsPolarDB::DOutputBandwidthVal = 256.0;

// scan initialization cost factor
const CDouble CCostModelParamsPolarDB::DInitScanFacorVal = 431.0;

// table scan cost unit
const CDouble CCostModelParamsPolarDB::DTableScanCostUnitVal = 5.50e-07;

// index scan initialization cost factor
const CDouble CCostModelParamsPolarDB::DInitIndexScanFactorVal = 142.0;

// index block cost unit
const CDouble CCostModelParamsPolarDB::DIndexBlockCostUnitVal = 1.27e-06;

// index filtering cost unit
const CDouble CCostModelParamsPolarDB::DIndexFilterCostUnitVal = 1.65e-04;

// index scan cost unit per tuple per width
const CDouble CCostModelParamsPolarDB::DIndexScanTupCostUnitVal = 3.66e-06;

// index scan random IO factor
const CDouble CCostModelParamsPolarDB::DIndexScanTupRandomFactorVal = 6.0;

// filter column cost unit
const CDouble CCostModelParamsPolarDB::DFilterColCostUnitVal = 3.29e-05;

// output tuple cost unit
const CDouble CCostModelParamsPolarDB::DOutputTupCostUnitVal = 1.86e-06;

// sending tuple cost unit in gather motion
const CDouble CCostModelParamsPolarDB::DGatherSendCostUnitVal = 4.58e-06;

// receiving tuple cost unit in gather motion
const CDouble CCostModelParamsPolarDB::DGatherRecvCostUnitVal = 2.20e-06;

// sending tuple cost unit in redistribute motion
const CDouble CCostModelParamsPolarDB::DRedistributeSendCostUnitVal = 2.33e-06;

// receiving tuple cost unit in redistribute motion
const CDouble CCostModelParamsPolarDB::DRedistributeRecvCostUnitVal = 8.0e-07;

// sending tuple cost unit in broadcast motion
const CDouble CCostModelParamsPolarDB::DBroadcastSendCostUnitVal = 4.965e-05;

// receiving tuple cost unit in broadcast motion
const CDouble CCostModelParamsPolarDB::DBroadcastRecvCostUnitVal = 1.35e-06;

// tuple cost unit in No-Op motion
const CDouble CCostModelParamsPolarDB::DNoOpCostUnitVal = 0;

// feeding cost per tuple per column in join operator
const CDouble CCostModelParamsPolarDB::DJoinFeedingTupColumnCostUnitVal = 8.69e-05;

// feeding cost per tuple per width in join operator
const CDouble CCostModelParamsPolarDB::DJoinFeedingTupWidthCostUnitVal = 6.09e-07;

// output cost per tuple in join operator
const CDouble CCostModelParamsPolarDB::DJoinOutputTupCostUnitVal = 3.50e-06;

// memory threshold for hash join spilling (in bytes)
const CDouble CCostModelParamsPolarDB::DHJSpillingMemThresholdVal = 50 * 1024 * 1024;

// initial cost for building hash table for hash join
const CDouble CCostModelParamsPolarDB::DHJHashTableInitCostFactorVal = 500.0;

// building hash table cost per tuple per column
const CDouble CCostModelParamsPolarDB::DHJHashTableColumnCostUnitVal = 5.0e-05;

// the unit cost to process each tuple with unit width when building a hash table
const CDouble CCostModelParamsPolarDB::DHJHashTableWidthCostUnitVal = 3.0e-06;

// hashing cost per tuple with unit width in hash join
const CDouble CCostModelParamsPolarDB::DHJHashingTupWidthCostUnitVal = 1.97e-05;

// feeding cost per tuple per column in hash join if spilling
const CDouble CCostModelParamsPolarDB::DHJFeedingTupColumnSpillingCostUnitVal = 1.97e-04;

// feeding cost per tuple with unit width in hash join if spilling
const CDouble CCostModelParamsPolarDB::DHJFeedingTupWidthSpillingCostUnitVal = 3.0e-06;

// hashing cost per tuple with unit width in hash join if spilling
const CDouble CCostModelParamsPolarDB::DHJHashingTupWidthSpillingCostUnitVal = 2.30e-05;

// cost for building hash table for per tuple per grouping column in hash aggregate
const CDouble CCostModelParamsPolarDB::DHashAggInputTupColumnCostUnitVal = 1.20e-04;

// cost for building hash table for per tuple with unit width in hash aggregate
const CDouble CCostModelParamsPolarDB::DHashAggInputTupWidthCostUnitVal = 1.12e-07;

// cost for outputting for per tuple with unit width in hash aggregate
const CDouble CCostModelParamsPolarDB::DHashAggOutputTupWidthCostUnitVal = 5.61e-07;

// sorting cost per tuple with unit width
const CDouble CCostModelParamsPolarDB::DSortTupWidthCostUnitVal = 5.67e-06;

// cost for processing per tuple with unit width
const CDouble CCostModelParamsPolarDB::DTupDefaultProcCostUnitVal = 1.0e-06;

// cost for materializing per tuple with unit width
const CDouble CCostModelParamsPolarDB::DMaterializeCostUnitVal = 4.68e-06;

// tuple update bandwidth
const CDouble CCostModelParamsPolarDB::DTupUpdateBandwidthVal = 256.0;

// network bandwidth
const CDouble CCostModelParamsPolarDB::DNetBandwidthVal = 1024.0;

// number of segments
const CDouble CCostModelParamsPolarDB::DSegmentsVal = 4.0;

// nlj factor
const CDouble CCostModelParamsPolarDB::DNLJFactorVal = 1.0;

// hj factor
const CDouble CCostModelParamsPolarDB::DHJFactorVal = 2.5;

// hash building factor
const CDouble CCostModelParamsPolarDB::DHashFactorVal = 2.0;

// default cost
const CDouble CCostModelParamsPolarDB::DDefaultCostVal = 100.0;

// largest estimation risk for which we don't penalize index join
const CDouble CCostModelParamsPolarDB::DIndexJoinAllowedRiskThreshold = 3;

// default bitmap IO co-efficient when NDV is larger
const CDouble CCostModelParamsPolarDB::DBitmapIOCostLargeNDV(0.0082);

// default bitmap IO co-efficient when NDV is smaller
const CDouble CCostModelParamsPolarDB::DBitmapIOCostSmallNDV(0.2138);

// default bitmap page cost when NDV is larger
const CDouble CCostModelParamsPolarDB::DBitmapPageCostLargeNDV(83.1651);

// default bitmap page cost when NDV is larger
const CDouble CCostModelParamsPolarDB::DBitmapPageCostSmallNDV(204.3810);

// default bitmap page cost with no assumption about NDV
const CDouble CCostModelParamsPolarDB::DBitmapPageCost(50.4381);

// default threshold of NDV for bitmap costing
const CDouble CCostModelParamsPolarDB::DBitmapNDVThreshold(200);

// cost of a bitmap scan rebind
const CDouble CCostModelParamsPolarDB::DBitmapScanRebindCost(0.06);

// see CCostModelPolarDB::CostHashJoin() for why this is needed
const CDouble CCostModelParamsPolarDB::DPenalizeHJSkewUpperLimit(10.0);

// POLAR px: table share scan factor
const CDouble CCostModelParamsPolarDB::DPolarTableShareScanFactor(1.0);

// POLAR px: share index scan factor
const CDouble CCostModelParamsPolarDB::DPolarShareIndexScanFactor(1.0);

#define GPOPT_COSTPARAM_NAME_MAX_LENGTH		80

// parameter names in the same order of param enumeration
const CHAR rgszCostParamNames[CCostModelParamsPolarDB::EcpSentinel][GPOPT_COSTPARAM_NAME_MAX_LENGTH] =
	{
	"SeqIOBandwidth",
	"RandomIOBandwidth",
	"TupProcBandwidth",
	"OutputBandwidth",
	"InitScanFacor",
	"TableScanCostUnit",
	"InitIndexScanFactor",
	"IndexBlockCostUnit",
	"IndexFilterCostUnit",
	"IndexScanTupCostUnit",
	"IndexScanTupRandomFactor",
	"FilterColCostUnit",
	"OutputTupCostUnit",
	"GatherSendCostUnit",
	"GatherRecvCostUnit",
	"RedistributeSendCostUnit",
	"RedistributeRecvCostUnit",
	"BroadcastSendCostUnit",
	"BroadcastRecvCostUnit",
	"NoOpCostUnit",
	"JoinFeedingTupColumnCostUnit",
	"JoinFeedingTupWidthCostUnit",
	"JoinOutputTupCostUnit",
	"HJSpillingMemThreshold",
	"HJHashTableInitCostFactor",
	"HJHashTableColumnCostUnit",
	"HJHashTableWidthCostUnit",
	"HJHashingTupWidthCostUnit",
	"HJFeedingTupColumnSpillingCostUnit",
	"HJFeedingTupWidthSpillingCostUnit",
	"HJHashingTupWidthSpillingCostUnit",
	"HashAggInputTupColumnCostUnit",
	"HashAggInputTupWidthCostUnit",
	"HashAggOutputTupWidthCostUnit",
	"SortTupWidthCostUnit",
	"TupDefaultProcCostUnit",
	"MaterializeCostUnit",
	"TupUpdateBandwidth",
	"NetworkBandwidth",
	"Segments",
	"NLJFactor",
	"HJFactor",
	"HashFactor",
	"DefaultCost",
	"IndexJoinAllowedRiskThreshold",
	"BitmapIOLargerNDV",
	"BitmapIOSmallerNDV",
	"BitmapPageCostLargerNDV",
	"BitmapPageCostSmallerNDV",
	"BitmapNDVThreshold",

	/* POLAR px */
	"PolarTableShareScanFactor",
	"PolarShareIndexScanFactor",
	};

//---------------------------------------------------------------------------
//	@function:
//		CCostModelParamsPolarDB::CCostModelParamsPolarDB
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CCostModelParamsPolarDB::CCostModelParamsPolarDB
	(
	CMemoryPool *mp
	)
	:
	m_mp(mp)
{
	GPOS_ASSERT(NULL != mp);

	for (ULONG ul = 0; ul < EcpSentinel; ul++)
	{
		m_rgpcp[ul] = NULL;
	}

	// populate param array with default param values
	m_rgpcp[EcpSeqIOBandwidth] = GPOS_NEW(mp) SCostParam(EcpSeqIOBandwidth, DSeqIOBandwidthVal, DSeqIOBandwidthVal - 128.0, DSeqIOBandwidthVal + 128.0);
	m_rgpcp[EcpRandomIOBandwidth] = GPOS_NEW(mp) SCostParam(EcpRandomIOBandwidth, DRandomIOBandwidthVal, DRandomIOBandwidthVal - 8.0, DRandomIOBandwidthVal + 8.0);
	m_rgpcp[EcpTupProcBandwidth] = GPOS_NEW(mp) SCostParam(EcpTupProcBandwidth, DTupProcBandwidthVal, DTupProcBandwidthVal - 32.0, DTupProcBandwidthVal + 32.0);
	m_rgpcp[EcpOutputBandwidth] = GPOS_NEW(mp) SCostParam(EcpOutputBandwidth, DOutputBandwidthVal, DOutputBandwidthVal - 32.0, DOutputBandwidthVal + 32.0);
	m_rgpcp[EcpInitScanFactor] = GPOS_NEW(mp) SCostParam(EcpInitScanFactor, DInitScanFacorVal, DInitScanFacorVal - 2.0, DInitScanFacorVal + 2.0);
	m_rgpcp[EcpTableScanCostUnit] = GPOS_NEW(mp) SCostParam(EcpTableScanCostUnit, DTableScanCostUnitVal, DTableScanCostUnitVal - 1.0, DTableScanCostUnitVal + 1.0);
	m_rgpcp[EcpInitIndexScanFactor] = GPOS_NEW(mp) SCostParam(EcpInitIndexScanFactor, DInitIndexScanFactorVal, DInitIndexScanFactorVal - 1.0, DInitIndexScanFactorVal + 1.0);
	m_rgpcp[EcpIndexBlockCostUnit] = GPOS_NEW(mp) SCostParam(EcpIndexBlockCostUnit, DIndexBlockCostUnitVal, DIndexBlockCostUnitVal - 1.0, DIndexBlockCostUnitVal + 1.0);
	m_rgpcp[EcpIndexFilterCostUnit] = GPOS_NEW(mp) SCostParam(EcpIndexFilterCostUnit, DIndexFilterCostUnitVal, DIndexFilterCostUnitVal - 1.0, DIndexFilterCostUnitVal + 1.0);
	m_rgpcp[EcpIndexScanTupCostUnit] = GPOS_NEW(mp) SCostParam(EcpIndexScanTupCostUnit, DIndexScanTupCostUnitVal, DIndexScanTupCostUnitVal - 1.0, DIndexScanTupCostUnitVal + 1.0);
	m_rgpcp[EcpIndexScanTupRandomFactor] = GPOS_NEW(mp) SCostParam(EcpIndexScanTupRandomFactor, DIndexScanTupRandomFactorVal, DIndexScanTupRandomFactorVal - 1.0, DIndexScanTupRandomFactorVal + 1.0);
	m_rgpcp[EcpFilterColCostUnit] = GPOS_NEW(mp) SCostParam(EcpFilterColCostUnit, DFilterColCostUnitVal, DFilterColCostUnitVal - 1.0, DFilterColCostUnitVal + 1.0);
	m_rgpcp[EcpOutputTupCostUnit] = GPOS_NEW(mp) SCostParam(EcpOutputTupCostUnit, DOutputTupCostUnitVal, DOutputTupCostUnitVal - 1.0, DOutputTupCostUnitVal + 1.0);
	m_rgpcp[EcpGatherSendCostUnit] = GPOS_NEW(mp) SCostParam(EcpGatherSendCostUnit, DGatherSendCostUnitVal, DGatherSendCostUnitVal - 0.0, DGatherSendCostUnitVal + 0.0);
	m_rgpcp[EcpGatherRecvCostUnit] = GPOS_NEW(mp) SCostParam(EcpGatherRecvCostUnit, DGatherRecvCostUnitVal, DGatherRecvCostUnitVal - 0.0, DGatherRecvCostUnitVal + 0.0);
	m_rgpcp[EcpRedistributeSendCostUnit] = GPOS_NEW(mp) SCostParam(EcpRedistributeSendCostUnit, DRedistributeSendCostUnitVal, DRedistributeSendCostUnitVal - 0.0, DRedistributeSendCostUnitVal + 0.0);
	m_rgpcp[EcpRedistributeRecvCostUnit] = GPOS_NEW(mp) SCostParam(EcpRedistributeRecvCostUnit, DRedistributeRecvCostUnitVal, DRedistributeRecvCostUnitVal - 0.0, DRedistributeRecvCostUnitVal + 0.0);
	m_rgpcp[EcpBroadcastSendCostUnit] = GPOS_NEW(mp) SCostParam(EcpBroadcastSendCostUnit, DBroadcastSendCostUnitVal, DBroadcastSendCostUnitVal - 0.0, DBroadcastSendCostUnitVal + 0.0);
	m_rgpcp[EcpBroadcastRecvCostUnit] = GPOS_NEW(mp) SCostParam(EcpBroadcastRecvCostUnit, DBroadcastRecvCostUnitVal, DBroadcastRecvCostUnitVal - 0.0, DBroadcastRecvCostUnitVal + 0.0);
	m_rgpcp[EcpNoOpCostUnit] = GPOS_NEW(mp) SCostParam(EcpNoOpCostUnit, DNoOpCostUnitVal, DNoOpCostUnitVal - 0.0, DNoOpCostUnitVal + 0.0);
	m_rgpcp[EcpJoinFeedingTupColumnCostUnit] = GPOS_NEW(mp) SCostParam(EcpJoinFeedingTupColumnCostUnit, DJoinFeedingTupColumnCostUnitVal, DJoinFeedingTupColumnCostUnitVal - 0.0, DJoinFeedingTupColumnCostUnitVal + 0.0);
	m_rgpcp[EcpJoinFeedingTupWidthCostUnit] = GPOS_NEW(mp) SCostParam(EcpJoinFeedingTupWidthCostUnit, DJoinFeedingTupWidthCostUnitVal, DJoinFeedingTupWidthCostUnitVal - 0.0, DJoinFeedingTupWidthCostUnitVal + 0.0);
	m_rgpcp[EcpJoinOutputTupCostUnit] = GPOS_NEW(mp) SCostParam(EcpJoinOutputTupCostUnit, DJoinOutputTupCostUnitVal, DJoinOutputTupCostUnitVal - 0.0, DJoinOutputTupCostUnitVal + 0.0);
	m_rgpcp[EcpHJSpillingMemThreshold] = GPOS_NEW(mp) SCostParam(EcpHJSpillingMemThreshold, DHJSpillingMemThresholdVal, DHJSpillingMemThresholdVal - 0.0, DHJSpillingMemThresholdVal + 0.0);
	m_rgpcp[EcpHJHashTableInitCostFactor] = GPOS_NEW(mp) SCostParam(EcpHJHashTableInitCostFactor, DHJHashTableInitCostFactorVal, DHJHashTableInitCostFactorVal - 0.0, DHJHashTableInitCostFactorVal + 0.0);
	m_rgpcp[EcpHJHashTableColumnCostUnit] = GPOS_NEW(mp) SCostParam(EcpHJHashTableColumnCostUnit, DHJHashTableColumnCostUnitVal, DHJHashTableColumnCostUnitVal - 0.0, DHJHashTableColumnCostUnitVal + 0.0);
	m_rgpcp[EcpHJHashTableWidthCostUnit] = GPOS_NEW(mp) SCostParam(EcpHJHashTableWidthCostUnit, DHJHashTableWidthCostUnitVal, DHJHashTableWidthCostUnitVal - 0.0, DHJHashTableWidthCostUnitVal + 0.0);
	m_rgpcp[EcpHJHashingTupWidthCostUnit] = GPOS_NEW(mp) SCostParam(EcpHJHashingTupWidthCostUnit, DHJHashingTupWidthCostUnitVal, DHJHashingTupWidthCostUnitVal - 0.0, DHJHashingTupWidthCostUnitVal + 0.0);
	m_rgpcp[EcpHJFeedingTupColumnSpillingCostUnit] = GPOS_NEW(mp) SCostParam(EcpHJFeedingTupColumnSpillingCostUnit, DHJFeedingTupColumnSpillingCostUnitVal, DHJFeedingTupColumnSpillingCostUnitVal - 0.0, DHJFeedingTupColumnSpillingCostUnitVal + 0.0);
	m_rgpcp[EcpHJFeedingTupWidthSpillingCostUnit] = GPOS_NEW(mp) SCostParam(EcpHJFeedingTupWidthSpillingCostUnit, DHJFeedingTupWidthSpillingCostUnitVal, DHJFeedingTupWidthSpillingCostUnitVal - 0.0, DHJFeedingTupWidthSpillingCostUnitVal + 0.0);
	m_rgpcp[EcpHJHashingTupWidthSpillingCostUnit] = GPOS_NEW(mp) SCostParam(EcpHJHashingTupWidthSpillingCostUnit, DHJHashingTupWidthSpillingCostUnitVal, DHJHashingTupWidthSpillingCostUnitVal - 0.0, DHJHashingTupWidthSpillingCostUnitVal + 0.0);
	m_rgpcp[EcpHashAggInputTupColumnCostUnit] = GPOS_NEW(mp) SCostParam(EcpHashAggInputTupColumnCostUnit, DHashAggInputTupColumnCostUnitVal, DHashAggInputTupColumnCostUnitVal - 0.0, DHashAggInputTupColumnCostUnitVal + 0.0);
	m_rgpcp[EcpHashAggInputTupWidthCostUnit] = GPOS_NEW(mp) SCostParam(EcpHashAggInputTupWidthCostUnit, DHashAggInputTupWidthCostUnitVal, DHashAggInputTupWidthCostUnitVal - 0.0, DHashAggInputTupWidthCostUnitVal + 0.0);
	m_rgpcp[EcpHashAggOutputTupWidthCostUnit] = GPOS_NEW(mp) SCostParam(EcpHashAggOutputTupWidthCostUnit, DHashAggOutputTupWidthCostUnitVal, DHashAggOutputTupWidthCostUnitVal - 0.0, DHashAggOutputTupWidthCostUnitVal + 0.0);
	m_rgpcp[EcpSortTupWidthCostUnit] = GPOS_NEW(mp) SCostParam(EcpSortTupWidthCostUnit, DSortTupWidthCostUnitVal, DSortTupWidthCostUnitVal - 0.0, DSortTupWidthCostUnitVal + 0.0);
	m_rgpcp[EcpTupDefaultProcCostUnit] = GPOS_NEW(mp) SCostParam(EcpTupDefaultProcCostUnit, DTupDefaultProcCostUnitVal, DTupDefaultProcCostUnitVal - 0.0, DTupDefaultProcCostUnitVal + 0.0);
	m_rgpcp[EcpMaterializeCostUnit] = GPOS_NEW(mp) SCostParam(EcpMaterializeCostUnit, DMaterializeCostUnitVal, DMaterializeCostUnitVal - 0.0, DMaterializeCostUnitVal + 0.0);
	m_rgpcp[EcpTupUpdateBandwith] = GPOS_NEW(mp) SCostParam(EcpTupUpdateBandwith, DTupUpdateBandwidthVal, DTupUpdateBandwidthVal - 32.0, DTupUpdateBandwidthVal + 32.0);
	m_rgpcp[EcpNetBandwidth] = GPOS_NEW(mp) SCostParam(EcpNetBandwidth, DNetBandwidthVal, DNetBandwidthVal - 128.0, DNetBandwidthVal + 128.0);
	m_rgpcp[EcpSegments] = GPOS_NEW(mp) SCostParam(EcpSegments, DSegmentsVal, DSegmentsVal - 2.0, DSegmentsVal + 2.0);
	m_rgpcp[EcpNLJFactor] = GPOS_NEW(mp) SCostParam(EcpNLJFactor, DNLJFactorVal, DNLJFactorVal - 0.5, DNLJFactorVal + 0.5);
	m_rgpcp[EcpHJFactor] = GPOS_NEW(mp) SCostParam(EcpHJFactor, DHJFactorVal, DHJFactorVal - 1.0, DHJFactorVal + 1.0);
	m_rgpcp[EcpHashFactor] = GPOS_NEW(mp) SCostParam(EcpHashFactor, DHashFactorVal, DHashFactorVal - 1.0, DHashFactorVal + 1.0);
	m_rgpcp[EcpDefaultCost] = GPOS_NEW(mp) SCostParam(EcpDefaultCost, DDefaultCostVal, DDefaultCostVal - 32.0, DDefaultCostVal + 32.0);
	m_rgpcp[EcpIndexJoinAllowedRiskThreshold] = GPOS_NEW(mp) SCostParam(EcpIndexJoinAllowedRiskThreshold, DIndexJoinAllowedRiskThreshold, 0, gpos::ulong_max);
	m_rgpcp[EcpBitmapIOCostLargeNDV] = GPOS_NEW(mp) SCostParam(EcpBitmapIOCostLargeNDV, DBitmapIOCostLargeNDV, DBitmapIOCostLargeNDV - 0.0001, DBitmapIOCostLargeNDV + 0.0001);
	m_rgpcp[EcpBitmapIOCostSmallNDV] = GPOS_NEW(mp) SCostParam(EcpBitmapIOCostSmallNDV, DBitmapIOCostSmallNDV, DBitmapIOCostSmallNDV - 0.0001, DBitmapIOCostSmallNDV + 0.0001);
	m_rgpcp[EcpBitmapPageCostLargeNDV] = GPOS_NEW(mp) SCostParam(EcpBitmapPageCostLargeNDV, DBitmapPageCostLargeNDV, DBitmapPageCostLargeNDV - 1.0, DBitmapPageCostLargeNDV + 1.0);
	m_rgpcp[EcpBitmapPageCostSmallNDV] = GPOS_NEW(mp) SCostParam(EcpBitmapPageCostSmallNDV, DBitmapPageCostSmallNDV, DBitmapPageCostSmallNDV - 1.0, DBitmapPageCostSmallNDV + 1.0);
	m_rgpcp[EcpBitmapPageCost] = GPOS_NEW(mp) SCostParam(EcpBitmapPageCost, DBitmapPageCost, DBitmapPageCost - 1.0, DBitmapPageCost + 1.0);
	m_rgpcp[EcpBitmapNDVThreshold] = GPOS_NEW(mp) SCostParam(EcpBitmapNDVThreshold, DBitmapNDVThreshold, DBitmapNDVThreshold - 1.0, DBitmapNDVThreshold + 1.0);
	m_rgpcp[EcpBitmapScanRebindCost] = GPOS_NEW(mp) SCostParam(EcpBitmapScanRebindCost, DBitmapScanRebindCost, DBitmapScanRebindCost - 1.0, DBitmapScanRebindCost + 1.0);
	m_rgpcp[EcpPenalizeHJSkewUpperLimit] = GPOS_NEW(mp) SCostParam(EcpPenalizeHJSkewUpperLimit, DPenalizeHJSkewUpperLimit, DPenalizeHJSkewUpperLimit - 1.0, DPenalizeHJSkewUpperLimit + 1.0);

	// POLAR px
	m_rgpcp[EcpPolarTableShareScanFactor] = GPOS_NEW(mp) SCostParam(EcpPolarTableShareScanFactor, DPolarTableShareScanFactor,DPolarTableShareScanFactor - 0.0, DPolarTableShareScanFactor + 0.0);
	m_rgpcp[EcpPolarShareIndexScanFactor] = GPOS_NEW(mp) SCostParam(EcpPolarShareIndexScanFactor, DPolarShareIndexScanFactor,DPolarShareIndexScanFactor - 0.0, DPolarShareIndexScanFactor + 0.0);
}


//---------------------------------------------------------------------------
//	@function:
//		CCostModelParamsPolarDB::~CCostModelParamsPolarDB
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CCostModelParamsPolarDB::~CCostModelParamsPolarDB()
{
	for (ULONG ul = 0; ul < EcpSentinel; ul++)
	{
		GPOS_DELETE(m_rgpcp[ul]);
		m_rgpcp[ul] = NULL;
	}
}


//---------------------------------------------------------------------------
//	@function:
//		CCostModelParamsPolarDB::PcpLookup
//
//	@doc:
//		Lookup param by id;
//
//
//---------------------------------------------------------------------------
CCostModelParamsPolarDB::SCostParam *
CCostModelParamsPolarDB::PcpLookup
	(
	ULONG id
	)
	const
{
	ECostParam ecp = (ECostParam) id;
	GPOS_ASSERT(EcpSentinel > ecp);

	return m_rgpcp[ecp];
}


//---------------------------------------------------------------------------
//	@function:
//		CCostModelParamsPolarDB::PcpLookup
//
//	@doc:
//		Lookup param by name;
//		return NULL if name is not recognized
//
//---------------------------------------------------------------------------
CCostModelParamsPolarDB::SCostParam *
CCostModelParamsPolarDB::PcpLookup
	(
	const CHAR *szName
	)
	const
{
	GPOS_ASSERT(NULL != szName);

	for (ULONG ul = 0; ul < EcpSentinel; ul++)
	{
		if (0 == clib::Strcmp(szName, rgszCostParamNames[ul]))
		{
			return PcpLookup((ECostParam) ul);
		}
	}

	return NULL;
}


//---------------------------------------------------------------------------
//	@function:
//		CCostModelParamsPolarDB::SetParam
//
//	@doc:
//		Set param by id
//
//---------------------------------------------------------------------------
void
CCostModelParamsPolarDB::SetParam
	(
	ULONG id,
	CDouble dVal,
	CDouble dLowerBound,
	CDouble dUpperBound
	)
{
	ECostParam ecp = (ECostParam) id;
	GPOS_ASSERT(EcpSentinel > ecp);

	GPOS_DELETE(m_rgpcp[ecp]);
	m_rgpcp[ecp] = NULL;
	m_rgpcp[ecp] =  GPOS_NEW(m_mp) SCostParam(ecp, dVal, dLowerBound, dUpperBound);
}


//---------------------------------------------------------------------------
//	@function:
//		CCostModelParamsPolarDB::SetParam
//
//	@doc:
//		Set param by name
//
//---------------------------------------------------------------------------
void
CCostModelParamsPolarDB::SetParam
	(
	const CHAR *szName,
	CDouble dVal,
	CDouble dLowerBound,
	CDouble dUpperBound
	)
{
	GPOS_ASSERT(NULL != szName);

	for (ULONG ul = 0; ul < EcpSentinel; ul++)
	{
		if (0 == clib::Strcmp(szName, rgszCostParamNames[ul]))
		{
			GPOS_DELETE(m_rgpcp[ul]);
			m_rgpcp[ul] = NULL;
			m_rgpcp[ul] = GPOS_NEW(m_mp) SCostParam(ul, dVal, dLowerBound, dUpperBound);

			return;
		}
	}
}


//---------------------------------------------------------------------------
//	@function:
//		CCostModelParamsPolarDB::OsPrint
//
//	@doc:
//		Print function
//
//---------------------------------------------------------------------------
IOstream &
CCostModelParamsPolarDB::OsPrint
	(
	IOstream &os
	)
	const
{
	for (ULONG ul = 0; ul < EcpSentinel; ul++)
	{
		SCostParam *pcp = PcpLookup((ECostParam) ul);
		os
			<< rgszCostParamNames[ul] << " : "
			<< pcp->Get()
			<< "  [" << pcp->GetLowerBoundVal() << "," << pcp->GetUpperBoundVal() <<"]"
			<< std::endl;
	}
	return os;
}

BOOL CCostModelParamsPolarDB::Equals(ICostModelParams *pcm) const
{
	CCostModelParamsPolarDB *pcmgOther = dynamic_cast<CCostModelParamsPolarDB *>(pcm);
	if (NULL == pcmgOther)
		return false;

	for (ULONG ul = 0U; ul < GPOS_ARRAY_SIZE(m_rgpcp); ul++)
	{
		if (!m_rgpcp[ul]->Equals(pcmgOther->m_rgpcp[ul]))
			return false;
	}

	return true;
}

const CHAR *
CCostModelParamsPolarDB::SzNameLookup(ULONG id) const
{
	ECostParam ecp = (ECostParam) id;
	GPOS_ASSERT(EcpSentinel > ecp);
	return rgszCostParamNames[ecp];
}

// EOF
