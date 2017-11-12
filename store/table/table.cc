#include "store/table.hh"
#include "store/filter_policy.hh"
#include "store/options.hh"
#include "store/table/block.hh"
#include "store/table/filter_block.hh"
#include "store/table/format.hh"
#include "store/util/coding.hh"
#include "store/reader.hh"

namespace store {
static thread_local sstable_cache _table_cache;
static inline sstable_cache& get_table_cache() {
    return _table_cache;
}
static thread_local block_cache _block_cache;
static inline block_cache& get_block_cache() {
    return _block_cache;
}

future<temporary_buffer<char>> read_block(lw_shared_ptr<file_random_access_reader> r, block_handle index);
lw_shared_ptr<reader> make_block_reader(lw_shared_ptr<table> ptable, block_handle index);

struct sstable::rep {
  ~rep() {
  }

  rep(lw_shared_ptr<file_reader> freader,
      block_handle metaindex,
      lw_shared_ptr<block> index_block,
      sstable_options opt)
      : _file_reader (freader)
      , _filter (nullptr)
      , _fileter_data()
      , _file_name ()
      , _metaindex_handle ()
      , _index_block (index_block)
      , _options(opt)
  {
  }

  lw_shared_ptr<file_reader> _file_reader;
  lw_shared_ptr<filter_block_reader> _filter = nullptr;
  managed_bytes _fileter_data;
  bytes _file_name;
  block_handle _metaindex_handle;  // Handle to metaindex_block: saved from footer
  lw_shared_ptr<block> _index_block = nullptr;
  sstable_options _options;
  future<> read_meta(const footer& footer);
  future<> read_filter(const slice& filter_handle_value);
};

future<temporary_buffer<char>> read_block(lw_shared_ptr<file_random_access_reader> r, block_handle index) {
    r->seek(index.offset());
    return r->read_exactly(index.size()).then([r] (auto&& buffer) {
        return std::move(buffer);
    }).hanlde_exception([index] (auto e) {
        // log it and throw it.
        throw io_exception();
    });;
}

sstable::sstable(std::unique_ptr<sstable::rep> r) : rep_(std::move(r)) {}

future<lw_shared_ptr<sstable>> open_sstable(bytes fname, const sstable_options& opts) {
    return open_file_dma(fname, open_flags::ro).then([this, &opts] (file file_) mutable {
        // mock, we will get the table cache from the thread local instance?
        auto cached_table = get_table_cache().find(fname);
        if (cached_table) {
            return make_ready_future<table> (cached_table);
        }
        return file_.size().then([this, file_ = std::move(file_), &opts] (uint64_t size) {
            if (size < footer::kEncodedLength) {
                return make_exception_future<>(lw_shared_ptr<table>({}));
            }
            auto f = make_checked_file(opts._read_error_handler, file_);
            auto r = make_lw_shared<file_random_access_reader>(std::move(f), size, opts.sstable_buffer_size);
            reader->seek(size - Footer::kEncodedLength);
            return r->read_exactly(Footer::kEncodedLength).then([this, r, &opts] (auto buffer) mutable {
                if (buffer.size() != footer::kEncodedLength) {
                    return make_exception_future<>(lw_shared_ptr<table>({}));
                }
                // read footer
                footer footer_;
                if (!footer_.decode_from(buffer.get(), buffer.size())) {
                    return make_exception_future<>(lw_shared_ptr<table>({}));
                }
                // read index block, and do not parse the index block now.
                auto fut = read_block(r, footer_.index_handle());
                return fut.then([this, r = std::move(r), _footer = std::move(_footer), &opts] (auto&& data) {
                    auto block_handle_key = convert_to_handle_key(footer_.index_handle());
                    auto index_block = cache->find_and_create(std::move(block_handle_key), std::move(data));
                    auto rep_ = std::make_unique<table::rep>(r, footer_.metaindex_handle(), index_block, opts);
                    auto table_instance = make_lw_shared<table>(rep_);
                    // read meta block
                    return rep_->read_meta(footer_.metaindex_handle()).then([this, table_instance] {
                        get_table_cache().insert(table_instance);
                        return make_ready_future<lw_shared_ptr<sstable>> ({table_instance});
                    });
                });
             });
        }).handle_exception([this, fname] (std::exception_ptr) {
            throw redis::io_exception();
        });
    });
}

future<> table::read_meta(lw_shared_ptr<file_random_access_reader> reader, block_handle metaindex_handle) {
    if (rep_->options.filter_policy == nullptr) {
      // Do not need any metadata
      return make_ready_future<>();
    }

    return read_block(reader, metaindex_handle).then([this] (auto&& key, auto&& data) {
        auto meta = make_lw_shared<block>(std::move(key), std::move(data));
        auto meta_block_reader = make_block_reader(meta, bytes_comparator());
        bytes key { "filter." + rep_->_options.filter_policy->name() };
        meta_block_reader->seek(key);
        auto data = meta_block_reader->current().data();
        block_handle filter_handle;
        if (filter_handle.decode_from(data)) {
            return read_block(reader, filter_handle).then([this] (auto&& data) mutable {
                // we do not put filter block into cache.
                rep_->_filter = make_lw_shared<filter_block_reader> (rep_->_options._filter_policy, std::move(data));
                return make_ready_future<>();
            });
        }
        return make_ready_future<>();
    });
}

table::~table() {
}

class block_reader : public reader::impl {
    lw_shared_ptr<block> _block;
    sstable_options _optons;
    uint32_t _restarts_offset = 0;
    uint32_t _num_restarts = 0;
    uint32_t _restart_index = 0;
    uint32_t _current_offset = 0;
    partition _current;
    bytes_view _data_view;
    bytes_view _value_view;
    bool _eof = false;
public:
    block_reader(lw_shared_ptr<block> b, sstable_options opt)
        , _block(b)
        , _options(opt)
        , _data_view ({b->data(), b->size()})
        , _value_view ()
        , _restarts_offset (0)
        , _num_restarts (b->num_restarts())
        , _restart_index(0)
        , _current_offset(0)
        , _eof(false)
    {
    }
    future<> seek_to_first() {
        seek_to_restart_point(0);
        parse_next_key();
        return make_ready_future<>();
    }

    future<> seek_to_last() {
        seek_to_restart_point(num_restarts_ - 1);
        while (parse_next_key() && next_entry_offset() < restarts_) { }
        return make_ready_future<>();
    }

    future<> seek(bytes key) {
        // binary search the target restart point.
        uint32_t left = 0;
        uint32_t right = num_restarts_ - 1;
        while (left < right) {
            uint32_t mid = (left + right + 1) / 2;
            uint32_t region_offset = get_restart_point(mid);
            uint32_t shared, non_shared, value_length;
            const char* key_ptr = decode_entry(data_ + region_offset, data_ + restarts_, &shared, &non_shared, &value_length);
            if (key_ptr == nullptr || (shared != 0)) {
                _eof = true;
                return make_ready_future<>();
            }
            auto mid_key = bytes_view { key_ptr, non_shared };
            if (compare(mid_key, target) < 0) {
                left = mid;
            } else {
                right = mid - 1;
            }
        }
        seek_to_restart_point(left);
        // leaner search the target key
        while (true) {
            if (!parse_next_key()) {
                return make_ready_future<>();
            }
            if (compare(_curent_key(), key) >= 0) {
                return make_ready_future<>();
            }
        }
    }

    future<> next() {
        parse_next_key();
        return make_ready_future<>();
    }
    partition current() const {
        return _current;
    }
    bool eof() const {
        return _current_offset >= restarts_ || _eof;
    }
private:
    inline uint32_t next_entry_offset() const {
        return static_cast<uint32_t>((_value_view.data() + _value_view.size()) - _data_view.data());
    }

    uint32_t get_restart_point(uint32_t index) {
        assert (index < _num_restarts);
        return decode_fixed32(data_ + restarts_ + index * sizeof(uint32_t));
    }

    void seek_to_restart_point(uint32_t index) {
        _current.key().clear();
        restart_index_ = index;
        uint32_t offset = get_restart_point(index);
        _value_view = bytes_view { _data_view.data() + offset, 0 };
    }

    bool parse_next_key() {
        _current_offset = next_entry_offset();
        const char* p = _data_view.data() + _current_offset;
        const char* limit = data_ + restarts_offset;
        if (p >= limit) {
            current_ = restarts_offset;
            restart_index_ = num_restarts_;
            return false;
        }

        uint32_t shared, non_shared, value_length;
        p = decode_entry(p, limit, &shared, &non_shared, &value_length);
        if (p == nullptr || key_.size() < shared) {
            return false;
        } else {
            _current.key().resize(shared);
            _current.key().append(p, non_shared);
            _value_view = { p + non_shared, value_length };
            while (restart_index_ + 1 < num_restarts_ && get_restart_point(restart_index_ + 1) < _current_offset) {
                ++restart_index_;
            }
            return true;
        }
    }
};

lw_shared_ptr<reader> make_block_reader(lw_shared_ptr<block> b, sstables_options opt) {
    return make_lw_shared<reader>(std::make_unique<block_reader>(b, std::move(opt)));
}

class sstable_reader : public reader::impl {
   lw_shared_ptr<table> _table;
   lw_shared_ptr<block_reader> _index_block_reader;
   lw_shared_ptr<block_reader> _data_block_reader;
   lw_shared_ptr<file_random_access_reader> _reader;
   bool _initialized = false;
public:
   sstable_reader(lw_shared_ptr<table> ptable) : _table(ptable) {}
   future<> seek_to_first() {
       return _index_block_reader->seek_to_first().then([this] {
            _initialized = true;
            block_handle data_handle;
            data_handle.decode_from(_index_block_reader->current().data());
            auto handle_key = convert_to_handle_key(data_handle);
            auto b  = _table->cache()->find(handle_key);
            if (b != nullptr) {
                _data_block_reader = make_block_reader(b, _table->options());
                return _data_block_reader.seek_to_first();
            }
            return read_block(_reader, data_handle).then([this, key = std::move(handle_key)] (auto&& data) {
                // cache the block. the block in the cache will be evicted by LRU or the file was deleted.
                auto b = _table->cache()->create(std::move(key), std::move(data));
                 _data_block_reader = make_block_reader(b, _table->options());
                return _data_block_reader->seek_to_first();
            });
       });
   }
   future<> seek_to_last() {
       return _index_block_reader->seek_to_last().then([this] {
            _initialized = true;
            block_handle data_handle;
            data_handle.decode_from(_index_block_reader->current().data());
            auto handle_key = convert_to_handle_key(data_handle);
            auto b  = _table->cache()->find(handle_key);
            if (b != nullptr) {
                _data_block_reader = make_block_reader(b, _table->options());
                return _data_block_reader.seek_to_last();
            }
            return read_block(_reader, data_handle).then([this, key = std::move(handle_key)] (auto&& data) {
                auto b = _table->cache()->create(std::move(key), std::move(data));
                 _data_block_reader = make_block_reader(b, _table->options());
                return _data_block_reader->seek_to_last();
            });
       });
   }
   future<> seek(bytes_view key) {
       return _index_block_reader->seek(key).then([this, key] {
            _initialized = true;
            block_handle data_handle;
            data_handle.decode_from(_index_block_reader->current().data());
            auto handle_key = convert_to_handle_key(data_handle);
            auto b  = _table->cache()->find(handle_key);
            if (b != nullptr) {
                _data_block_reader = make_block_reader(b, _table->options());
                return _data_block_reader.seek(key);
            }
            return read_block(_reader, data_handle).then([this, key = std::move(handle_key)] (auto&& data) {
                auto b = _table->cache()->create(std::move(key), std::move(data));
                _data_block_reader = make_block_reader(b, _table->options());
                return _data_block_reader->seek(ikey);
            });
       });
   }
   future<> next() {
       assert(_initialized);
       return _data_block_reader->next().then([this] mutable {
           if (_data_block_reader->current().empty()) {
               return _index_block_reader->next().then([this] mutable {
                   block_handle data_handle;
                   data_handle.decode_from(_index_block_reader->current().data());
                   auto handle_key = convert_to_handle_key(data_handle);
                   auto b  = _table->cache()->find(handle_key);
                   if (b != nullptr) {
                       _data_block_reader = make_block_reader(b, _table->options());
                       return _data_block_reader->seek_to_first();
                   }
                   return read_block(_reader, data_handle).then([this, key = std::move(handle_key)] (auto&& data) {
                       auto b = _table->cache()->create(std::move(key), std::move(data));
                       _data_block_reader = make_block_reader(b, _table->options());
                       return _data_block_reader->seek_to_first();
                   });
               });
           }
           return make_ready_future<>();
       });
   }
   partition current() {
       return _data_block_reader->current();
   }
   bool eof() {
       return _index_block_reader->eof() && _data_block_reader->eof();
   }
};

lw_shared_ptr<reader> make_sstable_reader(lw_shared_ptr<sstable> table) {
    return make_lw_shared<reader>(std::make_unique<sstable_reader>(table));
}

class combined_sstables_reader : public reader::impl {
    std::vector<lw_shared_ptr<table>> _sstables;
    std::vector<lw_shared_ptr<reader> _readers;
    lw_shared_ptr<reader> _current = nullptr;
    bool _eof = false;

public:
    combined_sstables_reader(std::vector<lw_shared_ptr<table>> sstables)
        : _sstables(std::move(sstables))
        , _readers(sstables.size())
        , _current(nullptr)
        , _eof(false)
    {
        for (size_t i = 0; i < sstables.size(); ++i) {
            _readers[i] = make_sstable_reader(sstables[i]);
        }
    }

    ~combined_sstables_reader()
    {
    }

    future<> seek_to_first() {
        return parallel_for_each(_sstables, [this] (auto sstable) mutable {
            sstable->seek_to_first();
        }).then(this] mutable {
            return find_smallest_one();
        });
    }

    future<> seek_to_last() {
        return parallel_for_each(_sstables, [this] (auto sstable) mutable {
            sstable->seek_to_last();
        }).then(this] mutable {
            return find_smallest_one();
        });
    }

    future<> seek(bytes_view key) {
        return parallel_for_each(_sstables, [this, key] (auto sstable) mutable {
            sstable->seek(key);
        }).then(this] mutable {
            return find_smallest_one();
        });
    }

    future<> next() {
        return parallel_for_each(_sstables, [this] (auto sstable) mutable {
            if (sstable == _current) {
                sstable->next();
            }
        }).then([this] mutable {
            return find_smallest_one();
        });
    }

    bool eof() const {
        if (_eof) {
            return _eof;
        }
        bool all_eof = false;
        for (auto& sstable : _sstables) {
            all_eof &= sstable->eof();
        }
        return all_eof;
    }

    partition current() const {
        return _current->current();
    }
private:
    future<> find_smallest_one() {
       auto less_comparator = [] (const lw_shared_ptr<table> a, const lw_shared_ptr<table> b) {
           return a->current().key() < b->current().key();
       };
       lw_shared_ptr<table> smallest = _sstables[0];
       for (size_t i = 1; i < _sstables.size(); ++i) {
           if (less_comparator(_sstables[i], smallest)) {
               smallest = _sstables[i];
           }
       }
       _current = smallest;
       if (_current == nullptr) {
           _eof = true;
       }
       return make_ready_future<>();
    }
};

lw_shared_ptr<reader> make_combined_sstables_reader(std::vector<lw_shared_ptr<table>> sstables) {
    return make_lw_shared<reader>(std::make_unique<combined_sstables_reader>(std::move(stables)));
}

}
