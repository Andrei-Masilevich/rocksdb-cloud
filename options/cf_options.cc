//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "options/cf_options.h"

#include <cassert>
#include <cinttypes>
#include <limits>
#include <string>

#include "options/db_options.h"
#include "options/options_helper.h"
#include "options/options_parser.h"
#include "port/port.h"
#include "rocksdb/cache.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/comparator.h"
#include "rocksdb/concurrent_task_limiter.h"
#include "rocksdb/configurable.h"
#include "rocksdb/convenience.h"
#include "rocksdb/env.h"
#include "rocksdb/file_system.h"
#include "rocksdb/merge_operator.h"
#include "rocksdb/options.h"
#include "rocksdb/table.h"
#include "rocksdb/utilities/object_registry.h"
#include "util/cast_util.h"

namespace rocksdb {
// offset_of is used to get the offset of a class data member
// ex: offset_of(&ColumnFamilyOptions::num_levels)
// This call will return the offset of num_levels in ColumnFamilyOptions class
//
// This is the same as offsetof() but allow us to work with non standard-layout
// classes and structures
// refs:
// http://en.cppreference.com/w/cpp/concept/StandardLayoutType
// https://gist.github.com/graphitemaster/494f21190bb2c63c5516
#ifndef ROCKSDB_LITE
static Status ParseCompressionOptions(const std::string& value,
                                      const std::string& name,
                                      CompressionOptions& compression_opts) {
  size_t start = 0;
  size_t end = value.find(':');
  if (end == std::string::npos) {
    return Status::InvalidArgument("unable to parse the specified CF option " +
                                   name);
  }
  compression_opts.window_bits = ParseInt(value.substr(start, end - start));
  start = end + 1;
  end = value.find(':', start);
  if (end == std::string::npos) {
    return Status::InvalidArgument("unable to parse the specified CF option " +
                                   name);
  }
  compression_opts.level = ParseInt(value.substr(start, end - start));
  start = end + 1;
  if (start >= value.size()) {
    return Status::InvalidArgument("unable to parse the specified CF option " +
                                   name);
  }
  end = value.find(':', start);
  compression_opts.strategy =
      ParseInt(value.substr(start, value.size() - start));
  // max_dict_bytes is optional for backwards compatibility
  if (end != std::string::npos) {
    start = end + 1;
    if (start >= value.size()) {
      return Status::InvalidArgument(
          "unable to parse the specified CF option " + name);
    }
    compression_opts.max_dict_bytes =
        ParseInt(value.substr(start, value.size() - start));
    end = value.find(':', start);
  }
  // zstd_max_train_bytes is optional for backwards compatibility
  if (end != std::string::npos) {
    start = end + 1;
    if (start >= value.size()) {
      return Status::InvalidArgument(
          "unable to parse the specified CF option " + name);
    }
    compression_opts.zstd_max_train_bytes =
        ParseInt(value.substr(start, value.size() - start));
    end = value.find(':', start);
  }
  // enabled is optional for backwards compatibility
  if (end != std::string::npos) {
    start = end + 1;
    if (start >= value.size()) {
      return Status::InvalidArgument(
          "unable to parse the specified CF option " + name);
    }
    compression_opts.enabled =
        ParseBoolean("", value.substr(start, value.size() - start));
  }
  return Status::OK();
}

static ColumnFamilyOptions dummy_cf_options;
template <typename T1>
int offset_of(T1 ColumnFamilyOptions::*member) {
  return int(size_t(&(dummy_cf_options.*member)) - size_t(&dummy_cf_options));
}
template <typename T1>
int offset_of(T1 AdvancedColumnFamilyOptions::*member) {
  return int(size_t(&(dummy_cf_options.*member)) - size_t(&dummy_cf_options));
}

static const std::string kNameComparator = "comparator";
static const std::string kNameEnv = "env";
static const std::string kNameMergeOperator = "merge_operator";

static std::unordered_map<std::string, OptionTypeInfo> cf_options_type_info = {
    /* not yet supported
    CompressionOptions compression_opts;
    TablePropertiesCollectorFactories table_properties_collector_factories;
    typedef std::vector<std::shared_ptr<TablePropertiesCollectorFactory>>
        TablePropertiesCollectorFactories;
    UpdateStatus (*inplace_callback)(char* existing_value,
                                     uint34_t* existing_value_size,
                                     Slice delta_value,
                                     std::string* merged_value);
    std::shared_ptr<ConcurrentTaskLimiter> compaction_thread_limiter;
    std::vector<DbPath> cf_paths;
     */
    {"report_bg_io_stats",
     {offset_of(&ColumnFamilyOptions::report_bg_io_stats), OptionType::kBoolean,
      OptionVerificationType::kNormal, OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, report_bg_io_stats)}},
    {"compaction_measure_io_stats",
     {0, OptionType::kBoolean, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kNone, 0}},
    {"disable_auto_compactions",
     {offset_of(&ColumnFamilyOptions::disable_auto_compactions),
      OptionType::kBoolean, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, disable_auto_compactions)}},
    {"filter_deletes",
     {0, OptionType::kBoolean, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"inplace_update_support",
     {offset_of(&ColumnFamilyOptions::inplace_update_support),
      OptionType::kBoolean, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone, 0}},
    {"level_compaction_dynamic_level_bytes",
     {offset_of(&ColumnFamilyOptions::level_compaction_dynamic_level_bytes),
      OptionType::kBoolean, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone, 0}},
    {"optimize_filters_for_hits",
     {offset_of(&ColumnFamilyOptions::optimize_filters_for_hits),
      OptionType::kBoolean, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone, 0}},
    {"paranoid_file_checks",
     {offset_of(&ColumnFamilyOptions::paranoid_file_checks),
      OptionType::kBoolean, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, paranoid_file_checks)}},
    {"force_consistency_checks",
     {offset_of(&ColumnFamilyOptions::force_consistency_checks),
      OptionType::kBoolean, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone, 0}},
    {"purge_redundant_kvs_while_flush",
     {offset_of(&ColumnFamilyOptions::purge_redundant_kvs_while_flush),
      OptionType::kBoolean, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kNone, 0}},
    {"verify_checksums_in_compaction",
     {0, OptionType::kBoolean, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"soft_pending_compaction_bytes_limit",
     {offset_of(&ColumnFamilyOptions::soft_pending_compaction_bytes_limit),
      OptionType::kUInt64T, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, soft_pending_compaction_bytes_limit)}},
    {"hard_pending_compaction_bytes_limit",
     {offset_of(&ColumnFamilyOptions::hard_pending_compaction_bytes_limit),
      OptionType::kUInt64T, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, hard_pending_compaction_bytes_limit)}},
    {"hard_rate_limit",
     {0, OptionType::kDouble, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"soft_rate_limit",
     {0, OptionType::kDouble, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"max_compaction_bytes",
     {offset_of(&ColumnFamilyOptions::max_compaction_bytes),
      OptionType::kUInt64T, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, max_compaction_bytes)}},
    {"expanded_compaction_factor",
     {0, OptionType::kInt, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"level0_file_num_compaction_trigger",
     {offset_of(&ColumnFamilyOptions::level0_file_num_compaction_trigger),
      OptionType::kInt, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, level0_file_num_compaction_trigger)}},
    {"level0_slowdown_writes_trigger",
     {offset_of(&ColumnFamilyOptions::level0_slowdown_writes_trigger),
      OptionType::kInt, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, level0_slowdown_writes_trigger)}},
    {"level0_stop_writes_trigger",
     {offset_of(&ColumnFamilyOptions::level0_stop_writes_trigger),
      OptionType::kInt, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, level0_stop_writes_trigger)}},
    {"max_grandparent_overlap_factor",
     {0, OptionType::kInt, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"max_mem_compaction_level",
     {0, OptionType::kInt, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kNone, 0}},
    {"max_write_buffer_number",
     {offset_of(&ColumnFamilyOptions::max_write_buffer_number),
      OptionType::kInt, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, max_write_buffer_number)}},
    {"max_write_buffer_number_to_maintain",
     {offset_of(&ColumnFamilyOptions::max_write_buffer_number_to_maintain),
      OptionType::kInt, OptionVerificationType::kNormal, OptionTypeFlags::kNone,
      0}},
    {"max_write_buffer_size_to_maintain",
     {offset_of(&ColumnFamilyOptions::max_write_buffer_size_to_maintain),
      OptionType::kInt64T, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone, 0}},
    {"min_write_buffer_number_to_merge",
     {offset_of(&ColumnFamilyOptions::min_write_buffer_number_to_merge),
      OptionType::kInt, OptionVerificationType::kNormal, OptionTypeFlags::kNone,
      0}},
    {"num_levels",
     {offset_of(&ColumnFamilyOptions::num_levels), OptionType::kInt,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone, 0}},
    {"source_compaction_factor",
     {0, OptionType::kInt, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"target_file_size_multiplier",
     {offset_of(&ColumnFamilyOptions::target_file_size_multiplier),
      OptionType::kInt, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, target_file_size_multiplier)}},
    {"arena_block_size",
     {offset_of(&ColumnFamilyOptions::arena_block_size), OptionType::kSizeT,
      OptionVerificationType::kNormal, OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, arena_block_size)}},
    {"inplace_update_num_locks",
     {offset_of(&ColumnFamilyOptions::inplace_update_num_locks),
      OptionType::kSizeT, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, inplace_update_num_locks)}},
    {"max_successive_merges",
     {offset_of(&ColumnFamilyOptions::max_successive_merges),
      OptionType::kSizeT, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, max_successive_merges)}},
    {"memtable_huge_page_size",
     {offset_of(&ColumnFamilyOptions::memtable_huge_page_size),
      OptionType::kSizeT, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, memtable_huge_page_size)}},
    {"memtable_prefix_bloom_huge_page_tlb_size",
     {0, OptionType::kSizeT, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"write_buffer_size",
     {offset_of(&ColumnFamilyOptions::write_buffer_size), OptionType::kSizeT,
      OptionVerificationType::kNormal, OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, write_buffer_size)}},
    {"bloom_locality",
     {offset_of(&ColumnFamilyOptions::bloom_locality), OptionType::kUInt32T,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone, 0}},
    {"memtable_prefix_bloom_bits",
     {0, OptionType::kUInt32T, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"memtable_prefix_bloom_size_ratio",
     {offset_of(&ColumnFamilyOptions::memtable_prefix_bloom_size_ratio),
      OptionType::kDouble, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, memtable_prefix_bloom_size_ratio)}},
    {"memtable_prefix_bloom_probes",
     {0, OptionType::kUInt32T, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"memtable_whole_key_filtering",
     {offset_of(&ColumnFamilyOptions::memtable_whole_key_filtering),
      OptionType::kBoolean, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, memtable_whole_key_filtering)}},
    {"min_partial_merge_operands",
     {0, OptionType::kUInt32T, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"max_bytes_for_level_base",
     {offset_of(&ColumnFamilyOptions::max_bytes_for_level_base),
      OptionType::kUInt64T, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, max_bytes_for_level_base)}},
    {"snap_refresh_nanos",
     {0, OptionType::kUInt64T, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kMutable, 0}},
    {"max_bytes_for_level_multiplier",
     {offset_of(&ColumnFamilyOptions::max_bytes_for_level_multiplier),
      OptionType::kDouble, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, max_bytes_for_level_multiplier)}},
    {"max_bytes_for_level_multiplier_additional",
     {offset_of(
          &ColumnFamilyOptions::max_bytes_for_level_multiplier_additional),
      OptionType::kVectorInt, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions,
               max_bytes_for_level_multiplier_additional)}},
    {"max_sequential_skip_in_iterations",
     {offset_of(&ColumnFamilyOptions::max_sequential_skip_in_iterations),
      OptionType::kUInt64T, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, max_sequential_skip_in_iterations)}},
    {"target_file_size_base",
     {offset_of(&ColumnFamilyOptions::target_file_size_base),
      OptionType::kUInt64T, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, target_file_size_base)}},
    {"rate_limit_delay_max_milliseconds",
     {0, OptionType::kUInt, OptionVerificationType::kDeprecated,
      OptionTypeFlags::kNone, 0}},
    {"compression",
     {offset_of(&ColumnFamilyOptions::compression),
      OptionType::kCompressionType, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, compression)}},
    {"compression_opts",
     {offset_of(&ColumnFamilyOptions::compression_opts), OptionType::kUnknown,
      OptionVerificationType::kNormal, OptionTypeFlags::kNone, 0,
      [](const std::string& name, const std::string& value,
         const ConfigOptions&, char* addr) {
        Status s;
        if (!value.empty()) {
          auto* opts = reinterpret_cast<CompressionOptions*>(addr);
          s = ParseCompressionOptions(value, name, *(opts));
        }
        return s;
      },
      [](const std::string&, const char*, const ConfigOptions&,
         std::string* value) {
        *value = "";  // Currently not checked
        return Status::OK();
      },
      [](const std::string&, const char*, const char*, const ConfigOptions&,
         std::string*) {
        return true;  // Not compared.
      }}},
    {"compression_per_level",
     {offset_of(&ColumnFamilyOptions::compression_per_level),
      OptionType::kVectorCompressionType, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone, 0}},
    {"bottommost_compression",
     {offset_of(&ColumnFamilyOptions::bottommost_compression),
      OptionType::kCompressionType, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone, 0}},
    {"bottommost_compression_opts",
     {offset_of(&ColumnFamilyOptions::bottommost_compression_opts),
      OptionType::kUnknown, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone, 0,
      [](const std::string& name, const std::string& value,
         const ConfigOptions&, char* addr) {
        Status s;
        if (!value.empty()) {
          s = ParseCompressionOptions(
              value, name, *(reinterpret_cast<CompressionOptions*>(addr)));
        }
        return s;
      },
      [](const std::string&, const char*, const ConfigOptions&,
         std::string* value) {
        *value = "";  // Currently not checked
        return Status::OK();
      },
      [](const std::string&, const char*, const char*, const ConfigOptions&,
         std::string*) {
        return true;  // Not compared.
      }}},
    {kNameComparator,
     OptionTypeInfo::AsCustomP<const Comparator>(
         offset_of(&ColumnFamilyOptions::comparator),
         OptionVerificationType::kByName,
         [](const std::string&, const char* addr, const ConfigOptions&,
            std::string* value) {
           // it's a const pointer of const Comparator*
           const auto* ptr = reinterpret_cast<const Comparator* const*>(addr);
           // Since the user-specified comparator will be wrapped by
           // InternalKeyComparator, we should persist the user-specified one
           // instead of InternalKeyComparator.
           if (*ptr == nullptr) {
             *value = kNullptrString;
           } else {
             const Comparator* root_comp = (*ptr)->GetRootComparator();
             if (root_comp == nullptr) {
               root_comp = (*ptr);
             }
             *value = root_comp->Name();
           }
           return Status::OK();
         },
         nullptr)},
    {"prefix_extractor",
     {offset_of(&ColumnFamilyOptions::prefix_extractor),
      OptionType::kSliceTransform, OptionVerificationType::kByNameAllowNull,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, prefix_extractor)}},
    {"memtable_insert_with_hint_prefix_extractor",
     {offset_of(
          &ColumnFamilyOptions::memtable_insert_with_hint_prefix_extractor),
      OptionType::kSliceTransform, OptionVerificationType::kByNameAllowNull,
      OptionTypeFlags::kNone, 0}},
    {"memtable_factory", OptionTypeInfo::AsCustomS<MemTableRepFactory>(
                             offset_of(&ColumnFamilyOptions::memtable_factory),
                             OptionVerificationType::kByName)},
    {"memtable",
     {offset_of(&ColumnFamilyOptions::memtable_factory),
      OptionType::kCustomizable, OptionVerificationType::kAlias,
      OptionTypeFlags::kCustomizableS, 0,
      [](const std::string&, const std::string& value,
         const ConfigOptions& opts, char* addr) {
        auto* factory =
            reinterpret_cast<std::shared_ptr<MemTableRepFactory>*>(addr);
        return MemTableRepFactory::CreateFromString(value, opts, factory);
      }}},
    {"table_factory", OptionTypeInfo::AsCustomS<TableFactory>(
                          offset_of(&ColumnFamilyOptions::table_factory),
                          OptionVerificationType::kByName)},
    {"block_based_table_factory",
     {offset_of(&ColumnFamilyOptions::table_factory), OptionType::kUnknown,
      OptionVerificationType::kAlias, OptionTypeFlags::kNone, 0,
      [](const std::string&, const std::string& value,
         const ConfigOptions& opts, char* addr) {
        auto* tf = reinterpret_cast<std::shared_ptr<TableFactory>*>(addr);
        Status s = TableFactory::CreateFromString(
            TableFactory::kBlockBasedTableName, opts, tf);
        if (s.ok()) {
          s = tf->get()->ConfigureFromString(value, opts);
        }
        return s;
      }}},
    {"plain_table_factory",
     {offset_of(&ColumnFamilyOptions::table_factory), OptionType::kUnknown,
      OptionVerificationType::kAlias, OptionTypeFlags::kNone, 0,
      [](const std::string&, const std::string& value,
         const ConfigOptions& opts, char* addr) {
        auto tf = reinterpret_cast<std::shared_ptr<TableFactory>*>(addr);
        Status s = TableFactory::CreateFromString(TableFactory::kPlainTableName,
                                                  opts, tf);
        if (s.ok()) {
          s = tf->get()->ConfigureFromString(value, opts);
        }
        return s;
      }}},
    {"compaction_filter",
     OptionTypeInfo::AsCustomP<const CompactionFilter>(
         offset_of(&ColumnFamilyOptions::compaction_filter),
         OptionVerificationType::kByName)},
    {"compaction_filter_factory",
     OptionTypeInfo::AsCustomS<CompactionFilterFactory>(
         offset_of(&ColumnFamilyOptions::compaction_filter_factory),
         OptionVerificationType::kByName)},
    {kNameMergeOperator, OptionTypeInfo::AsCustomS<MergeOperator>(
                             offset_of(&ColumnFamilyOptions::merge_operator),
                             OptionVerificationType::kByNameAllowFromNull)},
    {"compaction_style",
     {offset_of(&ColumnFamilyOptions::compaction_style),
      OptionType::kCompactionStyle, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone, 0}},
    {"compaction_pri",
     {offset_of(&ColumnFamilyOptions::compaction_pri),
      OptionType::kCompactionPri, OptionVerificationType::kNormal,
      OptionTypeFlags::kNone, 0}},
    {"compaction_options_fifo",
     {offset_of(&ColumnFamilyOptions::compaction_options_fifo),
      OptionType::kStruct, OptionVerificationType::kNormal,
      OptionTypeFlags::kMStruct,
      offsetof(struct MutableCFOptions, compaction_options_fifo)}},
    {"compaction_options_universal",
     {offset_of(&ColumnFamilyOptions::compaction_options_universal),
      OptionType::kStruct, OptionVerificationType::kNormal,
      OptionTypeFlags::kMStruct,
      offsetof(struct MutableCFOptions, compaction_options_universal)}},
    {"ttl",
     {offset_of(&ColumnFamilyOptions::ttl), OptionType::kUInt64T,
      OptionVerificationType::kNormal, OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, ttl)}},
    {"periodic_compaction_seconds",
     {offset_of(&ColumnFamilyOptions::periodic_compaction_seconds),
      OptionType::kUInt64T, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, periodic_compaction_seconds)}},
    {"sample_for_compression",
     {offset_of(&ColumnFamilyOptions::sample_for_compression),
      OptionType::kUInt64T, OptionVerificationType::kNormal,
      OptionTypeFlags::kMutable,
      offsetof(struct MutableCFOptions, sample_for_compression)}},
};

ImmutableCFOptions::ImmutableCFOptions(const Options& options)
    : ImmutableCFOptions(ImmutableDBOptions(options), options) {}

ImmutableCFOptions::ImmutableCFOptions(const ImmutableDBOptions& db_options,
                                       const ColumnFamilyOptions& cf_options)
    : compaction_style(cf_options.compaction_style),
      compaction_pri(cf_options.compaction_pri),
      user_comparator(cf_options.comparator),
      internal_comparator(InternalKeyComparator(cf_options.comparator)),
      merge_operator(cf_options.merge_operator.get()),
      compaction_filter(cf_options.compaction_filter),
      compaction_filter_factory(cf_options.compaction_filter_factory.get()),
      min_write_buffer_number_to_merge(
          cf_options.min_write_buffer_number_to_merge),
      max_write_buffer_number_to_maintain(
          cf_options.max_write_buffer_number_to_maintain),
      max_write_buffer_size_to_maintain(
          cf_options.max_write_buffer_size_to_maintain),
      inplace_update_support(cf_options.inplace_update_support),
      inplace_callback(cf_options.inplace_callback),
      info_log(db_options.info_log.get()),
      statistics(db_options.statistics.get()),
      rate_limiter(db_options.rate_limiter.get()),
      info_log_level(db_options.info_log_level),
      env(db_options.env),
      fs(db_options.fs.get()),
      allow_mmap_reads(db_options.allow_mmap_reads),
      allow_mmap_writes(db_options.allow_mmap_writes),
      db_paths(db_options.db_paths),
      memtable_factory(cf_options.memtable_factory.get()),
      table_factory(cf_options.table_factory.get()),
      table_properties_collector_factories(
          cf_options.table_properties_collector_factories),
      advise_random_on_open(db_options.advise_random_on_open),
      bloom_locality(cf_options.bloom_locality),
      purge_redundant_kvs_while_flush(
          cf_options.purge_redundant_kvs_while_flush),
      use_fsync(db_options.use_fsync),
      compression_per_level(cf_options.compression_per_level),
      bottommost_compression(cf_options.bottommost_compression),
      bottommost_compression_opts(cf_options.bottommost_compression_opts),
      compression_opts(cf_options.compression_opts),
      level_compaction_dynamic_level_bytes(
          cf_options.level_compaction_dynamic_level_bytes),
      access_hint_on_compaction_start(
          db_options.access_hint_on_compaction_start),
      new_table_reader_for_compaction_inputs(
          db_options.new_table_reader_for_compaction_inputs),
      num_levels(cf_options.num_levels),
      optimize_filters_for_hits(cf_options.optimize_filters_for_hits),
      force_consistency_checks(cf_options.force_consistency_checks),
      allow_ingest_behind(db_options.allow_ingest_behind),
      preserve_deletes(db_options.preserve_deletes),
      listeners(db_options.listeners),
      row_cache(db_options.row_cache),
      max_subcompactions(db_options.max_subcompactions),
      memtable_insert_with_hint_prefix_extractor(
          cf_options.memtable_insert_with_hint_prefix_extractor.get()),
      cf_paths(cf_options.cf_paths),
      compaction_thread_limiter(cf_options.compaction_thread_limiter) {}

// Multiple two operands. If they overflow, return op1.
uint64_t MultiplyCheckOverflow(uint64_t op1, double op2) {
  if (op1 == 0 || op2 <= 0) {
    return 0;
  }
  if (port::kMaxUint64 / op1 < op2) {
    return op1;
  }
  return static_cast<uint64_t>(op1 * op2);
}

// when level_compaction_dynamic_level_bytes is true and leveled compaction
// is used, the base level is not always L1, so precomupted max_file_size can
// no longer be used. Recompute file_size_for_level from base level.
uint64_t MaxFileSizeForLevel(const MutableCFOptions& cf_options,
    int level, CompactionStyle compaction_style, int base_level,
    bool level_compaction_dynamic_level_bytes) {
  if (!level_compaction_dynamic_level_bytes || level < base_level ||
      compaction_style != kCompactionStyleLevel) {
    assert(level >= 0);
    assert(level < (int)cf_options.max_file_size.size());
    return cf_options.max_file_size[level];
  } else {
    assert(level >= 0 && base_level >= 0);
    assert(level - base_level < (int)cf_options.max_file_size.size());
    return cf_options.max_file_size[level - base_level];
  }
}

void MutableCFOptions::RefreshDerivedOptions(int num_levels,
                                             CompactionStyle compaction_style) {
  max_file_size.resize(num_levels);
  for (int i = 0; i < num_levels; ++i) {
    if (i == 0 && compaction_style == kCompactionStyleUniversal) {
      max_file_size[i] = ULLONG_MAX;
    } else if (i > 1) {
      max_file_size[i] = MultiplyCheckOverflow(max_file_size[i - 1],
                                               target_file_size_multiplier);
    } else {
      max_file_size[i] = target_file_size_base;
    }
  }
}

void MutableCFOptions::Dump(Logger* log) const {
  // Memtable related options
  ROCKS_LOG_INFO(log,
                 "                        write_buffer_size: %" ROCKSDB_PRIszt,
                 write_buffer_size);
  ROCKS_LOG_INFO(log, "                  max_write_buffer_number: %d",
                 max_write_buffer_number);
  ROCKS_LOG_INFO(log,
                 "                         arena_block_size: %" ROCKSDB_PRIszt,
                 arena_block_size);
  ROCKS_LOG_INFO(log, "              memtable_prefix_bloom_ratio: %f",
                 memtable_prefix_bloom_size_ratio);
  ROCKS_LOG_INFO(log, "              memtable_whole_key_filtering: %d",
                 memtable_whole_key_filtering);
  ROCKS_LOG_INFO(log,
                 "                  memtable_huge_page_size: %" ROCKSDB_PRIszt,
                 memtable_huge_page_size);
  ROCKS_LOG_INFO(log,
                 "                    max_successive_merges: %" ROCKSDB_PRIszt,
                 max_successive_merges);
  ROCKS_LOG_INFO(log,
                 "                 inplace_update_num_locks: %" ROCKSDB_PRIszt,
                 inplace_update_num_locks);
  ROCKS_LOG_INFO(
      log, "                         prefix_extractor: %s",
      prefix_extractor == nullptr ? "nullptr" : prefix_extractor->Name());
  ROCKS_LOG_INFO(log, "                 disable_auto_compactions: %d",
                 disable_auto_compactions);
  ROCKS_LOG_INFO(log, "      soft_pending_compaction_bytes_limit: %" PRIu64,
                 soft_pending_compaction_bytes_limit);
  ROCKS_LOG_INFO(log, "      hard_pending_compaction_bytes_limit: %" PRIu64,
                 hard_pending_compaction_bytes_limit);
  ROCKS_LOG_INFO(log, "       level0_file_num_compaction_trigger: %d",
                 level0_file_num_compaction_trigger);
  ROCKS_LOG_INFO(log, "           level0_slowdown_writes_trigger: %d",
                 level0_slowdown_writes_trigger);
  ROCKS_LOG_INFO(log, "               level0_stop_writes_trigger: %d",
                 level0_stop_writes_trigger);
  ROCKS_LOG_INFO(log, "                     max_compaction_bytes: %" PRIu64,
                 max_compaction_bytes);
  ROCKS_LOG_INFO(log, "                    target_file_size_base: %" PRIu64,
                 target_file_size_base);
  ROCKS_LOG_INFO(log, "              target_file_size_multiplier: %d",
                 target_file_size_multiplier);
  ROCKS_LOG_INFO(log, "                 max_bytes_for_level_base: %" PRIu64,
                 max_bytes_for_level_base);
  ROCKS_LOG_INFO(log, "           max_bytes_for_level_multiplier: %f",
                 max_bytes_for_level_multiplier);
  ROCKS_LOG_INFO(log, "                                      ttl: %" PRIu64,
                 ttl);
  ROCKS_LOG_INFO(log, "              periodic_compaction_seconds: %" PRIu64,
                 periodic_compaction_seconds);
  std::string result;
  char buf[10];
  for (const auto m : max_bytes_for_level_multiplier_additional) {
    snprintf(buf, sizeof(buf), "%d, ", m);
    result += buf;
  }
  if (result.size() >= 2) {
    result.resize(result.size() - 2);
  } else {
    result = "";
  }

  ROCKS_LOG_INFO(log, "max_bytes_for_level_multiplier_additional: %s",
                 result.c_str());
  ROCKS_LOG_INFO(log, "        max_sequential_skip_in_iterations: %" PRIu64,
                 max_sequential_skip_in_iterations);
  ROCKS_LOG_INFO(log, "                     paranoid_file_checks: %d",
                 paranoid_file_checks);
  ROCKS_LOG_INFO(log, "                       report_bg_io_stats: %d",
                 report_bg_io_stats);
  ROCKS_LOG_INFO(log, "                              compression: %d",
                 static_cast<int>(compression));

  // Universal Compaction Options
  ROCKS_LOG_INFO(log, "compaction_options_universal.size_ratio : %d",
                 compaction_options_universal.size_ratio);
  ROCKS_LOG_INFO(log, "compaction_options_universal.min_merge_width : %d",
                 compaction_options_universal.min_merge_width);
  ROCKS_LOG_INFO(log, "compaction_options_universal.max_merge_width : %d",
                 compaction_options_universal.max_merge_width);
  ROCKS_LOG_INFO(
      log, "compaction_options_universal.max_size_amplification_percent : %d",
      compaction_options_universal.max_size_amplification_percent);
  ROCKS_LOG_INFO(log,
                 "compaction_options_universal.compression_size_percent : %d",
                 compaction_options_universal.compression_size_percent);
  ROCKS_LOG_INFO(log, "compaction_options_universal.stop_style : %d",
                 compaction_options_universal.stop_style);
  ROCKS_LOG_INFO(
      log, "compaction_options_universal.allow_trivial_move : %d",
      static_cast<int>(compaction_options_universal.allow_trivial_move));

  // FIFO Compaction Options
  ROCKS_LOG_INFO(log, "compaction_options_fifo.max_table_files_size : %" PRIu64,
                 compaction_options_fifo.max_table_files_size);
  ROCKS_LOG_INFO(log, "compaction_options_fifo.allow_compaction : %d",
                 compaction_options_fifo.allow_compaction);
}

MutableCFOptions::MutableCFOptions(const Options& options)
    : MutableCFOptions(ColumnFamilyOptions(options)) {}

// This is to handle backward compatibility, where compaction_options_fifo
// could be assigned a single scalar value, say, like "23", which would be
// assigned to max_table_files_size.

static bool FIFOCompactionOptionsSpecialCase(const std::string& opt_str,
                                             CompactionOptionsFIFO* options) {
  if (opt_str.find("=") != std::string::npos) {
    // New format. Go do your new parsing using ParseStructOptions.
    return false;
  }

  // Old format. Parse just a single uint64_t value.
  options->max_table_files_size = ParseUint64(opt_str);
  return true;
}

class ConfigurableMutableCFOptions : public Configurable {
 private:
  MutableCFOptions options_;

 public:
  ConfigurableMutableCFOptions(const MutableCFOptions& options)
      : options_(options) {
    RegisterOptions("ColumnFamilyOptions", &options_, &cf_options_type_info);
  }
  const OptionTypeMap* GetOptionsTypeMap(
      const std::string& options) const override {
    if (options == "compaction_options_fifo") {
      return &OptionsHelper::fifo_compaction_options_type_info;
    } else if (options == "compaction_options_universal") {
      return &OptionsHelper::universal_compaction_options_type_info;
    } else {
      return Configurable::GetOptionsTypeMap(options);
    }
  }

 protected:
  Status SetStruct(const std::string& opt_name, const std::string& opt_value,
                   const ConfigOptions& cfg_opts, char* opt_addr) override {
    if (opt_name == "compaction_options_fifo" &&
        FIFOCompactionOptionsSpecialCase(
            opt_value, reinterpret_cast<CompactionOptionsFIFO*>(opt_addr))) {
      return Status::OK();
    } else {
      return Configurable::SetStruct(opt_name, opt_value, cfg_opts, opt_addr);
    }
  }

  bool IsMutable() const override { return true; }
};

class ConfigurableCFOptions : public Configurable {
 private:
  ColumnFamilyOptions options_;
  const std::unordered_map<std::string, std::string>* opt_map;

 public:
  ConfigurableCFOptions(
      const ColumnFamilyOptions& options,
      const std::unordered_map<std::string, std::string>* opt_map = nullptr);

 protected:
  const OptionTypeMap* GetOptionsTypeMap(
      const std::string& options) const override {
    if (options == "compaction_options_fifo") {
      return &OptionsHelper::fifo_compaction_options_type_info;
    } else if (options == "compaction_options_universal") {
      return &OptionsHelper::universal_compaction_options_type_info;
    } else {
      return Configurable::GetOptionsTypeMap(options);
    }
  }
  Status SetStruct(const std::string& opt_name, const std::string& opt_value,
                   const ConfigOptions& cfg_opts, char* opt_addr) override {
    if (opt_name == "compaction_options_fifo" &&
        FIFOCompactionOptionsSpecialCase(
            opt_value, reinterpret_cast<CompactionOptionsFIFO*>(opt_addr))) {
      return Status::OK();
    } else {
      return Configurable::SetStruct(opt_name, opt_value, cfg_opts, opt_addr);
    }
  }

  const std::unordered_map<std::string, OptionsSanityCheckLevel>*
  GetOptionsSanityCheckLevel(const std::string& name) const override;
  bool VerifyOptionEqual(const std::string& opt_name,
                         const OptionTypeInfo& opt_info,
                         const char* this_offset, const char* that_offset,
                         const ConfigOptions& options) const override;
  bool IsConfigEqual(const std::string& opt_name,
                     const OptionTypeInfo& opt_info,
                     const Configurable* this_config,
                     const Configurable* that_config,
                     const ConfigOptions& options,
                     std::string* mismatch) const override;
};

ConfigurableCFOptions::ConfigurableCFOptions(
    const ColumnFamilyOptions& options,
    const std::unordered_map<std::string, std::string>* map)
    : options_(options), opt_map(map) {
  RegisterOptions("ColumnFamilyOptions", &options_, &cf_options_type_info);
}

bool ConfigurableCFOptions::VerifyOptionEqual(
    const std::string& opt_name, const OptionTypeInfo& opt_info,
    const char* this_offset, const char* that_offset,
    const ConfigOptions& options) const {
  std::string this_value, that_value;
  if (opt_info.verification != OptionVerificationType::kByName &&
      opt_info.verification != OptionVerificationType::kByNameAllowFromNull &&
      opt_info.verification != OptionVerificationType::kByNameAllowNull) {
    return false;
  } else if (opt_info.string_func != nullptr) {
    if (!opt_info.string_func(opt_name, this_offset, options, &this_value)
             .ok() ||
        !opt_info.string_func(opt_name, that_offset, options, &that_value)
             .ok()) {
      return false;
    }
  } else if (!SerializeOption(opt_name, opt_info, this_offset, "", options,
                              &this_value)
                  .ok() ||
             !SerializeOption(opt_name, opt_info, that_offset, "", options,
                              &that_value)
                  .ok()) {
    return false;
  }
  if (opt_map == nullptr) {
    return true;
  } else {
    auto iter = opt_map->find(opt_name);
    if (iter == opt_map->end()) {
      return true;
    } else if (opt_info.verification ==
                   OptionVerificationType::kByNameAllowNull &&
               (iter->second == kNullptrString ||
                this_value == kNullptrString)) {
      return true;
    } else if (opt_info.verification ==
                   OptionVerificationType::kByNameAllowFromNull &&
               iter->second == kNullptrString) {
      return true;
    } else {
      return (this_value == iter->second);
    }
  }
}

const std::unordered_map<std::string, OptionsSanityCheckLevel>*
ConfigurableCFOptions::GetOptionsSanityCheckLevel(
    const std::string& name) const {
  static const std::unordered_map<std::string, OptionsSanityCheckLevel>
      cf_sanity_level_options = {
          {"comparator", kSanityLevelLooselyCompatible},
          {"table_factory", kSanityLevelLooselyCompatible},
          {"merge_operator", kSanityLevelLooselyCompatible},
          {"compression_opts", kSanityLevelNone},
          {"bottommost_compression_opts", kSanityLevelNone},
      };
  if (name == "ColumnFamilyOptions") {
    return &cf_sanity_level_options;
  } else {
    return Configurable::GetOptionsSanityCheckLevel(name);
  }
}

bool ConfigurableCFOptions::IsConfigEqual(const std::string& opt_name,
                                          const OptionTypeInfo& opt_info,
                                          const Configurable* this_config,
                                          const Configurable* that_config,
                                          const ConfigOptions& options,
                                          std::string* mismatch) const {
  bool is_equal = Configurable::IsConfigEqual(opt_name, opt_info, this_config,
                                              that_config, options, mismatch);
  // If the options are equal and there is no config but there is a map
  if (is_equal && this_config == nullptr && opt_map != nullptr) {
    // Check if the option name exists in the map
    const auto iter = opt_map->find(opt_name);
    // If the name exists in the map and is not empty,
    // then the this_config should be set.
    if (iter != opt_map->end() && !iter->second.empty()) {
      is_equal = false;
    }
  }
  return is_equal;
}

Status GetColumnFamilyOptionsFromMap(
    const ColumnFamilyOptions& base_options,
    const std::unordered_map<std::string, std::string>& opts_map,
    const ConfigOptions& cfg_options, ColumnFamilyOptions* new_options) {
  ConfigurableCFOptions config(base_options);
  const ColumnFamilyOptions* updated =
      config.GetOptions<ColumnFamilyOptions>("ColumnFamilyOptions");
  Status status = config.ConfigureFromMap(opts_map, cfg_options);
  if (status.ok()) {
    *new_options = *updated;
  } else {
    *new_options = base_options;
  }
  return status;
}

Status GetColumnFamilyOptionsFromMapInternal(
    const ColumnFamilyOptions& base_options,
    const std::unordered_map<std::string, std::string>& opts_map,
    const ConfigOptions& cfg_options, ColumnFamilyOptions* new_options,
    std::unordered_map<std::string, std::string>* unused) {
  ConfigurableCFOptions config(base_options);
  const ColumnFamilyOptions* updated =
      config.GetOptions<ColumnFamilyOptions>("ColumnFamilyOptions");
  Status status = config.ConfigureFromMap(opts_map, cfg_options, unused);
  if (cfg_options.ignore_unknown_options || status.ok()) {
    *new_options = *updated;
  } else {
    *new_options = base_options;
  }
  return status;
}

Status GetColumnFamilyOptionsFromString(const ColumnFamilyOptions& base_options,
                                        const std::string& opts_str,
                                        const ConfigOptions& cfg_options,
                                        ColumnFamilyOptions* new_options) {
  std::unordered_map<std::string, std::string> opts_map;
  Status s = StringToMap(opts_str, &opts_map);
  if (!s.ok()) {
    *new_options = base_options;
    return s;
  }
  return GetColumnFamilyOptionsFromMap(base_options, opts_map, cfg_options,
                                       new_options);
}

Status GetColumnFamilyOptionNames(std::unordered_set<std::string>* option_names,
                                  bool use_mutable) {
  ConfigOptions cfg;
  if (use_mutable) {
    MutableCFOptions opts;
    ConfigurableMutableCFOptions config(opts);
    return config.GetOptionNames(cfg, option_names);
  } else {
    ColumnFamilyOptions opts;
    ConfigurableCFOptions config(opts);
    return config.GetOptionNames(cfg, option_names);
  }
}

// Deprecated.  Maintained for backward compatability
Status GetStringFromColumnFamilyOptions(std::string* opt_string,
                                        const ColumnFamilyOptions& cf_options,
                                        const std::string& delimiter) {
  ConfigOptions options;
  options.delimiter = delimiter;
  return GetStringFromColumnFamilyOptions(cf_options, options, opt_string);
}

Status GetStringFromColumnFamilyOptions(const ColumnFamilyOptions& cf_options,
                                        const ConfigOptions& options,
                                        std::string* opt_string) {
  ConfigurableCFOptions config(cf_options);
  return config.GetOptionString(options, opt_string);
}

Status GetMutableOptionsFromStrings(
    const MutableCFOptions& base_options,
    const std::unordered_map<std::string, std::string>& options_map,
    Logger* /* info_log */, MutableCFOptions* new_options) {
  assert(new_options);

  ConfigOptions cfg_options;
  ConfigurableMutableCFOptions config(base_options);
  Status status = config.ConfigureFromMap(options_map, cfg_options);
  if (status.ok()) {
    *new_options =
        *(config.GetOptions<MutableCFOptions>("ColumnFamilyOptions"));
  } else {
    *new_options = base_options;
  }
  return status;
}

Status RocksDBOptionsParser::VerifyCFOptions(
    const ColumnFamilyOptions& base_opt,
    const ColumnFamilyOptions& persisted_opt,
    const std::unordered_map<std::string, std::string>* persisted_opt_map,
    OptionsSanityCheckLevel sanity_check_level) {
  ConfigOptions options;
  options.sanity_level = sanity_check_level;
  return VerifyCFOptions(base_opt, persisted_opt, options, persisted_opt_map);
}

Status RocksDBOptionsParser::VerifyCFOptions(
    const ColumnFamilyOptions& base_opt,
    const ColumnFamilyOptions& persisted_opt, const ConfigOptions& options,
    const std::unordered_map<std::string, std::string>* persisted_opt_map) {
  ConfigurableCFOptions base_config(base_opt, persisted_opt_map);
  ConfigurableCFOptions persisted_config(persisted_opt, persisted_opt_map);
  std::string mismatch;
  if (!base_config.Matches(&persisted_config, options, &mismatch)) {
    std::string base_value;
    std::string persisted_value;
    const size_t kBufferSize = 2048;
    char buffer[kBufferSize];
    if (base_config.GetOption(mismatch, options, &base_value).ok() &&
        persisted_config.GetOption(mismatch, options, &persisted_value).ok()) {
      snprintf(buffer, sizeof(buffer),
               "[RocksDBOptionsParser]: "
               "failed the verification on ColumnFamilyOptions::%s --- "
               "The specified one is %s while the persisted one is %s.\n",
               mismatch.c_str(), base_value.c_str(), persisted_value.c_str());
    } else {
      snprintf(buffer, sizeof(buffer),
               "[RocksDBOptionsParser]: "
               "failed the verification on ColumnFamilyOptions::%s\n",
               mismatch.c_str());
    }
    return Status::InvalidArgument(Slice(buffer, sizeof(buffer)));
  }
  return Status::OK();
}

#endif  // ROCKSDB_LITE
}  // namespace rocksdb
