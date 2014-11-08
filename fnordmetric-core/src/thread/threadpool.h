/**
 * This file is part of the "FnordMetric" project
 *   Copyright (c) 2011-2014 Paul Asmuth, Google Inc.
 *
 * FnordMetric is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License v3.0. You should have received a
 * copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */
#ifndef _FNORDMETRIC_THREAD_THREADPOOL_H
#define _FNORDMETRIC_THREAD_THREADPOOL_H
#include <functional>
#include <atomic>
#include <fnordmetric/thread/task.h>
#include <fnordmetric/thread/taskscheduler.h>
#include <fnordmetric/util/exceptionhandler.h>

namespace fnord {
namespace thread {

/**
 * A threadpool is threadsafe
 */
class ThreadPool : public TaskScheduler {
public:
  ThreadPool(
      std::unique_ptr<fnord::util::ExceptionHandler> error_handler);

  void run(std::shared_ptr<Task> task) override;
  void runOnReadable(std::shared_ptr<Task> task, int fd) override;
  void runOnWritable(std::shared_ptr<Task> task, int fd) override;

protected:
  std::unique_ptr<fnord::util::ExceptionHandler> error_handler_;
};

}
}
#endif
