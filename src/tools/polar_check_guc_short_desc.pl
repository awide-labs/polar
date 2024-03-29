#!/usr/bin/perl
# polar_check_guc_short_desc.pl
#
# Copyright (c) 2024, Alibaba Group Holding Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# IDENTIFICATION
#	  src/tools/polar_check_guc_short_desc.pl

# Fixed the lack of a period in the short desc of the guc

sub process_guc_short_desc
{
	open(FH, $_[0]) or die "Cannot open for read";
	my @array = <FH>;
	my $need_process = 0;
	my $begin_process = 0;
	my $line_num = 0;
	foreach $a (@array)
	{
		# Anything preceding this text does not need to be processed
		if (index($a, "ConfigureNamesBool") > -1)
		{
			$begin_process = 1;
		}
		if ($begin_process == 1)
		{
			$need_process = 0;
			if (index($a, "gettext_noop") > -1)
			{
				$need_process = 1;

				if (index($a, "gettext_noop(\"(") > -1)
				{
					$need_process = 0;
				}
				if (index($a, "gettext_noop(\"^") > -1)
				{
					$need_process = 0;
				}
				if (index($a, "?\")") > -1)
				{
					$need_process = 0;
				}
			}

			if ($need_process == 1)
			{
				my $old_str = $a;
				$a =~ s/(([^.])("\)))/$2.$3/;
				if (!($old_str eq $a))
				{
					$array[$line_num] = $a;
				}
			}
		}
		$line_num++;
	}
	close FH;

	open(FH, ">", $_[0]) or die "Cannot open for write";
	print FH @array;
	close FH;
}

process_guc_short_desc("src/backend/utils/misc/guc.c");
# process_guc_short_desc("src/backend/utils/misc/guc_px.c");
