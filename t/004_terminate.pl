# pg_horizon_terminate_blocker(): authorization, refusals, and stale state.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run ();

my $node = PostgreSQL::Test::Cluster->new('terminate');
$node->init;
$node->append_conf('postgresql.conf', q{
autovacuum = off
max_prepared_transactions = 4
});
$node->start;

$node->safe_psql('postgres', q{
CREATE EXTENSION pg_horizon;
CREATE ROLE alice LOGIN;
CREATE ROLE bob LOGIN;
CREATE ROLE sigr LOGIN IN ROLE pg_signal_backend;
GRANT EXECUTE ON FUNCTION pg_horizon_terminate_blocker(integer, boolean) TO alice, sigr;
ALTER ROLE sigr SET pg_horizon.terminate_blockers = on;
});

sub as_user
{
	my ($user, $sql) = @_;
	my ($out, $err) = ('', '');
	my $rc = $node->psql('postgres', $sql,
		stdout => \$out, stderr => \$err,
		connstr => $node->connstr('postgres') . " user=$user");
	return ($rc, $out, $err);
}

# A session of a given role, idle in transaction with an xid. (background_psql
# only honours a role in the connection string from PostgreSQL 18 on, so use
# a plain psql pipe that works on every supported version.)
my $holder_n = 0;

sub open_session
{
	my ($user, $sql) = @_;
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
	eval { $s->{h}->finish; };
}

sub idle_holder
{
	my ($user) = @_;
	my $app = 'hz_holder_' . ++$holder_n;
	my $s = open_session($user,
		"SET application_name = '$app';\nBEGIN;\nSELECT pg_current_xact_id();\n");
	$node->poll_query_until('postgres',
		"SELECT count(*) = 1 FROM pg_stat_activity WHERE application_name = '$app' AND state = 'idle in transaction'")
	  or die "holder $app did not start";
	my $pid = $node->safe_psql('postgres',
		"SELECT pid FROM pg_stat_activity WHERE application_name = '$app'");
	return ($s, $pid);
}

sub alive
{
	return $node->safe_psql('postgres',
		"SELECT count(*) FROM pg_stat_activity WHERE pid = $_[0]");
}

# --- the GUC gate ------------------------------------------------------------
my ($s1, $p1) = idle_holder('bob');
my ($rc, $out, $err) = as_user('postgres', "SELECT pg_horizon_terminate_blocker($p1);");
isnt($rc, 0, 'refused while pg_horizon.terminate_blockers is off');
like($err, qr/pg_horizon\.terminate_blockers is off/, '... with the reason');
is(alive($p1), '1', '... and the session is untouched');

# A non-superuser cannot flip the switch, even before the library is loaded in
# its session (the value is only a placeholder until then, and is re-checked).
($rc, $out, $err) = as_user('alice', q{
SET pg_horizon.terminate_blockers = on;
SELECT severity FROM pg_horizon;
SHOW pg_horizon.terminate_blockers;
});
like($out, qr/^off$/m, 'a non-superuser cannot enable the GUC via a placeholder SET');

($rc, $out, $err) = as_user('alice', "SELECT pg_horizon_terminate_blocker($p1);");
isnt($rc, 0, 'alice (EXECUTE granted, GUC off) is refused');

# A role without EXECUTE cannot even call it.
($rc, $out, $err) = as_user('bob', "SELECT pg_horizon_terminate_blocker($p1);");
like($err, qr/permission denied for function pg_horizon_terminate_blocker/,
	'PUBLIC has no EXECUTE');

# --- refusals ---------------------------------------------------------------
my $log_off = -s $node->logfile;
($rc, $out, $err) = as_user('postgres', q{
SET pg_horizon.terminate_blockers = on;
SELECT pg_horizon_terminate_blocker(999999);
});
like($err, qr/is not holding an xmin\/xid/, 'unknown pid is refused');
($rc, $out, $err) = as_user('postgres', q{
SET pg_horizon.terminate_blockers = on;
SELECT pg_horizon_terminate_blocker(pg_backend_pid());
});
like($err, qr/refusing to terminate the current backend/, 'never the current backend');
($rc, $out, $err) = as_user('postgres', q{
SET pg_horizon.terminate_blockers = on;
SELECT pg_horizon_terminate_blocker(0);
});
like($err, qr/pid must be a live backend/, 'pid 0 is not a target');
($rc, $out, $err) = as_user('postgres', q{
SET pg_horizon.terminate_blockers = on;
SELECT pg_horizon_terminate_blocker(NULL) IS NULL;
});
like($out, qr/^t$/m, 'NULL in, NULL out');

# --- pg_signal_backend --------------------------------------------------------
my ($su, $psu) = idle_holder('postgres');
$log_off = -s $node->logfile;
($rc, $out, $err) = as_user('sigr', "SELECT pg_horizon_terminate_blocker($psu);");
isnt($rc, 0, 'pg_signal_backend cannot terminate a superuser session');
like($err, qr/permission denied to terminate process/, '... core says so');
is(alive($psu), '1', '... and it survives');
unlike(slurp_file($node->logfile, $log_off), qr/pg_horizon terminated pid $psu/,
	'a denied attempt is not logged as a termination');

$log_off = -s $node->logfile;
($rc, $out, $err) = as_user('sigr', "SELECT pg_horizon_terminate_blocker($p1);");
is($rc, 0, 'pg_signal_backend may terminate another role\'s idle-in-transaction session');
like($out, qr/^t$/m, '... and gets true');
ok($node->poll_query_until('postgres', "SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $p1"),
	'the session is gone');
like(slurp_file($node->logfile, $log_off), qr/pg_horizon terminated pid $p1 /,
	'the successful termination is logged');
close_session($s1);
close_session($su);
$node->safe_psql('postgres', "SELECT pg_terminate_backend($psu)") if alive($psu);

# --- active sessions need force => true --------------------------------------
my $active = IPC::Run::start(
	[ 'psql', '-XAtq', '-v', 'ON_ERROR_STOP=1', '-d', $node->connstr('postgres'),
	  '-c', 'SELECT pg_current_xact_id(), pg_sleep(120);' ],
	'<', \my $ain, '>', \my $aout, '2>', \my $aerr);
my $apid;
ok($node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE state = 'active' AND query LIKE '%pg_sleep(120)%' AND pid <> pg_backend_pid()"),
	'an active xid-holding session exists');
$apid = $node->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE query LIKE '%pg_sleep(120)%' AND pid <> pg_backend_pid()");
($rc, $out, $err) = as_user('postgres', "SET pg_horizon.terminate_blockers = on; SELECT pg_horizon_terminate_blocker($apid);");
like($err, qr/not a session pg_horizon will terminate/, 'active session refused without force');
is(alive($apid), '1', '... and left running');
($rc, $out, $err) = as_user('postgres', "SET pg_horizon.terminate_blockers = on; SELECT pg_horizon_terminate_blocker($apid, true);");
like($out, qr/^t$/m, 'force => true terminates it');
ok($node->poll_query_until('postgres', "SELECT count(*) = 0 FROM pg_stat_activity WHERE pid = $apid"),
	'and it is gone');
$active->kill_kill;

# --- stale state must not authorize a kill -----------------------------------
# The victim is idle in transaction when first seen, then goes active in the
# same instant the actor is still inside one transaction. Activity data used to
# be cached per transaction, so the actor still "saw" idle in transaction and
# killed a running statement without force.
my $victim = $node->background_psql('postgres');
$victim->query_safe('BEGIN; SELECT pg_current_xact_id();');
my $vpid = $victim->query_safe('SELECT pg_backend_pid();');
chomp $vpid;
my $actor = $node->background_psql('postgres', on_error_stop => 0);
$actor->query_safe('SET pg_horizon.terminate_blockers = on; BEGIN;');
is($actor->query_safe("SELECT state FROM pg_horizon_blockers WHERE pid = $vpid;"),
	'idle in transaction', 'first read sees the victim idle in transaction');
$victim->query_until(qr/now_active/,
	"\\echo now_active\nCOMMIT;\nSELECT pg_current_xact_id(), pg_sleep(120);\n");
ok($node->poll_query_until('postgres',
	"SELECT state = 'active' FROM pg_stat_activity WHERE pid = $vpid"),
	'the victim is now running a statement');
my ($aout2, $aret) = $actor->query("SELECT pg_horizon_terminate_blocker($vpid);");
like($actor->{stderr} // '', qr/not a session pg_horizon will terminate/,
	'the actor is refused although its transaction first saw idle in transaction');
is(alive($vpid), '1', 'the running statement was not killed');
$actor->query('ROLLBACK;');
$node->safe_psql('postgres', "SELECT pg_cancel_backend($vpid)");
$actor->quit;
eval { $victim->quit; };

done_testing();
