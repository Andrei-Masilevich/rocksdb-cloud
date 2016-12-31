//  Copyright (c) 2016-present, Rockset, Inc.  All rights reserved.
//
#include <iostream>
#include <fstream>
#include "aws/aws_env.h"

#ifdef USE_AWS

#include "cloud/aws_file.h"
#include "rocksdb/env.h"
#include "rocksdb/status.h"
#include "util/string_util.h"
#include "util/stderr_logger.h"

namespace rocksdb {

//
// The AWS credentials are specified to the constructor via
// access_key_id and secret_key.
//  
AwsEnv::AwsEnv(const std::string& bucket_prefix,
	     const std::string& access_key_id,
	     const std::string& secret_key,
	     std::shared_ptr<Logger> info_log,
	     const bool keep_local_sst_files)
  : bucket_prefix_(bucket_prefix),
    info_log_(info_log),
    keep_local_sst_files_(keep_local_sst_files),
    running_(true)  {
  posixEnv_ = Env::Default();
  Aws::InitAPI(Aws::SDKOptions());

  // create AWS creds
  Aws::Auth::AWSCredentials creds(Aws::String(access_key_id.c_str()),
                                  Aws::String(secret_key.c_str()));

  // create AWS S3 client with appropriate timeouts
  Aws::Client::ClientConfiguration config;
  config.connectTimeoutMs = 30000;
  config.requestTimeoutMs = 600000;
  s3client_ = std::make_shared<Aws::S3::S3Client>(creds, config);

  create_bucket_status_ = S3WritableFile::CreateBucketInS3(s3client_, bucket_prefix);
  if (!create_bucket_status_.ok()) {
    Log(InfoLogLevel::DEBUG_LEVEL, info_log,
        "[aws] NewAwsEnv Unable to  create bucket %s",
        create_bucket_status_.ToString().c_str());
  }

  // create Kinesis client for storing logs
  if (create_bucket_status_.ok()) {
    kinesis_client_ = std::make_shared<Aws::Kinesis::KinesisClient>(
		                    creds, config);
    if (kinesis_client_ == nullptr) {
      create_bucket_status_ = Status::IOError(
		      "Error in creating Kinesis client");
    }
  }

  // Create Kinesis stream and wait for it to be ready
  if (create_bucket_status_.ok()) {
    create_bucket_status_ = KinesisSystem::CreateStream(
		              this,
		              info_log_,
		              kinesis_client_,
		              bucket_prefix);
    if (!create_bucket_status_.ok()) {
      Log(InfoLogLevel::DEBUG_LEVEL, info_log,
          "[aws] NewAwsEnv Unable to  create stream %s",
          create_bucket_status_.ToString().c_str());
    }
  }

  if (create_bucket_status_.ok()) {
    // create tailer object
    KinesisSystem* f = new KinesisSystem(this, info_log);
    create_bucket_status_ = f->status();
    tailer_.reset(f);

    // create tailer thread
    if (create_bucket_status_.ok()) {
      auto lambda = [this]() {
                    tailer_->TailStream();
                    };
      tid_ = std::thread(lambda);
    }
  }
  if (!create_bucket_status_.ok()) {
    Log(InfoLogLevel::DEBUG_LEVEL, info_log,
        "[aws] NewAwsEnv Unable to create environment %s",
        create_bucket_status_.ToString().c_str());
  }
}

AwsEnv::~AwsEnv() {
  fprintf(stderr, "Destroying AwsEnv::Default()\n");
  running_ = false;
  if (tid_.joinable()) {
    tid_.join();
  }
}

Status AwsEnv::IsValid() {
  return create_bucket_status_;
}

//
// Check if options are compatible with the S3 storage system
//
Status AwsEnv::CheckOption(const EnvOptions& options) {
  // Cannot mmap files that reside on AWS S3, unless the file is also local
  if (options.use_mmap_reads && !keep_local_sst_files_) {
    std::string msg = "Mmap only if keep_local_sst_files_ is set";
    return Status::InvalidArgument(msg);
  }
  return Status::OK();
}

// 
// find out whether this is an sst file or a log file.
//
void AwsEnv::GetFileType(const std::string& fname,
		         bool* sstFile, bool* logFile) {
  *logFile = false;
  *sstFile = IsSstFile(fname);
  if (!*sstFile) {
    *logFile = IsLogFile(fname);
  }
}

// open a file for sequential reading
Status AwsEnv::NewSequentialFile(const std::string& fname,
                                unique_ptr<SequentialFile>* result,
                                const EnvOptions& options) {
  assert(IsValid().ok());

  // Get file type
  bool logfile;
  bool sstfile;
  GetFileType(fname, &sstfile, &logfile);

  Status st = CheckOption(options);
  if (!st.ok()) {
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[aws] NewSequentialFile file '%s' %s",
        fname.c_str(), st.ToString().c_str());
    return st;
  }
  if (sstfile) {
    // If this is a sst file and we are instructed to keep the local
    // sst file intact, then use local file system.
    if (keep_local_sst_files_) {
      return posixEnv_->NewSequentialFile(fname, result, options);
    }
    // read from S3
    S3ReadableFile* f = new S3ReadableFile(this, fname);
    if (!f->status().ok()) {
      return f->status();
    }
    result->reset(dynamic_cast<SequentialFile*>(f));

    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[s3] NewSequentialFile file %s %s",
        fname.c_str(), "ok");

  } else if (logfile) {               // read from Kinesis
    assert(tailer_->status().ok());

    // map  pathname to cache dir
    std::string pathname = KinesisSystem::GetCachePath(
		             tailer_->GetCacheDir(), Slice(fname));
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[Kinesis] NewSequentialFile logfile %s %s",
        pathname.c_str(), "ok");

    auto lambda = [this, pathname, &result, options]() -> Status {
                    return posixEnv_->NewSequentialFile(pathname, result, options);
                    };
    return KinesisSystem::Retry(this, lambda);
  } else {
    // This is neither a sst file or a log file. Read from default env.
    return posixEnv_->NewSequentialFile(fname, result, options);
  }
  return Status::OK();
}

// open a file for random reading
Status AwsEnv::NewRandomAccessFile(const std::string& fname,
                                    unique_ptr<RandomAccessFile>* result,
				    const EnvOptions& options) {
  assert(IsValid().ok());

  // Get file type
  bool logfile;
  bool sstfile;
  GetFileType(fname, &sstfile, &logfile);

  // Validate options
  Status st = CheckOption(options);
  if (!st.ok()) {
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[aws] NewRandomAccessFile file '%s' %s",
        fname.c_str(), st.ToString().c_str());
    return st;
  }
  if (sstfile) {
    // If this is a sst file and we are instructed to keep the local
    // sst file intact, then use local file system.
    if (keep_local_sst_files_) {
      return posixEnv_->NewRandomAccessFile(fname, result, options);
    }
    // read from S3
    S3ReadableFile* f = new S3ReadableFile(this, fname);
    if (!f->status().ok()) {
      return f->status();
    }
    result->reset(dynamic_cast<RandomAccessFile*>(f));

    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[s3] NewRandomAccessFile file %s %s",
        fname.c_str(), "ok");
  } else if (logfile) {
    // read from Kinesis
    assert(tailer_->status().ok());

    // map  pathname to cache dir
    std::string pathname = KinesisSystem::GetCachePath(
		             tailer_->GetCacheDir(), Slice(fname));
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[kinesis] NewRandomAccessFile logfile %s %s",
        pathname.c_str(), "ok");

    auto lambda = [this, pathname, &result, options]() -> Status {
                    return posixEnv_->NewRandomAccessFile(pathname, result, options);
                    };
    return KinesisSystem::Retry(this, lambda);
  } else {
    // This is neither a sst file or a log file. Read from default env.
    return posixEnv_->NewRandomAccessFile(fname, result, options);
  }
  return Status::OK();
}

// create a new file for writing
Status AwsEnv::NewWritableFile(const std::string& fname,
                                unique_ptr<WritableFile>* result,
                                const EnvOptions& options) {
  assert(IsValid().ok());
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[aws] NewWritableFile src '%s'",
      fname.c_str());

  // Get file type
  bool logfile;
  bool sstfile;
  GetFileType(fname, &sstfile, &logfile);
  result->reset();

  // If this is not an sst or log file, then use local file system
  if (!sstfile && !logfile) {
    return posixEnv_->NewWritableFile(fname, result, options);
  }

  if (sstfile) {
    S3WritableFile* f = new S3WritableFile(this, fname, options);
    if (f == nullptr || !f->status().ok()) {
      delete f;
      *result = nullptr;
      Status s =  Status::IOError("[aws] NewWritableFile", fname.c_str());
      Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
          "[s3] NewWritableFile src %s %s",
        fname.c_str(), s.ToString().c_str());
      return s;
    }
    result->reset(dynamic_cast<WritableFile*>(f));
  } else if (logfile) {
    KinesisWritableFile* f = new KinesisWritableFile(this, fname, options);
    if (f == nullptr || !f->status().ok()) {
      delete f;
      *result = nullptr;
      Status s =  Status::IOError("[aws] NewWritableFile", fname.c_str());
      Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
          "[kinesis] NewWritableFile src %s %s",
        fname.c_str(), s.ToString().c_str());
      return s;
    }
    result->reset(dynamic_cast<WritableFile*>(f));
  }
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[aws] NewWritableFile src %s %s",
      fname.c_str(), "ok");
  return Status::OK();
}

class S3Directory : public Directory {
 public:
  explicit S3Directory(AwsEnv* env, const std::string name)
	  : env_(env), name_(name) {
    status_ = env_->GetPosixEnv()->NewDirectory(name, &posixDir);
  }

  ~S3Directory() {}

  virtual Status Fsync() {
    if (!status_.ok()) {
      return status_;
    }
    return posixDir->Fsync();
  }

  virtual Status status() {
    return status_;
  }

 private:
  AwsEnv* env_;
  std::string name_;
  Status status_;
  unique_ptr<Directory> posixDir;
};

//
//  Returns success only if the directory-bucket exists in the
//  AWS S3 service and the posixEnv local directory exists as well.
//
Status AwsEnv::NewDirectory(const std::string& name,
                           unique_ptr<Directory>* result) {
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[aws] NewDirectory name '%s'",
      name.c_str());
  assert(IsValid().ok());

  result->reset(nullptr);
  assert(!IsSstFile(name));

  // Check if directory exists in S3
  Status st = PathExistsInS3(name, false);
  if (!st.ok()) {
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[aws] Directory %s does not exist",
        name.c_str(), st.ToString().c_str());
  }

  // create new object.
  S3Directory* d = new S3Directory(this, name);

  // Check if the path exists in local dir
  if (d == nullptr || !d->status().ok()) {
    delete d;
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[aws] NewDirectory name %s unable to create local dir",
        name.c_str());
    return d->status();
  }
  result->reset(d);
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[aws] NewDirectory name %s ok",
      name.c_str());
  return Status::OK();
}

//
// Check if the specified filename exists.
//
Status AwsEnv::FileExists(const std::string& fname) {
  assert(IsValid().ok());
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[aws] FileExists path '%s' ",
      fname.c_str());
  Status st;

  // Get file type
  bool logfile;
  bool sstfile;
  GetFileType(fname, &sstfile, &logfile);

  if (sstfile) {
    // If this is a sst file and we are instructed to keep the local
    // sst file intact, then use local file system.
    if (keep_local_sst_files_) {
      st = posixEnv_->FileExists(fname);
    } else {
    st = PathExistsInS3(fname, true);
    }
  } else if (logfile) {
    // read from Kinesis
    assert(tailer_->status().ok());

    // map  pathname to cache dir
    std::string pathname = KinesisSystem::GetCachePath(
		             tailer_->GetCacheDir(), Slice(fname));
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[kinesis] FileExists logfile %s %s",
        pathname.c_str(), "ok");

    auto lambda = [this, pathname]() -> Status {
                    return posixEnv_->FileExists(pathname);
                    };
    st = KinesisSystem::Retry(this, lambda);
  } else {
    st = posixEnv_->FileExists(fname);
  }
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[aws] FileExists path '%s' %s",
      fname.c_str(), st.ToString().c_str());
  return st;
}

//
// Check if the specified pathname exists as a file or directory
// in AWS-S3
//
Status AwsEnv::PathExistsInS3(const std::string& fname, bool isfile) {
  assert(IsValid().ok());

  // We could have used Aws::S3::Model::ListObjectsRequest to find
  // the file size, but a ListObjectsRequest is not guaranteed to
  // return the most recently created objects. Only a Get is
  // guaranteed to be consistent with Puts. So, we try to read
  // 0 bytes from the object.
  unique_ptr<SequentialFile> fd;
  Slice result;
  S3ReadableFile* f = new S3ReadableFile(this, fname, isfile);
  fd.reset(f);
  Status ret = f->Read(0, &result, nullptr);
  if (!ret.ok()) {
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[s3] PathExistsInS3 dir %s %s",
        fname.c_str(), ret.ToString().c_str());
    return ret;
  }
  // If keep_local_sst_files_, then a local copy of the file should exist
  // too. Print informational message if this situation occurs. It can occur
  // if the database is restarted on a new machine and the original files
  // are not available on the local storage.
  if (keep_local_sst_files_ && isfile) {
    Status st = posixEnv_->FileExists(fname);
    if (!st.ok()) {
      Log(InfoLogLevel::WARN_LEVEL, info_log_,
          "[s3] FileExists path %s exists in S3 but does not "
	  "exist locally. It will be served directly from S3. %s",
          fname.c_str(), st.ToString().c_str());
    }
  }
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] FileExists path % exists",
      fname.c_str());
  return Status::OK();
}

//
// Return the names of all children of the specified path from S3
//
Status AwsEnv::GetChildrenFromS3(const std::string& path,
                          std::vector<std::string>* result) {
  assert(IsValid().ok());
  // The bucket name
  Aws::String bucket = GetBucket(bucket_prefix_);

  // the starting object marker
  Aws::String prefix = Aws::String(path.c_str(), path.size());
  Aws::String marker;
  bool loop = true;

  // get info of bucket+object
  while (loop) {
    Aws::S3::Model::ListObjectsRequest request;
    request.SetBucket(bucket);
    request.SetMaxKeys(50);
    request.SetPrefix(prefix);
    request.SetMarker(marker);

    Aws::S3::Model::ListObjectsOutcome outcome =
	    s3client_->ListObjects(request);
    bool isSuccess = outcome.IsSuccess();
    if (!isSuccess) {
      const Aws::Client::AWSError<Aws::S3::S3Errors>& error = outcome.GetError();
      std::string errmsg(error.GetMessage().c_str());
      Aws::S3::S3Errors s3err = error.GetErrorType();
      if (s3err == Aws::S3::S3Errors::NO_SUCH_BUCKET ||
	  s3err == Aws::S3::S3Errors::NO_SUCH_KEY ||
	  s3err == Aws::S3::S3Errors::RESOURCE_NOT_FOUND) {
        Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
            "[s3] GetChildren dir %s does not exist",
            path.c_str(), errmsg.c_str());
        return Status::NotFound(path, errmsg.c_str());
      }
      return Status::IOError(path, errmsg.c_str());
    }
    const Aws::S3::Model::ListObjectsResult& res = outcome.GetResult();
    const Aws::Vector<Aws::S3::Model::Object>& objs = res.GetContents();
    for (auto o : objs) {
      const Aws::String& key = o.GetKey();
      // Our path should be a prefix of the fetched value
      std::string keystr(key.c_str(), key.size());
      assert(keystr.find(path) == 0);
      if (keystr.find(path) != 0) {
        loop = false;
	break;
      }
      assert(IsSstFile(keystr));
      result->push_back(keystr);
    }

    // If there are no more entries, then we are done.
    if (!res.GetIsTruncated()) {
      break;
    }
    // The new starting point
    marker = res.GetNextMarker();
  }
  return Status::OK();
}

Status AwsEnv::GetChildren(const std::string& path,
                          std::vector<std::string>* result) {
  assert(IsValid().ok());
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] GetChildren path '%s' ",
      path.c_str());
  assert(!IsSstFile(path));
  result->clear();

  // fetch the list of children from S3
  Status st = GetChildrenFromS3(path, result);
  if (!st.ok()) {
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[s3] GetChildren %s error from S3 ",
        path.c_str());
    return st;
  }

  // fetch all files that exist in the local posix directory
  std::vector<std::string> local_files;
  st = posixEnv_->GetChildren(path, &local_files);
  if (!st.ok()) {
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[s3] GetChildren %s error on local dir",
        path.c_str());
    return st;
  }

  // Append the local list with the result set. Do not append
  // any sst files from the local list, the reason being that if an
  // sst file exists locally but not in S3, then it is as good as
  // it does not exist at all (for db durability reasons).
  //
  for (auto const& value: local_files) {
    if (!IsSstFile(value)) {
      result->push_back(value);
    }
  }
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] GetChildren %s successfully returned %d files",
      path.c_str(), result->size());
  return Status::OK();
}

Status AwsEnv::DeleteFile(const std::string& fname) {
  assert(IsValid().ok());
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] DeleteFile src %s",
      fname.c_str());
  Status st;

  // Get file type
  bool logfile;
  bool sstfile;
  GetFileType(fname, &sstfile, &logfile);

  if (sstfile) {
    // Delete from S3 and local file system
    st = DeletePathInS3(fname);
    if (st.ok() && keep_local_sst_files_) {
      st = posixEnv_->DeleteFile(fname);
    }
  } else if (logfile) {
    // read from Kinesis
    assert(tailer_->status().ok());

    // Log a Delete record to kinesis stream
    KinesisWritableFile* f = new KinesisWritableFile(this, fname, EnvOptions());
    if (f == nullptr || !f->status().ok()) {
      st =  Status::IOError("[Kinesis] DeleteFile", fname.c_str());
      delete f;
    } else {
      st = f->LogDelete();
      delete f;
    }
  } else {
    st = posixEnv_->DeleteFile(fname);
  }
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] DeleteFile file %s %s",
      fname.c_str(), st.ToString().c_str());
  return st;
};

//
// Delete the specified path from S3
//
Status AwsEnv::DeletePathInS3(const std::string& fname) {
  assert(IsValid().ok());
  Aws::String bucket = GetBucket(bucket_prefix_);

  // The filename is the same as the object name in the bucket
  Aws::String object = Aws::String(fname.c_str(), fname.size());

  // create request
  Aws::S3::Model::DeleteObjectRequest request;
  request.SetBucket(bucket);
  request.SetKey(object);

  Aws::S3::Model::DeleteObjectOutcome outcome =
	    s3client_->DeleteObject(request);
  bool isSuccess = outcome.IsSuccess();
  if (!isSuccess) {
    const Aws::Client::AWSError<Aws::S3::S3Errors>& error = outcome.GetError();
    std::string errmsg(error.GetMessage().c_str());
    Aws::S3::S3Errors s3err = error.GetErrorType();
    if (s3err == Aws::S3::S3Errors::NO_SUCH_BUCKET ||
        s3err == Aws::S3::S3Errors::NO_SUCH_KEY ||
	s3err == Aws::S3::S3Errors::RESOURCE_NOT_FOUND) {
      Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
          "[s3] S3WritableFile error in deleting not-existent %s %",
          fname.c_str(), errmsg.c_str());
      return Status::NotFound(fname, errmsg.c_str());
    }
    return Status::IOError(fname, errmsg.c_str());
  }
  return Status::OK();
}

//
// Create a new directory.
// Create a new entry in AWS and create a directory in the
// local posix env.
//
Status AwsEnv::CreateDir(const std::string& dirname) {
  assert(IsValid().ok());
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] CreateDir dir '%s'",
      dirname.c_str());
  assert(!IsSstFile(dirname));

  // Get bucket name
  Aws::String bucket = GetBucket(bucket_prefix_);

  Aws::String object = Aws::String(dirname.c_str(), dirname.size());

  // create an empty object
  Aws::S3::Model::PutObjectRequest put_request;
  put_request.SetBucket(bucket);
  put_request.SetKey(object);
  Aws::S3::Model::PutObjectOutcome put_outcome =
	    s3client_->PutObject(put_request);
  bool isSuccess = put_outcome.IsSuccess();
  if (!isSuccess) {
    const Aws::Client::AWSError<Aws::S3::S3Errors>& error =
	      put_outcome.GetError();
    std::string errmsg(error.GetMessage().c_str(), error.GetMessage().size());
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[s3] CreateDir error in creating dir %s %s\n",
        dirname.c_str(), errmsg.c_str());
    return Status::IOError(dirname, errmsg.c_str());
  }
  // create the same directory in the posix filesystem as well
  Status st =  posixEnv_->CreateDir(dirname);

  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] CreateDir dir %s %s",
      dirname.c_str(), st.ToString().c_str());
  return st;
};

//
// Directories are created as a bucket in the AWS S3 service
// as well as a local directory via posix env.
//
Status AwsEnv::CreateDirIfMissing(const std::string& dirname) {
  assert(IsValid().ok());
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] CreateDirIfMissing dir '%s'",
       dirname.c_str());

  Aws::String bucket = GetBucket(bucket_prefix_);
  Aws::String object = Aws::String(dirname.c_str(), dirname.size());

  // create request
  Aws::S3::Model::PutObjectRequest put_request;
  put_request.SetBucket(bucket);
  put_request.SetKey(object);
  Aws::S3::Model::PutObjectOutcome put_outcome =
	    s3client_->PutObject(put_request);
  bool isSuccess = put_outcome.IsSuccess();
  if (!isSuccess) {
    const Aws::Client::AWSError<Aws::S3::S3Errors>& error =
	      put_outcome.GetError();
    std::string errmsg(error.GetMessage().c_str(), error.GetMessage().size());
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[s3] CreateDirIfMissing error in creating bucket %s %s",
        bucket.c_str(), errmsg.c_str());
    return Status::IOError(dirname, errmsg.c_str());
  }
  // create the same directory in the posix filesystem as well
  Status st = posixEnv_->CreateDirIfMissing(dirname);

  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] CreateDirIfMissing created dir %s %s",
      dirname.c_str(), st.ToString().c_str());
  return st;
};

Status AwsEnv::DeleteDir(const std::string& dirname) {
  assert(IsValid().ok());
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] DeleteDir src '%s'",
      dirname.c_str());
  assert(!IsSstFile(dirname));

  // Verify that the S3 directory has no children
  std::vector<std::string> results;
  Status st = GetChildrenFromS3(dirname, &results);
  if (st.ok() && results.size() != 0) {
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[s3] DeleteDir error in deleting nonempty dir %s with %d entries",
        dirname.c_str(), results.size());
    for (auto name: results) {
      Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
          "[s3] DeleteDir entry %s", name.c_str());
    }
    return Status::IOError("[s3] DeleteDir error in deleting nonempty dir",
		           dirname);
  }

  // Delete directory from S3
  st = DeletePathInS3(dirname);

  // delete the same directory in the posix filesystem as well
  if (st.ok()) {
    st = posixEnv_->DeleteDir(dirname);
  }
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] DeleteDir dir %s %s",
      dirname.c_str(), st.ToString().c_str());
  return st;
};

Status AwsEnv::GetFileSize(const std::string& fname, uint64_t* size) {
  assert(IsValid().ok());
  *size = 0L;
  Status st;

  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[aws] GetFileSize src '%s'", fname.c_str());

  // Get file type
  bool logfile;
  bool sstfile;
  GetFileType(fname, &sstfile, &logfile);

  if (sstfile) {
    // Get file length from S3
    st = GetFileInfoInS3(fname, size, nullptr);

    if (st.ok() && keep_local_sst_files_) {
      // Sanity check with local copy of sst file
      uint64_t local_size;
      Status ret = posixEnv_->GetFileSize(fname, &local_size);
      if (!ret.ok()) {
        Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
            "[aws] GetFileSize file %s exists in S3 but does not exist locally",
            fname.c_str());
      } else if (local_size != *size) {
        Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
            "[aws] GetFileSize file %s size on S3 %d but local size %ld",
            fname.c_str(), *size, local_size);
      }
    }
  } else if (logfile) {
    assert(tailer_->status().ok());

    // map  pathname to cache dir
    std::string pathname = KinesisSystem::GetCachePath(
		             tailer_->GetCacheDir(), Slice(fname));
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[kinesis] GetFileSize logfile %s %s",
        pathname.c_str(), "ok");

    auto lambda = [this, pathname, size]() -> Status {
                    return posixEnv_->GetFileSize(pathname, size);
                    };
    st = KinesisSystem::Retry(this, lambda);
  } else {
    st = posixEnv_->GetFileSize(fname, size);
  }
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[aws] GetFileSize src '%s' %s", fname.c_str(), st.ToString().c_str());
  info_log_->Flush();
  return st;
}

//
// Check if the specified pathname exists as a file or directory
// in AWS-S3
//

Status AwsEnv::GetFileInfoInS3(const std::string& fname, uint64_t* size,
		               uint64_t *modtime) {
  if (size) {
    *size = 0L;
  }
  if (modtime) {
    *modtime = 0L;
  }

  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] GetFileInfoInS3 src '%s'", fname.c_str());

  // We could have used Aws::S3::Model::ListObjectsRequest to find
  // the file size, but a ListObjectsRequest is not guaranteed to
  // return the most recently created objects. Only a Get is
  // guaranteed to be consistent with Puts. So, we try to read
  // 0 bytes from the object.
  unique_ptr<SequentialFile> fd;
  Slice result;
  S3ReadableFile* f = new S3ReadableFile(this, fname);
  fd.reset(f);
  Status ret = f->Read(0, &result, nullptr);
  if (!ret.ok()) {
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[s3] GetFileInfoInS3 dir %s %s",
        fname.c_str(), ret.ToString().c_str());
    return ret;
  }
  if (size) {
    *size = f->GetSize();
  }
  if (modtime) {
    *modtime = f->GetLastModTime();
  }
  return ret;
}

Status AwsEnv::GetFileModificationTime(const std::string& fname,
                                        uint64_t* time) {
  assert(IsValid().ok());
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[aws] GetFileModificationTime src '%s'",
      fname.c_str());
  Status st;

  // Get file type
  bool logfile;
  bool sstfile;
  GetFileType(fname, &sstfile, &logfile);

  if (sstfile) {
    // Get file length from S3
    st = GetFileInfoInS3(fname, nullptr, time);

    if (st.ok() && keep_local_sst_files_) {
      // Sanity check with local copy of sst file
      Status ret = posixEnv_->FileExists(fname);
      if (!ret.ok()) {
        Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
            "[s3] GetFileModificationTime file %s exists in S3 but does not exist locally",
            fname.c_str());
      }
    }
  } else if (logfile) {
    assert(tailer_->status().ok());

    // map  pathname to cache dir
    std::string pathname = KinesisSystem::GetCachePath(
		             tailer_->GetCacheDir(), Slice(fname));
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[kinesis] GetFileModificationTime logfile %s %s",
        pathname.c_str(), "ok");

    auto lambda = [this, pathname, time]() -> Status {
                    return posixEnv_->GetFileModificationTime(pathname, time);
                    };
    st = KinesisSystem::Retry(this, lambda);
  } else {
    st = posixEnv_->GetFileModificationTime(fname, time);
  }
  return st;
}

// The rename is not atomic. S3 does not support renaming natively.
// Copy file to a new object in S3 and then delete original object.
Status AwsEnv::RenameFile(const std::string& src, const std::string& target) {
  assert(IsValid().ok());
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[aws] RenameFile src '%s' target '%s'",
      src.c_str(), target.c_str());

  // Get file type of target
  bool logfile;
  bool sstfile;
  GetFileType(target, &sstfile, &logfile);

  // If the target is not an sst file, then use local file system
  if (!sstfile && !logfile) {
    return posixEnv_->RenameFile(src, target);
  }
  
  // Rename should never be called on sst files.
  if (sstfile) {
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[aws] RenameFile source sstfile %s %s is not supported",
        src.c_str(), target.c_str());
    assert(0);
    return Status::NotSupported(Slice(src), Slice(target));

  } else if (logfile) {

    // Rename should never be called on log files as well
    Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
        "[aws] RenameFile source logfile %s %s is not supported",
        src.c_str(), target.c_str());
    assert(0);
    return Status::NotSupported(Slice(src), Slice(target));
  }

  // TODO (dhruba)
  Status st;
  Aws::String target_bucket = "";
  Aws::String target_file = "";
  Aws::String source_url = Aws::String(src.c_str(), src.size());

  // create request
  Aws::S3::Model::CopyObjectRequest request;
  request.SetBucket(target_bucket);
  request.SetKey(target_file);
  request.SetCopySource(source_url);

  Aws::S3::Model::CopyObjectOutcome outcome =
	    s3client_->CopyObject(request);
  bool isSuccess = outcome.IsSuccess();
  if (!isSuccess) {
    const Aws::Client::AWSError<Aws::S3::S3Errors>& error = outcome.GetError();
    std::string errmsg(error.GetMessage().c_str());
    st = Status::IOError("AwsEnv::Rename", errmsg.c_str());
  }
  Log(InfoLogLevel::DEBUG_LEVEL, info_log_,
      "[s3] RenameFile src %s target %s: %s",
      src.c_str(), target.c_str(), st.ToString().c_str());
  return st;
}

Status AwsEnv::LockFile(const std::string& fname, FileLock** lock) {
  // there isn's a very good way to atomically check and create
  // a file via libs3
  *lock = nullptr;
  return Status::OK();
}

Status AwsEnv::UnlockFile(FileLock* lock) {
  return Status::OK();
}

Status AwsEnv::NewLogger(const std::string& fname,
                          shared_ptr<Logger>* result) {
  return posixEnv_->NewLogger(fname, result);
}

// The factory method for creating an S3 Env
AwsEnv* AwsEnv::NewAwsEnv(const std::string& bucket_prefix,
		       const std::string& access_key_id,
		       const std::string& secret_key,
		       std::shared_ptr<Logger> info_log) {
  AwsEnv* s =  new AwsEnv(bucket_prefix, access_key_id, secret_key, info_log);
  if (s == nullptr || !s->IsValid().ok()) {
    delete s;
    return nullptr;
  }
  return s;
}

//
// Retrieves the AWS credentials from two environment variables
// called "aws_access_key_id" and "aws_secret_access_key".
//
Status AwsEnv::GetTestCredentials(std::string* aws_access_key_id,
		                 std::string* aws_secret_access_key) {
  Status st;
  if (getenv("aws_access_key_id") == nullptr ||
      getenv("aws_secret_access_key") == nullptr) {
    std::string msg = "Skipping AWS tests. "
                      "AWS credentials should be set "
                      "using environment varaibles aws_access_key_id and "
                      "aws_secret_access_key";
    return Status::IOError(msg);
  }
  aws_access_key_id->assign(getenv("aws_access_key_id"));
  aws_secret_access_key->assign(getenv("aws_secret_access_key"));
  return st;
}

//
// Keep retrying the command until it is successful or the timeout has expired
//
Status KinesisSystem::Retry(Env* env, RetryType func) {
  using namespace std::chrono;
  Status stat;
  uint64_t start = env->NowMicros();

  while (true) {

    // If command is successful, return immediately
    stat = func();
    if (stat.ok()) {
      break;
    }
    // sleep for some time
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // If timeout has expired, return error
    uint64_t now = env->NowMicros();
    if (start + KinesisSystem::retry_period_micros < now) {
      stat = Status::TimedOut();
      break;
    }
  }
  return stat;  
}

}  // namespace rocksdb

#else // USE_AWS

// dummy placeholders used when S3 is not available
namespace rocksdb {
 Status AwsEnv::NewSequentialFile(const std::string& fname,
                                   unique_ptr<SequentialFile>* result,
                                   const EnvOptions& options) {
   return Status::NotSupported("Not compiled with aws support");
 }

 Status NewAwsEnv(Env** s3_env, const std::string& fsname) {
   return Status::NotSupported("Not compiled with aws support");
 }
}

#endif
