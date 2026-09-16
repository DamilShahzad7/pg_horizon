# PostgreSQL TAP tests live in t/; Cluster is provided by PGXS.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run ();

my $node = PostgreSQL::Test::Cluster->new('horizon');
$node->init(extra => [ '--wal-segsize', '1' ]);
$node->append_conf('postgresql.conf', q{
wal_level = logical
max_replication_slots = 4
max_wal_senders = 4
max_prepared_transactions = 5
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_horizon;');

my $severity = $node->safe_psql('postgres', 'SELECT severity FROM pg_horizon;');
like($severity, qr/^(ok|warning|critical|emergency)$/, 'cluster severity is a known value');

# A second session that stays idle in transaction must appear as a backend
# xmin holder. The observer session is excluded by default.
my $blocker = $node->background_psql('postgres');
my $pid = $blocker->query_safe('SELECT pg_backend_pid();');
chomp($pid);
$blocker->query_safe('BEGIN; SELECT pg_current_xact_id();');

ok($node->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_horizon_blockers(false) WHERE pid = $pid AND is_idle_in_transaction"),
	'idle-in-transaction backend is attributed as a blocker');

my $reason = $node->safe_psql('postgres',
	"SELECT reason LIKE '%idle in transaction%' FROM pg_horizon_blockers(false) WHERE pid = $pid");
is($reason, 't', 'blocker reason names idle in transaction');

my $safe = $node->safe_psql('postgres',
	"SELECT safe_to_terminate FROM pg_horizon_blockers(false) WHERE pid = $pid");
is($safe, 't', 'idle-in-transaction is marked safe_to_terminate');

my $rec = $node->safe_psql('postgres',
	"SELECT recommended_sql LIKE '%COMMIT%' AND recommended_sql LIKE '%pg_terminate_backend%' FROM pg_horizon_blockers(false) WHERE pid = $pid");
is($rec, 't', 'idle-in-transaction recommends commit first, terminate only if abandoned');

# Terminate stays refused while the GUC is off — no hidden kill path.
my $err = $node->psql('postgres',
	"SELECT pg_horizon_terminate_blocker($pid);");
isnt($err, 0, 'terminate refuses to run with GUC off');

# Guarded terminate uses pg_terminate_backend after the GUC is enabled.
my $terminated = $node->safe_psql('postgres', qq{
SET pg_horizon.terminate_blockers = on;
SELECT pg_horizon_terminate_blocker($pid);
});
is($terminated, 't', 'terminate signals idle-in-transaction xmin holder');

ok($node->poll_query_until('postgres',
		"SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $pid"),
	'terminated backend is gone');

eval { $blocker->quit; };

# Logical slot holds catalog_xmin. Consume/drop is the real fix, not VACUUM FULL.
$node->safe_psql('postgres',
	"SELECT slot_name FROM pg_create_logical_replication_slot('horizon_slot', 'test_decoding');");

my $slots = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_horizon_blockers(false) WHERE slot_name = 'horizon_slot' AND blocker_type = 'logical_slot'");
is($slots, '1', 'logical replication slot is attributed as a catalog xmin holder');

my $slot_sql = $node->safe_psql('postgres',
	"SELECT recommended_sql LIKE '%pg_drop_replication_slot%' FROM pg_horizon_blockers(false) WHERE slot_name = 'horizon_slot'");
is($slot_sql, 't', 'slot recommendation does not invent a vacuum workaround');

my $slot_plugin = $node->safe_psql('postgres',
	"SELECT plugin = 'test_decoding' FROM pg_horizon_blockers(false) WHERE slot_name = 'horizon_slot'");
is($slot_plugin, 't', 'logical slot plugin is exposed');

my $slot_cat = $node->safe_psql('postgres', q{
SELECT age(h.catalog_xmin) >= age(s.catalog_xmin)
FROM pg_horizon h, pg_replication_slots s
WHERE s.slot_name = 'horizon_slot' AND s.catalog_xmin IS NOT NULL
});
is($slot_cat, 't', 'catalog xmin is at least as old as the logical slot pin');

$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('horizon_slot');");

# Prepared transactions are dummy PGPROCs with pid 0; the gid is the fix.
$node->safe_psql('postgres', q{
BEGIN;
SELECT pg_current_xact_id();
PREPARE TRANSACTION 'horizon_prep';
});

my $prep = $node->safe_psql('postgres',
	"SELECT count(*) FROM pg_horizon_blockers(false) WHERE blocker_type = 'prepared' AND prepared_gid = 'horizon_prep'");
is($prep, '1', 'prepared transaction is attributed with its gid');

my $prep_sql = $node->safe_psql('postgres',
	"SELECT recommended_sql LIKE '%ROLLBACK PREPARED%' FROM pg_horizon_blockers(false) WHERE prepared_gid = 'horizon_prep'");
is($prep_sql, 't', 'prepared recommendation is COMMIT/ROLLBACK PREPARED, not vacuum');

$node->safe_psql('postgres', "ROLLBACK PREPARED 'horizon_prep';");

# Lock waiters are only attributed when the held mode actually conflicts.
$node->safe_psql('postgres', 'CREATE TABLE horizon_lock_demo(id int);');
my $holder = $node->background_psql('postgres');
my $hpid = $holder->query_safe('SELECT pg_backend_pid();');
chomp($hpid);
$holder->query_safe('BEGIN; LOCK TABLE horizon_lock_demo IN ACCESS EXCLUSIVE MODE; SELECT pg_current_xact_id();');

my ($win, $wout, $werr) = ('', '', '');
my $waiter = IPC::Run::start(
	[
		'psql', '-XAt', '-v', 'ON_ERROR_STOP=1',
		'-d', $node->connstr('postgres'),
		'-c', 'BEGIN; LOCK TABLE horizon_lock_demo IN ACCESS SHARE MODE;'
	],
	\$win, \$wout, \$werr);

ok($node->poll_query_until('postgres',
		q{SELECT count(*) > 0 FROM pg_locks WHERE NOT granted AND relation = 'horizon_lock_demo'::regclass}),
	'waiter is blocked on ACCESS EXCLUSIVE');

my $lockhit = $node->safe_psql('postgres',
	"SELECT holds_locks AND waiting_pids IS NOT NULL FROM pg_horizon_blockers(false) WHERE pid = $hpid AND is_idle_in_transaction");
is($lockhit, 't', 'conflicting lock waiter is attached to the xmin holder');

$waiter->kill_kill;
eval { $holder->query_safe('ROLLBACK;'); };
$holder->quit;
$node->safe_psql('postgres', 'DROP TABLE horizon_lock_demo;');

$node->stop;
done_testing();
