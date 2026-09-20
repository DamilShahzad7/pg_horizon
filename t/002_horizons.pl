# The horizons pg_horizon reports must be the ones VACUUM really uses.
#
# The oracle is VACUUM (VERBOSE) itself: the "removable cutoff" it prints is
# OldestXmin as computed by core for that relation's horizon kind. We compare
# pg_horizon's shared/catalog/data horizons against it in each situation that
# used to be wrong: a holder in another database, a prepared transaction in
# another database, logical slots (catalog only), invalidated slots,
# user_catalog_table, and the observing session's own snapshot.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('horizons');
$node->init(extra => [ '--wal-segsize', '1' ]);
$node->append_conf('postgresql.conf', q{
autovacuum = off
wal_level = logical
max_replication_slots = 8
max_wal_senders = 4
max_prepared_transactions = 8
max_slot_wal_keep_size = 8MB
});
$node->start;

$node->safe_psql('postgres', 'CREATE DATABASE db2;');
$node->safe_psql('postgres', 'CREATE EXTENSION pg_horizon;');
$node->safe_psql('db2', 'CREATE EXTENSION pg_horizon;');
$node->safe_psql('postgres', q{
CREATE TABLE t1 AS SELECT g AS id FROM generate_series(1, 100) g;
CREATE TABLE t_uc (id int) WITH (user_catalog_table = true);
});
$node->safe_psql('db2', 'CREATE TABLE other(id int);');

# What VACUUM would use as OldestXmin for this relation right now.
sub vac_cutoff
{
	my ($db, $rel) = @_;
	my ($out, $err) = ('', '');
	$node->psql($db, "VACUUM (VERBOSE) $rel;", stdout => \$out, stderr => \$err);
	$err =~ /removable cutoff: (\d+)/
	  or BAIL_OUT("no removable cutoff for $rel in: $err");
	return $1;
}

sub horizons
{
	my ($db) = @_;
	my $r = $node->safe_psql($db,
		'SELECT shared_xmin, catalog_xmin, data_xmin, next_xid FROM pg_horizon;');
	my ($s, $c, $d, $n) = split /\|/, $r;
	return { shared => $s, catalog => $c, data => $d, next => $n };
}

# Burn some XIDs so horizons are distinguishable from "latest completed + 1".
sub burn
{
	$node->safe_psql('postgres', 'SELECT pg_current_xact_id();') for 1 .. ($_[0] // 5);
}

sub compare_with_vacuum
{
	my ($label, $db, $data_rel) = @_;
	my $h = horizons($db);
	is($h->{data}, vac_cutoff($db, $data_rel),
		"$label: data_xmin == VACUUM cutoff of a user table");
	# Re-read: VACUUM does not consume XIDs, but keep the comparison tight.
	my $h2 = horizons($db);
	is($h2->{catalog}, vac_cutoff($db, 'pg_class'),
		"$label: catalog_xmin == VACUUM cutoff of a catalog");
	is($h2->{shared}, vac_cutoff($db, 'pg_database'),
		"$label: shared_xmin == VACUUM cutoff of a shared catalog");
}

# --- A. nothing running ---------------------------------------------------
burn();
compare_with_vacuum('quiet cluster', 'postgres', 't1');

# --- B. holder in the same database ----------------------------------------
my $hold1 = $node->background_psql('postgres');
my $xid1 = $hold1->query_safe('BEGIN; SELECT pg_current_xact_id()::text::bigint;');
chomp $xid1;
burn(10);
my $h = horizons('postgres');
is($h->{data}, $xid1, 'same-db holder pins data_xmin at its xid');
is($h->{shared}, $xid1, 'same-db holder pins shared_xmin');
compare_with_vacuum('same-db holder', 'postgres', 't1');
is($node->safe_psql('postgres',
		"SELECT count(*) FROM pg_horizon_blockers WHERE xid = '$xid1'::text::xid AND is_horizon_holder AND affects_data_horizon"),
	'1', 'same-db holder is flagged as a horizon holder that affects the data horizon');
is($node->safe_psql('postgres',
		"SELECT dominant_blocker LIKE '%idle in transaction%' FROM pg_horizon_explain('t1')"),
	't', 'explain names the same-db holder as dominant blocker');
is($node->safe_psql('postgres',
		"SELECT freeze_constraint FROM pg_horizon_explain('t1')"),
	'ok', 'a young table is ok even with a holder');
$hold1->query_safe('ROLLBACK;');
$hold1->quit;

# --- C. holder in ANOTHER database -----------------------------------------
my $hold2 = $node->background_psql('db2');
my $xid2 = $hold2->query_safe('BEGIN; SELECT pg_current_xact_id()::text::bigint;');
chomp $xid2;
burn(10);
$h = horizons('postgres');
cmp_ok($h->{data}, '>', $xid2,
	'other-db holder does NOT pin the data horizon (this was the false attribution)');
is($h->{shared}, $xid2, 'other-db holder does pin the shared horizon');
compare_with_vacuum('other-db holder', 'postgres', 't1');
is($node->safe_psql('postgres',
		"SELECT is_horizon_holder AND NOT affects_data_horizon AND NOT affects_catalog_horizon FROM pg_horizon_blockers WHERE datname = 'db2'"),
	't', 'other-db holder pins only the shared horizon and is scoped out of data/catalog');
is($node->safe_psql('postgres',
		"SELECT dominant_blocker IS NULL FROM pg_horizon_explain('t1')"),
	't', 'explain(user table) does not blame a session in another database');
is($node->safe_psql('postgres',
		"SELECT dominant_blocker IS NOT NULL FROM pg_horizon_explain('pg_database')"),
	't', 'explain(shared catalog) does blame it: shared relations see all databases');
is($node->safe_psql('postgres',
		"SELECT count(*) FROM pg_horizon_check WHERE horizon_holders = 1"),
	'1', 'check counts it as a holder of the shared horizon');
$hold2->query_safe('ROLLBACK;');
$hold2->quit;

# --- D. prepared transaction in another database ---------------------------
$node->safe_psql('db2', q{
BEGIN; SELECT pg_current_xact_id(); PREPARE TRANSACTION 'other_db_prep';
});
my $pxid = $node->safe_psql('db2',
	q{SELECT transaction::text::bigint FROM pg_prepared_xacts WHERE gid = 'other_db_prep'});
burn(10);
$h = horizons('postgres');
cmp_ok($h->{data}, '>', $pxid, 'prepared xact in another db does not pin this data horizon');
is($h->{shared}, $pxid, 'prepared xact in another db pins the shared horizon');
compare_with_vacuum('prepared xact in other db', 'postgres', 't1');
$node->safe_psql('db2', "ROLLBACK PREPARED 'other_db_prep';");

# --- E. prepared transaction in THIS database ------------------------------
$node->safe_psql('postgres', q{
BEGIN; SELECT pg_current_xact_id(); PREPARE TRANSACTION 'same_db_prep';
});
$pxid = $node->safe_psql('postgres',
	q{SELECT transaction::text::bigint FROM pg_prepared_xacts WHERE gid = 'same_db_prep'});
burn(10);
$h = horizons('postgres');
is($h->{data}, $pxid, 'prepared xact in this db pins the data horizon');
compare_with_vacuum('prepared xact in this db', 'postgres', 't1');
is($node->safe_psql('postgres',
		"SELECT count(*) FROM pg_horizon_blockers WHERE prepared_gid = 'same_db_prep' AND is_horizon_holder AND affects_data_horizon"),
	'1', 'same-db prepared xact is a data-horizon holder');
$node->safe_psql('postgres', "ROLLBACK PREPARED 'same_db_prep';");

# --- F. logical slot: catalog horizon only ---------------------------------
burn(3);
$node->safe_psql('postgres',
	"SELECT pg_create_logical_replication_slot('hz_slot', 'test_decoding');");
my $slot_cat = $node->safe_psql('postgres',
	"SELECT catalog_xmin::text::bigint FROM pg_replication_slots WHERE slot_name = 'hz_slot'");
burn(20);
$h = horizons('postgres');
is($h->{catalog}, $slot_cat, 'logical slot catalog_xmin pins the catalog horizon');
cmp_ok($h->{data}, '>', $slot_cat, 'logical slot does not pin the data horizon');
compare_with_vacuum('logical slot', 'postgres', 't1');
is($node->safe_psql('postgres',
		"SELECT freeze_constraint FROM pg_horizon_explain('pg_class')"),
	'ok', 'catalog table is ok');
is($node->safe_psql('postgres',
		"SELECT summary LIKE '%horizon=catalog%' FROM pg_horizon_explain('pg_class')"),
	't', 'explain(pg_class) is subject to the catalog horizon');
is($node->safe_psql('postgres',
		"SELECT summary LIKE '%horizon=data%' FROM pg_horizon_explain('t1')"),
	't', 'explain(t1) is subject to the data horizon');
is($node->safe_psql('postgres',
		"SELECT dominant_blocker LIKE 'Logical slot hz_slot%' FROM pg_horizon_explain('pg_class')"),
	't', 'the logical slot is the dominant blocker of catalog tables');
is($node->safe_psql('postgres',
		"SELECT dominant_blocker IS NULL FROM pg_horizon_explain('t1')"),
	't', 'the logical slot is not blamed for a plain data table');
# user_catalog_table is subject to the catalog horizon when wal_level=logical
is($node->safe_psql('postgres',
		"SELECT summary LIKE '%horizon=catalog%' FROM pg_horizon_explain('t_uc')"),
	't', 'user_catalog_table is classified with the catalog horizon');
is(vac_cutoff('postgres', 't_uc'), $h->{catalog},
	'user_catalog_table VACUUM cutoff is the catalog horizon');
is($node->safe_psql('postgres',
		"SELECT xmin_age IS NULL AND catalog_xmin_age IS NOT NULL AND horizon_age = catalog_xmin_age FROM pg_horizon_blockers WHERE slot_name = 'hz_slot'"),
	't', 'a catalog-only slot has NULL xmin_age and a usable horizon_age');
is($node->safe_psql('postgres',
		"SELECT slot_name FROM pg_horizon_blockers ORDER BY horizon_age DESC NULLS LAST LIMIT 1"),
	'hz_slot', 'ordering by horizon_age puts the oldest holder first');

# --- G. invalidated slot is not a holder (core ignores it) -----------------
$node->safe_psql('postgres', 'CREATE TABLE waste(a text);');
for my $i (1 .. 6)
{
	$node->safe_psql('postgres',
		"INSERT INTO waste SELECT repeat('x', 1000) FROM generate_series(1, 4000); SELECT pg_switch_wal();");
}
$node->safe_psql('postgres', 'CHECKPOINT;');
my $inval = $node->safe_psql('postgres',
	"SELECT wal_status = 'lost' FROM pg_replication_slots WHERE slot_name = 'hz_slot'");
SKIP:
{
	skip 'slot was not invalidated on this build', 3 unless $inval eq 't';
	is($node->safe_psql('postgres',
			"SELECT count(*) FROM pg_horizon_blockers WHERE slot_name = 'hz_slot'"),
		'0', 'an invalidated slot is not listed as a holder');
	compare_with_vacuum('invalidated slot', 'postgres', 't1');
}
$node->safe_psql('postgres', "SELECT pg_drop_replication_slot('hz_slot');");

# --- H. the observer's own snapshot ----------------------------------------
# pg_horizon reports what a VACUUM started elsewhere would see, so the
# observing session is excluded from its own report. Seen from another
# session, though, that old REPEATABLE READ snapshot is a real holder.
my $obs = $node->background_psql('postgres');
my $obs_pid = $obs->query_safe('SELECT pg_backend_pid();');
chomp $obs_pid;
my $obs_xmin = $obs->query_safe(
	'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t1; SELECT backend_xmin::text::bigint FROM pg_stat_activity WHERE pid = pg_backend_pid();');
$obs_xmin = (split /\n/, $obs_xmin)[-1];
burn(10);
my $seen = $obs->query_safe(
	'SELECT is_horizon_holder FROM pg_horizon_blockers(true) WHERE pid = pg_backend_pid();');
chomp $seen;
is($seen, 'f', 'the observing session is never reported as a holder in its own report');
my $od = $obs->query_safe('SELECT data_xmin::text::bigint FROM pg_horizon;');
chomp $od;
cmp_ok($od, '>', $obs_xmin, 'seen from inside, the observer\'s own snapshot is not part of the horizon');
is(horizons('postgres')->{data}, $obs_xmin,
	'seen from another session, the same snapshot pins the data horizon');
is($node->safe_psql('postgres',
		"SELECT is_horizon_holder FROM pg_horizon_blockers WHERE pid = $obs_pid"),
	't', 'and is flagged as a holder there');
$obs->query_safe('ROLLBACK;');
$obs->quit;

# --- I. temporary tables ----------------------------------------------------
is($node->safe_psql('postgres', q{
CREATE TEMP TABLE tmp_t(a int);
SELECT summary LIKE '%horizon=temporary%' FROM pg_horizon_explain('tmp_t');
}), 't', 'own temp table has a temporary horizon');

done_testing();
