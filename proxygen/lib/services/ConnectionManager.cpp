// Copyright 2004-present Facebook.  All rights reserved.
#include "proxygen/lib/services/ConnectionManager.h"

#include <glog/logging.h>
#include <thrift/lib/cpp/async/TEventBase.h>

using apache::thrift::async::TAsyncTimeoutSet;
using apache::thrift::async::TEventBase;
using std::chrono::milliseconds;

namespace proxygen {

ConnectionManager::ConnectionManager(TEventBase* eventBase,
    milliseconds timeout, Callback* callback)
  : connTimeouts_(new TAsyncTimeoutSet(eventBase, timeout)),
    callback_(callback),
    eventBase_(eventBase),
    idleIterator_(conns_.end()),
    idleLoopCallback_(this) {

}

void
ConnectionManager::addConnection(ManagedConnection* connection,
    bool timeout) {
  CHECK_NOTNULL(connection);
  ConnectionManager* oldMgr = connection->getConnectionManager();
  if (oldMgr != this) {
    if (oldMgr) {
      // 'connection' was being previously managed in a different thread.
      // We must remove it from that manager before adding it to this one.
      oldMgr->removeConnection(connection);
    }
    conns_.push_back(*connection);
    connection->setConnectionManager(this);
    if (callback_) {
      callback_->onConnectionAdded(*this);
    }
  }
  if (timeout) {
    scheduleTimeout(connection);
  }
}

void
ConnectionManager::scheduleTimeout(ManagedConnection* connection) {
  connTimeouts_->scheduleTimeout(connection);
}

void
ConnectionManager::removeConnection(ManagedConnection* connection) {
  if (connection->getConnectionManager() == this) {
    connection->cancelTimeout();
    connection->setConnectionManager(nullptr);

    // Un-link the connection from our list, being careful to keep the iterator
    // that we're using for idle shedding valid
    auto it = conns_.iterator_to(*connection);
    if (it == idleIterator_) {
      ++idleIterator_;
    }
    conns_.erase(it);

    if (callback_) {
      callback_->onConnectionRemoved(*this);
      if (getNumConnections() == 0) {
        callback_->onEmpty(*this);
      }
    }
  }
}

void
ConnectionManager::initiateGracefulShutdown(
  std::chrono::milliseconds idleGrace) {
  if (idleGrace.count() > 0) {
    idleLoopCallback_.scheduleTimeout(idleGrace);
    VLOG(3) << "Scheduling idle grace period of " << idleGrace.count() << "ms";
  } else {
    action_ = ShutdownAction::DRAIN2;
    VLOG(3) << "proceeding directly to closing idle connections";
  }
  drainAllConnections();
}

void
ConnectionManager::drainAllConnections() {
  DestructorGuard g(this);
  size_t numCleared = 0;
  size_t numKept = 0;

  auto it = idleIterator_ == conns_.end() ?
    conns_.begin() : idleIterator_;

  while (it != conns_.end() && (numKept + numCleared) < 64) {
    ManagedConnection& conn = *it++;
    if (action_ == ShutdownAction::DRAIN1) {
      conn.notifyPendingShutdown();
    } else {
      // Second time around: close idle sessions. If they aren't idle yet,
      // have them close when they are idle
      if (conn.isBusy()) {
        numKept++;
      } else {
        numCleared++;
      }
      conn.closeWhenIdle();
    }
  }

  if (action_ == ShutdownAction::DRAIN2) {
    VLOG(2) << "Idle connections cleared: " << numCleared <<
      ", busy conns kept: " << numKept;
  }
  if (it != conns_.end()) {
    idleIterator_ = it;
    eventBase_->runInLoop(&idleLoopCallback_);
  } else {
    action_ = ShutdownAction::DRAIN2;
  }
}

void
ConnectionManager::dropAllConnections() {
  DestructorGuard g(this);

  // Iterate through our connection list, and drop each connection.
  VLOG(3) << "connections to drop: " << conns_.size();
  idleLoopCallback_.cancelTimeout();
  unsigned i = 0;
  while (!conns_.empty()) {
    ManagedConnection& conn = conns_.front();
    conns_.pop_front();
    conn.cancelTimeout();
    conn.setConnectionManager(nullptr);
    // For debugging purposes, dump information about the first few
    // connections.
    static const unsigned MAX_CONNS_TO_DUMP = 2;
    if (++i <= MAX_CONNS_TO_DUMP) {
      conn.dumpConnectionState(3);
    }
    conn.dropConnection();
  }
  idleIterator_ = conns_.end();
  idleLoopCallback_.cancelLoopCallback();

  if (callback_) {
    callback_->onEmpty(*this);
  }
}

} // proxygen
