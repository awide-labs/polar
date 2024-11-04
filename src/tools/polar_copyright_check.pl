#!/usr/bin/perl

# polar_copyright_check.pl
#	  Update copyright for a specific file.
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
#	  src/tools/polar_copyright_check.pl

use strict;
use warnings;

use File::Find;
use File::Find::Rule;
use File::Basename;
use Tie::File;
use Getopt::Long;

#------ get current year ------
my $year = 1900 + ${ [ localtime(time) ] }[5];

#------ Apache 2.0 license template ------
my @license = (
	'Licensed under the Apache License, Version 2.0 (the "License");',
	'you may not use this file except in compliance with the License.',
	'You may obtain a copy of the License at',
	'',
	'http://www.apache.org/licenses/LICENSE-2.0',
	'',
	'Unless required by applicable law or agreed to in writing, software',
	'distributed under the License is distributed on an "AS IS" BASIS,',
	'WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.',
	'See the License for the specific language governing permissions and',
	'limitations under the License.');
my $license_line_len = @license;

my $portions = 'Portions';
my $cc = 'Copyright \(c\)';
my $ccliteral = 'Copyright (c)';
my $pgdg = 'PostgreSQL Global Development Group';
my $gpdb = 'Greenplum';
my $pivotal = 'Pivotal Software';
my $alibaba = 'Alibaba';
my $alibaba_inc = 'Alibaba Group Holding Limited';
my $identification = 'IDENTIFICATION';

my $invalid_comment_body = 'Invalid comment body';

my @common_typo = (
	'poalr', 'wirte', 'wrod', 'confict',
	'enalbe', 'cleard', 'recognisable', 'exsits',
	'conficts', 'sucess');
my @standard_comment_prefix = ('\/\* POLAR px\:', '\/\* POLAR\:');

#------ the max diff line before checking PG community files ------
my $max_diff_line = 80;

#------ logger prefix ------
my $logger_warn = "-->";
my $logger_log = "***";
my $logger_error = "FAILED :";

#--------------------------------------------------------------
# Input parameters
#     -f: check copyright for a specific file
#     -p: check copyright for all files from a specific commit
#     -s: check common spelling
#--------------------------------------------------------------
my ($file_abs_path, $prev_commit, $help, $check_spelling);

my %options = (
	"f=s" => \$file_abs_path,
	"p=s" => \$prev_commit,
	"h" => \$help,
	"s" => \$check_spelling,);

GetOptions(%options) || die help();

if ($help)
{
	die help();
}

#------ some branches will not be checked ------
my $current_branch = `git rev-parse --abbrev-ref HEAD | tr -d '\n'`
  || die "Cannot get current branch name.\n";
if ($current_branch =~ /^port_/ || $current_branch =~ /^merge_/)
{
	print "Branch '$current_branch' skipping the check.\n";
	exit 0;
}

my $last_commit_msg = `git log -1  --pretty='%s'`
  || die "Cannot get last commit message.\n";
if ($last_commit_msg =~ /Merge/)
{
	print "Merge commit, skipping the check.\n";
	exit 0;
}

#------ all files that need to be checked ------
my @files = ();
my %diff_lines_map;

if ($file_abs_path)
{
	return
	  unless (-e $file_abs_path)
	  || die "File '$file_abs_path' does not exist.\n";

	# for directory, traverse all underlying files
	if (-d $file_abs_path)
	{
		my @allfiles = File::Find::Rule->file()->in($file_abs_path);
		foreach my $file (@allfiles)
		{
			push @files, $file;
			$diff_lines_map{$file} = $max_diff_line;
		}
	}
	else
	{
		push @files, $file_abs_path;
		$diff_lines_map{$file_abs_path} = $max_diff_line;
	}
}
else
{
	if (!$prev_commit)
	{
		$prev_commit = "HEAD~1";
	}

	my $current_head_sha = `git rev-parse HEAD --`
	  || die "Cannot get the SHA of current HEAD.\n";
	my $prev_head_sha = `git rev-parse $prev_commit --`
	  || die "Cannot get the SHA of previous HEAD.\n";
	if ($prev_head_sha eq $current_head_sha)
	{
		print "No diff file to be checked, skipping.\n";
		exit 0;
	}

	my $diff_num_stat = `git diff -z --numstat $prev_commit --`
	  || die "Cannot get the diff between two HEADs.\n";

	my @diff_files = split('\0', $diff_num_stat);

	for (my $i = 0; $i < @diff_files; $i++)
	{
		my @stat = split(' ', $diff_files[$i]);
		my $add_line = ($stat[0] eq "-") ? 0 : $stat[0] + 0;
		my $remove_line = ($stat[1] eq "-") ? 0 : $stat[1] + 0;
		my $diff_line = $add_line + $remove_line;
		my $file;

		# the diff file name is printed in the same line, like:
		# $lines_add $lines_delete $file_name
		if (@stat > 2)
		{
			$file = $stat[2];
		}
		# the diff file is renamed, the output is like:
		# $lines_add $lines_delete
		# $old_file_name
		# $new_file_name
		# we use new file name to check
		else
		{
			$file = $diff_files[ $i + 2 ];
			$i += 2;
		}

		# to check only those files which exist currently
		if (-e $file)
		{
			push @files, $file;
			$diff_lines_map{$file} = $diff_line;
		}
	}
}

#------ exclude some files from the code base ------
my $code_base ||= '.' unless @ARGV;
my $excludes  ||= "$code_base/src/tools/polar_copyright_exclude_patterns"
  if $code_base && -f "$code_base/src/tools/polar_copyright_exclude_patterns";

process_exclude();


#------ do the check for the remaining files ------
foreach my $file (@files)
{
	copyright_check($file);
}


#-----------------------------------------------------
# entry point of single file copyright check
#-----------------------------------------------------
sub copyright_check
{
	my ($abspath) = @_;

	my $basename = basename($abspath);

	if (rindex($abspath, "./", 0) == 0)
	{
		$abspath = substr($abspath, 2);
	}

	if ($abspath =~ m/\.(c|h|cpp|hpp|cc)$/)
	{
		c_format_check($abspath, $basename);
	}
	elsif ($abspath =~ m/\.(pl|pm|sh)/)
	{
		script_format_check($abspath, $basename);
	}
	else
	{
		print "$logger_warn $abspath (ignored)\n";
	}

	return 0;
}

#-----------------------------------------------------
# .c/.h files format check
#-----------------------------------------------------
sub c_format_check
{
	my ($abspath, $basename) = @_;

	my @lines;
	tie @lines, "Tie::File", $abspath
	  or die
	  "Fail to open up file '$abspath', please check your permission.\n";

	print "$logger_warn $abspath\n";

	# file type to judge:
	# 1. file of open source (postgres/gpdb)
	# 2. file of open source modified by PolarDB
	# 3. file owned by PolarDB
	my $cc_portion_pg = -1;
	my $cc_portion_pg_alibaba = -1;
	my $cc_alibaba = -1;

	my $apachepattern = $license[4];

	my $license_block_start = 0;
	my $license_block_end = 0;

	my $diff_line = $diff_lines_map{$abspath};

	for (my $i = 0; $i < @lines; $i++)
	{
		# recognize the first comment block
		if ($lines[$i] =~ m/\/\*/)
		{
			$license_block_start = $i;
		}
		if ($lines[$i] =~ m/\*\//)
		{
			$license_block_end = $i;
			last;
		}

		# is it an opensource file?
		if ($lines[$i] =~ m/$cc.*($pgdg|$gpdb|$pivotal)/i)
		{
			$cc_portion_pg = $i;
		}
		# is it an opensource file modified by Alibaba?
		elsif ($lines[$i] =~ m/$cc.*$alibaba.*/i)
		{
			$cc_portion_pg_alibaba = $i;
		}
		# is it an Alibaba-owned file?
		elsif ($lines[$i] =~ m/$apachepattern/i)
		{
			$cc_alibaba = $i;
		}
	}

	# check common spell words and comments
	if ($check_spelling)
	{
		for (my $i = 0; $i < @lines; $i++)
		{
			foreach my $word (@common_typo)
			{
				if ($lines[$i] =~ m/$word/i)
				{
					print
					  "  $logger_error Common spell error detected at line $i: $word.\n";
					$lines[$i] = $lines[$i] . '<<<';
				}
			}

			foreach my $word (@standard_comment_prefix)
			{
				if ($lines[$i] =~ m/$word/i && $lines[$i] !~ m/$word/)
				{
					print
					  "  $logger_error Comment case error detected at line $i: $lines[$i].\n";
					$lines[$i] = $lines[$i] . '<<<';
				}
			}
		}
	}

	# judge if a new license template should be injected to the file
	my $inject_template = 0;

	# single line comment block: not allowed
	if ($license_block_start == $license_block_end)
	{
		print "  $logger_error The comment block is invalid.\n";
		$inject_template = 1;
	}
	# Postgres-only file: skip
	elsif ($cc_portion_pg >= 0 && $cc_portion_pg_alibaba < 0)
	{
		print "  $logger_log Open source file";
		$inject_template = 0;

		if ($diff_line < $max_diff_line)
		{
			print ", skip it.\n";
		}
		else
		{
			print
			  "\n  $logger_error Adding 'Portions copyright' for PolarDB.\n";
			$lines[$cc_portion_pg] =
			  " * $portions $ccliteral $year, $alibaba_inc\n"
			  . $lines[$cc_portion_pg];
		}
	}
	# Postgres file modified by PolarDB
	elsif ($cc_portion_pg >= 0 && $cc_portion_pg_alibaba >= 0)
	{
		print "  $logger_log Open source file modified by PolarDB.\n";
		$inject_template = 0;
	}
	# Alibaba-owned file
	elsif ($cc_alibaba >= 0)
	{
		print "  $logger_log PolarDB's file.\n";
		# format check
		$inject_template = !c_apache_license_format_check($abspath, $basename,
			$license_block_start, $license_block_end);
	}
	# ignore when diff line is less than threshold
	elsif ($diff_line < $max_diff_line && $abspath =~ m/\.(c|cpp|cc|h)$/)
	{
		print
		  "  $logger_log Diff is less than $max_diff_line lines, ignored.\n";
		$inject_template = 0;
	}
	# others: error copyright format
	else
	{
		print "  $logger_error Cannot recognize the copyright format.\n";
		$inject_template = 1;
	}

	# format error, inject a new license template
	if ($inject_template == 1)
	{
		print
		  "  $logger_error Injecting a new license template to the file...\n";
		$lines[0] =
		  c_copyright_template($basename, $abspath) . ($lines[0] || "");
	}

	untie @lines;

	return 0;
}

#-----------------------------------------------------
# .c/.h files license check
#-----------------------------------------------------
sub c_apache_license_format_check
{
	my ($abspath, $basename, $start_line, $end_line) = @_;

	my $i = 0;
	my @lines;
	tie @lines, "Tie::File", $abspath
	  or die
	  "Fail to open up file '$abspath', please check your permission.\n";

	for ($i = $start_line; $i < $end_line; $i++)
	{
		$lines[$i] = rstrip($lines[$i]);
	}

	# comment head
	if ($lines[$start_line] ne
		"/*-------------------------------------------------------------------------"
	  )
	{
		print
		  "  $logger_error Invalid comment header: should start with: /*-------------------------------------------------------------------------\n";
		return 0;
	}
	$start_line++;

	# skip some empty lines
	while ($start_line < $end_line
		&& $lines[$start_line] eq " *")
	{
		$start_line++;
	}
	if ($start_line >= $end_line)
	{
		print
		  "  $logger_error $invalid_comment_body: reached the end of the body.\n";
		return 0;
	}

	# file name check
	if ($lines[$start_line] ne (" * " . $basename))
	{
		print "  $logger_error $invalid_comment_body: no file name.\n";
		return 0;
	}
	$start_line++;

	# description check
	if ($start_line < $end_line && $lines[$start_line] ne " *")
	{
		if (rindex(
				$lines[$start_line], " *	  (Edit or remove this line)...",
				0) == 0)
		{
			print
			  "  $logger_error Empty description template, edit or remove it.\n";
			$lines[$start_line] = $lines[$start_line] . '.';
			$start_line++;
		}
		elsif (rindex($lines[$start_line], " *	  ", 0) != 0)
		{
			print "  $logger_error Error decription format.\n";
			return 0;
		}
	}

	# forward until copyright
	while ($start_line < $end_line
		&& $lines[$start_line] !~ m/$cc.*$alibaba_inc/i)
	{
		$start_line++;
	}
	if ($start_line >= $end_line)
	{
		print "  $logger_error $invalid_comment_body: no copyright.\n";
		return 0;
	}
	if ($lines[ $start_line - 1 ] ne " *")
	{
		$lines[$start_line] = " *\n" . $lines[$start_line];
	}
	if ($lines[ $start_line + 1 ] ne " *")
	{
		$lines[$start_line] .= "\n *";
	}
	$start_line++;

	# skip some empty lines
	while ($start_line < $end_line
		&& $lines[$start_line] eq " *")
	{
		$start_line++;
	}
	if ($start_line >= $end_line)
	{
		print
		  "  $logger_error $invalid_comment_body: reached the end of the body.\n";
		return 0;
	}

	# license check
	$i = 0;
	while ($start_line < $end_line
		&& $i < $license_line_len)
	{
		my $l = " *";
		if ($license[$i] ne "")
		{
			$l .= " ";
		}
		$l .= $license[$i];

		# not a valid license, break
		if ($lines[$start_line] ne $l)
		{
			last;
		}
	}
	continue
	{
		$start_line++;
		$i++;
	}
	if ($start_line >= $end_line || $i < $license_line_len)
	{
		print
		  "  $logger_error $invalid_comment_body: license declaration error.\n";
		return 0;
	}

	# skip some empty lines
	while ($start_line < $end_line
		&& $lines[$start_line] eq " *")
	{
		$start_line++;
	}
	if ($start_line >= $end_line)
	{
		print
		  "  $logger_error $invalid_comment_body: reached the end of the body.\n";
		return 0;
	}

	# identification
	if ($lines[$start_line] ne " * $identification")
	{
		print
		  "  $logger_error $invalid_comment_body: $identification expected.\n";
		return 0;
	}
	$start_line++;
	if ($start_line >= $end_line)
	{
		print
		  "  $logger_error $invalid_comment_body: reached the end of the body.\n";
		return 0;
	}

	# identification indent
	if ($lines[$start_line] !~ m/^ \*\t  .+/)
	{
		print
		  "  $logger_error $invalid_comment_body: invalid identification path indent.\n";
		return 0;
	}

	# trim the identification and check
	my $trim = substr($lines[$start_line], 2);
	$trim = lstrip($trim);
	$trim = rstrip($trim);
	if ($trim ne $abspath)
	{
		print
		  "  $logger_error $invalid_comment_body: invalid identification path.\n";
		return 0;
	}
	$start_line++;
	if ($start_line >= $end_line)
	{
		print
		  "  $logger_error $invalid_comment_body: reached the end of the body.\n";
		return 0;
	}

	# skip some empty lines
	while ($start_line < $end_line
		&& $lines[$start_line] eq " *")
	{
		$start_line++;
	}
	if ($start_line >= $end_line)
	{
		print
		  "  $logger_error $invalid_comment_body: reached the end of the body.\n";
		return 0;
	}

	# comment tail line
	if ($lines[$start_line] ne
		" *-------------------------------------------------------------------------"
	  )
	{
		print
		  "  $logger_error $invalid_comment_body: the tail should be: *-------------------------------------------------------------------------\n";
		return 0;
	}
	if ($lines[ $start_line - 1 ] ne " *")
	{
		$lines[$start_line] = " *\n" . $lines[$start_line];
	}

	untie @lines;

	# pass the check
	return 1;
}

#-----------------------------------------------------
# .c/.h files copyright template
#-----------------------------------------------------
sub c_copyright_template
{
	my ($name, $abspath) = @_;
	my $template = "";

	# head
	$template .=
	  "/*-------------------------------------------------------------------------\n";
	$template .= " *\n";

	# file name
	$template .= " * " . $name . "\n";
	$template .= " *	  (Edit or remove this line)...\n";

	# copyright
	$template .= " *\n";
	$template .= " * $ccliteral $year, $alibaba_inc\n";
	$template .= " *\n";

	# license
	foreach (@license)
	{
		$template .= " *" . ($_ eq "" ? "" : " " . $_) . "\n";
	}
	$template .= " *\n";

	# identification
	$template .= " * $identification\n";
	$template .= " *	  " . $abspath . "\n";
	$template .= " *\n";

	# tail
	$template .=
	  " *-------------------------------------------------------------------------\n";
	$template .= " */\n";

	return $template;
}

#-----------------------------------------------------
# .pl/.pm/.sh files format check
#-----------------------------------------------------
sub script_format_check
{
	my ($abspath, $basename) = @_;

	my @lines;
	tie @lines, "Tie::File", $abspath
	  or die
	  "Fail to open up file '$abspath', please check your permission.\n";

	print "$logger_warn $abspath\n";

	# file type to judge:
	# 1. file of open source (postgres)
	# 2. file of open source modified by PolarDB
	# 3. file owned by PolarDB
	my $cc_portion_pg = -1;
	my $cc_portion_pg_alibaba = -1;
	my $cc_alibaba = -1;

	my $apachepattern = $license[2];

	my $license_block_start = -1;
	my $license_block_end = -1;

	my $diff_line = $diff_lines_map{$abspath};

	for (my $i = 0; $i < @lines; $i++)
	{
		# skip the beginning of script like: #!/bin/bash
		if ($lines[$i] =~ m/#!/)
		{
			next;
		}
		# recognize the first comment block
		if ($lines[$i] =~ m/\#/)
		{
			$license_block_start =
			  ($license_block_start == -1) ? $i : $license_block_start;
			$license_block_end = $i;
		}
		else
		{
			if ($license_block_start != -1)
			{
				last;
			}
		}

		# is it an opensource file?
		if ($lines[$i] =~ m/$cc.*($pgdg|$gpdb|$pivotal)/i)
		{
			$cc_portion_pg = $i;
		}
		# is it an opensource file modified by Alibaba?
		elsif ($lines[$i] =~ m/$cc.*$alibaba.*/i)
		{
			$cc_portion_pg_alibaba = $i;
		}
		# is it an Alibaba-owned file?
		elsif ($lines[$i] =~ m/$apachepattern/i)
		{
			$cc_alibaba = $i;
		}
	}

	if ($check_spelling && index($basename, 'polar_copyright_check') == -1)
	{
		# check common spell words
		for (my $i = 0; $i < @lines; $i++)
		{
			foreach my $word (@common_typo)
			{
				if ($lines[$i] =~ m/$word/i)
				{
					print
					  "  $logger_error Common spell error detected at line $i: $word.\n";
					$lines[$i] = $lines[$i] . '<<<';
				}
			}
		}
	}

	# judge if a new license template should be injected to the file
	my $inject_template = 0;

	# Postgres-only file: skip
	if ($cc_portion_pg >= 0 && $cc_portion_pg_alibaba < 0)
	{
		print "  $logger_log Open source file, ";

		# since the copyright of scripts from PG community is not normalized,
		# we just skip these scripts from community
		print "skip it.\n";
		$inject_template = 0;
	}
	# Postgres file modified by PolarDB
	elsif ($cc_portion_pg >= 0 && $cc_portion_pg_alibaba >= 0)
	{
		print "  $logger_log Open source file modified by PolarDB.\n";
		$inject_template = 0;
	}
	# Alibaba-owned file
	elsif ($cc_alibaba >= 0)
	{
		print "  $logger_log PolarDB's file.\n";
		# format check
		$inject_template =
		  !script_apache_license_format_check($abspath, $basename,
			$license_block_start, $license_block_end);
	}
	else
	{
		print "  $logger_error Cannot recognize the copyright format.\n";
		$inject_template = 1;
	}

	# format error, inject a new license template
	if ($inject_template == 1)
	{
		print
		  "  $logger_error Injecting a new license template to the file...\n";
		$lines[1] =
		  script_copyright_template($basename, $abspath) . ($lines[1] || "");
	}

	untie @lines;

	return 0;
}

#-----------------------------------------------------
# .pl/.pm/.sh license check
#-----------------------------------------------------
sub script_apache_license_format_check
{
	my ($abspath, $basename, $start_line, $end_line) = @_;

	my $i = 0;
	my @lines;
	tie @lines, "Tie::File", $abspath
	  or die
	  "Fail to open up file '$abspath', please check your permission.\n";

	for ($i = $start_line; $i < $end_line; $i++)
	{
		$lines[$i] = rstrip($lines[$i]);
	}

	# file name check
	if ($lines[$start_line] ne ("# " . $basename))
	{
		print "  $logger_error $invalid_comment_body: no file name.\n";
		return 0;
	}
	$start_line++;

	# description check
	if ($start_line < $end_line && $lines[$start_line] ne "#")
	{
		if (rindex(
				$lines[$start_line], "#	  (Edit or remove this line)...",
				0) == 0)
		{
			print
			  "  $logger_error Empty description template, edit or remove it.\n";
			$lines[$start_line] = $lines[$start_line] . '.';
			$start_line++;
		}
		elsif (rindex($lines[$start_line], "#	  ", 0) != 0)
		{
			print "  $logger_error Error decription format.\n";
			return 0;
		}
	}

	# forward until copyright
	while ($start_line < $end_line
		&& $lines[$start_line] !~ m/$cc.*$alibaba_inc/i)
	{
		$start_line++;
	}
	if ($start_line >= $end_line)
	{
		print "  $logger_error $invalid_comment_body: no copyright.\n";
		return 0;
	}
	if ($lines[ $start_line - 1 ] ne "#")
	{
		$lines[$start_line] = "#\n" . $lines[$start_line];
	}
	if ($lines[ $start_line + 1 ] ne "#")
	{
		$lines[$start_line] .= "\n#";
	}
	$start_line++;

	# skip some empty lines
	while ($start_line < $end_line
		&& $lines[$start_line] eq "#")
	{
		$start_line++;
	}
	if ($start_line > $end_line)
	{
		print
		  "  $logger_error $invalid_comment_body: reached the end of the body.\n";
		return 0;
	}

	# license check
	$i = 0;
	while ($start_line < $end_line
		&& $i < $license_line_len)
	{
		my $l = "#";
		if ($license[$i] ne "")
		{
			$l .= " ";
		}
		$l .= $license[$i];

		# not a valid license, break
		if ($lines[$start_line] ne $l)
		{
			last;
		}
	}
	continue
	{
		$start_line++;
		$i++;
	}
	if ($start_line > $end_line || $i < $license_line_len)
	{
		print
		  "  $logger_error $invalid_comment_body: license declaration error.\n";
		return 0;
	}

	# skip some empty lines
	while ($start_line < $end_line
		&& $lines[$start_line] eq "#")
	{
		$start_line++;
	}
	if ($start_line > $end_line)
	{
		print
		  "  $logger_error $invalid_comment_body: reached the end of the body.\n";
		return 0;
	}

	# identification
	if ($lines[$start_line] ne "# $identification")
	{
		print
		  "  $logger_error $invalid_comment_body: $identification expected.\n";
		return 0;
	}
	$start_line++;
	if ($start_line > $end_line)
	{
		print
		  "  $logger_error $invalid_comment_body: reached the end of the body.\n";
		return 0;
	}

	# trim the identification and check
	my $trim = substr($lines[$start_line], 1);
	$trim = lstrip($trim);
	$trim = rstrip($trim);
	if ($trim ne $abspath)
	{
		print
		  "  $logger_error $invalid_comment_body: invalid identification path.\n";
		return 0;
	}

	untie @lines;

	# pass the check
	return 1;
}

#-----------------------------------------------------
# .pl/.pm/.sh files copyright template
#-----------------------------------------------------
sub script_copyright_template
{
	my ($name, $abspath) = @_;
	my $template = "";

	# file name
	$template .= "# " . $name . "\n";
	$template .= "#	  (Edit or remove this line)...\n";

	# copyright
	$template .= "#\n";
	$template .= "# $ccliteral $year, $alibaba_inc\n";
	$template .= "#\n";

	# license
	foreach (@license)
	{
		$template .= "#" . ($_ eq "" ? "" : " " . $_) . "\n";
	}
	$template .= "#\n";

	# identification
	$template .= "# $identification\n";
	$template .= "#	  " . $abspath . "\n\n";

	return $template;
}

#-----------------------------------------------------
# avoid checking excluded files defined in the
# pattern in 'polar_copyright_exclude_patterns'
#-----------------------------------------------------
sub process_exclude
{
	if ($excludes && @files)
	{
		open(my $eh, '<', $excludes)
		  || die "cannot open exclude file \"$excludes\"\n";
		while (my $line = <$eh>)
		{
			chomp $line;
			next if $line =~ m/^#/;
			next if $line =~ m/^\s*$/;
			my $rgx = qr!$line!;
			@files = grep { $_ !~ /$rgx/ } @files if $rgx;
		}
		close($eh);
	}
	return;
}

#-----------------------------------------------------
# utility functions
#-----------------------------------------------------
sub lstrip
{
	my ($str) = @_;
	$str =~ s/^\s+//;
	return $str;
}

sub rstrip
{
	my ($str) = @_;
	$str =~ s/\s+$//;
	return $str;
}

#-----------------------------------------------------
# -h
#-----------------------------------------------------
sub help
{
	return '
polar_copyright_check.pl [ -f file | -p prev_commit ]
    Check and update copyright of a specific file, or diff files from a specific commit to current HEAD.

    Syntax:  polar_copyright_check.pl [ -option value | --option=value ]
    -f:		the file to be checked               / Optional
    -p:		the previous commit hash or refs     / Optional
    -s:		check common spelling                / Optional

';
}
