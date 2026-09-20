# hot_standby_feedback holders on the primary, and the extension on a standby.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $primary = PostgreSQL::Test::Cluster->new('primary');
$primary->init(allows_streaming => 1);
$primary->append_conf('postgresql.conf', q{
autovacuum = off
max_replication_slots = 4
max_wal_senders = 4
});
$primary->start;
$primary->safe_psql('postgres', q{
CREATE EXTENSION pg_horizon;
CREATE TABLE t1 AS SELECT g AS id FROM generate_series(1, 100) g;
});

$primary->backup('bkp');
my $standby = PostgreSQL::Test::Cluster->new('standby');
$standby->init_from_backup($primary, 'bkp', has_streaming => 1);
$standby->append_conf('postgresql.conf', "hot_standby_feedback = on\n");
$standby->start;
$primary->wait_for_replay_catchup($standby);

sub burn
{
	$primary->safe_psql('postgres', 'SELECT pg_current_xact_id();') for 1 .. ($_[0] // 5);
}

# --- feedback without a slot: a walsender xmin --------------------------------
my $hold = $standby->background_psql('postgres');
$hold->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t1;');
burn(10);
ok($primary->poll_query_until('postgres',
	'SELECT count(*) = 1 FROM pg_stat_replication WHERE backend_xmin IS NOT NULL'),
	'the standby feedback xmin reaches the primary');

my $fb = $primary->safe_psql('postgres',
	"SELECT xmin::text::bigint FROM pg_horizon_blockers WHERE blocker_type = 'standby_feedback'");
is($fb, $primary->safe_psql('postgres', 'SELECT backend_xmin::text::bigint FROM pg_stat_replication'),
	'standby_feedback row carries the walsender xmin');
is($primary->safe_psql('postgres', q{
SELECT is_horizon_holder AND affects_data_horizon AND affects_catalog_horizon AND NOT safe_to_terminate
FROM pg_horizon_blockers WHERE blocker_type = 'standby_feedback'}),
	't', 'it pins every horizon (not scoped to one database) and is never terminable');
is($primary->safe_psql('postgres', 'SELECT data_xmin::text::bigint FROM pg_horizon'), $fb,
	'the primary data horizon is the feedback xmin');
like($primary->safe_psql('postgres', "SELECT dominant_blocker FROM pg_horizon_explain('t1')"),
	qr/^WAL sender pid \d+ applies hot_standby_feedback/, 'explain blames the walsender and points at the standby');

my ($out, $err) = ('', '');
my $rc = $primary->psql('postgres', q{
SET pg_horizon.terminate_blockers = on;
SELECT pg_horizon_terminate_blocker(pid) FROM pg_horizon_blockers WHERE blocker_type = 'standby_feedback';},
	stdout => \$out, stderr => \$err);
like($err, qr/not a session pg_horizon will terminate/, 'terminate refuses a walsender');
is($primary->safe_psql('postgres', 'SELECT count(*) FROM pg_stat_replication'), '1',
	'and the replica is still connected');

# --- the extension running ON the standby ------------------------------------------
is($standby->safe_psql('postgres', 'SELECT in_recovery FROM pg_horizon'), 't', 'standby reports in_recovery');
is($standby->safe_psql('postgres', q{SELECT count(*) FROM pg_horizon_blockers(true) WHERE state = 'idle in transaction'}),
	'1', 'the standby lists its own idle-in-transaction session');
is($standby->safe_psql('postgres', 'SELECT count(*) FROM pg_horizon_check'), '1', 'check works in recovery');
like($standby->safe_psql('postgres', "SELECT freeze_constraint FROM pg_horizon_explain('t1')"),
	qr/^(ok|horizon|vacuum_lag)$/, 'explain works in recovery');
like($standby->safe_psql('postgres', 'SELECT pg_horizon_report()'), qr/in_recovery:\s+yes/, 'report works in recovery');
like($standby->safe_psql('postgres', 'SELECT pg_horizon_report()'), qr/NOT excluded during recovery/,
	'and says that the observer is not excluded there');
is($standby->safe_psql('postgres', "SELECT count(*) > 0 FROM pg_horizon_relations WHERE relname = 't1'"),
	't', 'relations work in recovery');
is($standby->safe_psql('postgres', 'SELECT count(*) > 0 FROM pg_horizon_databases'),
	't', 'databases work in recovery');

$hold->query_safe('ROLLBACK;');
$hold->quit;
# An idle standby keeps sending a (recent) xmin, so the walsender row stays;
# what must happen is that the primary horizon moves past the old holder.
burn(5);
ok($primary->poll_query_until('postgres',
	"SELECT data_xmin::text::bigint > $fb FROM pg_horizon"),
	'the primary data horizon advances once the standby session ends');

# --- feedback through a physical slot ------------------------------------------------
$primary->safe_psql('postgres', "SELECT pg_create_physical_replication_slot('hz_phys');");
$standby->stop;
$standby->append_conf('postgresql.conf', "primary_slot_name = 'hz_phys'\n");
$standby->start;
$primary->wait_for_replay_catchup($standby);
my $hold2 = $standby->background_psql('postgres');
$hold2->query_safe('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t1;');
burn(10);
ok($primary->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_replication_slots WHERE slot_name = 'hz_phys' AND xmin IS NOT NULL"),
	'with a slot the xmin is recorded in the slot');
is($primary->safe_psql('postgres', q{
SELECT blocker_type || ':' || slot_name || ':' || is_horizon_holder || ':' || slot_active
FROM pg_horizon_blockers WHERE blocker_type = 'physical_slot'}),
	'physical_slot:hz_phys:true:true', 'the physical slot is the holder');
is($primary->safe_psql('postgres', q{
SELECT backend_type, state FROM pg_horizon_blockers WHERE blocker_type = 'physical_slot'}),
	'physical replication slot|active',
	'the slot row keeps its own type and state (it is not overwritten by its walsender)');
is($primary->safe_psql('postgres', q{
SELECT wal_retained_bytes IS NOT NULL FROM pg_horizon_blockers WHERE blocker_type = 'physical_slot'}),
	't', 'retained WAL is reported');
$hold2->query_safe('ROLLBACK;');
$hold2->quit;

done_testing();
