pg_horizon_sources = files(
  'pg_horizon.c',
  'horizon_collect.c',
  'horizon_funcs.c',
  'horizon_action.c',
)

if host_system == 'windows'
  pg_horizon_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_horizon',
    '--FILEDESC', 'pg_horizon - XID/MultiXact freeze horizon diagnosis',
  ])
endif

pg_horizon = shared_module('pg_horizon',
  pg_horizon_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_horizon

install_data(
  'pg_horizon.control',
  'pg_horizon--1.0.sql',
  'pg_horizon--1.0--1.1.sql',
  'pg_horizon--1.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_horizon',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      '001_basic',
      '002_explain',
      '003_permissions',
      '004_upgrade',
      '005_edge',
    ],
  },
  'tap': {
    'tests': [
      't/001_blockers.pl',
      't/002_horizons.pl',
      't/003_multixact.pl',
      't/004_terminate.pl',
      't/005_encoding.pl',
      't/006_replication.pl',
      't/007_visibility.pl',
    ],
  },
}
