/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <queue>

#include <nan.h>
#include <node.h>
#include <v8.h>
#include "grpc/grpc.h"
#include "grpc/grpc_security.h"
#include "grpc/support/alloc.h"
#include "grpc/support/log.h"
#include "grpc/support/time.h"

// TODO(murgatroid99): Remove this when the endpoint API becomes public
#ifdef GRPC_UV
extern "C" {
#include "src/core/lib/iomgr/pollset_uv.h"
}
#endif

#include "call.h"
#include "call_credentials.h"
#include "channel.h"
#include "channel_credentials.h"
#include "completion_queue.h"
#include "completion_queue_async_worker.h"
#include "server.h"
#include "server_credentials.h"
#include "slice.h"
#include "timeval.h"

using grpc::node::CreateSliceFromString;

using v8::FunctionTemplate;
using v8::Local;
using v8::Value;
using v8::Number;
using v8::Object;
using v8::Uint32;
using v8::String;

typedef struct log_args {
  gpr_log_func_args core_args;
  gpr_timespec timestamp;
} log_args;

typedef struct logger_state {
  Nan::Callback *callback;
  std::queue<log_args *> *pending_args;
  uv_mutex_t mutex;
  uv_async_t async;
  // Indicates that a logger has been set
  bool logger_set;
} logger_state;

logger_state grpc_logger_state;

static char *pem_root_certs = NULL;

void InitStatusConstants(Local<Object> exports) {
  Nan::HandleScope scope;
  Local<Object> status = Nan::New<Object>();
  Nan::Set(exports, Nan::New("status").ToLocalChecked(), status);
  Local<Value> OK(Nan::New<Uint32, uint32_t>(GRPC_STATUS_OK));
  Nan::Set(status, Nan::New("OK").ToLocalChecked(), OK);
  Local<Value> CANCELLED(Nan::New<Uint32, uint32_t>(GRPC_STATUS_CANCELLED));
  Nan::Set(status, Nan::New("CANCELLED").ToLocalChecked(), CANCELLED);
  Local<Value> UNKNOWN(Nan::New<Uint32, uint32_t>(GRPC_STATUS_UNKNOWN));
  Nan::Set(status, Nan::New("UNKNOWN").ToLocalChecked(), UNKNOWN);
  Local<Value> INVALID_ARGUMENT(
      Nan::New<Uint32, uint32_t>(GRPC_STATUS_INVALID_ARGUMENT));
  Nan::Set(status, Nan::New("INVALID_ARGUMENT").ToLocalChecked(),
           INVALID_ARGUMENT);
  Local<Value> DEADLINE_EXCEEDED(
      Nan::New<Uint32, uint32_t>(GRPC_STATUS_DEADLINE_EXCEEDED));
  Nan::Set(status, Nan::New("DEADLINE_EXCEEDED").ToLocalChecked(),
           DEADLINE_EXCEEDED);
  Local<Value> NOT_FOUND(Nan::New<Uint32, uint32_t>(GRPC_STATUS_NOT_FOUND));
  Nan::Set(status, Nan::New("NOT_FOUND").ToLocalChecked(), NOT_FOUND);
  Local<Value> ALREADY_EXISTS(
      Nan::New<Uint32, uint32_t>(GRPC_STATUS_ALREADY_EXISTS));
  Nan::Set(status, Nan::New("ALREADY_EXISTS").ToLocalChecked(), ALREADY_EXISTS);
  Local<Value> PERMISSION_DENIED(
      Nan::New<Uint32, uint32_t>(GRPC_STATUS_PERMISSION_DENIED));
  Nan::Set(status, Nan::New("PERMISSION_DENIED").ToLocalChecked(),
           PERMISSION_DENIED);
  Local<Value> UNAUTHENTICATED(
      Nan::New<Uint32, uint32_t>(GRPC_STATUS_UNAUTHENTICATED));
  Nan::Set(status, Nan::New("UNAUTHENTICATED").ToLocalChecked(),
           UNAUTHENTICATED);
  Local<Value> RESOURCE_EXHAUSTED(
      Nan::New<Uint32, uint32_t>(GRPC_STATUS_RESOURCE_EXHAUSTED));
  Nan::Set(status, Nan::New("RESOURCE_EXHAUSTED").ToLocalChecked(),
           RESOURCE_EXHAUSTED);
  Local<Value> FAILED_PRECONDITION(
      Nan::New<Uint32, uint32_t>(GRPC_STATUS_FAILED_PRECONDITION));
  Nan::Set(status, Nan::New("FAILED_PRECONDITION").ToLocalChecked(),
           FAILED_PRECONDITION);
  Local<Value> ABORTED(Nan::New<Uint32, uint32_t>(GRPC_STATUS_ABORTED));
  Nan::Set(status, Nan::New("ABORTED").ToLocalChecked(), ABORTED);
  Local<Value> OUT_OF_RANGE(
      Nan::New<Uint32, uint32_t>(GRPC_STATUS_OUT_OF_RANGE));
  Nan::Set(status, Nan::New("OUT_OF_RANGE").ToLocalChecked(), OUT_OF_RANGE);
  Local<Value> UNIMPLEMENTED(
      Nan::New<Uint32, uint32_t>(GRPC_STATUS_UNIMPLEMENTED));
  Nan::Set(status, Nan::New("UNIMPLEMENTED").ToLocalChecked(), UNIMPLEMENTED);
  Local<Value> INTERNAL(Nan::New<Uint32, uint32_t>(GRPC_STATUS_INTERNAL));
  Nan::Set(status, Nan::New("INTERNAL").ToLocalChecked(), INTERNAL);
  Local<Value> UNAVAILABLE(Nan::New<Uint32, uint32_t>(GRPC_STATUS_UNAVAILABLE));
  Nan::Set(status, Nan::New("UNAVAILABLE").ToLocalChecked(), UNAVAILABLE);
  Local<Value> DATA_LOSS(Nan::New<Uint32, uint32_t>(GRPC_STATUS_DATA_LOSS));
  Nan::Set(status, Nan::New("DATA_LOSS").ToLocalChecked(), DATA_LOSS);
}

void InitCallErrorConstants(Local<Object> exports) {
  Nan::HandleScope scope;
  Local<Object> call_error = Nan::New<Object>();
  Nan::Set(exports, Nan::New("callError").ToLocalChecked(), call_error);
  Local<Value> OK(Nan::New<Uint32, uint32_t>(GRPC_CALL_OK));
  Nan::Set(call_error, Nan::New("OK").ToLocalChecked(), OK);
  Local<Value> CALL_ERROR(Nan::New<Uint32, uint32_t>(GRPC_CALL_ERROR));
  Nan::Set(call_error, Nan::New("ERROR").ToLocalChecked(), CALL_ERROR);
  Local<Value> NOT_ON_SERVER(
      Nan::New<Uint32, uint32_t>(GRPC_CALL_ERROR_NOT_ON_SERVER));
  Nan::Set(call_error, Nan::New("NOT_ON_SERVER").ToLocalChecked(),
           NOT_ON_SERVER);
  Local<Value> NOT_ON_CLIENT(
      Nan::New<Uint32, uint32_t>(GRPC_CALL_ERROR_NOT_ON_CLIENT));
  Nan::Set(call_error, Nan::New("NOT_ON_CLIENT").ToLocalChecked(),
           NOT_ON_CLIENT);
  Local<Value> ALREADY_INVOKED(
      Nan::New<Uint32, uint32_t>(GRPC_CALL_ERROR_ALREADY_INVOKED));
  Nan::Set(call_error, Nan::New("ALREADY_INVOKED").ToLocalChecked(),
           ALREADY_INVOKED);
  Local<Value> NOT_INVOKED(
      Nan::New<Uint32, uint32_t>(GRPC_CALL_ERROR_NOT_INVOKED));
  Nan::Set(call_error, Nan::New("NOT_INVOKED").ToLocalChecked(), NOT_INVOKED);
  Local<Value> ALREADY_FINISHED(
      Nan::New<Uint32, uint32_t>(GRPC_CALL_ERROR_ALREADY_FINISHED));
  Nan::Set(call_error, Nan::New("ALREADY_FINISHED").ToLocalChecked(),
           ALREADY_FINISHED);
  Local<Value> TOO_MANY_OPERATIONS(
      Nan::New<Uint32, uint32_t>(GRPC_CALL_ERROR_TOO_MANY_OPERATIONS));
  Nan::Set(call_error, Nan::New("TOO_MANY_OPERATIONS").ToLocalChecked(),
           TOO_MANY_OPERATIONS);
  Local<Value> INVALID_FLAGS(
      Nan::New<Uint32, uint32_t>(GRPC_CALL_ERROR_INVALID_FLAGS));
  Nan::Set(call_error, Nan::New("INVALID_FLAGS").ToLocalChecked(),
           INVALID_FLAGS);
}

void InitOpTypeConstants(Local<Object> exports) {
  Nan::HandleScope scope;
  Local<Object> op_type = Nan::New<Object>();
  Nan::Set(exports, Nan::New("opType").ToLocalChecked(), op_type);
  Local<Value> SEND_INITIAL_METADATA(
      Nan::New<Uint32, uint32_t>(GRPC_OP_SEND_INITIAL_METADATA));
  Nan::Set(op_type, Nan::New("SEND_INITIAL_METADATA").ToLocalChecked(),
           SEND_INITIAL_METADATA);
  Local<Value> SEND_MESSAGE(Nan::New<Uint32, uint32_t>(GRPC_OP_SEND_MESSAGE));
  Nan::Set(op_type, Nan::New("SEND_MESSAGE").ToLocalChecked(), SEND_MESSAGE);
  Local<Value> SEND_CLOSE_FROM_CLIENT(
      Nan::New<Uint32, uint32_t>(GRPC_OP_SEND_CLOSE_FROM_CLIENT));
  Nan::Set(op_type, Nan::New("SEND_CLOSE_FROM_CLIENT").ToLocalChecked(),
           SEND_CLOSE_FROM_CLIENT);
  Local<Value> SEND_STATUS_FROM_SERVER(
      Nan::New<Uint32, uint32_t>(GRPC_OP_SEND_STATUS_FROM_SERVER));
  Nan::Set(op_type, Nan::New("SEND_STATUS_FROM_SERVER").ToLocalChecked(),
           SEND_STATUS_FROM_SERVER);
  Local<Value> RECV_INITIAL_METADATA(
      Nan::New<Uint32, uint32_t>(GRPC_OP_RECV_INITIAL_METADATA));
  Nan::Set(op_type, Nan::New("RECV_INITIAL_METADATA").ToLocalChecked(),
           RECV_INITIAL_METADATA);
  Local<Value> RECV_MESSAGE(Nan::New<Uint32, uint32_t>(GRPC_OP_RECV_MESSAGE));
  Nan::Set(op_type, Nan::New("RECV_MESSAGE").ToLocalChecked(), RECV_MESSAGE);
  Local<Value> RECV_STATUS_ON_CLIENT(
      Nan::New<Uint32, uint32_t>(GRPC_OP_RECV_STATUS_ON_CLIENT));
  Nan::Set(op_type, Nan::New("RECV_STATUS_ON_CLIENT").ToLocalChecked(),
           RECV_STATUS_ON_CLIENT);
  Local<Value> RECV_CLOSE_ON_SERVER(
      Nan::New<Uint32, uint32_t>(GRPC_OP_RECV_CLOSE_ON_SERVER));
  Nan::Set(op_type, Nan::New("RECV_CLOSE_ON_SERVER").ToLocalChecked(),
           RECV_CLOSE_ON_SERVER);
}

void InitPropagateConstants(Local<Object> exports) {
  Nan::HandleScope scope;
  Local<Object> propagate = Nan::New<Object>();
  Nan::Set(exports, Nan::New("propagate").ToLocalChecked(), propagate);
  Local<Value> DEADLINE(Nan::New<Uint32, uint32_t>(GRPC_PROPAGATE_DEADLINE));
  Nan::Set(propagate, Nan::New("DEADLINE").ToLocalChecked(), DEADLINE);
  Local<Value> CENSUS_STATS_CONTEXT(
      Nan::New<Uint32, uint32_t>(GRPC_PROPAGATE_CENSUS_STATS_CONTEXT));
  Nan::Set(propagate, Nan::New("CENSUS_STATS_CONTEXT").ToLocalChecked(),
           CENSUS_STATS_CONTEXT);
  Local<Value> CENSUS_TRACING_CONTEXT(
      Nan::New<Uint32, uint32_t>(GRPC_PROPAGATE_CENSUS_TRACING_CONTEXT));
  Nan::Set(propagate, Nan::New("CENSUS_TRACING_CONTEXT").ToLocalChecked(),
           CENSUS_TRACING_CONTEXT);
  Local<Value> CANCELLATION(
      Nan::New<Uint32, uint32_t>(GRPC_PROPAGATE_CANCELLATION));
  Nan::Set(propagate, Nan::New("CANCELLATION").ToLocalChecked(), CANCELLATION);
  Local<Value> DEFAULTS(Nan::New<Uint32, uint32_t>(GRPC_PROPAGATE_DEFAULTS));
  Nan::Set(propagate, Nan::New("DEFAULTS").ToLocalChecked(), DEFAULTS);
}

void InitConnectivityStateConstants(Local<Object> exports) {
  Nan::HandleScope scope;
  Local<Object> channel_state = Nan::New<Object>();
  Nan::Set(exports, Nan::New("connectivityState").ToLocalChecked(),
           channel_state);
  Local<Value> IDLE(Nan::New<Uint32, uint32_t>(GRPC_CHANNEL_IDLE));
  Nan::Set(channel_state, Nan::New("IDLE").ToLocalChecked(), IDLE);
  Local<Value> CONNECTING(Nan::New<Uint32, uint32_t>(GRPC_CHANNEL_CONNECTING));
  Nan::Set(channel_state, Nan::New("CONNECTING").ToLocalChecked(), CONNECTING);
  Local<Value> READY(Nan::New<Uint32, uint32_t>(GRPC_CHANNEL_READY));
  Nan::Set(channel_state, Nan::New("READY").ToLocalChecked(), READY);
  Local<Value> TRANSIENT_FAILURE(
      Nan::New<Uint32, uint32_t>(GRPC_CHANNEL_TRANSIENT_FAILURE));
  Nan::Set(channel_state, Nan::New("TRANSIENT_FAILURE").ToLocalChecked(),
           TRANSIENT_FAILURE);
  Local<Value> FATAL_FAILURE(Nan::New<Uint32, uint32_t>(GRPC_CHANNEL_SHUTDOWN));
  Nan::Set(channel_state, Nan::New("FATAL_FAILURE").ToLocalChecked(),
           FATAL_FAILURE);
}

void InitWriteFlags(Local<Object> exports) {
  Nan::HandleScope scope;
  Local<Object> write_flags = Nan::New<Object>();
  Nan::Set(exports, Nan::New("writeFlags").ToLocalChecked(), write_flags);
  Local<Value> BUFFER_HINT(Nan::New<Uint32, uint32_t>(GRPC_WRITE_BUFFER_HINT));
  Nan::Set(write_flags, Nan::New("BUFFER_HINT").ToLocalChecked(), BUFFER_HINT);
  Local<Value> NO_COMPRESS(Nan::New<Uint32, uint32_t>(GRPC_WRITE_NO_COMPRESS));
  Nan::Set(write_flags, Nan::New("NO_COMPRESS").ToLocalChecked(), NO_COMPRESS);
}

void InitLogConstants(Local<Object> exports) {
  Nan::HandleScope scope;
  Local<Object> log_verbosity = Nan::New<Object>();
  Nan::Set(exports, Nan::New("logVerbosity").ToLocalChecked(), log_verbosity);
  Local<Value> LOG_DEBUG(Nan::New<Uint32, uint32_t>(GPR_LOG_SEVERITY_DEBUG));
  Nan::Set(log_verbosity, Nan::New("DEBUG").ToLocalChecked(), LOG_DEBUG);
  Local<Value> LOG_INFO(Nan::New<Uint32, uint32_t>(GPR_LOG_SEVERITY_INFO));
  Nan::Set(log_verbosity, Nan::New("INFO").ToLocalChecked(), LOG_INFO);
  Local<Value> LOG_ERROR(Nan::New<Uint32, uint32_t>(GPR_LOG_SEVERITY_ERROR));
  Nan::Set(log_verbosity, Nan::New("ERROR").ToLocalChecked(), LOG_ERROR);
}

NAN_METHOD(MetadataKeyIsLegal) {
  if (!info[0]->IsString()) {
    return Nan::ThrowTypeError("headerKeyIsLegal's argument must be a string");
  }
  Local<String> key = Nan::To<String>(info[0]).ToLocalChecked();
  grpc_slice slice = CreateSliceFromString(key);
  info.GetReturnValue().Set(static_cast<bool>(grpc_header_key_is_legal(slice)));
  grpc_slice_unref(slice);
}

NAN_METHOD(MetadataNonbinValueIsLegal) {
  if (!info[0]->IsString()) {
    return Nan::ThrowTypeError(
        "metadataNonbinValueIsLegal's argument must be a string");
  }
  Local<String> value = Nan::To<String>(info[0]).ToLocalChecked();
  grpc_slice slice = CreateSliceFromString(value);
  info.GetReturnValue().Set(
      static_cast<bool>(grpc_header_nonbin_value_is_legal(slice)));
  grpc_slice_unref(slice);
}

NAN_METHOD(MetadataKeyIsBinary) {
  if (!info[0]->IsString()) {
    return Nan::ThrowTypeError(
        "metadataKeyIsLegal's argument must be a string");
  }
  Local<String> key = Nan::To<String>(info[0]).ToLocalChecked();
  grpc_slice slice = CreateSliceFromString(key);
  info.GetReturnValue().Set(static_cast<bool>(grpc_is_binary_header(slice)));
  grpc_slice_unref(slice);
}

static grpc_ssl_roots_override_result get_ssl_roots_override(
    char **pem_root_certs_ptr) {
  *pem_root_certs_ptr = pem_root_certs;
  if (pem_root_certs == NULL) {
    return GRPC_SSL_ROOTS_OVERRIDE_FAIL;
  } else {
    return GRPC_SSL_ROOTS_OVERRIDE_OK;
  }
}

/* This should only be called once, and only before creating any
 *ServerCredentials */
NAN_METHOD(SetDefaultRootsPem) {
  if (!info[0]->IsString()) {
    return Nan::ThrowTypeError(
        "setDefaultRootsPem's argument must be a string");
  }
  Nan::Utf8String utf8_roots(info[0]);
  size_t length = static_cast<size_t>(utf8_roots.length());
  if (length > 0) {
    const char *data = *utf8_roots;
    pem_root_certs = (char *)gpr_malloc((length + 1) * sizeof(char));
    memcpy(pem_root_certs, data, length + 1);
  }
}

NAUV_WORK_CB(LogMessagesCallback) {
  Nan::HandleScope scope;
  std::queue<log_args *> args;
  uv_mutex_lock(&grpc_logger_state.mutex);
  grpc_logger_state.pending_args->swap(args);
  uv_mutex_unlock(&grpc_logger_state.mutex);
  /* Call the callback with each log message */
  while (!args.empty()) {
    log_args *arg = args.front();
    args.pop();
    Local<Value> file = Nan::New(arg->core_args.file).ToLocalChecked();
    Local<Value> line = Nan::New<Uint32, uint32_t>(arg->core_args.line);
    Local<Value> severity =
        Nan::New(gpr_log_severity_string(arg->core_args.severity))
            .ToLocalChecked();
    Local<Value> message = Nan::New(arg->core_args.message).ToLocalChecked();
    Local<Value> timestamp =
        Nan::New<v8::Date>(grpc::node::TimespecToMilliseconds(arg->timestamp))
            .ToLocalChecked();
    const int argc = 5;
    Local<Value> argv[argc] = {file, line, severity, message, timestamp};
    grpc_logger_state.callback->Call(argc, argv);
    delete[] arg->core_args.message;
    delete arg;
  }
}

void node_log_func(gpr_log_func_args *args) {
  // TODO(mlumish): Use the core's log formatter when it becomes available
  log_args *args_copy = new log_args;
  size_t message_len = strlen(args->message) + 1;
  char *message = new char[message_len];
  memcpy(message, args->message, message_len);
  memcpy(&args_copy->core_args, args, sizeof(gpr_log_func_args));
  args_copy->core_args.message = message;
  args_copy->timestamp = gpr_now(GPR_CLOCK_REALTIME);

  uv_mutex_lock(&grpc_logger_state.mutex);
  grpc_logger_state.pending_args->push(args_copy);
  uv_mutex_unlock(&grpc_logger_state.mutex);

  uv_async_send(&grpc_logger_state.async);
}

void init_logger() {
  memset(&grpc_logger_state, 0, sizeof(logger_state));
  grpc_logger_state.pending_args = new std::queue<log_args *>();
  uv_mutex_init(&grpc_logger_state.mutex);
  uv_async_init(uv_default_loop(), &grpc_logger_state.async,
                LogMessagesCallback);
  uv_unref((uv_handle_t *)&grpc_logger_state.async);
  grpc_logger_state.logger_set = false;

  gpr_log_verbosity_init();
}

/* This registers a JavaScript logger for messages from the gRPC core. Because
   that handler has to be run in the context of the JavaScript event loop, it
   will be run asynchronously. To minimize the problems that could cause for
   debugging, we leave core to do its default synchronous logging until a
   JavaScript logger is set */
NAN_METHOD(SetDefaultLoggerCallback) {
  if (!info[0]->IsFunction()) {
    return Nan::ThrowTypeError(
        "setDefaultLoggerCallback's argument must be a function");
  }
  if (!grpc_logger_state.logger_set) {
    gpr_set_log_function(node_log_func);
    grpc_logger_state.logger_set = true;
  }
  grpc_logger_state.callback = new Nan::Callback(info[0].As<v8::Function>());
}

NAN_METHOD(SetLogVerbosity) {
  if (!info[0]->IsUint32()) {
    return Nan::ThrowTypeError("setLogVerbosity's argument must be a number");
  }
  gpr_log_severity severity =
      static_cast<gpr_log_severity>(Nan::To<uint32_t>(info[0]).FromJust());
  gpr_set_log_verbosity(severity);
}

void init(Local<Object> exports) {
  Nan::HandleScope scope;
  grpc_init();
  grpc_set_ssl_roots_override_callback(get_ssl_roots_override);
  init_logger();

  InitStatusConstants(exports);
  InitCallErrorConstants(exports);
  InitOpTypeConstants(exports);
  InitPropagateConstants(exports);
  InitConnectivityStateConstants(exports);
  InitWriteFlags(exports);
  InitLogConstants(exports);

#ifdef GRPC_UV
  grpc_pollset_work_run_loop = 0;
#endif

  grpc::node::Call::Init(exports);
  grpc::node::CallCredentials::Init(exports);
  grpc::node::Channel::Init(exports);
  grpc::node::ChannelCredentials::Init(exports);
  grpc::node::Server::Init(exports);
  grpc::node::ServerCredentials::Init(exports);

  grpc::node::CompletionQueueInit(exports);

  // Attach a few utility functions directly to the module
  Nan::Set(exports, Nan::New("metadataKeyIsLegal").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(MetadataKeyIsLegal))
               .ToLocalChecked());
  Nan::Set(
      exports, Nan::New("metadataNonbinValueIsLegal").ToLocalChecked(),
      Nan::GetFunction(Nan::New<FunctionTemplate>(MetadataNonbinValueIsLegal))
          .ToLocalChecked());
  Nan::Set(exports, Nan::New("metadataKeyIsBinary").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(MetadataKeyIsBinary))
               .ToLocalChecked());
  Nan::Set(exports, Nan::New("setDefaultRootsPem").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(SetDefaultRootsPem))
               .ToLocalChecked());
  Nan::Set(
      exports, Nan::New("setDefaultLoggerCallback").ToLocalChecked(),
      Nan::GetFunction(Nan::New<FunctionTemplate>(SetDefaultLoggerCallback))
          .ToLocalChecked());
  Nan::Set(exports, Nan::New("setLogVerbosity").ToLocalChecked(),
           Nan::GetFunction(Nan::New<FunctionTemplate>(SetLogVerbosity))
               .ToLocalChecked());
}

NODE_MODULE(grpc_node, init)
