#!/usr/bin/perl -w

# delta_coverage.pl
#	  Test coverage of new code with tests.
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
#	  src/tools/delta_coverage.pl

use strict;
use FileHandle;
use Getopt::Std;
use IO::File;
use POSIX qw(strftime);

format HEADER =

 cover_test.pl -f diff.log -d ./ -l output.log -p prev_commit
	Syntax:  cover_test.pl -option value

	-f:		the diff log file	/ Required
	-d:		the fundamental dir	/ optional
	-l:		the output			/ optional
	-p:		the prev commit id	/ optional
.

my $debug = 0;
my ($diff, $directory, $log, $prev_commit);
my $delta_path = "coverage";
&do_opt();

if (!defined $prev_commit)
{
	$prev_commit = `git rev-parse HEAD~1`;
}

my $reg_line = '(.*)\n$';
my $current_commit = `git rev-parse HEAD~0`;
my $current_branch = `git rev-parse --abbrev-ref HEAD`;

$diff = "diff.log";
$prev_commit = &ppgrep($prev_commit, $reg_line, 1);
$current_commit = &ppgrep($current_commit, $reg_line, 1);

$prev_commit = $prev_commit->[0];
$current_commit = $current_commit->[0];

system(
	"git config diff.renameLimit 10000;
		git diff $prev_commit $current_commit > $diff");

if (!defined $diff)
{
	&crit_record("pls input diff file");
}

if (!defined $directory)
{
	$directory = ".";
}

if (defined $log)
{
	print "pls check the execute message in [$delta_path/$log]\n";
	print "pls check the delta result in [$delta_path/delta_coverage.out]\n";
	my $fd =
	  POSIX::open("$delta_path/$log", &POSIX::O_CREAT | &POSIX::O_WRONLY,
		0640);
	POSIX::dup2($fd, 1);
	POSIX::dup2($fd, 2);
}

&record("directory=[$directory]; diff=[$diff];");

#interpret the diff file

my ($hd_diff) = &getFileHandle($diff, "r");    #diff.org file

if (!defined $hd_diff)
{
	&crit_record("diff [$diff] init fail, pls check it!");
}

&record("begin to interpret diff file!");

my $reg_diff = 'diff --git [ab]/(.+?)[\s\n]';
my $reg_add = '^\+(.*)';
my @reg_old = ('^-');
my @reg_del = ('^\+');


#my @reg_file		= ('\.h$','\.cc$','\.c$','\.ic$');
my @reg_file = ('\.c$', '\.cc$', '\.cpp$');

my $type_gcno = "gcno";
my $type_gcov = "gcov";
my $reg_cnt = '^@@.*?\d*,\d*.*?(\d*),(\d*) @@';


my $reg_gcov_1 = '^-:\s*(\d+):(.*)';
my $reg_gcov_2 = '^(#####|\d+):\s*(\d+):(.*)';
my $reg_gcov_3 = '^#####:\s*(\d+):(.*)';

my $reg_hgcov_file = '\.h\.gcov';
my $reg_file_prefix = '(.*)\..*';

my $line;
my %src_file = ();
my ($ref_raw_line, $raw_line, $key);
my ($bcnt, $ecnt) = (0, 0);
my $delta = -1;
my $current = 0;
my $in_block = 0;
my $skip_update = 0;

my $result_fd;
open($result_fd, ">$delta_path/delta_coverage.txt");

my $diff_line_num = 0;
my $no_test_line_num = 0;

while ($line = $hd_diff->getline())
{
	# find the file
	$ref_raw_line = &ppgrep($line, $reg_diff, 1);
	if (defined $ref_raw_line)
	{
		$in_block = 0;
		$delta = -1;
		$raw_line = $ref_raw_line->[0];
		$raw_line = "$directory/$raw_line";
		&debug_record("diff file has: $raw_line");
		my @add_lines = ();
		$key = $raw_line;
		$src_file{$key} = \@add_lines;

		$bcnt = 0;
		$ecnt = 0;

		# do not udpate cnt for not c/c++/h file
		my $ret = &filterLine(\@reg_file, $key);
		if ($ret == 0)
		{
			$skip_update = 0;
		}
		else
		{
			$skip_update = 1;
		}

		next;
	}

	$ref_raw_line = &ppgrep($line, $reg_cnt, 2);
	if (defined $ref_raw_line)
	{
		$in_block = 1;
		$delta = -1;
		my $c1 = $ref_raw_line->[0];
		my $c2 = $ref_raw_line->[1];
		$bcnt = $c1;
		$ecnt = $c1 + $c2 - 1;
		$current = $c1 - 1;
		print("\tdiff file has: $key: begin: $bcnt  end: $ecnt\n");
		next;
	}

	# do not udpate cnt for not c/c++/h file
	next if ($skip_update == 1);

	if ($in_block == 0)
	{
		next;
	}
	my $is_old = &filterLine(\@reg_old, $line);
	if ($is_old == 0)
	{
		next;
	}
	$current = $current + 1;

	#find the lines with head of '+'
	$ref_raw_line = &ppgrep($line, $reg_add, 1);
	if (defined $ref_raw_line)
	{
		my $new_ref_array = $src_file{$key};
		&update_ecnt_by_bcnt($new_ref_array, $bcnt);
		$raw_line = $ref_raw_line->[0];
		$delta = $delta + 1;

		#del the backspace
		$raw_line = &delbackspace($raw_line);
		if (!defined $raw_line)
		{
			next;
		}
		&debug_record("diff file lines: $raw_line");

		my $tmp_ref_array = $src_file{$key};
		my @st_line = ($raw_line, $bcnt, $bcnt + $delta);
		push(@$tmp_ref_array, \@st_line);
		next;
	}
	elsif (!defined $ref_raw_line)
	{
		$bcnt = $current + 1;
		$delta = -1;
	}
}

#-------------------------------print the currect line scope---------------------------------

my $value;
my $repeat = -1;
print "\n";
&record("begin to amend diff file[amend the bcnt, ecnt]!");
while (($key, $value) = each %src_file)
{
	$repeat = -1;
	foreach my $ref_correct (@$value)
	{
		my ($line_correct, $bcnt_correct, $ecnt_correct) = @$ref_correct;
		&debug_record(
			"\tline:$line_correct|bcnt=$bcnt_correct|ecnt=$ecnt_correct\n");
		if ($repeat == $bcnt_correct)
		{
			next;
		}
		else
		{
			$repeat = $bcnt_correct;
			print(
				"\tdiff file has: $key: begin: $bcnt_correct  end: $ecnt_correct\n"
			);
		}
	}
}
print "\n";

#-------------------------------filter the not c/c++/h source file---------------------------------
my @filtered_file = ();
my @valid_file = ();
my $all_file_cnt = 0;
while (($key, $value) = each %src_file)
{
	$all_file_cnt++;

	# filter the file
	my $ret = &filterLine(\@reg_file, $key);
	if ($ret == 0)
	{
		push(@valid_file, $key);
		next;
	}
	else
	{
		push(@filtered_file, $key);
	}
}

my $novalid_cnt = scalar(@filtered_file);
my $valid_cnt = $all_file_cnt - $novalid_cnt;

&record("The diff log include : [$all_file_cnt\t] files");

&record("The valid source file: [$valid_cnt\t]; listing as below:\n");

my $add_lines_cnt;

foreach my $file (@valid_file)
{
	$value = $src_file{$file};
	$add_lines_cnt = scalar(@$value);
	print "\t\t\t lines:[$add_lines_cnt\t];file:[$file]\n";
}

&record("The filtered file: [$novalid_cnt\t]; listing as below:\n");

foreach my $file (@filtered_file)
{
	$value = $src_file{$file};
	$add_lines_cnt = scalar(@$value);
	print "\t\t\t lines:[$add_lines_cnt\t];file:[$file]\n";
}

#---------------------------------begin to check-----------------------------------------------------

&record("begin to check files:");

my ($file_name, $file_gcno, $file_name_gcno,
	$file_gcov, $file_name_gcov, $file_path);

print $result_fd "==============Delta Code Coverage Start==============\n";

print $result_fd "current_branch = $current_branch\n";
print $result_fd
  "prev_commit = $prev_commit\ncurrent_commit = $current_commit\n\n";

my $total_test_line_num = 0;
my $total_diff_line_num = 0;
my $total_no_cover_line_num = 0;

my @no_compile_files = ();
my @no_test_files = ();

my $reg_no_test_directory = '^./src/test/';
my $no_test_directory;

foreach my $file (@valid_file)
{
	print("\nfile: $file------------------------------------------\n");

	if (-e $file)    #diff file
	{
		print $result_fd "file [$file] begin--------------------\n";
		$file_name = &getFileName($file);

		#----------------debug
		if ($debug and $file_name ne "mysqld.cc")
		{
			next;
		}

		#exclude files from "./src/test" directory
		$no_test_directory = &isgrep($file, $reg_no_test_directory);
		if ($no_test_directory == 0)
		{
			print $result_fd
			  "    INFO: Test files are not included in delta coverage.\n";
			print $result_fd "file [$file] end----------------------\n\n";
			next;
		}
		$file_path = &getFilePath($file);
		$file_name_gcno = &genTargetFileNameFor51($file_name, $type_gcno)
		  ;    #turn *.c.gcno to *.gcno
		$file_gcno = "$file_path/$file_name_gcno";

		&record("file = [$file]; gcno = [$file_gcno]");

		if (!defined $file_gcno or !-e $file_gcno)
		{
			&warn_record("file [$file_name_gcno] is not compiled.");
			print $result_fd "    WARNING: file [$file] is not compiled!\n";
			push(@no_compile_files, $file);
		}
		else
		{
			#gen the gcov file
			$file_name_gcov = &genTargetFileName($file_name, $type_gcov);
			$file_gcov = "$directory/$delta_path/$file_name_gcov";

			my $ret =
			  &gengcov($file_name, $file_name_gcno, $file_name_gcov,
				$file_path, $directory);
			if ($ret != 0)
			{
				&warn_record("file [$file_name_gcno] is not tested!");
				print $result_fd "    WARNING: file [$file] is not tested!\n";
				push(@no_test_files, $file);
			}
			else
			{
				&record("......begin to scan gcov file [$file_gcov]......");
				my $ref_diff_lines = $src_file{$file};

				print $result_fd "Test Situation:\n";
				&compare($ref_diff_lines, $file_gcov);

				my $test_line_num = $diff_line_num - $no_test_line_num;
				print $result_fd
				  "Test Rate: $test_line_num / $diff_line_num\n";
				$total_test_line_num = $total_test_line_num + $test_line_num;
				$total_diff_line_num = $total_diff_line_num + $diff_line_num;
			}
		}
		print $result_fd "file [$file] end----------------------\n\n";
	}
	else
	{
		&warn_record("file [$file] not exists!");
		print $result_fd "file [$file] does not exist";
	}

}

print $result_fd "\n---------------------------------------\n";
print $result_fd "Total Test Situation:\n";
print $result_fd "No Cover Lines: $total_no_cover_line_num\n";

my $no_compile_num = @no_compile_files;
my $no_test_num = @no_test_files;

print $result_fd "No Compile Files: $no_compile_num\n";
foreach my $file (@no_compile_files)
{
	print $result_fd "    $file\n";
}

print $result_fd "No Test Files: $no_test_num\n";
foreach my $file (@no_test_files)
{
	print $result_fd "    $file\n";
}

if ($total_diff_line_num == 0)
{
	print $result_fd "No different line in .c file\n";
}
else
{
	my $test_rate = ($total_test_line_num / $total_diff_line_num) * 100;
	print $result_fd
	  "Test Rate: $total_test_line_num / $total_diff_line_num = $test_rate%\n";
}

#------------------------------------------------------------
#correct the end line number
#------------------------------------------------------------
sub update_ecnt_by_bcnt()
{
	my ($ref_array, $bcnt) = @_;
	if (defined $ref_array)
	{
		foreach my $ref (@$ref_array)
		{
			my ($tmp_line, $tmp_bcnt, $tmp_ecnt) = @$ref;
			if ($tmp_bcnt == $bcnt)
			{
				@$ref = ($tmp_line, $tmp_bcnt, $tmp_ecnt + 1);
			}
		}

	}
}

#------------------------------------------------------------
#find the file
#------------------------------------------------------------
sub find_gcno()
{
	my ($file_name_gcno, $dir) = @_;
	my @result = `find $dir -name '$file_name_gcno'`;
	if (defined $result[0])
	{
		chomp($result[0]);
		return $result[0];
	}
	else
	{

		return undef;
	}
}

#----------------------------------------------------
#append the file type
#----------------------------------------------------
sub genTargetFileName()
{
	my ($file_name, $type) = @_;
	my $type_file = "$file_name.$type";
	return $type_file;
}


sub genTargetFileNameFor51()
{
	my ($file_name, $type) = @_;
	my $ref_file_prefix = &ppgrep($file_name, $reg_file_prefix, 1);
	my $file_prefix = $ref_file_prefix->[0];
	return "$file_prefix.$type";

}
#-----------------------------------------------------
#get the filename from string
#-----------------------------------------------------
sub getFileName()
{
	my ($full_path) = @_;
	my $file_name;
	if ($full_path =~ /.*\/(.*?)$/)
	{
		$file_name = $1;
	}
	else
	{
		$file_name = $full_path;
	}
	return $file_name;
}

#-----------------------------------------------------
# POLAR: get the file path from string
#-----------------------------------------------------
sub getFilePath()
{
	my ($full_path) = @_;
	my $file_path;
	if ($full_path =~ /(.*)\/(.*?)$/)
	{
		$file_path = $1;
	}
	else
	{
		$file_path = $full_path;
	}
	return $file_path;
}
#------------------------------------------------------------------------
#
#------------------------------------------------------------------------
sub compare()
{
	my ($ref_diff_lines, $file_gcov) = @_;

	my $hd_gcov = &getFileHandle($file_gcov, "r");
	my $reg = "^#####:";

	my $reg_no_cover = '\/\*no cover line\*\/$';         #"/*no cover line*/"
	my $reg_no_cover_begin = '\/\*no cover begin\*\/$';  #"/*no cover begin*/"
	my $reg_no_cover_end = '\/\*no cover end\*\/$';      #"/*no cover end*/"

	my $no_cover_line_num = 0;

	my $in_no_cover = 0;
	my $after_no_cover_line = 0;

	my $bcnt;
	my $ecnt;
	my $sr_cnt;
	my $cov_ret;

	my $temp_ret;
	my $temp_line;
	my $ret;

	#print output to report.txt

	$diff_line_num = 0;
	$no_test_line_num = 0;
	while (my $line = $hd_gcov->getline())
	{
		my $parse_line = $line;
		#&debug_record("the gcno lines: $parse_line");

		#step 1: del backspace
		$parse_line = &delbackspace($parse_line);
		if (!defined $parse_line)
		{
			next;
		}

		# "/*no cover line*/"
		$cov_ret = &isgrep($parse_line, $reg_no_cover);
		if ($cov_ret == 0)    #find "/*no cover line*/"
		{
			$after_no_cover_line = 1;
			next;
		}

		# "/*no cover begin*/"
		$cov_ret = &isgrep($parse_line, $reg_no_cover_begin);
		if ($cov_ret == 0 and $in_no_cover == 0)
		{
			$in_no_cover = 1;
			next;
		}

		# "/*no cover end*/"
		$cov_ret = &isgrep($parse_line, $reg_no_cover_end);
		if ($cov_ret == 0 and $in_no_cover == 1)
		{
			$in_no_cover = 0;
			next;
		}

		#step 2: not grep -
		$ret = &isgrep($parse_line, $reg_gcov_1);    #return 0 if match '-'
		if ($ret == 0)
		{
			next;
		}

		if (   $in_no_cover == 1
			or $after_no_cover_line == 1
		  ) #(between "/*no cover begin*/" and "/*no cover end*/") or (after "/*no cover line*/)"
		{
			$after_no_cover_line = 0;
			$no_cover_line_num++;
			next;
		}

		$temp_line = $parse_line;
		$parse_line = &ppgrep($parse_line, $reg_gcov_2, 3);
		if (!defined $parse_line)
		{
			next;
		}

		$sr_cnt = $parse_line->[1];        #line number
		$parse_line = $parse_line->[2];    #statement

		#step 3: del backspace
		$parse_line = &delbackspace($parse_line);
		if (!defined $parse_line)
		{
			next;
		}

		#step 4: del the no meaning line
		$ret = &ismeaning($parse_line);
		if ($ret != 0)
		{
			next;
		}

		$temp_ret = &isgrep($temp_line, $reg_gcov_3); #retrun 0 if match #####

		foreach my $diff_line (@$ref_diff_lines)
		{
			$ret = &isMatchStr($parse_line, $sr_cnt, $diff_line);
			&debug_record("begin: $line  end: $diff_line : result is $ret");

			my $string;
			if ($ret == 0)
			{

				$diff_line_num = $diff_line_num + 1;
				print "$line";
				print $result_fd "$line";

				if ($temp_ret == 0)
				{
					#print $result_fd "$line";
					$no_test_line_num = $no_test_line_num + 1;
				}
				last;
			}
		}
	}

	if ($no_cover_line_num > 0)
	{
		$total_no_cover_line_num =
		  $total_no_cover_line_num + $no_cover_line_num;

		my $str2;
		my $str1 = "INFO: Filter the no cover lines: $no_cover_line_num";
		if ($in_no_cover == 1 or $after_no_cover_line == 1)
		{
			$str2 = "FATAL: Some 'no cover hint' don't match, pls check!!!";
		}
		else
		{
			$str2 = "INFO: All 'no cover hint' match correctly";
		}
		&sp_warn_record($str1, $str2);

		if ($in_no_cover == 1 or $after_no_cover_line == 1)
		{
			exit(1);
		}
	}

}

#------------------------------------------------------------------
#judge it is meaning line
#------------------------------------------------------------------
sub ismeaning()
{
	my ($str) = @_;
	if ($str =~ /[0-9]|[a-z]|[A-Z]/)
	{
		return 0;
	}
	else
	{
		return 1;
	}
}

#------------------------------------------------------------------
#
#------------------------------------------------------------------
sub isMatchStr()
{
	my ($parse_line, $sr_cnt, $diff_line) = @_;
	my ($bcnt, $ecnt);
	($diff_line, $bcnt, $ecnt) = @$diff_line;
	my $ret = &isfullstr($parse_line, $diff_line);
	if ($ret == 0)
	{
		if ($bcnt != 0 or $ecnt != 0)
		{
			if ($sr_cnt >= $bcnt and $sr_cnt <= $ecnt)
			{
				return 0;
			}
			else
			{
				return 1;
			}
		}
		else
		{
			return 0;
		}
	}
	return 1;
}

#------------------------------------------------------------------
#
#------------------------------------------------------------------
sub issubstr()
{
	my ($full, $str) = @_;
	my $pos = index($full, $str);
	if (defined $pos and $pos >= 0)
	{
		return 0;
	}
	return 1;
}

#------------------------------------------------------------------
#
#------------------------------------------------------------------
sub isfullstr()
{
	my ($full, $str) = @_;
	if ($full eq $str)
	{
		return 0;
	}
	return 1;
}

#------------------------------------------------------------------
#generate the gcov file
#------------------------------------------------------------------
sub gengcov()
{
	my ($file_name, $file_name_gcno, $file_name_gcov, $file_path, $directory)
	  = @_;

	my $file_gcno = "$file_path/$file_name_gcno";

	if (!defined $file_gcno)
	{
		return 1;
	}

	# POLAR
	my $ret1 =
	  system("cd $file_path; gcov $file_name -o $file_name_gcno >/dev/null")
	  ;    #return 0 if success

	my $ret2 = system(
		"mv $file_path/$file_name_gcov $directory/$delta_path/$file_name_gcov"
	);

	$ret1 = $ret1 >> 8;
	$ret2 = $ret2 >> 8;
	if ($ret1 == 0 && $ret2 == 0)
	{
		return 0;
	}
	else
	{
		return 1;
	}
}

#-------------------------------------------------------
#replace the file type
#-------------------------------------------------------
sub replaceFileType()
{
	my ($str, $target) = @_;
	my $newstr = $str;
	if ($str =~ /(.*)\..*?$/)
	{
		return "$1.$target";
	}
	return undef;
}

#-------------------------------------------------------
#truncate the backspace on the head and tail
#-------------------------------------------------------
sub delbackspace()
{
	my ($str) = @_;
	if (!defined $str or $str eq "")
	{
		return undef;
	}
	$str =~ s/^\s+//g;
	$str =~ s/\$\s+//g;
	if ($str eq "")
	{
		return undef;
	}
	return $str;
}

#------------------------------------------------------
#judge the str is in array
#------------------------------------------------------
sub isStrInArray()
{
	my (@array, $line) = @_;
	foreach my $tmp (@array)
	{
		if ($line eq $tmp)
		{
			return 0;
		}
	}
	return 1;
}

#-------------------------------------------------------
#justify is success to grep the line according to regex
#-------------------------------------------------------
sub isgrep()
{
	my ($line, $reg) = @_;
	if ($line =~ /$reg/)
	{
		return 0;
	}
	else
	{
		return 1;
	}
}

#------------------------------------------------------
#grep the result
#------------------------------------------------------
sub ppgrep()
{
	my ($line, $reg, $cnt) = @_;
	my @ret = ();
	if ($line =~ /$reg/i)
	{
		if ($cnt >= 1)
		{
			push(@ret, $1);
		}
		if ($cnt >= 2)
		{
			push(@ret, $2);
		}
		if ($cnt >= 3)
		{
			push(@ret, $3);
		}
		if ($cnt >= 4)
		{
			return undef;
		}
		return \@ret;
	}
	else
	{
		return undef;
	}
}

#----------------------------------------------------------
#filter the lines according to reg array
#----------------------------------------------------------
sub filterLine()
{
	my ($ref_reg, $line) = @_;
	my @arr_filter = @$ref_reg;
	foreach my $reg (@arr_filter)
	{
		my $ret = &isgrep($line, $reg);
		if ($ret == 0)
		{
			return 0;
		}
	}
	return 1;
}

#------------------------------------------
#record the information
#------------------------------------------
sub record()
{
	my ($string) = @_;
	my $current_time = strftime("%Y-%m-%d %H:%M:%S", localtime(time));
	print("$current_time: info: $string\n");
}

#------------------------------------------
#record the crit
#------------------------------------------
sub crit_record()
{
	my ($string) = @_;
	my $current_time = strftime("%Y-%m-%d %H:%M:%S", localtime(time));
	print("$current_time: crit: $string\n");
	exit(1);
}

#------------------------------------------
#record the crit
#------------------------------------------
sub debug_record()
{
	my ($string) = @_;
	if ($debug == 1)
	{
		my $current_time = strftime("%Y-%m-%d %H:%M:%S", localtime(time));
		print("$current_time: debug: $string\n");
	}
}

#------------------------------------------
#record the warn
#------------------------------------------
sub warn_record()
{
	my ($string) = @_;
	my $current_time = strftime("%Y-%m-%d %H:%M:%S", localtime(time));
	print("$current_time: warn: $string\n");
}

sub sp_warn_record()
{
	my ($str1, $str2) = @_;

	print $result_fd "    $str1\n";
	print $result_fd "    $str2\n";
}

#---------------------------------------------------------------------
#init the file handle
#---------------------------------------------------------------------
sub getFileHandle
{
	my ($file, $mode) = @_;
	my ($hd);
	$hd = new FileHandle($file, "$mode");
	return $hd;
}

#---------------------------------------------------------------------------------
#split the input parameters
#---------------------------------------------------------------------------------
sub do_opt
{
	use vars '$opt_h', '$opt_H', '$opt_f', '$opt_d', '$opt_l', '$opt_p';
	my $returncode = getopts('hH:f:d:l:p:');
	&do_help() if (defined $opt_h or defined $opt_H or $returncode != 1);
	$diff = $opt_f if (defined $opt_f);
	$directory = $opt_d if (defined $opt_d);
	$log = $opt_l if (defined $opt_l);
	$prev_commit = $opt_p if (defined $opt_p);
	$prev_commit = "$prev_commit\n";
}

#----------------------------------------------------------------------------------
#give help information and exit;
#---------------------------------------------------------------------------------
sub do_help
{
	$~ = "HEADER";
	write;
	exit(1);
}
