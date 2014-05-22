/*
  Copyright 2014 DataStax

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#ifndef __CASS_POOL_HPP_INCLUDED__
#define __CASS_POOL_HPP_INCLUDED__

#include <list>
#include <string>
#include <algorithm>
#include <functional>

#include "client_connection.hpp"
#include "session.hpp"
#include "timer.hpp"
#include "prepare_handler.hpp"

namespace cass {

class Pool {
  typedef std::list<ClientConnection*> ConnectionCollection;
  typedef std::function<void(Host host)> ConnectCallback;
  typedef std::function<void(Host host)> CloseCallback;

  Host host_;
  uv_loop_t* loop_;
  SSLContext* ssl_context_;
  const Config& config_;
  ConnectionCollection connections_;
  ConnectionCollection connections_pending_;
  std::list<RequestHandler*> pending_request_queue_;
  bool is_closing_;
  ConnectCallback connect_callback_;
  CloseCallback close_callback_;
  RetryCallback retry_callback_;

 public:
  class PoolHandler : public ResponseCallback {
    public:
      PoolHandler(Pool* pool,
                  ClientConnection* connection,
                  RequestHandler* request_handler)
        : pool_(pool)
        , connection_(connection)
        , request_handler_(request_handler) { }

      virtual Message* request() const {
        return request_handler_->request();
      }

      virtual void on_set(Message* response) {
        switch(response->opcode) {
          case CQL_OPCODE_RESULT:
            request_handler_->on_set(response);
            break;
          case CQL_OPCODE_ERROR:
            on_error_response(response);
            break;
          default:
            request_handler_->on_set(response);
            // TODO(mpenick): Log this
            connection_->defunct();
        }
        finish_request();
      }

      virtual void on_error(CassError code, const std::string& message) {
        if(code == CASS_ERROR_LIB_WRITE_ERROR) {
          pool_->retry_callback_(request_handler_.get(), RETRY_WITH_NEXT_HOST);
          //pool_->io_worker_->retry(request_handler_.get());
        } else {
          request_handler_->on_error(code, message);
        }
        finish_request();
      }

      virtual void on_timeout() {
        request_handler_->on_timeout();
        finish_request();
      }

    private:
      void finish_request() {
        if(connection_->is_ready()) {
          pool_->execute_pending_request(connection_);
        }
      }

      void on_error_response(Message* response) {
        BodyError* error = static_cast<BodyError*>(response->body.get());
        switch(error->code) {
          case CQL_ERROR_UNPREPARED: {
            connection_->execute(new PrepareHandler(pool_->retry_callback_, request_handler_.release()));
            break;
          }
          default:
            request_handler_->on_set(response);
            break;
        }
      }

      Pool* pool_;
      ClientConnection* connection_;
      std::unique_ptr<RequestHandler> request_handler_;
  };

  Pool(const Host& host,
       uv_loop_t* loop,
       SSLContext* ssl_context,
       const Config& config,
       ConnectCallback connect_callback,
       CloseCallback close_callback,
       RetryCallback retry_callback)
    : host_(host)
    , loop_(loop)
    , ssl_context_(ssl_context)
    , config_(config)
    , is_closing_(false)
    , connect_callback_(connect_callback)
    , close_callback_(close_callback)
    , retry_callback_(retry_callback) {
    for (size_t i = 0; i < config.core_connections_per_host(); ++i) {
      spawn_connection();
    }
  }

  void on_connection_connect(ClientConnection* connection) {
    connect_callback_(host_);
    connections_pending_.remove(connection);

    if(is_closing_) {
      connection->close();
    } else if(connection->is_ready()) {
        connections_.push_back(connection);
        execute_pending_request(connection);
    }
  }

  void on_connection_close(ClientConnection* connection) {
    connections_.remove(connection);

    if(connection->is_defunct()) {
      is_closing_ = true; // TODO(mpenick): Conviction policy
    }

    maybe_close();
  }

  void maybe_close() {
    if(is_closing_) {
      for(auto c : connections_) {
        if(!c->is_closing()) {
          c->close();
        }
      }
      if(connections_.empty() &&
         connections_pending_.empty() &&
         pending_request_queue_.empty()) {
        close_callback_(host_);
      }
    }
  }

  void close() {
    is_closing_ = true;
    for (auto c : connections_) {
      c->close();
    }
    maybe_close();
  }

  void set_keyspace() {
    // TODO(mstump)
  }

  void spawn_connection() {
    if(is_closing_) {
      return;
    }

    ClientConnection* connection
        = new ClientConnection(loop_,
                               ssl_context_ ? ssl_context_->session_new() : nullptr,
                               host_,
                               std::bind(&Pool::on_connection_connect, this,
                                         std::placeholders::_1),
                               std::bind(&Pool::on_connection_close, this,
                                         std::placeholders::_1));

    connection->connect();
    connections_pending_.push_back(connection);
  }

  void maybe_spawn_connection() {
    if (connections_pending_.size() >= config_.max_simultaneous_creation()) {
      return;
    }

    if (connections_.size() + connections_pending_.size()
        >= config_.max_connections_per_host()) {
      return;
    }

    spawn_connection();
  }

  static bool least_busy_comp(
      ClientConnection* a,
      ClientConnection* b) {
    return a->available_streams() < b->available_streams();
  }

  ClientConnection* find_least_busy() {
    ConnectionCollection::iterator it =
        std::max_element(
          connections_.begin(),
          connections_.end(),
          Pool::least_busy_comp);
    if ((*it)->is_ready() && (*it)->available_streams()) {
      return *it;
    }
    return nullptr;
  }

  ClientConnection* borrow_connection() {
    if(is_closing_) {
      return nullptr;
    }

    if(connections_.empty()) {
      for (size_t i = 0; i < config_.core_connections_per_host(); ++i) {
        spawn_connection();
      }
      return nullptr;
    }

    maybe_spawn_connection();

    return find_least_busy();
  }

  bool execute(ClientConnection* connection, RequestHandler* request_handler) {
    return connection->execute(new PoolHandler(this, connection, request_handler));
  }

  void on_timeout(Timer* timer) {
    RequestHandler* request_handler = static_cast<RequestHandler*>(timer->data());
    pending_request_queue_.remove(request_handler);
    retry_callback_(request_handler, RETRY_WITH_NEXT_HOST);
    maybe_close();
  }

  bool wait_for_connection(RequestHandler* request_handler) {
    if(is_closing_ || pending_request_queue_.size() + 1 > config_.max_pending_requests()) {
      return false;
    }
    request_handler->timer = Timer::start(loop_, config_.connect_timeout(),
                                request_handler,
                                std::bind(&Pool::on_timeout, this, std::placeholders::_1));
    pending_request_queue_.push_back(request_handler);
    return true;
  }

  void execute_pending_request(ClientConnection* connection) {
    if(!pending_request_queue_.empty()) {
      RequestHandler* request_handler = pending_request_queue_.front();
      pending_request_queue_.pop_front();
      if(request_handler->timer) {
        Timer::stop(request_handler->timer);
        request_handler->timer = nullptr;
      }
      if(!execute(connection, request_handler)) {
        retry_callback_(request_handler, RETRY_WITH_NEXT_HOST);
      }
    }
  }
};

} // namespace cass

#endif