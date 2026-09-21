# Text the extension hands back must stay valid in the database encoding even
# when it has to be shortened: the fixed buffers used to be cut with strlcpy(),
# which can land in the middle of a multibyte character.
use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use IPC::Run ();

my $node = PostgreSQL::Test::Cluster->new('encoding');
$node->init(extra => [ '-E', 'UTF8', '--locale=C.UTF-8' ]);
$node->append_conf('postgresql.conf', q{
autovacuum = off
track_activity_query_size = 4096
max_prepared_transactions = 4
});
$node->start;

is($node->safe_psql('postgres', 'SHOW server_encoding;'), 'UTF8', 'database is UTF8');
$node->safe_psql('postgres', 'CREATE EXTENSION pg_horizon;');

my $e = "\xc3\xa9";    # "e acute", two bytes

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

# Byte 1023 of the statement falls inside a character: 'SELECT ' + quote is 8
# bytes, and 1023 - 8 is odd.
my $s = open_session($node, (getpwuid($<))[0],
	"SET application_name = 'hz_query';\nBEGIN;\nSELECT pg_current_xact_id();\nSELECT '" . ($e x 1500) . "';\n");
ok($node->poll_query_until('postgres',
	"SELECT count(*) = 1 FROM pg_stat_activity WHERE application_name = 'hz_query' AND state = 'idle in transaction'"),
	'the long-query session is idle in transaction');
my $pid = $node->safe_psql('postgres',
	"SELECT pid FROM pg_stat_activity WHERE application_name = 'hz_query'");

my $len = $node->safe_psql('postgres',
	"SELECT octet_length(query) FROM pg_horizon_blockers WHERE pid = $pid");
cmp_ok($len, '<=', 1023, 'query is clipped to the buffer');
my ($rc, $out, $err) = ('', '', '');
$node->psql('postgres',
	"SELECT length(query), query = convert_from(convert_to(query, 'UTF8'), 'UTF8') FROM pg_horizon_blockers WHERE pid = $pid",
	stdout => \$out, stderr => \$err);
is($err, '', 'length(query) does not fail with an encoding error');
like($out, qr/^\d+\|t$/, 'the clipped query is valid UTF8');
is($node->safe_psql('postgres',
		"SELECT count(*) FROM pg_horizon_blockers WHERE pid = $pid AND query LIKE 'SELECT ''%'"),
	'1', 'and still starts with the statement text');
close_session($s);

# A prepared transaction whose gid is as long and as multibyte as allowed.
my $gid = "g" . ($e x 99);    # 199 bytes: the maximum accepted
$node->safe_psql('postgres', qq{
BEGIN; SELECT pg_current_xact_id(); PREPARE TRANSACTION '$gid';
});
($rc, $out, $err) = (0, '', '');
$node->psql('postgres',
	q{SELECT length(prepared_gid), length(recommended_sql), length(reason) FROM pg_horizon_blockers WHERE blocker_type = 'prepared'},
	stdout => \$out, stderr => \$err);
is($err, '', 'a maximal multibyte gid produces valid text everywhere');
like($out, qr/^100\|\d+\|\d+$/, 'gid is intact (100 characters)');
is($node->safe_psql('postgres',
		q{SELECT recommended_sql LIKE '%ROLLBACK PREPARED%' AND recommended_sql LIKE '%COMMIT PREPARED%' FROM pg_horizon_blockers WHERE blocker_type = 'prepared'}),
	't', 'the recommendation shows both outcomes, commented out');
is($node->safe_psql('postgres',
		q{SELECT bool_and(l ~ '^(--|SELECT)') FROM (SELECT unnest(string_to_array(recommended_sql, E'\n')) AS l FROM pg_horizon_blockers WHERE blocker_type = 'prepared') x}),
	't', 'no line of the recommendation is a bare destructive statement');
$node->safe_psql('postgres', qq{ROLLBACK PREPARED '$gid';});

# Multibyte identifiers survive into the VACUUM statement.
my $ident = 'tab' . $e . $e . 'le';
$node->safe_psql('postgres', qq{CREATE TABLE "$ident" (id int);});
is($node->safe_psql('postgres', qq{SELECT pg_horizon_vacuum_sql('"$ident"')}),
	qq{VACUUM (FREEZE, VERBOSE) public."$ident";}, 'multibyte table name is quoted correctly');

# application_name is clipped by the server; make sure a multibyte one is valid.
my $app = $node->background_psql('postgres');
$app->query_safe("SET client_min_messages = warning; SET application_name = '" . ($e x 40) . "'; BEGIN; SELECT pg_current_xact_id();");
my $apid = $app->query_safe('SELECT pg_backend_pid();');
chomp $apid;
($out, $err) = ('', '');
$node->psql('postgres',
	"SELECT length(application_name) FROM pg_horizon_blockers WHERE pid = $apid",
	stdout => \$out, stderr => \$err);
is($err, '', 'multibyte application_name is valid');
$app->query_safe('ROLLBACK;');
$app->quit;

# --- hostile prepared-transaction names ---------------------------------------
# The advice keeps destructive statements behind "--", and a comment ends at the
# first line break. A GID can contain any character, so every literal in the
# advice must stay on one physical line, or the text after a newline would run
# when an operator pastes the advice.
$node->safe_psql('postgres', 'CREATE TABLE inj_probe(a int);');
# Real characters (not escapes) in the GIDs, sent through dollar quoting so the
# test setup itself cannot be broken by them.
my @evil = (
	"nl\nCREATE TABLE pwned_nl(a int); --",
	"cr\rCREATE TABLE pwned_cr(a int); --",
	"crlf\r\nCREATE TABLE pwned_crlf(a int); --",
	"mix\\';CREATE TABLE pwned_quote(a int); --\x0b\x01",
);
for my $g (@evil)
{
	$node->safe_psql('postgres', "BEGIN; SELECT pg_current_xact_id(); PREPARE TRANSACTION \$g\$$g\$g\$;");
}
is($node->safe_psql('postgres', "SELECT count(*) FROM pg_horizon_blockers WHERE blocker_type = 'prepared'"),
	scalar(@evil), 'the hostile prepared transactions are listed');

my $advice = $node->safe_psql('postgres',
	"SELECT string_agg(recommended_sql, E'\n') FROM pg_horizon_blockers WHERE blocker_type = 'prepared'");
my @bad = grep { $_ ne '' && $_ !~ /^(--|SELECT )/ } split /\r?\n|\r/, $advice;
is(scalar(@bad), 0, 'every physical line of the advice is a comment or a read-only SELECT');
is($node->safe_psql('postgres',
		q{SELECT count(*) FROM pg_horizon_blockers WHERE blocker_type = 'prepared' AND (recommended_sql ~ E'\r' OR reason ~ E'[\n\r]')}),
	'0', 'no carriage return in the advice and no line break in any reason');

# Run the advice the way an operator would (paste it into psql).
my ($aout, $aerr) = ('', '');
$node->psql('postgres', $advice, stdout => \$aout, stderr => \$aerr);
is($node->safe_psql('postgres', q{SELECT count(*) FROM pg_class WHERE relname LIKE 'pwned%'}),
	'0', 'executing the advice ran nothing embedded in a GID');

# The escaped literal still finds exactly its own transaction.
is($node->safe_psql('postgres', q{
SELECT count(*) FROM pg_horizon_blockers b
WHERE blocker_type = 'prepared'
  AND EXISTS (SELECT 1 FROM pg_prepared_xacts p WHERE p.gid = b.prepared_gid)}),
	scalar(@evil), 'gids are reported exactly, only the advice text is escaped');
$node->safe_psql('postgres', "ROLLBACK PREPARED \$g\$$_\$g\$;") for @evil;

done_testing();
