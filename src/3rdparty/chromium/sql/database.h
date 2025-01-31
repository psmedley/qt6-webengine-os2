// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SQL_DATABASE_H_
#define SQL_DATABASE_H_

#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/callback.h"
#include "base/compiler_specific.h"
#include "base/component_export.h"
#include "base/containers/flat_map.h"
#include "base/feature_list.h"
#include "base/gtest_prod_util.h"
#include "base/memory/ref_counted.h"
#include "base/sequence_checker.h"
#include "base/strings/string_piece.h"
#include "base/threading/scoped_blocking_call.h"
#include "sql/internal_api_token.h"
#include "sql/sql_features.h"
#include "sql/statement_id.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

struct sqlite3;
struct sqlite3_stmt;

namespace base {
class FilePath;
namespace trace_event {
class ProcessMemoryDump;
}  // namespace trace_event
}  // namespace base

namespace sql {

class DatabaseMemoryDumpProvider;
class Statement;

namespace test {
class ScopedErrorExpecter;
}  // namespace test

struct COMPONENT_EXPORT(SQL) DatabaseOptions {
  // Default page size for newly created databases.
  //
  // Guaranteed to match SQLITE_DEFAULT_PAGE_SIZE.
  static constexpr int kDefaultPageSize = 4096;
  DatabaseOptions(bool el, int ps = kDefaultPageSize, int cs = 0, bool mastd = false, bool evd = false) noexcept :
      exclusive_locking(el), page_size(ps), cache_size(cs), mmap_alt_status_discouraged(mastd), enable_views_discouraged(evd) {}
  // If true, the database can only be opened by one process at a time.
  //
  // SQLite supports a locking protocol that allows multiple processes to safely
  // operate on the same database at the same time. The locking protocol is used
  // on every transaction, and comes with a small performance penalty.
  //
  // Setting this to true causes the locking protocol to be used once, when the
  // database is opened. No other process will be able to access the database at
  // the same time.
  //
  // More details at https://www.sqlite.org/pragma.html#pragma_locking_mode
  //
  // SQLite's locking protocol is summarized at
  // https://www.sqlite.org/c3ref/io_methods.html
  //
  // Exclusive mode is strongly recommended. It reduces the I/O cost of setting
  // up a transaction. It also removes the need of handling transaction failures
  // due to lock contention.
  bool exclusive_locking = true;

  // If true, enables SQLite's Write-Ahead Logging (WAL).
  //
  // WAL integration is under development, and should not be used in shipping
  // Chrome features yet. In particular, our custom database recovery code does
  // not support the WAL log file.
  //
  // WAL mode is currently not fully supported on FuchsiaOS. It will only be
  // turned on if the database is also using exclusive locking mode.
  // (https://crbug.com/1082059)
  //
  // Note: Changing page size is not supported when in WAL mode. So running
  // 'PRAGMA page_size = <new-size>' will result in no-ops.
  //
  // More details at https://www.sqlite.org/wal.html
  bool wal_mode =
      base::FeatureList::IsEnabled(sql::features::kEnableWALModeByDefault);

  // Database page size.
  //
  // New Chrome features should set an explicit page size in their
  // DatabaseOptions initializers, even if they use the default page size. This
  // makes it easier to track the page size used by the databases on the users'
  // devices.
  //
  // The value in this option is only applied to newly created databases. In
  // other words, changing the value doesn't impact the databases that have
  // already been created on the users' devices. So, changing the value in the
  // code without a lot of work (re-creating existing databases) will result in
  // inconsistent page sizes across the fleet of user devices, which will make
  // it (even) more difficult to reason about database performance.
  //
  // Larger page sizes result in shallower B-trees, because they allow an inner
  // page to hold more keys. On the flip side, larger page sizes may result in
  // more I/O when making small changes to existing records.
  //
  // Must be a power of two between 512 and 65536 inclusive.
  //
  // TODO(pwnall): Replace the default with an invalid value after all
  //               sql::Database users explicitly initialize page_size.
  int page_size = kDefaultPageSize;

  // The size of in-memory cache, in pages.
  //
  // New Chrome features should set an explicit cache size in their
  // DatabaseOptions initializers, even if they use the default cache size. This
  // makes it easier to track the cache size used by the databases on the users'
  // devices. The default page size of 4,096 bytes results in a cache size of
  // 500 pages.
  //
  // SQLite's database cache will take up at most (`page_size` * `cache_size`)
  // bytes of RAM.
  //
  // 0 invokes SQLite's default, which is currently to size up the cache to use
  // exactly 2,048,000 bytes of RAM.
  //
  // TODO(pwnall): Replace the default with an invalid value after all
  //               sql::Database users explicitly initialize page_size.
  int cache_size = 0;

  // Stores mmap failures in the SQL schema, instead of the meta table.
  //
  // This option is strongly discouraged for new databases, and will eventually
  // be removed.
  //
  // If this option is true, the mmap status is stored in the database schema.
  // Like any other schema change, changing the mmap status invalidates all
  // pre-compiled SQL statements.
  bool mmap_alt_status_discouraged = false;

  // If true, enables SQL views (a discouraged feature) for this database.
  //
  // The use of views is discouraged for Chrome code. See README.md for details
  // and recommended replacements.
  //
  // If this option is false, CREATE VIEW and DROP VIEW succeed, but SELECT
  // statements targeting views fail.
  bool enable_views_discouraged = false;

  // If true, enables virtual tables (a discouraged feature) for this database.
  //
  // The use of virtual tables is discouraged for Chrome code. See README.md for
  // details and recommended replacements.
  //
  // If this option is false, CREATE VIRTUAL TABLE and DROP VIRTUAL TABLE
  // succeed, but statements targeting virtual tables fail.
  bool enable_virtual_tables_discouraged = false;
};

// Handle to an open SQLite database.
//
// Instances of this class are thread-unsafe and DCHECK that they are accessed
// on the same sequence.
//
// When a Database instance goes out of scope, any uncommitted transactions are
// rolled back.
class COMPONENT_EXPORT(SQL) Database {
 private:
  class StatementRef;  // Forward declaration, see real one below.

 public:
  // Creates an instance that can receive Open() / OpenInMemory() calls.
  //
  // Some `options` members are only applied to newly created databases.
  //
  // Most operations on the new instance will fail until Open() / OpenInMemory()
  // is called.
  explicit Database(DatabaseOptions options);

  // This constructor is deprecated.
  //
  // When transitioning away from this default constructor, consider setting
  // DatabaseOptions::explicit_locking to true. For historical reasons, this
  // constructor results in DatabaseOptions::explicit_locking set to false.
  //
  // TODO(crbug.com/1126968): Remove this constructor after migrating all
  //                          uses to the explicit constructor below.
  Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  ~Database();

  // Allows mmapping to be disabled globally by default in the calling process.
  // Must be called before any threads attempt to create a Database.
  //
  // TODO(crbug.com/1117049): Remove this global configuration.
  static void DisableMmapByDefault();

  // Pre-init configuration ----------------------------------------------------

  // The page size that will be used when creating a new database.
  int page_size() const { return options_.page_size; }

  // Returns whether a database will be opened in WAL mode.
  bool UseWALMode() const;

  // Opt out of memory-mapped file I/O.
  void set_mmap_disabled() { mmap_disabled_ = true; }

  // Set an error-handling callback.  On errors, the error number (and
  // statement, if available) will be passed to the callback.
  //
  // If no callback is set, the default action is to crash in debug
  // mode or return failure in release mode.
  using ErrorCallback = base::RepeatingCallback<void(int, Statement*)>;
  void set_error_callback(const ErrorCallback& callback) {
    error_callback_ = callback;
  }
  bool has_error_callback() const { return !error_callback_.is_null(); }
  void reset_error_callback() { error_callback_.Reset(); }

  // Developer-friendly database ID used in logging output and memory dumps.
  void set_histogram_tag(const std::string& tag);

  // Run "PRAGMA integrity_check" and post each line of
  // results into |messages|.  Returns the success of running the
  // statement - per the SQLite documentation, if no errors are found the
  // call should succeed, and a single value "ok" should be in messages.
  bool FullIntegrityCheck(std::vector<std::string>* messages);

  // Runs "PRAGMA quick_check" and, unlike the FullIntegrityCheck method,
  // interprets the results returning true if the the statement executes
  // without error and results in a single "ok" value.
  bool QuickIntegrityCheck() WARN_UNUSED_RESULT;

  // Meant to be called from a client error callback so that it's able to
  // get diagnostic information about the database.
  std::string GetDiagnosticInfo(int extended_error, Statement* statement);

  // Reports memory usage into provided memory dump with the given name.
  bool ReportMemoryUsage(base::trace_event::ProcessMemoryDump* pmd,
                         const std::string& dump_name);

  // Initialization ------------------------------------------------------------

  // Initializes the SQL database for the given file, returning true if the
  // file could be opened. You can call this or OpenInMemory.
  bool Open(const base::FilePath& path) WARN_UNUSED_RESULT;

  // Initializes the SQL database for a temporary in-memory database. There
  // will be no associated file on disk, and the initial database will be
  // empty. You can call this or Open.
  bool OpenInMemory() WARN_UNUSED_RESULT;

  // Create a temporary on-disk database.  The database will be
  // deleted after close.  This kind of database is similar to
  // OpenInMemory() for small databases, but can page to disk if the
  // database becomes large.
  bool OpenTemporary() WARN_UNUSED_RESULT;

  // Returns true if the database has been successfully opened.
  bool is_open() const { return static_cast<bool>(db_); }

  // Closes the database. This is automatically performed on destruction for
  // you, but this allows you to close the database early. You must not call
  // any other functions after closing it. It is permissable to call Close on
  // an uninitialized or already-closed database.
  void Close();

  // Reads the first <cache-size>*<page-size> bytes of the file to prime the
  // filesystem cache.  This can be more efficient than faulting pages
  // individually.  Since this involves blocking I/O, it should only be used if
  // the caller will immediately read a substantial amount of data from the
  // database.
  //
  // TODO(shess): Design a set of histograms or an experiment to inform this
  // decision.  Preloading should almost always improve later performance
  // numbers for this database simply because it pulls operations forward, but
  // if the data isn't actually used soon then preloading just slows down
  // everything else.
  void Preload();

  // Release all non-essential memory associated with this database connection.
  void TrimMemory();

  // Raze the database to the ground.  This approximates creating a
  // fresh database from scratch, within the constraints of SQLite's
  // locking protocol (locks and open handles can make doing this with
  // filesystem operations problematic).  Returns true if the database
  // was razed.
  //
  // false is returned if the database is locked by some other
  // process.
  //
  // NOTE(shess): Raze() will DCHECK in the following situations:
  // - database is not open.
  // - the database has a transaction open.
  // - a SQLite issue occurs which is structural in nature (like the
  //   statements used are broken).
  // Since Raze() is expected to be called in unexpected situations,
  // these all return false, since it is unlikely that the caller
  // could fix them.
  //
  // The database's page size is taken from |options_.page_size|.  The
  // existing database's |auto_vacuum| setting is lost (the
  // possibility of corruption makes it unreliable to pull it from the
  // existing database).  To re-enable on the empty database requires
  // running "PRAGMA auto_vacuum = 1;" then "VACUUM".
  //
  // NOTE(shess): For Android, SQLITE_DEFAULT_AUTOVACUUM is set to 1,
  // so Raze() sets auto_vacuum to 1.
  //
  // TODO(shess): Raze() needs a database so cannot clear SQLITE_NOTADB.
  // TODO(shess): Bake auto_vacuum into Database's API so it can
  // just pick up the default.
  bool Raze();

  // Breaks all outstanding transactions (as initiated by
  // BeginTransaction()), closes the SQLite database, and poisons the
  // object so that all future operations against the Database (or
  // its Statements) fail safely, without side effects.
  //
  // This is intended as an alternative to Close() in error callbacks.
  // Close() should still be called at some point.
  void Poison();

  // Raze() the database and Poison() the handle.  Returns the return
  // value from Raze().
  // TODO(shess): Rename to RazeAndPoison().
  bool RazeAndClose();

  // Delete the underlying database files associated with |path|. This should be
  // used on a database which is not opened by any Database instance. Open
  // Database instances pointing to the database can cause odd results or
  // corruption (for instance if a hot journal is deleted but the associated
  // database is not).
  //
  // Returns true if the database file and associated journals no
  // longer exist, false otherwise.  If the database has never
  // existed, this will return true.
  static bool Delete(const base::FilePath& path);

  // Transactions --------------------------------------------------------------

  // Transaction management. We maintain a virtual transaction stack to emulate
  // nested transactions since sqlite can't do nested transactions. The
  // limitation is you can't roll back a sub transaction: if any transaction
  // fails, all transactions open will also be rolled back. Any nested
  // transactions after one has rolled back will return fail for Begin(). If
  // Begin() fails, you must not call Commit or Rollback().
  //
  // Normally you should use sql::Transaction to manage a transaction, which
  // will scope it to a C++ context.
  bool BeginTransaction();
  void RollbackTransaction();
  bool CommitTransaction();

  // Rollback all outstanding transactions.  Use with care, there may
  // be scoped transactions on the stack.
  void RollbackAllTransactions();

  // Returns the current transaction nesting, which will be 0 if there are
  // no open transactions.
  int transaction_nesting() const { return transaction_nesting_; }

  // Attached databases---------------------------------------------------------

  // Attaches an existing database to this connection.
  //
  // `attachment_point` must only contain lowercase letters.
  //
  // Attachment APIs are only exposed for use in recovery. General use is
  // discouraged in Chrome. The README has more details.
  //
  // On the SQLite version shipped with Chrome (3.21+, Oct 2017), databases can
  // be attached while a transaction is opened. However, these databases cannot
  // be detached until the transaction is committed or aborted.
  bool AttachDatabase(const base::FilePath& other_db_path,
                      base::StringPiece attachment_point,
                      InternalApiToken);

  // Detaches a database that was previously attached with AttachDatabase().
  //
  // `attachment_point` must match the argument of a previously successsful
  // AttachDatabase() call.
  //
  // Attachment APIs are only exposed for use in recovery. General use is
  // discouraged in Chrome. The README has more details.
  bool DetachDatabase(base::StringPiece attachment_point, InternalApiToken);

  // Statements ----------------------------------------------------------------

  // Executes a SQL statement. Returns true for success, and false for failure.
  //
  // `sql` should be a single SQL statement. Production code should not execute
  // multiple SQL statements at once, to facilitate crash debugging. Test code
  // should use ExecuteScriptForTesting().
  //
  // `sql` cannot have parameters. Statements with parameters can be handled by
  // sql::Statement. See GetCachedStatement() and GetUniqueStatement().
  bool Execute(const char* sql) WARN_UNUSED_RESULT;

  // Executes a sequence of SQL statements.
  //
  // Returns true if all statements execute successfully. If a statement fails,
  // stops and returns false. Calls should be wrapped in ASSERT_TRUE().
  //
  // The database's error handler is not invoked when errors occur. This method
  // is a convenience for setting up a complex on-disk database state, such as
  // an old schema version with test contents.
  bool ExecuteScriptForTesting(const char* sql_script) WARN_UNUSED_RESULT;

  // Returns a statement for the given SQL using the statement cache. It can
  // take a nontrivial amount of work to parse and compile a statement, so
  // keeping commonly-used ones around for future use is important for
  // performance.
  //
  // The SQL_FROM_HERE macro is the recommended way of generating a StatementID.
  // Code that generates custom IDs must ensure that a StatementID is never used
  // for different SQL statements. Failing to meet this requirement results in
  // incorrect behavior, and should be caught by a DCHECK.
  //
  // The SQL statement passed in |sql| must match the SQL statement reported
  // back by SQLite. Mismatches are caught by a DCHECK, so any code that has
  // automated test coverage or that was manually tested on a DCHECK build will
  // not exhibit this problem. Mismatches generally imply that the statement
  // passed in has extra whitespace or comments surrounding it, which waste
  // storage and CPU cycles.
  //
  // If the |sql| has an error, an invalid, inert StatementRef is returned (and
  // the code will crash in debug). The caller must deal with this eventuality,
  // either by checking validity of the |sql| before calling, by correctly
  // handling the return of an inert statement, or both.
  //
  // Example:
  //   sql::Statement stmt(database_.GetCachedStatement(
  //       SQL_FROM_HERE, "SELECT * FROM foo"));
  //   if (!stmt)
  //     return false;  // Error creating statement.
  scoped_refptr<StatementRef> GetCachedStatement(StatementID id,
                                                 const char* sql);

  // Used to check a |sql| statement for syntactic validity. If the statement is
  // valid SQL, returns true.
  bool IsSQLValid(const char* sql);

  // Returns a non-cached statement for the given SQL. Use this for SQL that
  // is only executed once or only rarely (there is overhead associated with
  // keeping a statement cached).
  //
  // See GetCachedStatement above for examples and error information.
  scoped_refptr<StatementRef> GetUniqueStatement(const char* sql);

  // Performs a passive checkpoint on the main attached database if it is in
  // WAL mode. Returns true if the checkpoint was successful and false in case
  // of an error. It is a no-op if the database is not in WAL mode.
  //
  // Note: Checkpointing is a very slow operation and will block any writes
  // until it is finished. Please use with care.
  bool CheckpointDatabase();

  // Info querying -------------------------------------------------------------

  // Returns true if the given structure exists.  Instead of test-then-create,
  // callers should almost always prefer the "IF NOT EXISTS" version of the
  // CREATE statement.
  bool DoesIndexExist(base::StringPiece index_name);
  bool DoesTableExist(base::StringPiece table_name);
  bool DoesViewExist(base::StringPiece table_name);

  // Returns true if a column with the given name exists in the given table.
  //
  // Calling this method on a VIEW returns an unspecified result.
  //
  // This should only be used by migration code for legacy features that do not
  // use MetaTable, and need an alternative way of figuring out the database's
  // current version.
  bool DoesColumnExist(const char* table_name, const char* column_name);

  // Returns sqlite's internal ID for the last inserted row. Valid only
  // immediately after an insert.
  int64_t GetLastInsertRowId() const;

  // Returns sqlite's count of the number of rows modified by the last
  // statement executed. Will be 0 if no statement has executed or the database
  // is closed.
  int GetLastChangeCount() const;

  // Approximates the amount of memory used by SQLite for this database.
  //
  // This measures the memory used for the page cache (most likely the biggest
  // consumer), database schema, and prepared statements.
  //
  // The memory used by the page cache can be recovered by calling TrimMemory(),
  // which will cause SQLite to drop the page cache.
  int GetMemoryUsage();

  // Errors --------------------------------------------------------------------

  // Returns the error code associated with the last sqlite operation.
  int GetErrorCode() const;

  // Returns the errno associated with GetErrorCode().  See
  // SQLITE_LAST_ERRNO in SQLite documentation.
  int GetLastErrno() const;

  // Returns a pointer to a statically allocated string associated with the
  // last sqlite operation.
  const char* GetErrorMessage() const;

  // Return a reproducible representation of the schema equivalent to
  // running the following statement at a sqlite3 command-line:
  //   SELECT type, name, tbl_name, sql FROM sqlite_master ORDER BY 1, 2, 3, 4;
  std::string GetSchema();

  // Returns |true| if there is an error expecter (see SetErrorExpecter), and
  // that expecter returns |true| when passed |error|.  Clients which provide an
  // |error_callback| should use IsExpectedSqliteError() to check for unexpected
  // errors; if one is detected, DLOG(DCHECK) is generally appropriate (see
  // OnSqliteError implementation).
  static bool IsExpectedSqliteError(int error);

  // Computes the path of a database's rollback journal.
  //
  // The journal file is created at the beginning of the database's first
  // transaction. The file may be removed and re-created between transactions,
  // depending on whether the database is opened in exclusive mode, and on
  // configuration options. The journal file does not exist when the database
  // operates in WAL mode.
  //
  // This is intended for internal use and tests. To preserve our ability to
  // iterate on our SQLite configuration, features must avoid relying on
  // the existence of specific files.
  static base::FilePath JournalPath(const base::FilePath& db_path);

  // Computes the path of a database's write-ahead log (WAL).
  //
  // The WAL file exists while a database is opened in WAL mode.
  //
  // This is intended for internal use and tests. To preserve our ability to
  // iterate on our SQLite configuration, features must avoid relying on
  // the existence of specific files.
  static base::FilePath WriteAheadLogPath(const base::FilePath& db_path);

  // Computes the path of a database's shared memory (SHM) file.
  //
  // The SHM file is used to coordinate between multiple processes using the
  // same database in WAL mode. Thus, this file only exists for databases using
  // WAL and not opened in exclusive mode.
  //
  // This is intended for internal use and tests. To preserve our ability to
  // iterate on our SQLite configuration, features must avoid relying on
  // the existence of specific files.
  static base::FilePath SharedMemoryFilePath(const base::FilePath& db_path);

  // Internal state accessed by other classes in //sql.
  sqlite3* db(InternalApiToken) const { return db_; }
  bool poisoned(InternalApiToken) const { return poisoned_; }

 private:
  // Allow test-support code to set/reset error expecter.
  friend class test::ScopedErrorExpecter;

  // Statement accesses StatementRef which we don't want to expose to everybody
  // (they should go through Statement).
  friend class Statement;

  FRIEND_TEST_ALL_PREFIXES(SQLDatabaseTest, CachedStatement);
  FRIEND_TEST_ALL_PREFIXES(SQLDatabaseTest, CollectDiagnosticInfo);
  FRIEND_TEST_ALL_PREFIXES(SQLDatabaseTest, GetAppropriateMmapSize);
  FRIEND_TEST_ALL_PREFIXES(SQLDatabaseTest, GetAppropriateMmapSizeAltStatus);
  FRIEND_TEST_ALL_PREFIXES(SQLDatabaseTest, OnMemoryDump);
  FRIEND_TEST_ALL_PREFIXES(SQLDatabaseTest, RegisterIntentToUpload);
  FRIEND_TEST_ALL_PREFIXES(SQLiteFeaturesTest, WALNoClose);

  // Internal initialize function used by both Init and InitInMemory. The file
  // name is always 8 bits since we want to use the 8-bit version of
  // sqlite3_open. The string can also be sqlite's special ":memory:" string.
  //
  // |retry_flag| controls retrying the open if the error callback
  // addressed errors using RazeAndClose().
  enum Retry { NO_RETRY = 0, RETRY_ON_POISON };
  bool OpenInternal(const std::string& file_name, Retry retry_flag);

  // Internal close function used by Close() and RazeAndClose().
  // |forced| indicates that orderly-shutdown checks should not apply.
  void CloseInternal(bool forced);

  // Construct a ScopedBlockingCall to annotate IO calls, but only if
  // database wasn't open in memory. ScopedBlockingCall uses |from_here| to
  // declare its blocking execution scope (see https://www.crbug/934302).
  void InitScopedBlockingCall(
      const base::Location& from_here,
      absl::optional<base::ScopedBlockingCall>* scoped_blocking_call) const {
    if (!in_memory_)
      scoped_blocking_call->emplace(from_here, base::BlockingType::MAY_BLOCK);
  }

  // Internal helper for Does*Exist() functions.
  bool DoesSchemaItemExist(base::StringPiece name, base::StringPiece type);

  // Accessors for global error-expecter, for injecting behavior during tests.
  // See test/scoped_error_expecter.h.
  using ErrorExpecterCallback = base::RepeatingCallback<bool(int)>;
  static ErrorExpecterCallback* current_expecter_cb_;
  static void SetErrorExpecter(ErrorExpecterCallback* expecter);
  static void ResetErrorExpecter();

  // A StatementRef is a refcounted wrapper around a sqlite statement pointer.
  // Refcounting allows us to give these statements out to sql::Statement
  // objects while also optionally maintaining a cache of compiled statements
  // by just keeping a refptr to these objects.
  //
  // A statement ref can be valid, in which case it can be used, or invalid to
  // indicate that the statement hasn't been created yet, has an error, or has
  // been destroyed.
  //
  // The Database may revoke a StatementRef in some error cases, so callers
  // should always check validity before using.
  class COMPONENT_EXPORT(SQL) StatementRef
      : public base::RefCounted<StatementRef> {
   public:
    REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

    // |database| is the sql::Database instance associated with
    // the statement, and is used for tracking outstanding statements
    // and for error handling.  Set to nullptr for invalid refs.
    // |stmt| is the actual statement, and should only be null
    // to create an invalid ref.  |was_valid| indicates whether the
    // statement should be considered valid for diagnostic purposes.
    // |was_valid| can be true for a null |stmt| if the Database has
    // been forcibly closed by an error handler.
    StatementRef(Database* database, sqlite3_stmt* stmt, bool was_valid);

    StatementRef(const StatementRef&) = delete;
    StatementRef& operator=(const StatementRef&) = delete;

    // When true, the statement can be used.
    bool is_valid() const { return !!stmt_; }

    // When true, the statement is either currently valid, or was
    // previously valid but the database was forcibly closed.  Used
    // for diagnostic checks.
    bool was_valid() const { return was_valid_; }

    // If we've not been linked to a database, this will be null.
    Database* database() const { return database_; }

    // Returns the sqlite statement if any. If the statement is not active,
    // this will return nullptr.
    sqlite3_stmt* stmt() const { return stmt_; }

    // Destroys the compiled statement and sets it to nullptr. The statement
    // will no longer be active. |forced| is used to indicate if
    // orderly-shutdown checks should apply (see Database::RazeAndClose()).
    void Close(bool forced);

    // Construct a ScopedBlockingCall to annotate IO calls, but only if
    // database wasn't open in memory. ScopedBlockingCall uses |from_here| to
    // declare its blocking execution scope (see https://www.crbug/934302).
    void InitScopedBlockingCall(
        const base::Location& from_here,
        absl::optional<base::ScopedBlockingCall>* scoped_blocking_call) const {
      if (database_)
        database_->InitScopedBlockingCall(from_here, scoped_blocking_call);
    }

   private:
    friend class base::RefCounted<StatementRef>;

    ~StatementRef();

    Database* database_;
    sqlite3_stmt* stmt_;
    bool was_valid_;
  };
  friend class StatementRef;

  // Executes a rollback statement, ignoring all transaction state. Used
  // internally in the transaction management code.
  void DoRollback();

  // Called by a StatementRef when it's being created or destroyed. See
  // open_statements_ below.
  void StatementRefCreated(StatementRef* ref);
  void StatementRefDeleted(StatementRef* ref);

  // Called when a sqlite function returns an error, which is passed
  // as |err|.  The return value is the error code to be reflected
  // back to client code.  |stmt| is non-null if the error relates to
  // an sql::Statement instance.  |sql| is non-nullptr if the error
  // relates to non-statement sql code (Execute, for instance).  Both
  // can be null, but both should never be set.
  // NOTE(shess): Originally, the return value was intended to allow
  // error handlers to transparently convert errors into success.
  // Unfortunately, transactions are not generally restartable, so
  // this did not work out.
  int OnSqliteError(int err, Statement* stmt, const char* sql) const;

  // Like Execute(), but returns the error code given by SQLite.
  //
  // This is only exposed to the Database implementation. Code that uses
  // sql::Database should not be concerned with SQLite error codes.
  int ExecuteAndReturnErrorCode(const char* sql) WARN_UNUSED_RESULT;

  // Like |Execute()|, but retries if the database is locked.
  bool ExecuteWithTimeout(const char* sql,
                          base::TimeDelta ms_timeout) WARN_UNUSED_RESULT;

  // Implementation helper for GetUniqueStatement() and GetCachedStatement().
  scoped_refptr<StatementRef> GetStatementImpl(const char* sql);

  bool IntegrityCheckHelper(const char* pragma_sql,
                            std::vector<std::string>* messages)
      WARN_UNUSED_RESULT;

  // Release page-cache memory if memory-mapped I/O is enabled and the database
  // was changed.  Passing true for |implicit_change_performed| allows
  // overriding the change detection for cases like DDL (CREATE, DROP, etc),
  // which do not participate in the total-rows-changed tracking.
  void ReleaseCacheMemoryIfNeeded(bool implicit_change_performed);

  // Returns the results of sqlite3_db_filename(), which should match the path
  // passed to Open().
  base::FilePath DbPath() const;

  // Helper to collect diagnostic info for a corrupt database.
  std::string CollectCorruptionInfo();

  // Helper to collect diagnostic info for errors.
  std::string CollectErrorInfo(int error, Statement* stmt) const;

  // Calculates a value appropriate to pass to "PRAGMA mmap_size = ".  So errors
  // can make it unsafe to map a file, so the file is read using regular I/O,
  // with any errors causing 0 (don't map anything) to be returned.  If the
  // entire file is read without error, a large value is returned which will
  // allow the entire file to be mapped in most cases.
  //
  // Results are recorded in the database's meta table for future reference, so
  // the file should only be read through once.
  size_t GetAppropriateMmapSize();

  // Helpers for GetAppropriateMmapSize().
  bool GetMmapAltStatus(int64_t* status);
  bool SetMmapAltStatus(int64_t status);

  // sqlite3_prepare_v3() flags for this database.
  int SqlitePrepareFlags() const;

  // The actual sqlite database. Will be null before Init has been called or if
  // Init resulted in an error.
  sqlite3* db_ = nullptr;

  // TODO(shuagga@microsoft.com): Make `options_` const after removing all
  // setters.
  DatabaseOptions options_;

  // Holds references to all cached statements so they remain active.
  //
  // flat_map is appropriate here because the codebase has ~400 cached
  // statements, and each statement is at most one insertion in the map
  // throughout a process' lifetime.
  base::flat_map<StatementID, scoped_refptr<StatementRef>> statement_cache_;

  // A list of all StatementRefs we've given out. Each ref must register with
  // us when it's created or destroyed. This allows us to potentially close
  // any open statements when we encounter an error.
  std::set<StatementRef*> open_statements_;

  // Number of currently-nested transactions.
  int transaction_nesting_ = 0;

  // True if any of the currently nested transactions have been rolled back.
  // When we get to the outermost transaction, this will determine if we do
  // a rollback instead of a commit.
  bool needs_rollback_ = false;

  // True if database is open with OpenInMemory(), False if database is open
  // with Open().
  bool in_memory_ = false;

  // |true| if the Database was closed using RazeAndClose().  Used
  // to enable diagnostics to distinguish calls to never-opened
  // databases (incorrect use of the API) from calls to once-valid
  // databases.
  bool poisoned_ = false;

  // |true| if SQLite memory-mapped I/O is not desired for this database.
  bool mmap_disabled_;

  // |true| if SQLite memory-mapped I/O was enabled for this database.
  // Used by ReleaseCacheMemoryIfNeeded().
  bool mmap_enabled_ = false;

  // Used by ReleaseCacheMemoryIfNeeded() to track if new changes have happened
  // since memory was last released.
  int total_changes_at_last_release_ = 0;

  ErrorCallback error_callback_;

  // Developer-friendly database ID used in logging output and memory dumps.
  std::string histogram_tag_;

  // Stores the dump provider object when db is open.
  std::unique_ptr<DatabaseMemoryDumpProvider> memory_dump_provider_;
};

}  // namespace sql

#endif  // SQL_DATABASE_H_
