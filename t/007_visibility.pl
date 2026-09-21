# Disclosure rules, consistency of counts, no waiting on locks, no ghosts.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run ();

my $node = PostgreSQL::Test::Cluster->new('visibility');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
max_prepared_transactions = 8
});
$node->start;

$node->safe_psql('postgres', q{
CREATE EXTENSION pg_horizon;
CREATE ROLE alice LOGIN;
CREATE ROLE bob LOGIN;
CREATE ROLE monitor LOGIN IN ROLE pg_read_all_stats;
CREATE TABLE t1 AS SELECT g AS id FROM generate_series(1, 10) g;
GRANT ALL ON t1 TO alice, bob;
});

# A session that keeps its *last statement* as pg_stat_activity.query.
# (BackgroundPsql appends an empty ";" statement after every query, which
# would replace the text we want to look at.)
sub open_session
{
	my ($node, $user, $sql) = @_;
	my %s = (in => $sql, out => '', err => '');
	$s{h} = IPC::Run::start(
		[ 'psql', '-XAtq', '-d', $node->connstr('postgres') . " user=$user" ],
		'<', \$s{in}, '>', \$s{out}, '2>', \$s{err});
	$s{h}->pump_nb;
	return \%s;
}

sub close_session
{
	my ($s) = @_;
	$s->{in} .= "ROLLBACK;\n\\q\n";
	$s->{h}->finish;
}

sub as_user
{
	my ($user, $sql) = @_;
	my ($out, $err) = ('', '');
	$node->psql('postgres', $sql, stdout => \$out, stderr => \$err,
		connstr => $node->connstr('postgres') . " user=$user");
	return ($out, $err);
}

# --- what one role may learn about another role's session --------------------
my $bob = open_session($node, 'bob',
	"SET application_name = 'bob-batch';\nBEGIN;\nSELECT pg_current_xact_id();\nSELECT 'secret-marker';\n");
ok($node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE application_name = 'bob-batch' AND state = 'idle in transaction'"),
	'bob\'s session is idle in transaction');
my $bpid = $node->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE application_name = 'bob-batch'");

my $cols = q{state IS NULL AS state_null, xact_start IS NULL AS xs_null, wait_event_type IS NULL AS wet_null,
             wait_event IS NULL AS we_null, backend_type IS NULL AS bt_null, is_idle_in_transaction IS NULL AS idle_null,
             safe_to_terminate, recommended_sql IS NULL AS rec_null, query, usename, application_name, xid IS NOT NULL AS has_xid};
my ($out, $err) = as_user('alice', "SELECT $cols FROM pg_horizon_blockers WHERE pid = $bpid");
is($err, '', 'an unprivileged role can query the view');
is($out, 't|t|t|t|t|t|f|t|<insufficient privilege>|bob|bob-batch|t',
	'another role\'s session: state, xact_start, wait event, type, query and advice are hidden; pid, role, application and xid are not');
like((as_user('alice', "SELECT reason FROM pg_horizon_blockers WHERE pid = $bpid"))[0],
	qr/not visible to this role/, 'the reason says why');
unlike((as_user('alice', "SELECT reason || coalesce(recommended_sql,'') FROM pg_horizon_blockers WHERE pid = $bpid"))[0],
	qr/idle in transaction|secret-marker|pg_terminate_backend/, 'and leaks neither state nor a kill command');

($out, $err) = as_user('bob', "SELECT state, is_idle_in_transaction, safe_to_terminate, query FROM pg_horizon_blockers WHERE pid = $bpid");
is($out, "idle in transaction|t|t|SELECT 'secret-marker';", 'the owner sees everything about their own session');
($out, $err) = as_user('monitor', "SELECT state, query FROM pg_horizon_blockers WHERE pid = $bpid");
is($out, "idle in transaction|SELECT 'secret-marker';", 'pg_read_all_stats sees everything');
($out, $err) = as_user('postgres', "SELECT state FROM pg_horizon_blockers WHERE pid = $bpid");
is($out, 'idle in transaction', 'superuser sees everything');

# Aggregates do not leak details but still count.
($out, $err) = as_user('alice', "SELECT blocker_count FROM pg_horizon");
is($out, '1', 'alice still gets the count');
like((as_user('alice', "SELECT message FROM pg_horizon_check"))[0], qr/./, 'and a check message');
unlike((as_user('alice', "SELECT pg_horizon_report()"))[0], qr/idle in transaction|bob-batch/,
	'the report shows no state or application of a hidden session');

# Slots and prepared transactions are public catalogs and stay visible.
$node->safe_psql('postgres', q{BEGIN; SELECT pg_current_xact_id(); PREPARE TRANSACTION 'vis_prep';});
($out, $err) = as_user('alice', "SELECT prepared_gid, state FROM pg_horizon_blockers WHERE blocker_type = 'prepared'");
is($out, 'vis_prep|prepared', 'prepared transactions are visible to everyone, as in pg_prepared_xacts');
($out, $err) = as_user('alice', "SELECT count(*) FROM pg_prepared_xacts");
is($err, '', 'pg_prepared_xacts is readable by an unprivileged role (the extension relies on it)');

# --- a finished prepared transaction is not a holder ---------------------------
$node->safe_psql('postgres', q{ROLLBACK PREPARED 'vis_prep';});
close_session($bob);
$node->safe_psql('postgres', 'SELECT pg_current_xact_id();') for 1 .. 3;
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_horizon_blockers WHERE blocker_type = 'prepared'}),
	'0', 'after ROLLBACK PREPARED no phantom prepared holder remains');
$node->safe_psql('postgres', q{BEGIN; SELECT pg_current_xact_id(); PREPARE TRANSACTION 'vis_prep2';});
$node->safe_psql('postgres', q{COMMIT PREPARED 'vis_prep2';});
$node->safe_psql('postgres', 'SELECT pg_current_xact_id();') for 1 .. 3;
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_horizon_blockers WHERE blocker_type = 'prepared'}),
	'0', 'nor after COMMIT PREPARED');
is($node->safe_psql('postgres', q{SELECT horizon_holders FROM pg_horizon_check}),
	'0', 'and the check sees no holders');
is($node->safe_psql('postgres', q{SELECT data_xmin = next_xid FROM pg_horizon}),
	't', 'the data horizon is back at "now"');

# --- pg_horizon.min_xmin_age is applied everywhere ---------------------------------
my $h1 = $node->background_psql('postgres');
$h1->query_safe('BEGIN; SELECT pg_current_xact_id();');
$node->safe_psql('postgres', 'SELECT pg_current_xact_id();') for 1 .. 5;
is($node->safe_psql('postgres', q{SELECT (SELECT count(*) FROM pg_horizon_blockers) || '/' || (SELECT blocker_count FROM pg_horizon) || '/' || (SELECT horizon_holders FROM pg_horizon_check)}),
	'1/1/1', 'default: view, blocker_count and horizon_holders agree');
is($node->safe_psql('postgres', q{SET pg_horizon.min_xmin_age = 1000000; SELECT (SELECT count(*) FROM pg_horizon_blockers) || '/' || (SELECT blocker_count FROM pg_horizon) || '/' || (SELECT horizon_holders FROM pg_horizon_check)}),
	'0/0/0', 'min_xmin_age hides the holder from all three');
like($node->safe_psql('postgres', q{SET pg_horizon.min_xmin_age = 1000000; SELECT pg_horizon_report()}),
	qr/\(no listed holder pins a horizon\)/, 'and from the report');
$h1->query_safe('ROLLBACK;');
$h1->quit;

# --- diagnostics never wait behind ACCESS EXCLUSIVE ------------------------------
my $locker = $node->background_psql('postgres');
$locker->query_safe('BEGIN; LOCK TABLE t1 IN ACCESS EXCLUSIVE MODE;');
my $t0 = time;
($out, $err) = as_user('postgres', q{
SET lock_timeout = '3s';
SELECT freeze_constraint FROM pg_horizon_explain('t1');
SELECT length(pg_horizon_vacuum_sql('t1')) > 0;
SELECT count(*) FROM pg_horizon_relations WHERE relname = 't1';
SELECT severity FROM pg_horizon;
SELECT count(*) FROM pg_horizon_blockers;
});
is($err, '', 'explain, vacuum_sql, relations, status and blockers all run while t1 is ACCESS EXCLUSIVE locked');
cmp_ok(time - $t0, '<', 3, 'without waiting for the lock');
$locker->query_safe('ROLLBACK;');
$locker->quit;

# --- errors are friendly ---------------------------------------------------------
$node->safe_psql('postgres', q{CREATE TABLE parted (id int) PARTITION BY RANGE (id); CREATE VIEW v1 AS SELECT 1;});
like((as_user('postgres', "SELECT * FROM pg_horizon_explain('parted')"))[1],
	qr/is a partitioned table/, 'partitioned table gets an explanation, not "not a table"');
like((as_user('postgres', "SELECT * FROM pg_horizon_explain('v1')"))[1],
	qr/not a table, materialized view, or TOAST table/, 'a view is refused');
like((as_user('postgres', "SELECT pg_horizon_vacuum_sql(99999999::oid::regclass)"))[1],
	qr/relation with OID 99999999 does not exist/, 'a missing OID is a normal error');
unlike((as_user('postgres', "SELECT pg_horizon_vacuum_sql(99999999::oid::regclass)"))[1],
	qr/could not open/, 'not an internal "could not open relation" error');

# --- repeated calls do not leak SPI or memory --------------------------------------
$node->safe_psql('postgres', q{BEGIN; SELECT pg_current_xact_id(); PREPARE TRANSACTION 'leak_prep';});
my $log_off = -s $node->logfile;
($out, $err) = as_user('postgres', q{SELECT count(DISTINCT g) FROM generate_series(1, 400) g, LATERAL (SELECT count(*) FROM pg_horizon_blockers(g % 2 = 0)) x; SELECT count(*) FROM (SELECT (pg_horizon_status()).blocker_count FROM generate_series(1, 400)) s});
is($err, '', '400 calls with a prepared transaction present run cleanly');
unlike(slurp_file($node->logfile, $log_off), qr/WARNING|SPI|leak/i, 'and log no warnings');
$node->safe_psql('postgres', q{ROLLBACK PREPARED 'leak_prep';});

# --- the severity explanation must not name a hidden session -----------------------
# A holder older than 1,000,000 XIDs makes the cluster "warning" and the
# explanation used to say "pid N has been idle in transaction" to everybody.
my $stale = open_session($node, 'bob',
	"SET application_name = 'bob-stale';\nBEGIN;\nSELECT pg_current_xact_id();\n");
ok($node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE application_name = 'bob-stale' AND state = 'idle in transaction'"),
	'the stale holder is idle in transaction');
my $spid = $node->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE application_name = 'bob-stale'");
my $burn = $node->basedir . '/burn.sql';
append_to_file($burn, "SELECT pg_current_xact_id();\n");
$node->command_ok(
	[ 'pgbench', '-n', '-f', $burn, '-c', '8', '-j', '4', '-t', '135000',
	  '-h', $node->host, '-p', $node->port, 'postgres' ],
	'burned about a million XIDs');

($out, $err) = as_user('postgres', "SELECT status, message FROM pg_horizon_check");
like($out, qr/^warning\|pid $spid has been idle in transaction holding the horizon/,
	'a privileged caller gets the full explanation');
($out, $err) = as_user('alice', "SELECT status, message FROM pg_horizon_check");
like($out, qr/^warning\|/, 'an unprivileged caller still sees the warning');
unlike($out, qr/idle in transaction|\b$spid\b/, '... but its explanation names neither the pid nor the state');
like($out, qr/another role has held the horizon/, '... and says why it is vague');
($out, $err) = as_user('alice', "SELECT pg_horizon_report()");
unlike($out, qr/idle in transaction|\bpid $spid has\b/, 'the report explanation is redacted as well');
($out, $err) = as_user('bob', "SELECT message FROM pg_horizon_check");
like($out, qr/pid $spid has been idle in transaction/, 'the session\'s own role sees the full explanation');
close_session($stale);

done_testing();
