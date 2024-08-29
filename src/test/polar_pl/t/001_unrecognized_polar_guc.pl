
#!/usr/bin/perl

# 001_unrecognized_polar_guc.pl
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
#	  src/test/polar_pl/t/001_unrecognized_polar_guc.pl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node_primary = PostgreSQL::Test::Cluster->new('primary');
$node_primary->init(allows_streaming => 1);

# unrecognized polar GUC
$node_primary->append_conf('postgresql.conf', 'polar_51e92d6b = on');

$node_primary->start;

my $result = $node_primary->safe_psql('postgres', "SELECT 1;");
is($result, 1, "db is running");

# done with the node
$node_primary->stop;

done_testing();
