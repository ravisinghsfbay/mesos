/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __STATUS_UPDATE_MANAGER_HPP__
#define __STATUS_UPDATE_MANAGER_HPP__

#include <ostream>
#include <queue>
#include <string>
#include <utility>

#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timeout.hpp>

#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/utils.hpp>
#include <stout/uuid.hpp>

#include "common/type_utils.hpp"

#include "messages/messages.hpp"

namespace mesos {
namespace internal {
namespace slave {

// Forward declarations.
class StatusUpdateManagerProcess;
struct StatusUpdateStream;


// StatusUpdateManager is responsible for
// 1) Reliably sending status updates to the master (and hence, the scheduler).
// 2) Checkpointing the update to disk (optional).
// 3) Sending ACKs to the executor (optional).
// 4) Receiving ACKs from the scheduler.
class StatusUpdateManager
{
public:
  StatusUpdateManager();
  virtual ~StatusUpdateManager();

  void initialize(const PID<Slave>& slave);

  // Enqueues the status update to reliably send the update to the master.
  // If 'path' is provided, the update is also checkpointed to the given path.
  // @return A future indicating whether the update is handled
  //         successfully (e.g. checkpointed).
  process::Future<Try<Nothing> > update(
      const StatusUpdate& update,
      bool checkpoint,
      const Option<std::string>& path);

  // Receives the ACK from the scheduler and checkpoints it to disk if
  // necessary. Also, sends the next pending status update, if any.
  // @return A future of indicating whether the acknowledgement
  //         is handled successfully (e.g. checkpointed).
  process::Future<Try<Nothing> > acknowledgement(
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const std::string& uuid);

  // TODO(vinod): Remove this hack once the new leader detector code is merged.
  void newMasterDetected(const UPID& pid);

  // Closes all the status update streams corresponding to this framework.
  // NOTE: This stops retrying any pending status updates for this framework.
  void cleanup(const FrameworkID& frameworkId);

private:
  StatusUpdateManagerProcess* process;
};


// StatusUpdateStream handles the status updates and acknowledgements
// of a task, checkpointing them if necessary. It also holds the information
// about received, acknowledged and pending status updates.
// NOTE: A task is expected to have a globally unique ID across the lifetime
// of a framework. In other words the tuple (taskId, frameworkId) should be
// always unique.
struct StatusUpdateStream
{
  StatusUpdateStream(const TaskID& _taskId,
                     const FrameworkID& _frameworkId,
                     const Option<std::string>& _path,
                     int oflag = O_CREAT | O_RDWR)
    : taskId(_taskId), frameworkId(_frameworkId), path(_path), error(None())
  {
    if (path.isSome()) {
      // Create the base updates directory, if it doesn't exist.
      Try<Nothing> mkdir = os::mkdir(os::dirname(path.get()).get());
      if (mkdir.isError()) {
        error = "Failed to create " + os::dirname(path.get()).get();
        return;
      }

      // Open the updates file.
      Try<int> result = os::open(
          path.get(), oflag | O_SYNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

      if(result.isError()) {
        error = "Failed to open '" + path.get() + "' for status updates";
        return;
      }

      // We keep the file open through the lifetime of the task, because it
      // makes it easy to append status update records to the file.
      fd = result.get();
    }
  }

  ~StatusUpdateStream()
  {
    if (fd.isSome()) {
      Try<Nothing> close = os::close(fd.get());
      if (close.isError()) {
        LOG(ERROR) << "Failed to close file '" << path.get() << "': "
                   << close.error();
      }
    }
  }

  Try<Nothing> update(const StatusUpdate& update)
  {
    if (error.isSome()) {
      return Error(error.get());
    }

    // Check that this status update has not already been acknowledged.
    // This could happen in the rare case when the slave received the ACK
    // from the framework, died, but slave's ACK to the executor never made it!
    if (acknowledged.contains(update.uuid())) {
      LOG(WARNING) << "Ignoring status update " << update
                   << " that has already been acknowledged by the framework!";
      return Nothing();
    }

    // Check that this update hasn't already been received.
    // This could happen if the slave receives a status update from an executor,
    // then crashes after it writes it to disk but before it sends an ack.
    if (received.contains(update.uuid())) {
      LOG(WARNING) << "Ignoring duplicate status update " << update;
      return Nothing();
    }

    // Handle the update, checkpointing if necessary.
    return handle(update, StatusUpdateRecord::UPDATE);
  }

  Try<Nothing> acknowledgement(
      const TaskID& taskId,
      const FrameworkID& frameworkId,
      const std::string& uuid,
      const StatusUpdate& update)
  {
    if (error.isSome()) {
      return Error(error.get());
    }

    CHECK(uuid.compare(update.uuid()) == 0)
      << "Unexpected UUID mismatch! (received " << uuid
      << ", expecting " << update.uuid()
      << ") for update " << stringify(update);

    // Handle the ACK, checkpointing if necessary.
    return handle(update, StatusUpdateRecord::ACK);
  }

  // Returns the next update (or none, if empty) in the queue.
  Result<StatusUpdate> next()
  {
    if (error.isSome()) {
      return Error(error.get());
    }

    if (!pending.empty()) {
      return pending.front();
    }

    return None();
  }

  // TODO(vinod): Explore semantics to make 'timeout' and 'pending' private.
  Option<Timeout> timeout; // Timeout for resending status update.
  std::queue<StatusUpdate> pending;

private:
  // Handles the status update and writes it to disk, if necessary.
  // TODO(vinod): The write has to be asynchronous to avoid status updates that
  // are being checkpointed, blocking the processing of other updates.
  // One solution is to wrap the protobuf::write inside async, but its probably
  // too much of an overhead to spin up a new libprocess per status update?
  // A better solution might be to be have async write capability for file io.
  Try<Nothing> handle(
      const StatusUpdate& update,
      const StatusUpdateRecord::Type& type)
  {
    CHECK(error.isNone());

    LOG(INFO) << "Handling " << type << " for status update " << update;

    // Checkpoint the update if necessary.
    if (path.isSome()) {
      LOG(INFO) << "Checkpointing " << type << " for status update " << update;

      CHECK_SOME(fd);

      StatusUpdateRecord record;
      record.set_type(type);

      if (type == StatusUpdateRecord::UPDATE) {
        record.mutable_update()->CopyFrom(update);
      } else {
        record.set_uuid(update.uuid());
      }

      Try<Nothing> write = ::protobuf::write(fd.get(), record);
      if (write.isError()) {
        error = "Failed to write status update " + stringify(update) +
                " to '" + path.get() + "': " + write.error();
        return Error(error.get());
      }
    }

    if (type == StatusUpdateRecord::UPDATE) {
      // Record this update.
      received.insert(update.uuid());

      // Add it to the pending updates queue.
      pending.push(update);
    } else {
      // Record this ACK.
      acknowledged.insert(update.uuid());

      // Remove the corresponding update from the pending queue.
      pending.pop();
    }

    return Nothing();
  }

  const TaskID taskId;
  const FrameworkID frameworkId;

  hashset<std::string> received;
  hashset<std::string> acknowledged;

  const Option<std::string> path; // File path of the update stream.
  Option<int> fd; // File descriptor to the update stream.

  Option<std::string> error; // Potential non-retryable error.
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {


#endif // __STATUS_UPDATE_MANAGER_HPP__
