// Copyright (c) 2017 Rockset

#ifndef ROCKSDB_LITE

#ifdef USE_AWS

#include "rocksdb/cloud/db_cloud.h"
#include <algorithm>
#include <chrono>
#include "cloud/aws/aws_env.h"
#include "cloud/aws/aws_file.h"
#include "cloud/db_cloud_impl.h"
#include "cloud/filename.h"
#include "cloud/manifest_reader.h"
#include "rocksdb/options.h"
#include "rocksdb/status.h"
#include "util/logging.h"
#include "util/string_util.h"
#include "util/testharness.h"
#ifndef OS_WIN
#include <unistd.h>
#endif

namespace rocksdb {

class CloudTest : public testing::Test {
 public:
  CloudTest() {
    base_env_ = Env::Default();
    dbname_ = test::TmpDir() + "/db_cloud";
    clone_dir_ = test::TmpDir() + "/ctest";
    src_bucket_prefix_ = "dbcloud." + AwsEnv::GetTestBucketSuffix();
    src_object_prefix_ = dbname_;
    // Set dest = src by default
    dest_bucket_prefix_ = src_bucket_prefix_;
    dest_object_prefix_ = src_object_prefix_;
    options_.create_if_missing = true;
    persistent_cache_path_ = "";
    persistent_cache_size_gb_ = 0;

    DestroyDir(dbname_);
    CreateLoggerFromOptions(dbname_, options_, &options_.info_log);

    // Get cloud credentials
    AwsEnv::GetTestCredentials(&cloud_env_options_.credentials.access_key_id,
                               &cloud_env_options_.credentials.secret_key,
                               &region_);
    Cleanup();
  }

  void Cleanup() {
    ASSERT_TRUE(!aenv_);

    CloudEnv* aenv;
    // create a dummy aws env
    ASSERT_OK(CloudEnv::NewAwsEnv(
        base_env_, src_bucket_prefix_, src_object_prefix_, region_,
        dest_bucket_prefix_, dest_object_prefix_, region_, cloud_env_options_,
        options_.info_log, &aenv));
    aenv_.reset(aenv);
    // delete all pre-existing contents from the bucket
    Status st = aenv_->EmptyBucket(src_bucket_prefix_);
    ASSERT_TRUE(st.ok() || st.IsNotFound());
    aenv_.reset();

    // delete and create directory where clones reside
    DestroyDir(clone_dir_);
    ASSERT_OK(base_env_->CreateDir(clone_dir_));
  }

  std::set<std::string> GetSSTFiles() {
    std::vector<std::string> files;
    aenv_->GetBaseEnv()->GetChildren(dbname_, &files);
    std::set<std::string> sst_files;
    for (auto& f : files) {
      if (IsSstFile(RemoveEpoch(f))) {
        sst_files.insert(f);
      }
    }
    return sst_files;
  }

  void DestroyDir(const std::string& dir) {
    std::string cmd = "rm -rf " + dir;
    int rc = system(cmd.c_str());
    ASSERT_EQ(rc, 0);
  }

  virtual ~CloudTest() {
    CloseDB();
  }

  void CreateAwsEnv() {
    CloudEnv* aenv;
    ASSERT_OK(CloudEnv::NewAwsEnv(
        base_env_, src_bucket_prefix_, src_object_prefix_, region_,
        dest_bucket_prefix_, dest_object_prefix_, region_, cloud_env_options_,
        options_.info_log, &aenv));
    aenv_.reset(aenv);
  }

  // Open database via the cloud interface
  void OpenDB() {
    ASSERT_NE(cloud_env_options_.credentials.access_key_id.size(), 0);
    ASSERT_NE(cloud_env_options_.credentials.secret_key.size(), 0);

    // Create new AWS env
    CreateAwsEnv();
    options_.env = aenv_.get();

    // default column family
    ColumnFamilyOptions cfopt = options_;
    std::vector<ColumnFamilyDescriptor> column_families;
    column_families.emplace_back(
        ColumnFamilyDescriptor(kDefaultColumnFamilyName, cfopt));
    std::vector<ColumnFamilyHandle*> handles;

    ASSERT_TRUE(db_ == nullptr);
    ASSERT_OK(DBCloud::Open(options_, dbname_, column_families,
                            persistent_cache_path_, persistent_cache_size_gb_,
                            &handles, &db_));
    ASSERT_OK(db_->GetDbIdentity(dbid_));

    // Delete the handle for the default column family because the DBImpl
    // always holds a reference to it.
    ASSERT_TRUE(handles.size() > 0);
    delete handles[0];
  }

  // Creates and Opens a clone
  void CloneDB(const std::string& clone_name, const std::string& src_bucket,
               const std::string& src_object_path,
               const std::string& dest_bucket,
               const std::string& dest_object_path,
               std::unique_ptr<DBCloud>* cloud_db,
               std::unique_ptr<CloudEnv>* cloud_env) {
    // The local directory where the clone resides
    std::string cname = clone_dir_ + "/" + clone_name;

    CloudEnv* cenv;
    DBCloud* clone_db;

    // If there is no destination bucket, then the clone needs to copy
    // all sst fies from source bucket to local dir
    CloudEnvOptions copt = cloud_env_options_;
    if (dest_bucket.empty()) {
      copt.keep_local_sst_files = true;
    }

    // Create new AWS env
    ASSERT_OK(CloudEnv::NewAwsEnv(base_env_, src_bucket, src_object_path,
                                  region_, dest_bucket, dest_object_path,
                                  region_, copt, options_.info_log, &cenv));

    // sets the cloud env to be used by the env wrapper
    options_.env = cenv;

    // Returns the cloud env that was created
    cloud_env->reset(cenv);

    // default column family
    ColumnFamilyOptions cfopt = options_;

    std::vector<ColumnFamilyDescriptor> column_families;
    column_families.emplace_back(
        ColumnFamilyDescriptor(kDefaultColumnFamilyName, cfopt));
    std::vector<ColumnFamilyHandle*> handles;

    ASSERT_OK(DBCloud::Open(options_, cname, column_families,
                            persistent_cache_path_, persistent_cache_size_gb_,
                            &handles, &clone_db));
    cloud_db->reset(clone_db);

    // Delete the handle for the default column family because the DBImpl
    // always holds a reference to it.
    ASSERT_TRUE(handles.size() > 0);
    delete handles[0];
  }

  void CloseDB() {
    if (db_) {
      db_->Flush(FlushOptions());  // convert pending writes to sst files
      delete db_;
      db_ = nullptr;
    }
  }

  void SetPersistentCache(const std::string& path, uint64_t size_gb) {
    persistent_cache_path_ = path;
    persistent_cache_size_gb_ = size_gb;
  }

  Status GetCloudLiveFilesSrc(std::set<uint64_t>* list) {
    std::unique_ptr<ManifestReader> manifest(
        new ManifestReader(options_.info_log, aenv_.get(), src_bucket_prefix_));
    return manifest->GetLiveFiles(src_object_prefix_, list);
  }

 protected:
  Env* base_env_;
  Options options_;
  std::string dbname_;
  std::string clone_dir_;
  std::string src_bucket_prefix_;
  std::string src_object_prefix_;
  std::string dest_bucket_prefix_;
  std::string dest_object_prefix_;
  CloudEnvOptions cloud_env_options_;
  std::string region_;
  std::string dbid_;
  std::string persistent_cache_path_;
  uint64_t persistent_cache_size_gb_;
  DBCloud* db_;
  unique_ptr<CloudEnv> aenv_;
};

//
// Most basic test. Create DB, write one key, close it and then check to see
// that the key exists.
//
TEST_F(CloudTest, BasicTest) {
  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  CloseDB();
  value.clear();

  // Reopen and validate
  OpenDB();
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_EQ(value, "World");

  std::set<uint64_t> live_files;
  ASSERT_OK(GetCloudLiveFilesSrc(&live_files));
  ASSERT_GT(live_files.size(), 0);
  CloseDB();
}

TEST_F(CloudTest, GetChildrenTest) {
  // Create some objects in S3
  OpenDB();
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Flush(FlushOptions()));

  CloseDB();
  DestroyDir(dbname_);
  OpenDB();

  std::vector<std::string> children;
  ASSERT_OK(aenv_->GetChildren(dbname_, &children));
  int sst_files = 0;
  for (auto c : children) {
    if (IsSstFile(c)) {
      sst_files++;
    }
  }
  // This verifies that GetChildren() works on S3. We deleted the S3 file
  // locally, so the only way to actually get it through GetChildren() if
  // listing S3 buckets works.
  EXPECT_EQ(sst_files, 1);
}

//
// Create and read from a clone.
//
TEST_F(CloudTest, Newdb) {
  std::string master_dbid;
  std::string newdb1_dbid;
  std::string newdb2_dbid;

  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  ASSERT_OK(db_->GetDbIdentity(master_dbid));
  CloseDB();
  value.clear();

  {
    // Create and Open a new instance
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("newdb1", src_bucket_prefix_, src_object_prefix_, "", "", &cloud_db,
            &cloud_env);

    // Retrieve the id of the first reopen
    ASSERT_OK(cloud_db->GetDbIdentity(newdb1_dbid));

    // This reopen has the same src and destination paths, so it is
    // not a clone, but just a reopen.
    ASSERT_EQ(newdb1_dbid, master_dbid);

    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);

    // Open master and write one more kv to it. The dest bucket is empty,
    // so writes go to local dir only.
    OpenDB();
    ASSERT_OK(db_->Put(WriteOptions(), "Dhruba", "Borthakur"));

    // check that the newly written kv exists
    value.clear();
    ASSERT_OK(db_->Get(ReadOptions(), "Dhruba", &value));
    ASSERT_TRUE(value.compare("Borthakur") == 0);

    // check that the earlier kv exists too
    value.clear();
    ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);
    CloseDB();

    // Assert  that newdb1 cannot see the second kv because the second kv
    // was written to local dir only.
    ASSERT_TRUE(cloud_db->Get(ReadOptions(), "Dhruba", &value).IsNotFound());
  }
  {
    // Create another instance using a different local dir but the same two
    // buckets as newdb1. This should be identical in contents with newdb1.
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("newdb2", src_bucket_prefix_, src_object_prefix_, "", "", &cloud_db,
            &cloud_env);

    // Retrieve the id of the second clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb2_dbid));

    // Since we used the same src and destination buckets & paths for both
    // newdb1 and newdb2, we should get the same dbid as newdb1
    ASSERT_EQ(newdb1_dbid, newdb2_dbid);

    // check that both the kvs appear in the clone
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Dhruba", &value));
    ASSERT_TRUE(value.compare("Borthakur") == 0);
  }

  CloseDB();
}

//
// Create and read from a clone.
//
TEST_F(CloudTest, TrueClone) {
  std::string master_dbid;
  std::string newdb1_dbid;
  std::string newdb2_dbid;
  std::string newdb3_dbid;

  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  ASSERT_OK(db_->GetDbIdentity(master_dbid));
  CloseDB();
  value.clear();
  {
    // Create a new instance with different src and destination paths.
    // This is true clone and should have all the contents of the masterdb
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath1", src_bucket_prefix_, src_object_prefix_,
            src_bucket_prefix_, "clone1_path", &cloud_db, &cloud_env);

    // Retrieve the id of the clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb1_dbid));

    // Since we used the different src and destination paths for both
    // the master and clone1, the clone should have its own identity.
    ASSERT_NE(master_dbid, newdb1_dbid);

    // check that the original kv appears in the clone
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);

    // write a new value to the clone
    ASSERT_OK(cloud_db->Put(WriteOptions(), "Hello", "Clone1"));
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("Clone1") == 0);
  }
  {
    // Reopen clone1 with a different local path
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath2", src_bucket_prefix_, src_object_prefix_,
            src_bucket_prefix_, "clone1_path", &cloud_db, &cloud_env);

    // Retrieve the id of the clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb2_dbid));
    ASSERT_EQ(newdb1_dbid, newdb2_dbid);
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("Clone1") == 0);
  }
  {
    // Reopen clone1 with the same local path as above.
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath2", src_bucket_prefix_, src_object_prefix_,
            src_bucket_prefix_, "clone1_path", &cloud_db, &cloud_env);

    // Retrieve the id of the clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb2_dbid));
    ASSERT_EQ(newdb1_dbid, newdb2_dbid);
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("Clone1") == 0);
  }
  {
    // Create clone2
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath3",  // xxx try with localpath2
            src_bucket_prefix_, src_object_prefix_, src_bucket_prefix_,
            "clone2_path", &cloud_db, &cloud_env);

    // Retrieve the id of the clone db
    ASSERT_OK(cloud_db->GetDbIdentity(newdb3_dbid));
    ASSERT_NE(newdb2_dbid, newdb3_dbid);

    // verify that data is still as it was in the original db.
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);

    // Assert that there are no redundant sst files
    CloudEnvImpl* env = static_cast<CloudEnvImpl*>(cloud_env.get());
    std::vector<std::string> to_be_deleted;
    ASSERT_OK(env->FindObsoleteFiles(src_bucket_prefix_, &to_be_deleted));
    // TODO(igor): Re-enable once purger code is fixed
    // ASSERT_EQ(to_be_deleted.size(), 0);

    // Assert that there are no redundant dbid
    ASSERT_OK(env->FindObsoleteDbid(src_bucket_prefix_, &to_be_deleted));
    // TODO(igor): Re-enable once purger code is fixed
    // ASSERT_EQ(to_be_deleted.size(), 0);
  }
}

//
// verify that dbid registry is appropriately handled
//
TEST_F(CloudTest, DbidRegistry) {
  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);

  // Assert that there is one db in the registry
  DbidList dbs;
  ASSERT_OK(aenv_->GetDbidList(src_bucket_prefix_, &dbs));
  ASSERT_EQ(dbs.size(), 1);

  CloseDB();
}

TEST_F(CloudTest, KeepLocalFiles) {
  cloud_env_options_.keep_local_sst_files = true;
  // Create two files
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_OK(db_->Put(WriteOptions(), "Hello2", "World2"));
  ASSERT_OK(db_->Flush(FlushOptions()));

  CloseDB();
  DestroyDir(dbname_);
  OpenDB();

  std::vector<std::string> files;
  ASSERT_OK(Env::Default()->GetChildren(dbname_, &files));
  long sst_files =
      std::count_if(files.begin(), files.end(), [](const std::string& file) {
        return file.find("sst") != std::string::npos;
      });
  ASSERT_EQ(sst_files, 2);

  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_EQ(value, "World");
  ASSERT_OK(db_->Get(ReadOptions(), "Hello2", &value));
  ASSERT_EQ(value, "World2");
  CloseDB();
}

TEST_F(CloudTest, CopyToFromS3) {
  std::string fname = dbname_ + "/100000.sst";

  // Create aws env
  cloud_env_options_.keep_local_sst_files = true;
  CreateAwsEnv();
  ((CloudEnvImpl*)aenv_.get())->TEST_InitEmptyCloudManifest();
  char buffer[1 * 1024 * 1024];

  // create a 10 MB file and upload it to cloud
  {
    unique_ptr<WritableFile> writer;
    ASSERT_OK(aenv_->NewWritableFile(fname, &writer, EnvOptions()));

    for (int i = 0; i < 10; i++) {
      ASSERT_OK(writer->Append(Slice(buffer, sizeof(buffer))));
    }
    // sync and close file
  }

  // delete the file manually.
  ASSERT_OK(base_env_->DeleteFile(fname));

  // reopen file for reading. It should be refetched from cloud storage.
  {
    unique_ptr<RandomAccessFile> reader;
    ASSERT_OK(aenv_->NewRandomAccessFile(fname, &reader, EnvOptions()));

    uint64_t offset = 0;
    for (int i = 0; i < 10; i++) {
      Slice result;
      char* scratch = &buffer[0];
      ASSERT_OK(reader->Read(offset, sizeof(buffer), &result, scratch));
      ASSERT_EQ(result.size(), sizeof(buffer));
      offset += sizeof(buffer);
    }
  }
}

TEST_F(CloudTest, DelayFileDeletion) {
  std::string fname = dbname_ + "/000010.sst";

  // Create aws env
  cloud_env_options_.keep_local_sst_files = true;
  CreateAwsEnv();
  ((CloudEnvImpl*)aenv_.get())->TEST_InitEmptyCloudManifest();
  ((AwsEnv*)aenv_.get())->TEST_SetFileDeletionDelay(std::chrono::seconds(2));

  auto createFile = [&]() {
    unique_ptr<WritableFile> writer;
    ASSERT_OK(aenv_->NewWritableFile(fname, &writer, EnvOptions()));

    for (int i = 0; i < 10; i++) {
      ASSERT_OK(writer->Append("igor"));
    }
    // sync and close file
  };

  for (int iter = 0; iter <= 1; ++iter) {
    createFile();
    // delete the file
    ASSERT_OK(aenv_->DeleteFile(fname));
    // file should still be there
    ASSERT_OK(aenv_->FileExists(fname));

    if (iter == 1) {
      // should prevent the deletion
      createFile();
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
    auto st = aenv_->FileExists(fname);
    if (iter == 0) {
      // in iter==0 file should be deleted after 2 seconds
      ASSERT_TRUE(st.IsNotFound());
    } else {
      // in iter==1 file should not be deleted because we wrote the new file
      ASSERT_OK(st);
    }
  }
}

// Verify that a savepoint copies all src files to destination
TEST_F(CloudTest, Savepoint) {
  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  CloseDB();
  value.clear();
  std::string dest_path = "/clone2_path";
  {
    // Create a new instance with different src and destination paths.
    // This is true clone and should have all the contents of the masterdb
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath1", src_bucket_prefix_, src_object_prefix_,
            src_bucket_prefix_, dest_path, &cloud_db, &cloud_env);

    // check that the original kv appears in the clone
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);

    // there should be only one sst file
    std::vector<LiveFileMetaData> flist;
    cloud_db->GetLiveFilesMetaData(&flist);
    ASSERT_TRUE(flist.size() == 1);

    auto remapped_fname =
        ((CloudEnvImpl*)cloud_env.get())->RemapFilename(flist[0].name);
    // source path
    std::string spath = src_object_prefix_ + "/" + remapped_fname;
    ASSERT_OK(cloud_env->ExistsObject(src_bucket_prefix_, spath));

    // Verify that the destination path does not have any sst files
    std::string dpath = dest_path + "/" + remapped_fname;
    ASSERT_TRUE(
        cloud_env->ExistsObject(src_bucket_prefix_, dpath).IsNotFound());

    // write a new value to the clone
    ASSERT_OK(cloud_db->Put(WriteOptions(), "Hell", "Done"));
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hell", &value));
    ASSERT_TRUE(value.compare("Done") == 0);

    // Invoke savepoint to populate destination path from source path
    ASSERT_OK(cloud_db->Savepoint());

    // check that the sst file is copied to dest path
    ASSERT_OK(cloud_env->ExistsObject(src_bucket_prefix_, dpath));
  }
  {
    // Reopen the clone
    std::unique_ptr<CloudEnv> cloud_env;
    std::unique_ptr<DBCloud> cloud_db;
    CloneDB("localpath2", src_bucket_prefix_, src_object_prefix_,
            src_bucket_prefix_, dest_path, &cloud_db, &cloud_env);

    // check that the both kvs appears in the clone
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hello", &value));
    ASSERT_TRUE(value.compare("World") == 0);
    value.clear();
    ASSERT_OK(cloud_db->Get(ReadOptions(), "Hell", &value));
    ASSERT_TRUE(value.compare("Done") == 0);
  }
}

TEST_F(CloudTest, Encryption) {
  // Create aws env
  cloud_env_options_.server_side_encryption = true;
  char* key_id = getenv("AWS_KMS_KEY_ID");
  if (key_id != nullptr) {
    cloud_env_options_.encryption_key_id = std::string(key_id);
    Log(options_.info_log, "Found encryption key id in env variable %s",
        key_id);
  }

  OpenDB();

  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  // create a file
  ASSERT_OK(db_->Flush(FlushOptions()));
  CloseDB();

  OpenDB();
  std::string value;
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_EQ(value, "World");
  CloseDB();
}

TEST_F(CloudTest, KeepLocalLog) {
  cloud_env_options_.keep_local_log_files = false;

  // Create two files
  OpenDB();

  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_OK(db_->Put(WriteOptions(), "Hello2", "World2"));
  ASSERT_OK(db_->Flush(FlushOptions()));

  CloseDB();
}

// Test whether we are able to recover nicely from two different writers to the
// same S3 bucket. (The feature that was enabled by CLOUDMANIFEST)
TEST_F(CloudTest, TwoDBsOneBucket) {
  auto firstDB = dbname_;
  auto secondDB = dbname_ + "-1";
  cloud_env_options_.keep_local_sst_files = true;
  std::string value;

  OpenDB();
  // Create two files
  ASSERT_OK(db_->Put(WriteOptions(), "First", "File"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  ASSERT_OK(db_->Put(WriteOptions(), "Second", "File"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  auto files = GetSSTFiles();
  EXPECT_EQ(files.size(), 2);
  CloseDB();

  // Open again, with no destination bucket
  dest_bucket_prefix_.clear();
  dest_object_prefix_.clear();
  OpenDB();
  ASSERT_OK(db_->Put(WriteOptions(), "Third", "File"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  auto newFiles = GetSSTFiles();
  EXPECT_EQ(newFiles.size(), 3);
  // Remember the third file we created
  std::vector<std::string> diff;
  std::set_difference(newFiles.begin(), newFiles.end(), files.begin(),
                      files.end(), std::inserter(diff, diff.begin()));
  ASSERT_EQ(diff.size(), 1);
  auto thirdFile = diff[0];
  CloseDB();

  // Open in a different directory with destination bucket set
  dbname_ = secondDB;
  dest_bucket_prefix_ = src_bucket_prefix_;
  dest_object_prefix_ = src_object_prefix_;
  OpenDB();
  ASSERT_OK(db_->Put(WriteOptions(), "Third", "DifferentFile"));
  ASSERT_OK(db_->Flush(FlushOptions()));
  CloseDB();

  // Open back in the first directory with no destination
  dbname_ = firstDB;
  dest_bucket_prefix_.clear();
  dest_object_prefix_.clear();
  OpenDB();
  // Changes to the cloud database should make no difference for us
  ASSERT_OK(db_->Get(ReadOptions(), "Third", &value));
  EXPECT_EQ(value, "File");
  CloseDB();

  // Reopen in the first directory, this time with destination path
  dbname_ = firstDB;
  dest_bucket_prefix_ = src_bucket_prefix_;
  dest_object_prefix_ = src_object_prefix_;
  OpenDB();
  // Changes to the cloud database should be pulled down now.
  ASSERT_OK(db_->Get(ReadOptions(), "Third", &value));
  EXPECT_EQ(value, "DifferentFile");
  files = GetSSTFiles();
  // Should no longer be in my directory because it's not part of the new
  // MANIFEST.
  EXPECT_TRUE(files.find(thirdFile) == files.end());
  CloseDB();
}

// This test is similar to TwoDBsOneBucket, but is much more chaotic and illegal
// -- it runs two databases on exact same S3 bucket. The work on CLOUDMANIFEST
// enables us to run in that configuration for extended amount of time (1 hour
// by default) without any issues -- the last CLOUDMANIFEST writer wins.
TEST_F(CloudTest, TwoConcurrentWriters) {
  auto firstDB = dbname_;
  auto secondDB = dbname_ + "-1";

  DBCloud *db1, *db2;
  CloudEnv *aenv1, *aenv2;

  auto openDB1 = [&] {
    dbname_ = firstDB;
    OpenDB();
    db1 = db_;
    db_ = nullptr;
    aenv1 = aenv_.release();
  };
  auto openDB2 = [&] {
    dbname_ = secondDB;
    OpenDB();
    db2 = db_;
    db_ = nullptr;
    aenv2 = aenv_.release();
  };
  auto closeDB1 = [&] {
    db_ = db1;
    aenv_.reset(aenv1);
    CloseDB();
  };
  auto closeDB2 = [&] {
    db_ = db2;
    aenv_.reset(aenv2);
    CloseDB();
  };

  openDB1();
  openDB2();

  // Create bunch of files, reopening the databases during
  for (int i = 0; i < 5; ++i) {
    closeDB1();
    if (i == 2) {
      DestroyDir(firstDB);
    }
    // opening the database makes me a master (i.e. CLOUDMANIFEST points to my
    // manifest), my writes are applied to the shared space!
    openDB1();
    for (int j = 0; j < 5; ++j) {
      auto key = ToString(i) + ToString(j) + "1";
      ASSERT_OK(db1->Put(WriteOptions(), key, "FirstDB"));
      ASSERT_OK(db1->Flush(FlushOptions()));
    }
    closeDB2();
    if (i == 2) {
      DestroyDir(secondDB);
    }
    // opening the database makes me a master (i.e. CLOUDMANIFEST points to my
    // manifest), my writes are applied to the shared space!
    openDB2();
    for (int j = 0; j < 5; ++j) {
      auto key = ToString(i) + ToString(j) + "2";
      ASSERT_OK(db2->Put(WriteOptions(), key, "SecondDB"));
      ASSERT_OK(db2->Flush(FlushOptions()));
    }
  }

  dbname_ = firstDB;
  // This write should not be applied, because DB2 is currently the owner of the
  // S3 bucket
  ASSERT_OK(db1->Put(WriteOptions(), "ShouldNotBeApplied", ""));
  ASSERT_OK(db1->Flush(FlushOptions()));

  closeDB1();
  closeDB2();

  openDB1();
  for (int i = 0; i < 5; ++i) {
    for (int j = 0; j < 5; ++j) {
      std::string val;
      auto key = ToString(i) + ToString(j);
      ASSERT_OK(db1->Get(ReadOptions(), key + "1", &val));
      EXPECT_EQ(val, "FirstDB");
      ASSERT_OK(db1->Get(ReadOptions(), key + "2", &val));
      EXPECT_EQ(val, "SecondDB");
    }
  }

  std::string v;
  ASSERT_TRUE(db1->Get(ReadOptions(), "ShouldNotBeApplied", &v).IsNotFound());
}

#ifdef AWS_DO_NOT_RUN
//
// Verify that we can cache data from S3 in persistent cache.
//
TEST_F(CloudTest, PersistentCache) {
  std::string pcache = test::TmpDir() + "/persistent_cache";
  SetPersistentCache(pcache, 1);

  // Put one key-value
  OpenDB();
  std::string value;
  ASSERT_OK(db_->Put(WriteOptions(), "Hello", "World"));
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_TRUE(value.compare("World") == 0);
  CloseDB();
  value.clear();

  // Reopen and validate
  OpenDB();
  ASSERT_OK(db_->Get(ReadOptions(), "Hello", &value));
  ASSERT_EQ(value, "World");
  CloseDB();
}
#endif /* AWS_DO_NOT_RUN */

}  //  namespace rocksdb

// A black-box test for the cloud wrapper around rocksdb
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else  // USE_AWS

#include <stdio.h>

int main(int argc, char** argv) {
  fprintf(stderr,
          "SKIPPED as DBCloud is supported only when USE_AWS is defined.\n");
  return 0;
}
#endif

#else  // ROCKSDB_LITE

#include <stdio.h>

int main(int argc, char** argv) {
  fprintf(stderr, "SKIPPED as DBCloud is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // !ROCKSDB_LITE
