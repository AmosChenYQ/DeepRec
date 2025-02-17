/* Copyright 2022 The DeepRec Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
======================================================================*/
#ifndef TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_DRAM_LEVELDB_STORAGE_H_
#define TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_DRAM_LEVELDB_STORAGE_H_

#include "tensorflow/core/framework/embedding/leveldb_kv.h"
#include "tensorflow/core/framework/embedding/cpu_hash_map_kv.h"
#include "tensorflow/core/framework/embedding/multi_tier_storage.h"
#include "tensorflow/core/framework/embedding/single_tier_storage.h"

namespace tensorflow {
template <class V>
class ValuePtr;

template <class K, class V>
class EmbeddingVar;

namespace embedding {
template<typename K, typename V>
class DramLevelDBStore : public MultiTierStorage<K, V> {
 public:
  DramLevelDBStore(const StorageConfig& sc, Allocator* alloc,
      LayoutCreator<V>* lc, const std::string& name)
      : MultiTierStorage<K, V>(sc, name) {
    dram_ = new DramStorage<K, V>(sc, alloc, lc, new LocklessHashMap<K, V>());
    leveldb_ = new LevelDBStore<K, V>(sc, alloc, lc);
  }

  ~DramLevelDBStore() override {
    MultiTierStorage<K, V>::DeleteFromEvictionManager();
    delete dram_;
    delete leveldb_;
  }

  TF_DISALLOW_COPY_AND_ASSIGN(DramLevelDBStore);

  Status Get(K key, ValuePtr<V>** value_ptr) override {
    Status s = dram_->Get(key, value_ptr);
    if (!s.ok()) {
      s = leveldb_->Get(key, value_ptr);
    }
    return s;
  }

  void Insert(K key, ValuePtr<V>* value_ptr) override {
    LOG(FATAL)<<"Unsupport Insert(K, ValuePtr<V>*) in DramLevelDBStore.";
  }

  void Insert(K key, ValuePtr<V>** value_ptr,
              size_t alloc_len) override {
    dram_->Insert(key, value_ptr, alloc_len);
  }

  Status GetOrCreate(K key, ValuePtr<V>** value_ptr,
      size_t size, CopyBackFlag &need_copyback) override {
    LOG(FATAL)<<"GetOrCreate(K key, ValuePtr<V>** value_ptr, "
              <<"size_t size, CopyBackFlag &need_copyback) "
              <<"in DramLevelDBStore can not be called.";
  }

  Status GetOrCreate(K key, ValuePtr<V>** value_ptr,
      size_t size) override {
    Status s = dram_->Get(key, value_ptr);
    if (s.ok()) {
      return s;
    }
    s = leveldb_->Get(key, value_ptr);
    if (s.ok()) {
      s = dram_->TryInsert(key, *value_ptr);
      if (s.ok()) {
        return s;
      }
      leveldb_->DestroyValuePtr(*value_ptr);
      return dram_->Get(key, value_ptr);
    }
    dram_->Insert(key, value_ptr, size);
    return Status::OK();
  }
 
  Status Remove(K key) override {
    dram_->Remove(key);
    leveldb_->Remove(key);
    return Status::OK();
  }

  bool IsUseHbm() override {
    return false;
  }

  bool IsSingleHbm() override {
    return false;
  }

  bool IsUsePersistentStorage() override {
    /*The return value is set to false temporarily,
      because the corresponding interface is not implemented.*/
    return false;
  }

  void iterator_mutex_lock() override {
    leveldb_->get_mutex()->lock();
  }

  void iterator_mutex_unlock() override {
    leveldb_->get_mutex()->unlock();
  }

  int64 Size() const override {
    int64 total_size = dram_->Size();
    total_size += leveldb_->Size();
    return total_size;
  }

  int64 Size(int level) const override {
    if (level == 0) {
      return dram_->Size();
    } else if (level == 1) {
      return leveldb_->Size();
    } else {
      return -1;
    }
  }

  int LookupTier(K key) const override {
    Status s = dram_->Contains(key);
    if (s.ok())
      return 0;
    s = leveldb_->Contains(key);
    if (s.ok())
      return 1;
    return -1;
  }

  Status GetSnapshot(std::vector<K>* key_list,
      std::vector<ValuePtr<V>*>* value_ptr_list) override {
    {
      mutex_lock l(*(dram_->get_mutex()));
      TF_CHECK_OK(dram_->GetSnapshot(key_list, value_ptr_list));
    }
    {
      mutex_lock l(*(leveldb_->get_mutex()));
      TF_CHECK_OK(leveldb_->GetSnapshot(key_list, value_ptr_list));
    }
    return Status::OK();
  }

  Status Shrink(const ShrinkArgs& shrink_args) override {
    dram_->Shrink(shrink_args);
    leveldb_->Shrink(shrink_args);
    return Status::OK();
  }

  int64 GetSnapshot(std::vector<K>* key_list,
      std::vector<V* >* value_list,
      std::vector<int64>* version_list,
      std::vector<int64>* freq_list,
      const EmbeddingConfig& emb_config,
      FilterPolicy<K, V, EmbeddingVar<K, V>>* filter,
      embedding::Iterator** it) override {
    {
      mutex_lock l(*(dram_->get_mutex()));
      std::vector<ValuePtr<V>*> value_ptr_list;
      std::vector<K> key_list_tmp;
      TF_CHECK_OK(dram_->GetSnapshot(&key_list_tmp, &value_ptr_list));
      MultiTierStorage<K, V>::SetListsForCheckpoint(
          key_list_tmp, value_ptr_list, emb_config,
          key_list, value_list, version_list, freq_list);
    }
    {
      mutex_lock l(*(leveldb_->get_mutex()));
      *it = leveldb_->GetIterator();
    }
    return key_list->size();
  }

  Status Eviction(K* evict_ids, int64 evict_size) override {
    ValuePtr<V>* value_ptr;
    for (int64 i = 0; i < evict_size; ++i) {
      if (dram_->Get(evict_ids[i], &value_ptr).ok()) {
        TF_CHECK_OK(leveldb_->Commit(evict_ids[i], value_ptr));
        TF_CHECK_OK(dram_->Remove(evict_ids[i]));
        dram_->DestroyValuePtr(value_ptr);
      }
    }
    return Status::OK();
  }

  Status EvictionWithDelayedDestroy(K* evict_ids, int64 evict_size) override {
    mutex_lock l(*(dram_->get_mutex()));
    mutex_lock l1(*(leveldb_->get_mutex()));
    MultiTierStorage<K, V>::ReleaseInvalidValuePtr(dram_->alloc_);
    ValuePtr<V>* value_ptr = nullptr;
    for (int64 i = 0; i < evict_size; ++i) {
      if (dram_->Get(evict_ids[i], &value_ptr).ok()) {
        TF_CHECK_OK(leveldb_->Commit(evict_ids[i], value_ptr));
        TF_CHECK_OK(dram_->Remove(evict_ids[i]));
        MultiTierStorage<K, V>::KeepInvalidValuePtr(value_ptr);
      }
    }
    return Status::OK();
  }

 protected:
  void SetTotalDims(int64 total_dims) override {
    leveldb_->SetTotalDims(total_dims);
  }

 private:
  DramStorage<K, V>* dram_;
  LevelDBStore<K, V>* leveldb_;
};
} // embedding
} // tensorflow

#endif // TENSORFLOW_CORE_FRAMEWORK_EMBEDDING_DRAM_LEVELDB_STORAGE_H_
