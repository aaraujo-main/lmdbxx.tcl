package require lmdbxx 1.0.0

set tmpBase [expr {[info exists ::env(TMPDIR)] ? $::env(TMPDIR) : "/tmp"}]
set dbDir [file join $tmpBase "lmdbxx_demo_mapsize"]
file delete -force $dbDir
file mkdir $dbDir

# Default (small) mapsize: writing a very large value fails with MDB_MAP_FULL.
set myEnv [lmdbxx::open_env_shared $dbDir 0 0644]
set myDb [lmdbxx::open_db_shared $myEnv]

set bigValue [string repeat "x" 20000000]
set caught [catch {
    lmdbxx::eval_write myDb {
        lmdbxx::put "big" $bigValue
    }
} errMsg]
puts "large put without mapsize increase caught: $caught ($errMsg)"

# Release the handles so the environment fully closes, then reopen fresh.
unset myDb myEnv
file delete -force $dbDir
file mkdir $dbDir

set myEnv2 [lmdbxx::open_env_shared $dbDir 0 0644]
lmdbxx::env_set_mapsize $myEnv2 1073741824
set myDb2 [lmdbxx::open_db_shared $myEnv2]

lmdbxx::eval_write myDb2 {
    lmdbxx::put "big" $bigValue
}
lmdbxx::eval_read myDb2 {
    puts "exists after mapsize increase: [lmdbxx::exists "big"]"
}

unset myDb2 myEnv2
file delete -force $dbDir
puts "mapsize.tcl: OK"
