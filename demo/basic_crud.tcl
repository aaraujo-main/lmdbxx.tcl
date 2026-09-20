package require lmdbxx 1.0.0

set tmpBase [expr {[info exists ::env(TMPDIR)] ? $::env(TMPDIR) : "/tmp"}]
set dbDir [file join $tmpBase "lmdbxx_demo_basic_crud"]
file delete -force $dbDir
file mkdir $dbDir

set myEnv [lmdbxx::open_env_shared $dbDir 0 0644]

# Optional: resize map before any transaction, to avoid MDB_MAP_FULL on the
# default (small) LMDB map size.
lmdbxx::env_set_mapsize $myEnv 1073741824

set myDb [lmdbxx::open_db_shared $myEnv]

lmdbxx::eval_write myDb {
    lmdbxx::put "key1" "value1"
    puts "get after put: [lmdbxx::get "key1"]"
    puts "exists after put: [lmdbxx::exists "key1"]"
    lmdbxx::del "key1"
    puts "exists after del (still inside write txn): [lmdbxx::exists "key1"]"
}

lmdbxx::eval_read myDb {
    puts "exists after commit (separate read txn): [lmdbxx::exists "key1"]"
}

unset myDb myEnv
file delete -force $dbDir
puts "basic_crud.tcl: OK"
