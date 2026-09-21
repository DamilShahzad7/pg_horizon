# MultiXact wraparound state must be real, not "0 because nobody is running".
#
# Cluster-level mxid_age used to come from GetOldestMultiXactId(), which is the
# oldest MultiXact still *in use*; on an idle system that is always "now", so
# the cluster reported age 0 while datminmxid was far past the freeze limit.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('multixact');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
autovacuum_multixact_freeze_max_age = 10000
vacuum_multixact_failsafe_age = 60000
});
$node->start;

$node->safe_psql('postgres', q{
CREATE EXTENSION pg_horizon;
CREATE TABLE m(id int PRIMARY KEY, v int);
INSERT INTO m VALUES (1, 0), (2, 0);
});

sub burn_multixacts
{
	my ($row, $tx) = @_;
	my $file = $node->basedir . "/mx_$row.sql";
	append_to_file($file,
		"BEGIN;\nSELECT id FROM m WHERE id = $row FOR SHARE;\nSELECT pg_sleep(0.001);\nCOMMIT;\n");
	$node->command_ok(
		[ 'pgbench', '-n', '-f', $file, '-c', '16', '-j', '4', '-t', $tx,
		  '-h', $node->host, '-p', $node->port, 'postgres' ],
		"pgbench created multixacts on row $row");
}

sub status
{
	return $node->safe_psql('postgres',
		'SELECT mxid_age, severity FROM pg_horizon;');
}

# --- nothing yet ---------------------------------------------------------------
my ($age0, $sev0) = split /\|/, status();
cmp_ok($age0, '<', 10000, 'starts below the limit');
is($sev0, 'ok', 'severity ok');

# --- past autovacuum_multixact_freeze_max_age ----------------------------------
burn_multixacts(2, 1500);
my $real = $node->safe_psql('postgres',
	'SELECT max(mxid_age(datminmxid)) FROM pg_database;');
cmp_ok($real, '>', 10000, "datminmxid age $real is past autovacuum_multixact_freeze_max_age");
my ($age, $sev) = split /\|/, status();
cmp_ok($age, '>=', $real, 'cluster mxid_age is the real frontier age, not 0');
is($sev, 'warning', 'severity is warning');
is($node->safe_psql('postgres', 'SELECT status || code FROM pg_horizon_check;'),
	'warning1', 'check status warning, code 1');
like($node->safe_psql('postgres', 'SELECT message FROM pg_horizon_check;'),
	qr/MultiXact age \d+ is past autovacuum_multixact_freeze_max_age/,
	'check explains that it is the MultiXact age');
is($node->safe_psql('postgres',
		"SELECT freeze_constraint FROM pg_horizon_databases WHERE datname = 'postgres'"),
	'aggressive_autovacuum', 'per-database state agrees');
is($node->safe_psql('postgres',
		"SELECT mxid_freeze_max_age || '/' || mxid_failsafe_age FROM pg_horizon"),
	'10000/60000', 'the view exposes the multixact limits');

# --- the relation classification, vacuum can advance ---------------------------
is($node->safe_psql('postgres', "SELECT freeze_constraint FROM pg_horizon_explain('m')"),
	'vacuum_lag', 'nothing holds a multixact: an aged table is vacuum_lag');
like($node->safe_psql('postgres', "SELECT diagnosis FROM pg_horizon_explain('m')"),
	qr/has not been vacuumed far enough/, 'diagnosis says vacuum lag');

# --- past vacuum_multixact_failsafe_age ----------------------------------------
for my $i (1 .. 12)
{
	last if (split /\|/, status())[0] >= 60000;
	burn_multixacts(2, 3000);
}
($age, $sev) = split /\|/, status();
cmp_ok($age, '>=', 60000, 'past the multixact failsafe age');
is($sev, 'critical', 'severity is critical');
is($node->safe_psql('postgres',
		"SELECT freeze_constraint FROM pg_horizon_databases WHERE datname = 'postgres'"),
	'failsafe', 'per-database state is failsafe');

# --- a live multixact pins the horizon -----------------------------------------
$node->safe_psql('postgres', 'VACUUM (FREEZE) m;');
is($node->safe_psql('postgres', "SELECT freeze_constraint FROM pg_horizon_explain('m')"),
	'ok', 'after VACUUM FREEZE the table is ok again');

my $a = $node->background_psql('postgres');
my $b = $node->background_psql('postgres');
$a->query_safe('BEGIN; SELECT id FROM m WHERE id = 1 FOR SHARE;');
$b->query_safe('BEGIN; SELECT id FROM m WHERE id = 1 FOR SHARE;');    # makes a MultiXact
burn_multixacts(2, 1200);
$node->safe_psql('postgres', 'VACUUM (FREEZE) m;');
my ($rmx, $mage) = split /\|/, $node->safe_psql('postgres',
	"SELECT relminmxid, mxid_age FROM pg_horizon_relations WHERE relname = 'm'");
my $hor = $node->safe_psql('postgres',
	q{SELECT substring(summary from 'mxid_horizon=([0-9]+)') FROM pg_horizon_explain('m')});
cmp_ok($mage, '>', 10000, "table is past the limit again ($mage), because the old multixact is still live");
is($rmx, $hor, 'relminmxid is stuck at the oldest live MultiXact');
is($node->safe_psql('postgres', "SELECT freeze_constraint FROM pg_horizon_explain('m')"),
	'horizon', 'so the verdict is horizon: VACUUM cannot help');
like($node->safe_psql('postgres', "SELECT diagnosis FROM pg_horizon_explain('m')"),
	qr/relminmxid is not older than the oldest MultiXact still in use/,
	'and the diagnosis names the MultiXact');
is($node->safe_psql('postgres',
		"SELECT freeze_constraint FROM pg_horizon_relations WHERE relname = 'm'"),
	'horizon', 'pg_horizon_relations agrees');

$a->query_safe('ROLLBACK;');
$b->query_safe('ROLLBACK;');
$a->quit;
$b->quit;
$node->safe_psql('postgres', 'VACUUM (FREEZE) m;');
is($node->safe_psql('postgres', "SELECT freeze_constraint FROM pg_horizon_explain('m')"),
	'ok', 'once the sessions finish, VACUUM FREEZE advances it and the table is ok');

done_testing();
