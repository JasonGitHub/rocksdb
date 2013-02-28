// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include "db/filename.h"
#include "db/dbformat.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"

namespace leveldb {

Status BuildTable(const std::string& dbname,
                  Env* env,
                  const Options& options,
                  TableCache* table_cache,
                  Iterator* iter,
                  FileMetaData* meta,
                  const Comparator* user_comparator,
                  const SequenceNumber newest_snapshot,
                  const SequenceNumber earliest_seqno_in_memtable) {
  Status s;
  meta->file_size = 0;
  iter->SeekToFirst();

  // If the sequence number of the smallest entry in the memtable is
  // smaller than the most recent snapshot, then we do not trigger
  // removal of duplicate/deleted keys as part of this builder.
  bool purge = options.purge_redundant_kvs_while_flush;
  if (earliest_seqno_in_memtable <= newest_snapshot) {
    purge = false;
  }

  std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    unique_ptr<WritableFile> file;
    s = env->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }
    TableBuilder* builder = new TableBuilder(options, file.get(), 0);

    // the first key is the smallest key
    Slice key = iter->key();
    meta->smallest.DecodeFrom(key);

    if (purge) {
      ParsedInternalKey prev_ikey;
      std::string prev_value;
      std::string prev_key;

      // store first key-value
      prev_key.assign(key.data(), key.size());
      prev_value.assign(iter->value().data(), iter->value().size());
      ParseInternalKey(Slice(prev_key), &prev_ikey);
      assert(prev_ikey.sequence >= earliest_seqno_in_memtable);

      for (iter->Next(); iter->Valid(); iter->Next()) {
        ParsedInternalKey this_ikey;
        Slice key = iter->key();
        ParseInternalKey(key, &this_ikey);
        assert(this_ikey.sequence >= earliest_seqno_in_memtable);

        if (user_comparator->Compare(prev_ikey.user_key, this_ikey.user_key)) {
          // This key is different from previous key.
          // Output prev key and remember current key
          builder->Add(Slice(prev_key), Slice(prev_value));
          prev_key.assign(key.data(), key.size());
          prev_value.assign(iter->value().data(), iter->value().size());
          ParseInternalKey(Slice(prev_key), &prev_ikey);
        } else {
          // seqno within the same key are in decreasing order
          assert(this_ikey.sequence < prev_ikey.sequence);
          // This key is an earlier version of the same key in prev_key.
          // Skip current key.
        }
      }
      // output last key
      builder->Add(Slice(prev_key), Slice(prev_value));
      meta->largest.DecodeFrom(Slice(prev_key));

    } else {
      for (; iter->Valid(); iter->Next()) {
        Slice key = iter->key();
        meta->largest.DecodeFrom(key);
        builder->Add(key, iter->value());
      }
    }

    // Finish and check for builder errors
    if (s.ok()) {
      s = builder->Finish();
      if (s.ok()) {
        meta->file_size = builder->FileSize();
        assert(meta->file_size > 0);
      }
    } else {
      builder->Abandon();
    }
    delete builder;

    // Finish and check for file errors
    if (s.ok() && !options.disableDataSync) {
      if (options.use_fsync) {
        s = file->Fsync();
      } else {
        s = file->Sync();
      }
    }
    if (s.ok()) {
      s = file->Close();
    }

    if (s.ok()) {
      // Verify that the table is usable
      Iterator* it = table_cache->NewIterator(ReadOptions(),
                                              meta->number,
                                              meta->file_size);
      s = it->status();
      delete it;
    }
  }

  // Check for input iterator errors
  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (s.ok() && meta->file_size > 0) {
    // Keep it
  } else {
    env->DeleteFile(fname);
  }
  return s;
}

}  // namespace leveldb
