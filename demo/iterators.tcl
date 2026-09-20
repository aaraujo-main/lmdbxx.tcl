package require lmdbxx 1.0.0

set tmpBase [expr {[info exists ::env(TMPDIR)] ? $::env(TMPDIR) : "/tmp"}]
set dbDir [file join $tmpBase "lmdbxx_demo_iterators"]
file delete -force $dbDir
file mkdir $dbDir

set myEnv [lmdbxx::open_env_shared $dbDir 0 0644]
lmdbxx::env_set_mapsize $myEnv 1073741824
set myDb [lmdbxx::open_db_shared $myEnv]

lmdbxx::eval_write myDb {
    foreach k {key1 key2 key3 key4 key5} {
        lmdbxx::put $k "data-$k"
    }
}

lmdbxx::eval_read myDb {
    puts "foreach (ascending):"
    lmdbxx::foreach {key data} {
        puts "  $key -> $data"
    }

    puts "foreach_reverse (descending):"
    lmdbxx::foreach_reverse {key data} {
        puts "  $key -> $data"
    }

    puts "forrange key2..key4 (ascending, inclusive):"
    lmdbxx::forrange {key data} "key2" "key4" {
        puts "  $key -> $data"
    }

    puts "forrange_reverse key2..key4 (descending, inclusive):"
    lmdbxx::forrange_reverse {key data} "key2" "key4" {
        puts "  $key -> $data"
    }

    puts "forrange with empty start (unbounded), end=key3:"
    lmdbxx::forrange {key data} "" "key3" {
        puts "  $key -> $data"
    }
}

unset myDb myEnv
file delete -force $dbDir
puts "iterators.tcl: OK"
