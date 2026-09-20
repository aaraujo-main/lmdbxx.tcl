package require lmdbxx 1.0.0

set tmpBase [expr {[info exists ::env(TMPDIR)] ? $::env(TMPDIR) : "/tmp"}]
set dbDir [file join $tmpBase "lmdbxx_demo_transactions"]
file delete -force $dbDir
file mkdir $dbDir

set myEnv [lmdbxx::open_env_shared $dbDir 0 0644]
lmdbxx::env_set_mapsize $myEnv 1073741824
set myDb [lmdbxx::open_db_shared $myEnv]

# A Tcl error raised inside eval_write aborts the transaction; the write
# never becomes visible.
set caught [catch {
    lmdbxx::eval_write myDb {
        lmdbxx::put "willRollback" "value"
        error "forced failure"
    }
} errMsg]
puts "rollback body error caught: $caught ($errMsg)"

lmdbxx::eval_read myDb {
    puts "exists after rollback: [lmdbxx::exists "willRollback"]"
}

# A body that runs to completion commits normally.
lmdbxx::eval_write myDb {
    lmdbxx::put "committed" "value"
}
lmdbxx::eval_read myDb {
    puts "exists after commit: [lmdbxx::exists "committed"]"
}

# Write commands are rejected inside eval_read.
set caught [catch {
    lmdbxx::eval_read myDb {
        lmdbxx::put "shouldFail" "value"
    }
} errMsg]
puts "put inside eval_read caught: $caught ($errMsg)"

unset myDb myEnv
file delete -force $dbDir
puts "transactions.tcl: OK"
