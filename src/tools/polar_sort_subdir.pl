#!/usr/bin/perl

# polar_sort_subdir.pl
#	  Sort the SUBDIRS in ASCII order in unit of hunks of a specific file.
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
#	  src/tools/polar_sort_subdir.pl

use strict;
use warnings;

use Getopt::Long;

#------ logger prefix ------
my $logger_warn = "-->";
my $logger_log = "***";
my $logger_error = "FAILED :";

#--------------------------------------------------------------
# Input parameters
#     -f: check copyright for a specific file
#     -h: help
#--------------------------------------------------------------
my ($file_abs_path, $help);

my %options = (
	"f=s" => \$file_abs_path,
	"h" => \$help,);

GetOptions(%options) || die help();

if ($help)
{
	die help();
}

my %patterns = (
	'SUBDIRS \+=' => {},
	'ALWAYS_SUBDIRS \+=' => {}
	  # Add more patterns as needed
);

foreach my $pattern (keys %patterns)
{

	# Read input file
	open(my $fh, '<', $file_abs_path)
	  or die "Could not open file '$file_abs_path' $!";
	my @lines = <$fh>;
	close($fh);

	# Process and sort SUBDIRS lines as hunk
	my @sorted_lines;
	my @current_hunk;
	my $in_subdirs_section = 0;
	my %seen;
	foreach my $line (@lines)
	{
		if ($line =~ /^$pattern/)
		{
			$in_subdirs_section = 1;
			push @current_hunk, $line;
		}
		elsif ($in_subdirs_section)
		{
			$in_subdirs_section = 0;
			push @sorted_lines, sort { lc($a) cmp lc($b) } grep !$seen{$_}++,
			  @current_hunk;
			@current_hunk = ();
			push @sorted_lines, $line;
		}
		else
		{
			push @sorted_lines, $line;
		}
	}

	# Deduplicate SUBDIRS lines in the whole file
	my @deduplicated_lines = sort { lc($a) cmp lc($b) } grep !$seen{$_}++,
	  @sorted_lines;

	# Write the sorted lines back to the input file
	open(my $out_fh, '>', $file_abs_path)
	  or die "Could not open file '$file_abs_path' $!";
	print $out_fh @sorted_lines;
	close($out_fh);

}

#-----------------------------------------------------
# -h
#-----------------------------------------------------
sub help
{
	return '
polar_sort_subdir.pl [ -f file ]
    Sort the SUBDIRS in ASCII order in unit of hunks of a specific file.

    Syntax:  polar_sort_subdir.pl [ -option value | --option=value ]
    -f:		the file to be checked

';
}
