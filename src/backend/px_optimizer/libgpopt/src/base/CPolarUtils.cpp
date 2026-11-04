//---------------------------------------------------------------------------
//
// PolarDB PX Optimizer
//
//	Copyright (C) 2021, Alibaba Group Holding Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//	@filename:
//		CPolarUtils.cpp
//
//	@doc:
//		Polar Utils functions
//---------------------------------------------------------------------------
#include "gpopt/base/CPolarUtils.h"

using namespace gpopt;

PxPlannerStats& PxPlannerStats::get_px_planner_stats(void)
{
	static PxPlannerStats px_planner_stats;
	return px_planner_stats;
}

void PxPlannerStats::polar_set_error_flag(bool flag)
{
	PxPlannerStats::get_px_planner_stats().px_planner_error = flag;
}

bool PxPlannerStats::polar_get_error_flag(void)
{
	return PxPlannerStats::get_px_planner_stats().px_planner_error;
}

// EOF
