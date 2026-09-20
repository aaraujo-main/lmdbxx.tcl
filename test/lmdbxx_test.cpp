#include "lmdbxx.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

class LmdbxxTclTest : public ::testing::Test {
public:
    void SetUp() override {
        interp_ = Tcl_CreateInterp();
        ASSERT_NE(interp_, nullptr);
        Tcl_Init(interp_); // best effort; our commands are core, don't need the script library
        ASSERT_EQ(TCL_OK, Lmdbxx_Init(interp_)) << Tcl_GetStringResult(interp_);

        static std::atomic<int> counter{0};
        const auto* testInfo = ::testing::UnitTest::GetInstance()->current_test_info();
        std::ostringstream oss;
        oss << ::testing::TempDir() << "lmdbxx_test_" << testInfo->test_suite_name() << "_"
            << testInfo->name() << "_" << ::getpid() << "_" << counter.fetch_add(1);
        tempDir_ = oss.str();
        std::filesystem::remove_all(tempDir_);
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        if (interp_) {
            Tcl_DeleteInterp(interp_);
            interp_ = nullptr;
        }
        std::filesystem::remove_all(tempDir_);
    }

    std::string DbPath(const std::string& name = "db") const {
        return tempDir_ + "/" + name;
    }

    void EvalOk(const std::string& script) {
        ASSERT_EQ(TCL_OK, Tcl_EvalEx(interp_, script.c_str(), -1, 0))
            << "script: " << script << "\nerror: " << Tcl_GetStringResult(interp_);
    }

    void EvalError(const std::string& script, const std::string& expectedSubstr) {
        ASSERT_EQ(TCL_ERROR, Tcl_EvalEx(interp_, script.c_str(), -1, 0))
            << "script unexpectedly succeeded: " << script;
        std::string msg = Tcl_GetStringResult(interp_);
        ASSERT_NE(msg.find(expectedSubstr), std::string::npos)
            << "expected substring '" << expectedSubstr << "' in: " << msg;
    }

    std::string EvalResult(const std::string& script) {
        EvalOk(script);
        return std::string(Tcl_GetStringResult(interp_));
    }

    // Length-aware variant for values that may contain embedded NUL bytes.
    std::string EvalResultBytes(const std::string& script) {
        EvalOk(script);
        int len = 0;
        unsigned char* s = Tcl_GetByteArrayFromObj(Tcl_GetObjResult(interp_), &len);
        return std::string(reinterpret_cast<const char*>(s), static_cast<std::size_t>(len));
    }

    Tcl_Interp* interp_ = nullptr;
    std::string tempDir_;
};

} // namespace

// ---- Init / package ---------------------------------------------------

TEST_F(LmdbxxTclTest, PackageRequireSucceeds) {
    EXPECT_EQ(EvalResult("package require lmdbxx 1.0.0"), "1.0.0");
}

TEST_F(LmdbxxTclTest, ExposesLmdbConstantsInNamespace) {
    EXPECT_EQ(EvalResult("set lmdbxx::MDB_NOSYNC"), "65536");
    EXPECT_EQ(EvalResult("expr {$lmdbxx::MDB_NOSYNC | $lmdbxx::MDB_NOMETASYNC}"), "327680");
    EXPECT_EQ(EvalResult("set lmdbxx::MDB_NOOVERWRITE"), "16");
    EXPECT_EQ(EvalResult("set missing {}\n"
                         "foreach name {MDB_NOSUBDIR MDB_RDONLY MDB_CREATE MDB_APPEND} {\n"
                         "    if {![info exists ::lmdbxx::$name]} { set missing $name }\n"
                         "}\n"
                         "set missing"),
              "");
}

// ---- Environment --------------------------------------------------------

TEST_F(LmdbxxTclTest, OpenEnvSharedCreatesFile) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_TRUE(std::filesystem::exists(path + "/data.mdb"));
}

TEST_F(LmdbxxTclTest, OpenEnvSharedReusesHandleForSamePath) {
    std::string path = DbPath();
    EvalOk("set env1 [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set env2 [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set db1 [lmdbxx::open_db_shared $env1]");
    EvalOk("set db2 [lmdbxx::open_db_shared $env2]");
    EvalOk("lmdbxx::eval_write db1 { lmdbxx::put \"k\" \"via-env1\" }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read db2 { lmdbxx::get \"k\" }"), "via-env1");
}

TEST_F(LmdbxxTclTest, OpenEnvSharedRelativePathResolvesAgainstCwd) {
    std::string oldCwd = std::filesystem::current_path().string();
    std::filesystem::current_path(tempDir_);
    EvalOk("set envRel [lmdbxx::open_env_shared {reldb} 0 0644]");
    std::string absPath = tempDir_ + "/reldb";
    EvalOk("set envAbs [lmdbxx::open_env_shared {" + absPath + "} 0 0644]");
    EvalOk("set dbRel [lmdbxx::open_db_shared $envRel]");
    EvalOk("set dbAbs [lmdbxx::open_db_shared $envAbs]");
    EvalOk("lmdbxx::eval_write dbRel { lmdbxx::put \"k\" \"via-rel\" }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read dbAbs { lmdbxx::get \"k\" }"), "via-rel");
    std::filesystem::current_path(oldCwd);
}

TEST_F(LmdbxxTclTest, OpenEnvSharedConflictingFlagsErrors) {
    std::string path = DbPath();
    EvalOk("set env1 [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalError("lmdbxx::open_env_shared {" + path + "} 0 0600", "already open");
}

TEST_F(LmdbxxTclTest, OpenEnvSharedSupportsNoSubdirFileEnvironment) {
    std::string path = DbPath("single-file.mdb");
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} $lmdbxx::MDB_NOSUBDIR 0644]");
    EXPECT_TRUE(std::filesystem::is_regular_file(path));
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put key value }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::get key }"), "value");
}

TEST_F(LmdbxxTclTest, NumericArgumentsRejectNegativeAndOutOfRangeValues) {
    std::string path = DbPath();
    EvalError("lmdbxx::open_env_shared {" + path + "/negative-flags} -1 0644", "value out of range");
    EvalError("lmdbxx::open_env_shared {" + path + "/negative-mode} 0 -1", "value out of range");
    EvalError("lmdbxx::open_env_shared {" + path + "/large-flags} 4294967296 0644", "value out of range");
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "/mapsize} 0 0644]");
    EvalError("lmdbxx::env_set_mapsize $myEnv -1", "must not be negative");
}

// ---- Mapsize --------------------------------------------------------------

TEST_F(LmdbxxTclTest, EnvSetMapsizeBeforeAnyTxnSucceeds) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("lmdbxx::env_set_mapsize $myEnv 1073741824");
}

TEST_F(LmdbxxTclTest, EnvSetMapsizeAfterOpenDbSharedErrors) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::env_set_mapsize $myEnv 1073741824", "transaction");
}

TEST_F(LmdbxxTclTest, EnvSetMapsizeAfterEvalReadErrors) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_read myDb { lmdbxx::exists \"k\" }");
    EvalError("lmdbxx::env_set_mapsize $myEnv 1073741824", "transaction");
}

TEST_F(LmdbxxTclTest, LargeValueFailsWithoutMapsizeIncrease) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("set bigValue [string repeat \"x\" 20000000]");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::put \"big\" $bigValue }", "mdb_put");
}

TEST_F(LmdbxxTclTest, LargeValueSucceedsAfterMapsizeIncrease) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("lmdbxx::env_set_mapsize $myEnv 1073741824");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("set bigValue [string repeat \"x\" 20000000]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put \"big\" $bigValue }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists \"big\" }"), "1");
}

// ---- Database ---------------------------------------------------------

TEST_F(LmdbxxTclTest, OpenDbSharedReusesCachedDbi) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set db1 [lmdbxx::open_db_shared $myEnv]");
    EvalOk("set db2 [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write db1 { lmdbxx::put \"k\" \"v\" }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read db2 { lmdbxx::get \"k\" }"), "v");
}

TEST_F(LmdbxxTclTest, OpenDbSharedInsideActiveTxnErrors) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::open_db_shared $myEnv }", "transaction");
}

// ---- CRUD ---------------------------------------------------------------

TEST_F(LmdbxxTclTest, PutGetRoundTripBinarySafe) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    // Note: LMDB itself rejects zero-length keys (MDB_BAD_VALSIZE), so the
    // zero-length-value case below uses a non-empty key; embedded NUL bytes
    // are exercised in both key and value via the "a\x00b" case.
    EvalOk("lmdbxx::eval_write myDb {\n"
           "    lmdbxx::put \"a\\x00b\" \"v\\x00w\"\n"
           "    lmdbxx::put \"emptyval\" \"\"\n"
           "}");
    EXPECT_EQ(EvalResultBytes("lmdbxx::eval_read myDb { lmdbxx::get \"a\\x00b\" }"), std::string("v\0w", 3));
    EXPECT_EQ(EvalResultBytes("lmdbxx::eval_read myDb { lmdbxx::get \"emptyval\" }"), std::string(""));
}

TEST_F(LmdbxxTclTest, EmptyKeyFailsAndRollsBackWriteTransaction) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::put valid value; lmdbxx::put {} bad }", "mdb_put");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists valid }"), "0");
}

TEST_F(LmdbxxTclTest, PutOverwritesExistingKey) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put \"k\" \"v1\" }");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put \"k\" \"v2\" }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::get \"k\" }"), "v2");
}

TEST_F(LmdbxxTclTest, GetMissingKeyErrors) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_read myDb { lmdbxx::get \"missing\" }", "not found");
}

TEST_F(LmdbxxTclTest, ExistsReturnsZeroOrOne) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put \"k\" \"v\" }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists \"k\" }"), "1");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists \"missing\" }"), "0");
}

TEST_F(LmdbxxTclTest, DelMissingKeyErrors) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::del \"missing\" }", "not found");
}

TEST_F(LmdbxxTclTest, DelAndMdelRemoveExistingKeys) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::mput {key1 value1 key2 value2 key3 value3} }");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::del key1; lmdbxx::mdel {key2 key3} }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    list [lmdbxx::exists key1] [lmdbxx::exists key2] [lmdbxx::exists key3]\n"
                         "}"),
              "0 0 0");
}

TEST_F(LmdbxxTclTest, PutWithFlagsForwardsToMdbPut) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put \"k\" \"v1\" }");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::put \"k\" \"v2\" 16 }", "exists");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::get \"k\" }"), "v1");
}

TEST_F(LmdbxxTclTest, MputMgetLgetRoundTrip) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::mput {key1 value1 key2 value2 key3 value3} }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::mget {key1 key2 key3} }"),
              "key1 value1 key2 value2 key3 value3");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::lget {key3 key1 key2} }"),
              "value3 value1 value2");
}

TEST_F(LmdbxxTclTest, MgetOmitsMissingKeysAndLgetPreservesPlaceholders) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::mput {key1 value1 key3 value3} }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::mget {key3 missing key1} }"),
              "key3 value3 key1 value1");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { llength [lmdbxx::lget {key3 missing key1}] }"), "3");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { binary encode hex [lindex [lmdbxx::lget {missing}] 0] }"), "");
}

TEST_F(LmdbxxTclTest, MputRejectsOddListBeforeWriting) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::mput {key value dangling} }", "even number");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists key }"), "0");
}

TEST_F(LmdbxxTclTest, MputFlagsAndDuplicateKeysFollowPutSemantics) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::mput {key old key new} }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::get key }"), "new");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::mput {key rejected} 16 }", "exists");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::get key }"), "new");
}

TEST_F(LmdbxxTclTest, CaughtMputFailureKeepsEarlierWritesInTransaction) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put existing old }");
    EvalOk("lmdbxx::eval_write myDb {\n"
           "    catch { lmdbxx::mput {new value existing rejected} $lmdbxx::MDB_NOOVERWRITE }\n"
           "}");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::get new }"), "value");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::get existing }"), "old");
}

TEST_F(LmdbxxTclTest, MdelErrorsAndUncaughtFailureRollsBack) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::mput {key1 value1 key2 value2} }");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::mdel {key1 missing key2} }", "index 1");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists key1 }"), "1");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists key2 }"), "1");
}

TEST_F(LmdbxxTclTest, MdelCaughtFailureCommitsPriorDeletes) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::mput {key1 value1 key2 value2} }");
    EvalOk("lmdbxx::eval_write myDb { catch {lmdbxx::mdel {key1 missing key2}} }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists key1 }"), "0");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists key2 }"), "1");
}

TEST_F(LmdbxxTclTest, BatchCommandsRequireCorrectTransactionContext) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::mput {key value}", "write operation requires");
    EvalError("lmdbxx::mget {key}", "no active transaction");
    EvalError("lmdbxx::lget {key}", "no active transaction");
    EvalError("lmdbxx::mdel {key}", "write operation requires");
    EvalError("lmdbxx::eval_read myDb { lmdbxx::mdel {key} }", "write operation requires");
    EvalError("lmdbxx::eval_read myDb { lmdbxx::mput {key value} }", "write operation requires");
}

TEST_F(LmdbxxTclTest, BatchCommandsAcceptEmptyListsAndBinaryValues) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EXPECT_EQ(EvalResult("lmdbxx::eval_write myDb { llength [lmdbxx::mput {}] }"), "0");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { llength [lmdbxx::mget {}] }"), "0");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { llength [lmdbxx::lget {}] }"), "0");
    EXPECT_EQ(EvalResult("lmdbxx::eval_write myDb { lmdbxx::mput [list \"a\\x00b\" \"v\\x00w\"] }"), "");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { binary encode hex [dict get [lmdbxx::mget [list \"a\\x00b\"]] \"a\\x00b\"] }"), "760077");
}

// ---- Transactions ---------------------------------------------------------

TEST_F(LmdbxxTclTest, EvalWriteCommitsOnSuccess) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put \"k\" \"v\" }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::get \"k\" }"), "v");
}

TEST_F(LmdbxxTclTest, EvalReadDoesNotPersistWrites) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_read myDb { lmdbxx::put \"k\" \"v\" }", "write");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists \"k\" }"), "0");
}

TEST_F(LmdbxxTclTest, EvalWriteCanReadItsOwnWrites) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EXPECT_EQ(EvalResult("lmdbxx::eval_write myDb {\n"
                         "    lmdbxx::put key value\n"
                         "    list [lmdbxx::get key] [lmdbxx::exists key]\n"
                         "}"),
              "value 1");
}

TEST_F(LmdbxxTclTest, EvalWriteRollsBackOnTclError) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::put \"k\" \"v\"; error boom }", "boom");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists \"k\" }"), "0");
}

TEST_F(LmdbxxTclTest, EvalWriteRollsBackOnReturn) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("proc doWrite {db} { lmdbxx::eval_write $db { lmdbxx::put \"k\" \"v\"; return 42 } }");
    EXPECT_EQ(EvalResult("doWrite myDb"), "42");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists \"k\" }"), "0");
}

TEST_F(LmdbxxTclTest, EvalWriteRollsBackOnBreakContinueAtTopLevel) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::put \"k\" \"v\"; break }", "");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists \"k\" }"), "0");
}

TEST_F(LmdbxxTclTest, EvalWriteRollsBackOnContinueAtTopLevel) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::put k v; continue }", "");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::exists k }"), "0");
}

TEST_F(LmdbxxTclTest, NestedEvalWriteErrors) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::eval_write myDb { lmdbxx::put \"k\" \"v\" } }",
              "already active");
    EvalError("lmdbxx::eval_read myDb { lmdbxx::eval_write myDb { lmdbxx::put \"k\" \"v\" } }",
              "already active");
}

TEST_F(LmdbxxTclTest, ReadCommandOutsideTxnErrors) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::get \"k\"", "no active transaction");
    EvalError("lmdbxx::exists \"k\"", "no active transaction");
    EvalError("lmdbxx::foreach {k v} {}", "no active transaction");
}

TEST_F(LmdbxxTclTest, WriteCommandInsideEvalReadErrors) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_read myDb { lmdbxx::put \"k\" \"v\" }", "write operation requires");
    EvalError("lmdbxx::eval_read myDb { lmdbxx::del \"k\" }", "write operation requires");
}

TEST_F(LmdbxxTclTest, FailedWriteLeavesTransactionUsable) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_write myDb { lmdbxx::put key value; error boom }", "boom");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put key recovered }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::get key }"), "recovered");
}

// ---- Iterators --------------------------------------------------------

namespace {
void SeedKeys(LmdbxxTclTest& t, const std::string& envVar, const std::string& dbVar) {
    t.EvalOk("lmdbxx::eval_write " + dbVar + " {\n"
             "    foreach k {key1 key2 key3 key4 key5} {\n"
             "        lmdbxx::put $k \"data-$k\"\n"
             "    }\n"
             "}");
}
} // namespace

TEST_F(LmdbxxTclTest, ForeachVisitsAllPairsAscending) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    SeedKeys(*this, "myEnv", "myDb");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::foreach {k v} { lappend out $k }\n"
                         "    set out\n"
                         "}"),
              "key1 key2 key3 key4 key5");
}

TEST_F(LmdbxxTclTest, ForeachReverseVisitsAllPairsDescending) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    SeedKeys(*this, "myEnv", "myDb");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::foreach_reverse {k v} { lappend out $k }\n"
                         "    set out\n"
                         "}"),
              "key5 key4 key3 key2 key1");
}

TEST_F(LmdbxxTclTest, ForrangeInclusiveBoundsBothSet) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    SeedKeys(*this, "myEnv", "myDb");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::forrange {k v} \"key2\" \"key4\" { lappend out $k }\n"
                         "    set out\n"
                         "}"),
              "key2 key3 key4");
}

TEST_F(LmdbxxTclTest, ForrangeEmptyStartIsUnbounded) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    SeedKeys(*this, "myEnv", "myDb");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::forrange {k v} \"\" \"key2\" { lappend out $k }\n"
                         "    set out\n"
                         "}"),
              "key1 key2");
}

TEST_F(LmdbxxTclTest, ForrangeEmptyEndIsUnbounded) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    SeedKeys(*this, "myEnv", "myDb");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::forrange {k v} \"key4\" \"\" { lappend out $k }\n"
                         "    set out\n"
                         "}"),
              "key4 key5");
}

TEST_F(LmdbxxTclTest, ForrangeReverseInclusiveBounds) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    SeedKeys(*this, "myEnv", "myDb");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::forrange_reverse {k v} \"key2\" \"key4\" { lappend out $k }\n"
                         "    set out\n"
                         "}"),
              "key4 key3 key2");
}

TEST_F(LmdbxxTclTest, ForrangeReverseEmptyBoundsAndNoMatch) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    SeedKeys(*this, "myEnv", "myDb");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::forrange_reverse {k v} \"\" \"key3\" { lappend out $k }\n"
                         "    set out\n"
                         "}"),
              "key3 key2 key1");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::forrange_reverse {k v} \"key3\" \"\" { lappend out $k }\n"
                         "    set out\n"
                         "}"),
              "key5 key4 key3");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::forrange_reverse {k v} \"key5\" \"key1\" { lappend out $k }\n"
                         "    set out\n"
                         "}"),
              "");
}

TEST_F(LmdbxxTclTest, RangesHandleNonExactBounds) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    SeedKeys(*this, "myEnv", "myDb");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::forrange {k v} key2x key4x { lappend out $k }\n"
                         "    set out\n"
                         "}"),
              "key3 key4");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::forrange_reverse {k v} key2x key4x { lappend out $k }\n"
                         "    set out\n"
                         "}"),
              "key4 key3");
}

TEST_F(LmdbxxTclTest, EmptyDatabaseIteratorsDoNotInvokeBody) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "/empty} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set count 0\n"
                         "    lmdbxx::foreach {k v} { incr count }\n"
                         "    lmdbxx::foreach_reverse {k v} { incr count }\n"
                         "    lmdbxx::forrange {k v} a z { incr count }\n"
                         "    lmdbxx::forrange_reverse {k v} a z { incr count }\n"
                         "    set count\n"
                         "}"),
              "0");
}

TEST_F(LmdbxxTclTest, IteratorPreservesBinaryKeysAndValues) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put [binary format H* 610062] [binary format H* 760077] }");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::foreach {k v} { lappend out [binary encode hex $k] [binary encode hex $v] }\n"
                         "    set out\n"
                         "}"),
              "610062 760077");
}

TEST_F(LmdbxxTclTest, IteratorBodyBreakStopsEarly) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    SeedKeys(*this, "myEnv", "myDb");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::foreach {k v} {\n"
                         "        if {$k eq \"key3\"} break\n"
                         "        lappend out $k\n"
                         "    }\n"
                         "    set out\n"
                         "}"),
              "key1 key2");
}

TEST_F(LmdbxxTclTest, IteratorBodyContinueSkipsIteration) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    SeedKeys(*this, "myEnv", "myDb");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb {\n"
                         "    set out {}\n"
                         "    lmdbxx::foreach {k v} {\n"
                         "        if {$k eq \"key3\"} continue\n"
                         "        lappend out $k\n"
                         "    }\n"
                         "    set out\n"
                         "}"),
              "key1 key2 key4 key5");
}

TEST_F(LmdbxxTclTest, IteratorBodyErrorPropagates) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    SeedKeys(*this, "myEnv", "myDb");
    EvalError("lmdbxx::eval_read myDb {\n"
              "    lmdbxx::foreach {k v} {\n"
              "        if {$k eq \"key3\"} { error boom }\n"
              "    }\n"
              "}",
              "boom");
}

TEST_F(LmdbxxTclTest, IteratorVarSpecMustBeTwoDistinctNames) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalError("lmdbxx::eval_read myDb { lmdbxx::foreach {k} {} }", "two-element");
    EvalError("lmdbxx::eval_read myDb { lmdbxx::foreach {k k} {} }", "distinct");
    EvalError("lmdbxx::eval_read myDb { lmdbxx::foreach {{} v} {} }", "empty");
}

// ---- Lifecycle --------------------------------------------------------

TEST_F(LmdbxxTclTest, HandlesCloseAutomaticallyOnUnset) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put \"k\" \"v\" }");
    EvalOk("unset myDb");
    EvalOk("unset myEnv");
    // Reopening the same path after the handles are released must succeed
    // (previous environment closed automatically) and see the earlier write.
    EvalOk("set myEnv2 [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb2 [lmdbxx::open_db_shared $myEnv2]");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb2 { lmdbxx::get \"k\" }"), "v");
}

TEST_F(LmdbxxTclTest, DatabaseHandleKeepsEnvironmentAlive) {
    std::string path = DbPath();
    EvalOk("set myEnv [lmdbxx::open_env_shared {" + path + "} 0 0644]");
    EvalOk("set myDb [lmdbxx::open_db_shared $myEnv]");
    EvalOk("lmdbxx::eval_write myDb { lmdbxx::put k value }");
    EvalOk("unset myEnv");
    EXPECT_EQ(EvalResult("lmdbxx::eval_read myDb { lmdbxx::get k }"), "value");
    EvalOk("unset myDb");
}
