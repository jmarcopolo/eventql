/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include "stx/logging.h"
#include "stx/http/HTTPFileDownload.h"
#include "zbase/mapreduce/MapReduceScheduler.h"

using namespace stx;

namespace zbase {

MapReduceScheduler::MapReduceScheduler(
    const AnalyticsSession& session,
    RefPtr<MapReduceJobSpec> job,
    const MapReduceShardList& shards,
    thread::ThreadPool* tpool,
    AnalyticsAuth* auth,
    const String& cachedir,
    size_t max_concurrent_tasks /* = kDefaultMaxConcurrentTasks */) :
    session_(session),
    job_(job),
    shards_(shards),
    shard_status_(shards_.size(), MapReduceShardStatus::PENDING),
    shard_results_(shards_.size()),
    tpool_(tpool),
    auth_(auth),
    cachedir_(cachedir),
    max_concurrent_tasks_(max_concurrent_tasks),
    done_(false),
    error_(false),
    num_shards_running_(0),
    num_shards_completed_(0) {}

void MapReduceScheduler::execute() {
  std::unique_lock<std::mutex> lk(mutex_);

  for (;;) {
    logDebug(
        "z1.mapreduce",
        "Running job; progress=$0/$1 ($2 runnning)",
        num_shards_completed_,
        shards_.size(),
        num_shards_running_);

    job_->updateProgress(MapReduceJobStatus{
      .num_tasks_total = shards_.size(),
      .num_tasks_completed = num_shards_completed_,
      .num_tasks_running = num_shards_running_
    });

    if (error_) {
      auto err_str = StringUtil::join(errors_, ", ");
      RAISEF(
          kRuntimeError,
          "MapReduce execution failed: $0",
          err_str);
    }


    if (done_) {
      break;
    }

    if (startJobs() > 0) {
      continue;
    }

    cv_.wait(lk);
  }
}

size_t MapReduceScheduler::startJobs() {
  if (num_shards_running_ >= max_concurrent_tasks_) {
    return 0;
  }

  if (num_shards_completed_ + num_shards_running_ >= shards_.size()) {
    return 0;
  }

  size_t num_started = 0;
  for (size_t i = 0; i < shards_.size(); ++i) {
    if (shard_status_[i] != MapReduceShardStatus::PENDING) {
      continue;
    }

    bool ready = true;
    for (auto dep : shards_[i]->dependencies) {
      if (shard_status_[dep] != MapReduceShardStatus::COMPLETED) {
        ready = false;
      }
    }

    if (!ready) {
      continue;
    }

    ++num_shards_running_;
    ++num_started;
    shard_status_[i] = MapReduceShardStatus::RUNNING;
    auto shard = shards_[i];

    auto base = mkRef(this);
    tpool_->run([this, i, shard, base] {
      bool error = false;
      String error_str;
      Option<MapReduceShardResult> result;
      try {
        result = shard->task->execute(shard, base);
      } catch (const StandardException& e) {
        error_str = e.what();
        error = true;

        logError(
            "z1.mapreduce",
            e,
            "MapReduceTaskShard failed");
      }

      {
        std::unique_lock<std::mutex> lk(mutex_);

        shard_results_[i] = result;
        shard_status_[i] = error
            ? MapReduceShardStatus::ERROR
            : MapReduceShardStatus::COMPLETED;

        --num_shards_running_;
        if (++num_shards_completed_ == shards_.size()) {
          done_ = true;
        }

        if (error) {
          error_ = true;
          errors_.emplace_back(error_str);
        }

        lk.unlock();
        cv_.notify_all();
      }
    });

    if (num_shards_running_ >= max_concurrent_tasks_) {
      break;
    }
  }

  return num_started;
}

void MapReduceScheduler::sendResult(const String& key, const String& value) {
  job_->sendResult(key, value);
}

Option<String> MapReduceScheduler::getResultURL(size_t task_index) {
  std::unique_lock<std::mutex> lk(mutex_);
  if (task_index >= shards_.size()) {
    RAISEF(kIndexError, "invalid task index: $0", task_index);
  }

  if (shard_status_[task_index] != MapReduceShardStatus::COMPLETED) {
    RAISEF(kIndexError, "task is not completed: $0", task_index);
  }

  const auto& result = shard_results_[task_index];
  if (result.isEmpty()) {
    return None<String>();
  }

  auto result_path = FileUtil::joinPaths(
      cachedir_,
      StringUtil::format("mr-result-$0", result.get().result_id.toString()));

  return Some(StringUtil::format(
      "http://$0/api/v1/mapreduce/result/$1",
      result.get().host.addr.ipAndPort(),
      result.get().result_id.toString()));
}

Option<String> MapReduceScheduler::downloadResult(size_t task_index) {
  std::unique_lock<std::mutex> lk(mutex_);
  if (task_index >= shards_.size()) {
    RAISEF(kIndexError, "invalid task index: $0", task_index);
  }

  if (shard_status_[task_index] != MapReduceShardStatus::COMPLETED) {
    RAISEF(kIndexError, "task is not completed: $0", task_index);
  }

  const auto& result = shard_results_[task_index];
  if (result.isEmpty()) {
    return None<String>();
  }

  auto result_path = FileUtil::joinPaths(
      cachedir_,
      StringUtil::format("mr-result-$0", result.get().result_id.toString()));

  auto url = StringUtil::format(
      "http://$0/api/v1/mapreduce/result/$1",
      result.get().host.addr.ipAndPort(),
      result.get().result_id.toString());

  auto api_token = auth_->encodeAuthToken(session_);

  http::HTTPMessage::HeaderList auth_headers;
  auth_headers.emplace_back(
      "Authorization",
      StringUtil::format("Token $0", api_token));

  auto req = http::HTTPRequest::mkGet(url, auth_headers);

  http::HTTPClient http_client;
  http::HTTPFileDownload download(req, result_path);
  auto res = download.download(&http_client);
  if (res.statusCode() != 200) {
    RAISEF(kRuntimeError, "received non-201 response for $0", url);
  }

  return Some(result_path);
}


} // namespace zbase
