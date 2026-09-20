#!/usr/bin/env tclsh

# Compare Tcl dict, lmdbxx, and Tcl sqlite3 for insert, lookup, and scan.

proc usage {} {
    puts stderr {usage: benchmark.tcl ?-records count? ?-value-size bytes? ?-runs count?}
    exit 2
}

set records 10000
set valueSize 64
set runs 3
for {set i 0} {$i < $::argc} {incr i} {
    set option [lindex $::argv $i]
    if {$option in {-records -value-size -runs}} {
        incr i
        if {$i >= $::argc || ![string is integer -strict [lindex $::argv $i]]} {
            usage
        }
        set value [lindex $::argv $i]
        if {$value < 1} { usage }
        switch -- $option {
            -records { set records $value }
            -value-size { set valueSize $value }
            -runs { set runs $value }
        }
    } else {
        usage
    }
}

if {[catch {package require lmdbxx 1.0.0} error]} {
    puts stderr "lmdbxx unavailable: $error"
    exit 1
}

set tmpBase [expr {[info exists ::env(TMPDIR)] ? $::env(TMPDIR) : "/tmp"}]
set root [file join $tmpBase "lmdbxx_benchmark_[pid]"]
file delete -force $root
file mkdir $root

set value [string repeat x $valueSize]
set keys {}
for {set i 0} {$i < $records} {incr i} {
    lappend keys [format "key%08d" $i]
}

proc elapsed {script} {
    set started [clock microseconds]
    set result [uplevel 1 $script]
    return [list [expr {[clock microseconds] - $started}] $result]
}

proc rate {count micros} {
    if {$micros == 0} { return Inf }
    return [format %.0f [expr {$count * 1000000.0 / $micros}]]
}

proc benchmark_dict {keys value runs} {
    set insertTimes {}
    set lookupTimes {}
    set scanTimes {}
    set batchPutTimes {}
    set batchGetTimes {}
    set batchDelTimes {}
    set pairs {}
    foreach key $keys { lappend pairs $key $value }
    for {set run 0} {$run < $runs} {incr run} {
        set table [dict create]
        lassign [elapsed {
            foreach key $keys { dict set table $key $value }
        }] micros ignored
        lappend insertTimes $micros

        lassign [elapsed {
            set checksum 0
            foreach key $keys { incr checksum [string length [dict get $table $key]] }
            set checksum
        }] micros checksum
        lappend lookupTimes $micros

        lassign [elapsed {
            set count 0
            dict for {key data} $table { incr count [string length $data] }
            set count
        }] micros scanned
        if {$scanned != [expr {[llength $keys] * [string length $value]}]} {
            error "dict scan checksum mismatch"
        }
        lappend scanTimes $micros

        lassign [elapsed {
            set batchTable [dict create]
            dict for {key data} $pairs { dict set batchTable $key $data }
        }] micros ignored
        lappend batchPutTimes $micros
        lassign [elapsed {
            set checksum 0
            dict for {key ignored} $pairs { incr checksum [string length [dict get $batchTable $key]] }
            set checksum
        }] micros checksum
        lappend batchGetTimes $micros
        lassign [elapsed {
            dict for {key ignored} $pairs { dict unset batchTable $key }
        }] micros ignored
        lappend batchDelTimes $micros
    }
    return [list $insertTimes $lookupTimes $scanTimes $batchPutTimes $batchGetTimes $batchDelTimes]
}

proc benchmark_lmdb {keys value valueSize runs root label flags} {
    set path [file join $root "lmdb_$label"]
    file mkdir $path
    set envHandle [lmdbxx::open_env_shared $path $flags 0644]
    lmdbxx::env_set_mapsize $envHandle [expr {max(67108864, [llength $keys] * ($valueSize + 128) * 4)}]
    set dbHandle [lmdbxx::open_db_shared $envHandle]
    set ::lmdbxxBenchmarkDb $dbHandle
    set insertTimes {}
    set lookupTimes {}
    set scanTimes {}
    set batchPutTimes {}
    set batchGetTimes {}
    set batchDelTimes {}
    set pairs {}
    foreach key $keys { lappend pairs $key $value }
    for {set run 0} {$run < $runs} {incr run} {
        lmdbxx::eval_write lmdbxxBenchmarkDb {
            foreach key $keys { lmdbxx::put $key $value }
        }
        lassign [elapsed {
            lmdbxx::eval_write lmdbxxBenchmarkDb {
                foreach key $keys { lmdbxx::put $key $value }
            }
        }] micros ignored
        lappend insertTimes $micros

        lassign [elapsed {
            set checksum 0
            lmdbxx::eval_read lmdbxxBenchmarkDb {
                foreach key $keys { incr checksum [string length [lmdbxx::get $key]] }
            }
            set checksum
        }] micros checksum
        lappend lookupTimes $micros

        lassign [elapsed {
            set scanned 0
            lmdbxx::eval_read lmdbxxBenchmarkDb {
                lmdbxx::foreach {key data} { incr scanned [string length $data] }
            }
            set scanned
        }] micros scanned
        if {$scanned != [expr {[llength $keys] * [string length $value]}]} {
            error "LMDB scan checksum mismatch"
        }
        lappend scanTimes $micros

        lassign [elapsed {
            lmdbxx::eval_write lmdbxxBenchmarkDb { lmdbxx::mput $pairs }
        }] micros ignored
        lappend batchPutTimes $micros
        lassign [elapsed {
            set batchResult {}
            lmdbxx::eval_read lmdbxxBenchmarkDb { set batchResult [lmdbxx::mget $keys] }
            set checksum 0
            foreach {key data} $batchResult { incr checksum [string length $data] }
            set checksum
        }] micros checksum
        lappend batchGetTimes $micros
        lassign [elapsed {
            lmdbxx::eval_write lmdbxxBenchmarkDb { lmdbxx::mdel $keys }
        }] micros ignored
        lappend batchDelTimes $micros

        lmdbxx::eval_write lmdbxxBenchmarkDb { lmdbxx::mput $pairs }
    }
    unset ::lmdbxxBenchmarkDb dbHandle envHandle
    return [list $insertTimes $lookupTimes $scanTimes $batchPutTimes $batchGetTimes $batchDelTimes]
}

proc benchmark_sqlite {keys value runs root label journalMode synchronous} {
    if {[catch {package require sqlite3} error]} {
        return [list unavailable $error]
    }
    set database [file join $root "sqlite_$label.db"]
    sqlite3 db $database
    db eval "PRAGMA journal_mode = $journalMode; PRAGMA synchronous = $synchronous; CREATE TABLE entries (key TEXT PRIMARY KEY, value TEXT)"
    set insertTimes {}
    set lookupTimes {}
    set scanTimes {}
    set batchPutTimes {}
    set batchGetTimes {}
    set batchDelTimes {}
    for {set run 0} {$run < $runs} {incr run} {
        lassign [elapsed {
            db eval {BEGIN}
            foreach key $keys { db eval {INSERT OR REPLACE INTO entries VALUES ($key, $value)} }
            db eval {COMMIT}
        }] micros ignored
        lappend insertTimes $micros

        lassign [elapsed {
            set checksum 0
            foreach key $keys {
                db eval {SELECT length(value) AS size FROM entries WHERE key = $key} row {
                    incr checksum $row(size)
                }
            }
            set checksum
        }] micros checksum
        lappend lookupTimes $micros

        lassign [elapsed {
            set scanned 0
            db eval {SELECT value FROM entries} row { incr scanned [string length $row(value)] }
            set scanned
        }] micros scanned
        if {$scanned != [expr {[llength $keys] * [string length $value]}]} {
            error "SQLite scan checksum mismatch"
        }
        lappend scanTimes $micros

        lassign [elapsed {
            db eval {BEGIN}
            foreach key $keys { db eval {INSERT OR REPLACE INTO entries VALUES ($key, $value)} }
            db eval {COMMIT}
        }] micros ignored
        lappend batchPutTimes $micros
        lassign [elapsed {
            set checksum 0
            foreach key $keys {
                db eval {SELECT length(value) AS size FROM entries WHERE key = $key} row {
                    incr checksum $row(size)
                }
            }
            set checksum
        }] micros checksum
        lappend batchGetTimes $micros
        lassign [elapsed {
            db eval {BEGIN}
            foreach key $keys { db eval {DELETE FROM entries WHERE key = $key} }
            db eval {COMMIT}
        }] micros ignored
        lappend batchDelTimes $micros

        db eval {BEGIN}
        foreach key $keys { db eval {INSERT OR REPLACE INTO entries VALUES ($key, $value)} }
        db eval {COMMIT}
    }
    db close
    return [list $insertTimes $lookupTimes $scanTimes $batchPutTimes $batchGetTimes $batchDelTimes]
}

proc median values {
    set values [lsort -integer $values]
    return [lindex $values [expr {[llength $values] / 2}]]
}

proc print_result {name result records} {
    if {[lindex $result 0] eq "unavailable"} {
        puts [format "%-14s | %-12s | %-12s | %-12s" $name unavailable unavailable unavailable]
        return
    }
    foreach {insert lookup scan} $result break
    puts [format "%-14s | %12d | %12d | %12d" \
        $name [rate $records [median $insert]] [rate $records [median $lookup]] [rate $records [median $scan]]]
}

proc print_batch_result {name result records} {
    if {[lindex $result 0] eq "unavailable"} {
        puts [format "%-14s | %-12s | %-12s | %-12s" $name unavailable unavailable unavailable]
        return
    }
    foreach {insert lookup scan batchPut batchGet batchDel} $result break
    puts [format "%-14s | %12d | %12d | %12d" \
        $name [rate $records [median $batchPut]] [rate $records [median $batchGet]] [rate $records [median $batchDel]]]
}

puts "lmdbxx.tcl benchmark"
puts "records=$records value_bytes=$valueSize runs=$runs"
puts ""
puts "profiles"
puts "  dict        Tcl hash table; no durability setting"
puts "  lmdb-safe   LMDB flags=0; commit sync enabled"
puts "  lmdb-fast   LMDB flags=MDB_NOSYNC (65536); commit fsync disabled"
puts "  sqlite-safe SQLite journal=DELETE, synchronous=FULL"
puts "  sqlite-fast SQLite journal=MEMORY, synchronous=OFF"
puts ""
puts [format "%-14s | %12s | %12s | %12s" implementation {insert ops/s} {lookup ops/s} {scan ops/s}]
set dictResult [benchmark_dict $keys $value $runs]
set lmdbSafeResult [benchmark_lmdb $keys $value $valueSize $runs $root safe 0]
set lmdbFastResult [benchmark_lmdb $keys $value $valueSize $runs $root fast $lmdbxx::MDB_NOSYNC]
set sqliteSafeResult [benchmark_sqlite $keys $value $runs $root safe DELETE FULL]
set sqliteFastResult [benchmark_sqlite $keys $value $runs $root fast MEMORY OFF]
print_result dict $dictResult $records
print_result lmdb-safe $lmdbSafeResult $records
print_result lmdb-fast $lmdbFastResult $records
print_result sqlite-safe $sqliteSafeResult $records
print_result sqlite-fast $sqliteFastResult $records
puts ""
puts "batch workload (mput/mget/mdel equivalents)"
puts [format "%-14s | %12s | %12s | %12s" implementation {mput ops/s} {mget ops/s} {mdel ops/s}]
print_batch_result dict $dictResult $records
print_batch_result lmdb-safe $lmdbSafeResult $records
print_batch_result lmdb-fast $lmdbFastResult $records
print_batch_result sqlite-safe $sqliteSafeResult $records
print_batch_result sqlite-fast $sqliteFastResult $records
puts ""
puts "safe profiles favor crash durability; fast profiles favor throughput and can lose recent writes on crash."
puts ""
puts "Results are workload-specific. Run on same machine with same options for comparison."
file delete -force $root
