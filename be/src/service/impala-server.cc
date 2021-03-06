// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "service/impala-server.h"

#include <algorithm>
#include <exception>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/unordered_set.hpp>
#include <thrift/protocol/TDebugProtocol.h>
#include <boost/foreach.hpp>
#include <boost/bind.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <google/heap-profiler.h>
#include <google/malloc_extension.h>
#include <rapidjson/rapidjson.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "catalog/catalog-server.h"
#include "catalog/catalog-util.h"
#include "codegen/llvm-codegen.h"
#include "common/logging.h"
#include "common/version.h"
#include "exec/exec-node.h"
#include "exec/hdfs-table-sink.h"
#include "exec/scan-node.h"
#include "exprs/expr.h"
#include "rpc/authentication.h"
#include "rpc/thrift-util.h"
#include "rpc/thrift-server.h"
#include "rpc/thrift-thread.h"
#include "runtime/data-stream-mgr.h"
#include "runtime/client-cache.h"
#include "runtime/descriptors.h"
#include "runtime/data-stream-sender.h"
#include "runtime/row-batch.h"
#include "runtime/plan-fragment-executor.h"
#include "runtime/hdfs-fs-cache.h"
#include "runtime/exec-env.h"
#include "runtime/lib-cache.h"
#include "runtime/timestamp-value.h"
#include "service/query-exec-state.h"
#include "statestore/simple-scheduler.h"
#include "util/bit-util.h"
#include "util/container-util.h"
#include "util/debug-util.h"
#include "util/impalad-metrics.h"
#include "util/network-util.h"
#include "util/parse-util.h"
#include "util/string-parser.h"
#include "util/time.h"
#include "util/url-coding.h"
#include "util/webserver.h"

#include "gen-cpp/Types_types.h"
#include "gen-cpp/ImpalaService.h"
#include "gen-cpp/DataSinks_types.h"
#include "gen-cpp/Types_types.h"
#include "gen-cpp/ImpalaService.h"
#include "gen-cpp/ImpalaService_types.h"
#include "gen-cpp/ImpalaInternalService.h"
#include "gen-cpp/Frontend_types.h"

using namespace std;
using namespace boost;
using namespace boost::algorithm;
using namespace boost::filesystem;
using namespace boost::uuids;
using namespace apache::thrift;
using namespace apache::thrift::protocol;
using namespace apache::thrift::transport;
using namespace apache::thrift::server;
using namespace apache::thrift::concurrency;
using namespace apache::hive::service::cli::thrift;
using namespace beeswax;
using namespace boost::posix_time;

DECLARE_int32(be_port);
DECLARE_string(nn);
DECLARE_int32(nn_port);
DECLARE_bool(enable_process_lifetime_heap_profiling);
DECLARE_string(heap_profile_dir);
DECLARE_string(authorized_proxy_user_config);
DECLARE_bool(abort_on_config_error);

DEFINE_int32(beeswax_port, 21000, "port on which Beeswax client requests are served");
DEFINE_int32(hs2_port, 21050, "port on which HiveServer2 client requests are served");

DEFINE_int32(fe_service_threads, 64,
    "number of threads available to serve client requests");
DEFINE_int32(be_service_threads, 64,
    "(Advanced) number of threads available to serve backend execution requests");
DEFINE_string(default_query_options, "", "key=value pair of default query options for"
    " impalad, separated by ','");
DEFINE_int32(query_log_size, 25, "Number of queries to retain in the query log. If -1, "
                                 "the query log has unbounded size.");
DEFINE_bool(log_query_to_file, true, "if true, logs completed query profiles to file.");

// TODO: this logging should go into a per query log.
DEFINE_int32(log_mem_usage_interval, 0, "If non-zero, impalad will output memory usage "
    "every log_mem_usage_interval'th fragment completion.");

DEFINE_int32(max_audit_event_log_file_size, 5000, "The maximum size (in queries) of the "
    "audit event log file before a new one is created (if event logging is enabled)");
DEFINE_string(audit_event_log_dir, "", "The directory in which audit event log files are "
    "written. Setting this flag will enable audit event logging.");
DEFINE_bool(abort_on_failed_audit_event, true, "Shutdown Impala if there is a problem "
    "recording an audit event.");

DEFINE_string(profile_log_dir, "", "The directory in which profile log files are"
    " written. If blank, defaults to <log_file_dir>/profiles");
DEFINE_int32(max_profile_log_file_size, 5000, "The maximum size (in queries) of the "
    "profile log file before a new one is created");

DEFINE_int32(cancellation_thread_pool_size, 5,
    "(Advanced) Size of the thread-pool processing cancellations due to node failure");

DEFINE_string(ssl_server_certificate, "", "The full path to the SSL certificate file used"
    " to authenticate Impala to clients. If set, both Beeswax and HiveServer2 ports will "
    "only accept SSL connections");
DEFINE_string(ssl_private_key, "", "The full path to the private key used as a "
    "counterpart to the public key contained in --ssl_server_certificate. If "
    "--ssl_server_certificate is set, this option must be set as well.");
DEFINE_string(ssl_client_ca_certificate, "", "(Advanced) The full path to a certificate "
    "used by Thrift clients to check the validity of a server certificate. May either be "
    "a certificate for a third-party Certificate Authority, or a copy of the certificate "
    "the client expects to receive from the server.");

DEFINE_int32(idle_session_timeout, 0, "The time, in seconds, that a session may be idle"
    " for before it is closed (and all running queries cancelled) by Impala. If 0, idle"
    " sessions are never expired.");
DEFINE_int32(idle_query_timeout, 0, "The time, in seconds, that a query may be idle for"
    " (i.e. no processing work is done and no updates are received from the client) "
    "before it is cancelled. If 0, idle queries are never expired.");

namespace impala {

ThreadManager* fe_tm;
ThreadManager* be_tm;

// Prefix of profile and event log filenames. The version number is
// internal, and does not correspond to an Impala release - it should
// be changed only when the file format changes.
const string PROFILE_LOG_FILE_PREFIX = "impala_profile_log_1.0-";
const string AUDIT_EVENT_LOG_FILE_PREFIX = "impala_audit_event_log_1.0-";

const uint32_t MAX_CANCELLATION_QUEUE_SIZE = 65536;

const string BEESWAX_SERVER_NAME = "beeswax-frontend";
const string HS2_SERVER_NAME = "hiveserver2-frontend";

// Execution state of a single plan fragment.
class ImpalaServer::FragmentExecState {
 public:
  FragmentExecState(const TUniqueId& query_id, int backend_num,
                    const TUniqueId& fragment_instance_id, ExecEnv* exec_env,
                    const TNetworkAddress& coord_hostport)
    : query_id_(query_id),
      backend_num_(backend_num),
      fragment_instance_id_(fragment_instance_id),
      executor_(exec_env,
          bind<void>(mem_fn(&ImpalaServer::FragmentExecState::ReportStatusCb),
                     this, _1, _2, _3)),
      client_cache_(exec_env->client_cache()),
      coord_hostport_(coord_hostport) {
  }

  // Calling the d'tor releases all memory and closes all data streams
  // held by executor_.
  ~FragmentExecState() {
  }

  // Returns current execution status, if there was an error. Otherwise cancels
  // the fragment and returns OK.
  Status Cancel();

  // Call Prepare() and create and initialize data sink.
  Status Prepare(const TExecPlanFragmentParams& exec_params);

  // Main loop of plan fragment execution. Blocks until execution finishes.
  void Exec();

  const TUniqueId& query_id() const { return query_id_; }
  const TUniqueId& fragment_instance_id() const { return fragment_instance_id_; }

  void set_exec_thread(Thread* exec_thread) { exec_thread_.reset(exec_thread); }

 private:
  TUniqueId query_id_;
  int backend_num_;
  TUniqueId fragment_instance_id_;
  PlanFragmentExecutor executor_;
  ImpalaInternalServiceClientCache* client_cache_;
  TExecPlanFragmentParams exec_params_;

  // initiating coordinator to which we occasionally need to report back
  // (it's exported ImpalaInternalService)
  const TNetworkAddress coord_hostport_;

  // the thread executing this plan fragment
  scoped_ptr<Thread> exec_thread_;

  // protects exec_status_
  mutex status_lock_;

  // set in ReportStatusCb();
  // if set to anything other than OK, execution has terminated w/ an error
  Status exec_status_;

  // Callback for executor; updates exec_status_ if 'status' indicates an error
  // or if there was a thrift error.
  void ReportStatusCb(const Status& status, RuntimeProfile* profile, bool done);

  // Update exec_status_ w/ status, if the former isn't already an error.
  // Returns current exec_status_.
  Status UpdateStatus(const Status& status);
};

Status ImpalaServer::FragmentExecState::UpdateStatus(const Status& status) {
  lock_guard<mutex> l(status_lock_);
  if (!status.ok() && exec_status_.ok()) exec_status_ = status;
  return exec_status_;
}

Status ImpalaServer::FragmentExecState::Cancel() {
  lock_guard<mutex> l(status_lock_);
  RETURN_IF_ERROR(exec_status_);
  executor_.Cancel();
  return Status::OK;
}

Status ImpalaServer::FragmentExecState::Prepare(
    const TExecPlanFragmentParams& exec_params) {
  exec_params_ = exec_params;
  RETURN_IF_ERROR(executor_.Prepare(exec_params));
  executor_.OptimizeLlvmModule();
  return Status::OK;
}

void ImpalaServer::FragmentExecState::Exec() {
  // Open() does the full execution, because all plan fragments have sinks
  executor_.Open();
  executor_.Close();
}

// There can only be one of these callbacks in-flight at any moment, because
// it is only invoked from the executor's reporting thread.
// Also, the reported status will always reflect the most recent execution status,
// including the final status when execution finishes.
void ImpalaServer::FragmentExecState::ReportStatusCb(
    const Status& status, RuntimeProfile* profile, bool done) {
  DCHECK(status.ok() || done);  // if !status.ok() => done
  Status exec_status = UpdateStatus(status);

  Status coord_status;
  ImpalaInternalServiceConnection coord(client_cache_, coord_hostport_, &coord_status);
  if (!coord_status.ok()) {
    stringstream s;
    s << "couldn't get a client for " << coord_hostport_;
    UpdateStatus(Status(TStatusCode::INTERNAL_ERROR, s.str()));
    return;
  }

  TReportExecStatusParams params;
  params.protocol_version = ImpalaInternalServiceVersion::V1;
  params.__set_query_id(query_id_);
  params.__set_backend_num(backend_num_);
  params.__set_fragment_instance_id(fragment_instance_id_);
  exec_status.SetTStatus(&params);
  params.__set_done(done);
  profile->ToThrift(&params.profile);
  params.__isset.profile = true;

  RuntimeState* runtime_state = executor_.runtime_state();
  DCHECK(runtime_state != NULL);
  // Only send updates to insert status if fragment is finished, the coordinator
  // waits until query execution is done to use them anyhow.
  if (done) {
    TInsertExecStatus insert_status;

    if (runtime_state->hdfs_files_to_move()->size() > 0) {
      insert_status.__set_files_to_move(*runtime_state->hdfs_files_to_move());
    }
    if (runtime_state->num_appended_rows()->size() > 0) {
      insert_status.__set_num_appended_rows(*runtime_state->num_appended_rows());
    }
    if (runtime_state->insert_stats()->size() > 0) {
      insert_status.__set_insert_stats(*runtime_state->insert_stats());
    }

    params.__set_insert_exec_status(insert_status);
  }

  // Send new errors to coordinator
  runtime_state->GetUnreportedErrors(&(params.error_log));
  params.__isset.error_log = (params.error_log.size() > 0);

  TReportExecStatusResult res;
  Status rpc_status;
  try {
    try {
      coord->ReportExecStatus(res, params);
    } catch (TTransportException& e) {
      VLOG_RPC << "Retrying ReportExecStatus: " << e.what();
      rpc_status = coord.Reopen();
      if (!rpc_status.ok()) {
        // we need to cancel the execution of this fragment
        UpdateStatus(rpc_status);
        executor_.Cancel();
        return;
      }
      coord->ReportExecStatus(res, params);
    }
    rpc_status = Status(res.status);
  } catch (TException& e) {
    stringstream msg;
    msg << "ReportExecStatus() to " << coord_hostport_ << " failed:\n" << e.what();
    VLOG_QUERY << msg.str();
    rpc_status = Status(TStatusCode::INTERNAL_ERROR, msg.str());
  }

  if (!rpc_status.ok()) {
    // we need to cancel the execution of this fragment
    UpdateStatus(rpc_status);
    executor_.Cancel();
  }
}

const char* ImpalaServer::SQLSTATE_SYNTAX_ERROR_OR_ACCESS_VIOLATION = "42000";
const char* ImpalaServer::SQLSTATE_GENERAL_ERROR = "HY000";
const char* ImpalaServer::SQLSTATE_OPTIONAL_FEATURE_NOT_IMPLEMENTED = "HYC00";
const int ImpalaServer::ASCII_PRECISION = 16; // print 16 digits for double/float

// Work item for ImpalaServer::cancellation_thread_pool_.
class CancellationWork {
 public:
  CancellationWork(const TUniqueId& query_id, const Status& cause, bool unregister)
      : query_id_(query_id), cause_(cause), unregister_(unregister) {
  }

  CancellationWork() {
  }

  const TUniqueId& query_id() const { return query_id_; }
  const Status& cause() const { return cause_; }
  const bool unregister() const { return unregister_; }

  bool operator<(const CancellationWork& other) const {
    return query_id_ < other.query_id_;
  }

  bool operator==(const CancellationWork& other) const {
    return query_id_ == other.query_id_;
  }

 private:
  // Id of query to be canceled.
  TUniqueId query_id_;

  // Error status containing a list of failed impalads causing the cancellation.
  Status cause_;

  // If true, unregister the query rather than cancelling it. Calling UnregisterQuery()
  // does call CancelInternal eventually, but also ensures that the query is torn down and
  // archived.
  bool unregister_;
};

ImpalaServer::ImpalaServer(ExecEnv* exec_env)
    : exec_env_(exec_env) {
  // Initialize default config
  InitializeConfigVariables();

#ifndef ADDRESS_SANITIZER
  // tcmalloc and address sanitizer can not be used together
  if (FLAGS_enable_process_lifetime_heap_profiling) {
    HeapProfilerStart(FLAGS_heap_profile_dir.c_str());
  }
#endif

  frontend_.reset(new Frontend());

  Status status = frontend_->ValidateSettings();
  if (!status.ok()) {
    LOG(ERROR) << status.GetErrorMsg();
    if (FLAGS_abort_on_config_error) {
      LOG(ERROR) << "Aborting Impala Server startup due to improper configuration";
      exit(1);
    }
  }

  if (!InitProfileLogging().ok()) {
    LOG(ERROR) << "Query profile archival is disabled";
    FLAGS_log_query_to_file = false;
  }

  if (!InitAuditEventLogging().ok()) {
    LOG(ERROR) << "Aborting Impala Server startup due to failure initializing "
               << "audit event logging";
    exit(1);
  }

  if (!FLAGS_authorized_proxy_user_config.empty()) {
    // Parse the proxy user configuration using the format:
    // <proxy user>=<comma separated list of users they are allowed to impersonate>
    // See FLAGS_authorized_proxy_user_config for more details.
    vector<string> proxy_user_config;
    split(proxy_user_config, FLAGS_authorized_proxy_user_config, is_any_of(";"),
        token_compress_on);
    if (proxy_user_config.size() > 0) {
      BOOST_FOREACH(const string& config, proxy_user_config) {
        size_t pos = config.find("=");
        if (pos == string::npos) {
          LOG(ERROR) << "Invalid proxy user configuration. No mapping value specified "
                     << "for the proxy user. For more information review usage of the "
                     << "--authorized_proxy_user_config flag: " << config;
          exit(1);
        }
        string proxy_user = config.substr(0, pos);
        string config_str = config.substr(pos + 1);
        vector<string> parsed_allowed_users;
        split(parsed_allowed_users, config_str, is_any_of(","), token_compress_on);
        unordered_set<string> allowed_users(parsed_allowed_users.begin(),
            parsed_allowed_users.end());
        authorized_proxy_user_config_.insert(make_pair(proxy_user, allowed_users));
      }
    }
  }

  Webserver::PathHandlerCallback varz_callback =
      bind<void>(mem_fn(&ImpalaServer::RenderHadoopConfigs), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/varz", varz_callback);

  Webserver::PathHandlerCallback query_callback =
      bind<void>(mem_fn(&ImpalaServer::QueryStatePathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/queries", query_callback);

  Webserver::PathHandlerCallback sessions_callback =
      bind<void>(mem_fn(&ImpalaServer::SessionPathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/sessions", sessions_callback);

  Webserver::PathHandlerCallback catalog_callback =
      bind<void>(mem_fn(&ImpalaServer::CatalogPathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/catalog", catalog_callback);

  Webserver::PathHandlerCallback catalog_objects_callback =
      bind<void>(mem_fn(&ImpalaServer::CatalogObjectsPathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/catalog_objects",
      catalog_objects_callback, false, false);

  Webserver::PathHandlerCallback profile_callback =
      bind<void>(mem_fn(&ImpalaServer::QueryProfilePathHandler), this, _1, _2);
  exec_env->webserver()->
      RegisterPathHandler("/query_profile", profile_callback, true, false);

  Webserver::PathHandlerCallback cancel_callback =
      bind<void>(mem_fn(&ImpalaServer::CancelQueryPathHandler), this, _1, _2);
  exec_env->webserver()->
      RegisterPathHandler("/cancel_query", cancel_callback, true, false);

  Webserver::PathHandlerCallback profile_encoded_callback =
      bind<void>(mem_fn(&ImpalaServer::QueryProfileEncodedPathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/query_profile_encoded",
      profile_encoded_callback, false, false);

  Webserver::PathHandlerCallback inflight_query_ids_callback =
      bind<void>(mem_fn(&ImpalaServer::InflightQueryIdsPathHandler), this, _1, _2);
  exec_env->webserver()->RegisterPathHandler("/inflight_query_ids",
      inflight_query_ids_callback, false, false);

  // Initialize impalad metrics
  ImpaladMetrics::CreateMetrics(exec_env->metrics());
  ImpaladMetrics::IMPALA_SERVER_START_TIME->Update(
      TimestampValue::local_time().DebugString());

  // Register the membership callback if required
  if (exec_env->subscriber() != NULL) {
    StateStoreSubscriber::UpdateCallback cb =
        bind<void>(mem_fn(&ImpalaServer::MembershipCallback), this, _1, _2);
    exec_env->subscriber()->AddTopic(SimpleScheduler::IMPALA_MEMBERSHIP_TOPIC, true, cb);

    StateStoreSubscriber::UpdateCallback catalog_cb =
        bind<void>(mem_fn(&ImpalaServer::CatalogUpdateCallback), this, _1, _2);
    exec_env->subscriber()->AddTopic(
        CatalogServer::IMPALA_CATALOG_TOPIC, true, catalog_cb);
  }

  EXIT_IF_ERROR(UpdateCatalogMetrics());

  // Initialise the cancellation thread pool with 5 (by default) threads. The max queue
  // size is deliberately set so high that it should never fill; if it does the
  // cancellations will get ignored and retried on the next statestore heartbeat.
  cancellation_thread_pool_.reset(new ThreadPool<CancellationWork>(
          "impala-server", "cancellation-worker",
      FLAGS_cancellation_thread_pool_size, MAX_CANCELLATION_QUEUE_SIZE,
      bind<void>(&ImpalaServer::CancelFromThreadPool, this, _1, _2)));

  if (FLAGS_idle_session_timeout > 0) {
    session_timeout_thread_.reset(new Thread("impala-server", "session-expirer",
            bind<void>(&ImpalaServer::ExpireSessions, this)));
  }

  if (FLAGS_idle_query_timeout > 0) {
    query_expiration_thread_.reset(new Thread("impala-server", "query-expirer",
            bind<void>(&ImpalaServer::ExpireQueries, this)));
  }
}

Status ImpalaServer::LogAuditRecord(const ImpalaServer::QueryExecState& exec_state,
    const TExecRequest& request) {
  stringstream ss;
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();
  // Each log entry is a timestamp mapped to a JSON object
  ss << ms_since_epoch();
  writer.String(ss.str().c_str());
  writer.StartObject();
  writer.String("query_id");
  writer.String(PrintId(exec_state.query_id()).c_str());
  writer.String("session_id");
  writer.String(PrintId(exec_state.session_id()).c_str());
  writer.String("start_time");
  writer.String(exec_state.start_time().DebugString().c_str());
  writer.String("authorization_failure");
  writer.Bool(Frontend::IsAuthorizationError(exec_state.query_status()));
  writer.String("status");
  writer.String(exec_state.query_status().GetErrorMsg().c_str());
  writer.String("user");
  // If do_as_user() is empty, the "user" field should be set to the connected user
  // and "impersonator" field should be Null. Otherwise, the "user" should be set to
  // the current do_as_user() and "impersonator" should be the connected user.
  if (exec_state.do_as_user().empty()) {
    writer.String(exec_state.user().c_str());
    writer.String("impersonator");
    writer.Null();
  } else {
    writer.String(exec_state.do_as_user().c_str());
    writer.String("impersonator");
    writer.String(exec_state.user().c_str());
  }
  writer.String("statement_type");
  if (request.stmt_type == TStmtType::DDL) {
    if (request.catalog_op_request.op_type == TCatalogOpType::DDL) {
      writer.String(
          PrintTDdlType(request.catalog_op_request.ddl_params.ddl_type).c_str());
    } else {
      writer.String(PrintTCatalogOpType(request.catalog_op_request.op_type).c_str());
    }
  } else {
    writer.String(PrintTStmtType(request.stmt_type).c_str());
  }
  writer.String("network_address");
  writer.String(
      lexical_cast<string>(exec_state.parent_session()->network_address).c_str());
  writer.String("sql_statement");
  writer.String(replace_all_copy(exec_state.sql_stmt(), "\n", " ").c_str());
  writer.String("catalog_objects");
  writer.StartArray();
  BOOST_FOREACH(const TAccessEvent& event, request.access_events) {
    writer.StartObject();
    writer.String("name");
    writer.String(event.name.c_str());
    writer.String("object_type");
    writer.String(PrintTCatalogObjectType(event.object_type).c_str());
    writer.String("privilege");
    writer.String(event.privilege.c_str());
    writer.EndObject();
  }
  writer.EndArray();
  writer.EndObject();
  writer.EndObject();
  Status status = audit_event_logger_->AppendEntry(buffer.GetString());
  if (!status.ok()) {
    LOG(ERROR) << "Unable to record audit event record: " << status.GetErrorMsg();
    if (FLAGS_abort_on_failed_audit_event) {
      LOG(ERROR) << "Shutting down Impala Server due to abort_on_failed_audit_event=true";
      exit(1);
    }
  }
  return status;
}

bool ImpalaServer::IsAuditEventLoggingEnabled() {
  return !FLAGS_audit_event_log_dir.empty();
}

Status ImpalaServer::InitAuditEventLogging() {
  if (!IsAuditEventLoggingEnabled()) {
    LOG(INFO) << "Event logging is disabled";
    return Status::OK;
  }
  audit_event_logger_.reset(new SimpleLogger(FLAGS_audit_event_log_dir,
     AUDIT_EVENT_LOG_FILE_PREFIX, FLAGS_max_audit_event_log_file_size));
  RETURN_IF_ERROR(audit_event_logger_->Init());
  audit_event_logger_flush_thread_.reset(new Thread("impala-server",
        "audit-event-log-flush", &ImpalaServer::AuditEventLoggerFlushThread, this));
  return Status::OK;
}

Status ImpalaServer::InitProfileLogging() {
  if (!FLAGS_log_query_to_file) return Status::OK;

  if (FLAGS_profile_log_dir.empty()) {
    stringstream ss;
    ss << FLAGS_log_dir << "/profiles/";
    FLAGS_profile_log_dir = ss.str();
  }
  profile_logger_.reset(new SimpleLogger(FLAGS_profile_log_dir,
      PROFILE_LOG_FILE_PREFIX, FLAGS_max_profile_log_file_size));
  RETURN_IF_ERROR(profile_logger_->Init());
  profile_log_file_flush_thread_.reset(new Thread("impala-server", "log-flush-thread",
      &ImpalaServer::LogFileFlushThread, this));

  return Status::OK;
}

void ImpalaServer::RenderHadoopConfigs(const Webserver::ArgumentMap& args,
    stringstream* output) {
  frontend_->RenderHadoopConfigs(args.find("raw") != args.end(), output);
}

// We expect the query id to be passed as one parameter, 'query_id'.
// Returns true if the query id was present and valid; false otherwise.
static bool ParseQueryId(const Webserver::ArgumentMap& args, TUniqueId* id) {
  Webserver::ArgumentMap::const_iterator it = args.find("query_id");
  if (it == args.end()) {
    return false;
  } else {
    return ParseId(it->second, id);
  }
}

void ImpalaServer::CancelQueryPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  TUniqueId unique_id;
  if (!ParseQueryId(args, &unique_id)) {
    (*output) << "Invalid query id";
    return;
  }
  Status status("Cancelled from Impala's debug web interface");
  if (UnregisterQuery(unique_id, &status)) {
    (*output) << "Query cancellation successful";
  } else {
    (*output) << "Error cancelling query: " << unique_id << " not found";
  }
}

void ImpalaServer::QueryProfilePathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  TUniqueId unique_id;
  if (!ParseQueryId(args, &unique_id)) {
    (*output) << "Invalid query id";
    return;
  }

  if (args.find("raw") == args.end()) {
    (*output) << "<pre>";
    stringstream ss;
    Status status = GetRuntimeProfileStr(unique_id, false, &ss);
    if (!status.ok()) {
      (*output) << status.GetErrorMsg();
    } else {
      EscapeForHtml(ss.str(), output);
    }
    (*output) << "</pre>";
  } else {
    Status status = GetRuntimeProfileStr(unique_id, false, output);
    if (!status.ok()) {
      (*output) << status.GetErrorMsg();
    }
  }
}

void ImpalaServer::QueryProfileEncodedPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  TUniqueId unique_id;
  if (!ParseQueryId(args, &unique_id)) {
    (*output) << "Invalid query id";
    return;
  }

  Status status = GetRuntimeProfileStr(unique_id, true, output);
  if (!status.ok()) (*output) << status.GetErrorMsg();
}

void ImpalaServer::InflightQueryIdsPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  lock_guard<mutex> l(query_exec_state_map_lock_);
  BOOST_FOREACH(const QueryExecStateMap::value_type& exec_state, query_exec_state_map_) {
    *output << exec_state.second->query_id() << "\n";
  }
}

Status ImpalaServer::GetRuntimeProfileStr(const TUniqueId& query_id,
    bool base64_encoded, stringstream* output) {
  DCHECK(output != NULL);
  // Search for the query id in the active query map
  {
    lock_guard<mutex> l(query_exec_state_map_lock_);
    QueryExecStateMap::const_iterator exec_state = query_exec_state_map_.find(query_id);
    if (exec_state != query_exec_state_map_.end()) {
      if (base64_encoded) {
        exec_state->second->profile().SerializeToArchiveString(output);
      } else {
        exec_state->second->profile().PrettyPrint(output);
      }
      return Status::OK;
    }
  }

  // The query was not found the active query map, search the query log.
  {
    lock_guard<mutex> l(query_log_lock_);
    QueryLogIndex::const_iterator query_record = query_log_index_.find(query_id);
    if (query_record == query_log_index_.end()) {
      stringstream ss;
      ss << "Query id " << PrintId(query_id) << " not found.";
      return Status(ss.str());
    }
    if (base64_encoded) {
      (*output) << query_record->second->encoded_profile_str;
    } else {
      (*output) << query_record->second->profile_str;
    }
  }
  return Status::OK;
}

void ImpalaServer::RenderSingleQueryTableRow(const ImpalaServer::QueryStateRecord& record,
    bool render_end_time, bool render_cancel, stringstream* output) {
  (*output) << "<tr>"
            << "<td>" << record.user << "</td>"
            << "<td>" << record.default_db << "</td>"
            << "<td>" << record.stmt << "</td>"
            << "<td>"
            << _TStmtType_VALUES_TO_NAMES.find(record.stmt_type)->second
            << "</td>";

  // Output start/end times
  (*output) << "<td>" << record.start_time.DebugString() << "</td>";
  if (render_end_time) {
    (*output) << "<td>" << record.end_time.DebugString() << "</td>";
  }

  // Output progress
  (*output) << "<td>";
  if (record.has_coord == false) {
    (*output) << "N/A";
  } else {
    (*output) << record.num_complete_fragments << " / " << record.total_fragments
              << " (" << setw(4);
    if (record.total_fragments == 0) {
      (*output) << " (0%)";
    } else {
      (*output) <<
          (100.0 * record.num_complete_fragments / (1.f * record.total_fragments))
                << "%)";
    }
  }

  // Output state and rows fetched
  (*output) << "</td>"
            << "<td>" << _QueryState_VALUES_TO_NAMES.find(record.query_state)->second
            << "</td><td>" << record.num_rows_fetched << "</td>";

  // Output profile
  (*output) << "<td><a href='/query_profile?query_id=" << record.id
            << "'>Profile</a></td>";
  if (render_cancel) {
    (*output) << "<td><a href='/cancel_query?query_id=" << record.id
              << "'>Cancel</a></td>";
  }
  (*output) << "</tr>" << endl;
}

void ImpalaServer::QueryStatePathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  set<QueryStateRecord, QueryStateRecord> sorted_query_records;
  {
    lock_guard<mutex> l(query_exec_state_map_lock_);
    (*output) << "<h2>Queries</h2>";
    (*output) << "This page lists all running queries, plus any completed queries that "
"are archived in memory. The size of that archive is controlled with the "
"--query_log_size command line parameter.";
    (*output) << "<h3>" << query_exec_state_map_.size() << " queries in flight</h3>"
              << endl;
    (*output) << "<table class='table table-hover table-border'><tr>"
              << "<th>User</th>" << endl
              << "<th>Default Db</th>" << endl
              << "<th>Statement</th>" << endl
              << "<th>Query Type</th>" << endl
              << "<th>Start Time</th>" << endl
              << "<th>Backend Progress</th>" << endl
              << "<th>State</th>" << endl
              << "<th># rows fetched</th>" << endl
              << "<th>Profile</th>" << endl
              << "<th>Action</th>" << endl
              << "</tr>";
    BOOST_FOREACH(
        const QueryExecStateMap::value_type& exec_state, query_exec_state_map_) {
      // TODO: Do this in the browser so that sorts on other keys are possible.
      sorted_query_records.insert(QueryStateRecord(*exec_state.second));
    }
  }

  BOOST_FOREACH(const QueryStateRecord& record, sorted_query_records) {
    RenderSingleQueryTableRow(record, false, true, output);
  }

  (*output) << "</table>";

  // Print the query location counts.
  (*output) << "<h3>Query Locations</h3>";
  (*output) << "<table class='table table-hover table-bordered'>";
  (*output) << "<tr><th>Location</th><th>Number of Fragments</th></tr>" << endl;
  {
    lock_guard<mutex> l(query_locations_lock_);
    BOOST_FOREACH(const QueryLocations::value_type& location, query_locations_) {
      (*output) << "<tr><td>" << location.first << "<td><b>" << location.second.size()
                << "</b></td></tr>";
    }
  }
  (*output) << "</table>";

  // Print the query log
  if (FLAGS_query_log_size == 0) {
    (*output) << "<h3>No queries archived in memory (--query_log_size is 0)</h3>";
    return;
  }

  if (FLAGS_query_log_size > 0) {
    (*output) << "<h3>Last " << FLAGS_query_log_size << " Completed Queries</h3>";
  } else {
    (*output) << "<h3>All Completed Queries</h3>";
  }
  (*output) << "<table class='table table-hover table-border'><tr>"
            << "<th>User</th>" << endl
            << "<th>Default Db</th>" << endl
            << "<th>Statement</th>" << endl
            << "<th>Query Type</th>" << endl
            << "<th>Start Time</th>" << endl
            << "<th>End Time</th>" << endl
            << "<th>Backend Progress</th>" << endl
            << "<th>State</th>" << endl
            << "<th># rows fetched</th>" << endl
            << "<th>Profile</th>" << endl
            << "</tr>";

  {
    lock_guard<mutex> l(query_log_lock_);
    BOOST_FOREACH(const QueryStateRecord& log_entry, query_log_) {
      RenderSingleQueryTableRow(log_entry, true, false, output);
    }
  }

  (*output) << "</table>";
}

void ImpalaServer::SessionPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  (*output) << "<h2>Sessions</h2>" << endl;
  lock_guard<mutex> l(session_state_map_lock_);
  (*output) << "There are " << session_state_map_.size() << " active sessions." << endl
            << "<table class='table table-bordered table-hover'>"
            << "<tr><th>Session Type</th>"
            << "<th>Open Queries</th>"
            << "<th>User</th>"
            << "<th>Session ID</th>"
            << "<th>Network Address</th>"
            << "<th>Default Database</th>"
            << "<th>Start Time</th>"
            << "<th>Last Accessed</th>"
            << "<th>Expired</th>"
            << "<th>Closed</th>"
            << "<th>Ref count</th>"
            <<"</tr>"
            << endl;
  (*output) << boolalpha;
  BOOST_FOREACH(const SessionStateMap::value_type& session, session_state_map_) {
    (*output) << "<tr>"
              << "<td>" << PrintTSessionType(session.second->session_type) << "</td>"
              << "<td>" << session.second->inflight_queries.size() << "</td>"
              << "<td>" << session.second->user << "</td>"
              << "<td>" << session.first << "</td>"
              << "<td>" << session.second->network_address << "</td>"
              << "<td>" << session.second->database << "</td>"
              << "<td>" << session.second->start_time.DebugString() << "</td>"
              << "<td>" << session.second->last_accessed.DebugString() << "</td>"
              << "<td>" << session.second->expired << "</td>"
              << "<td>" << session.second->closed << "</td>"
              << "<td>" << session.second->ref_count << "</td>"
              << "</tr>";
  }
  (*output) << "</table>";
}

void ImpalaServer::CatalogPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  TGetDbsResult get_dbs_result;
  Status status = frontend_->GetDbNames(NULL, NULL, &get_dbs_result);
  if (!status.ok()) {
    (*output) << "Error: " << status.GetErrorMsg();
    return;
  }
  vector<string>& db_names = get_dbs_result.dbs;

  if (args.find("raw") == args.end()) {
    (*output) << "<h2>Catalog</h2>" << endl;

    // Build a navigation string like [ default | tpch | ... ]
    vector<string> links;
    BOOST_FOREACH(const string& db, db_names) {
      stringstream ss;
      ss << "<a href='#" << db << "'>" << db << "</a>";
      links.push_back(ss.str());
    }
    (*output) << "[ " <<  join(links, " | ") << " ] ";

    BOOST_FOREACH(const string& db, db_names) {
      (*output) << "<a id='" << db << "'><h3>" << db << "</h3></a>";
      TGetTablesResult get_table_results;
      Status status = frontend_->GetTableNames(db, NULL, NULL, &get_table_results);
      if (!status.ok()) {
        (*output) << "Error: " << status.GetErrorMsg();
        continue;
      }
      vector<string>& table_names = get_table_results.tables;
      (*output) << "<p>" << db << " contains <b>" << table_names.size()
                << "</b> tables</p>";

      (*output) << "<ul>" << endl;
      BOOST_FOREACH(const string& table, table_names) {
        (*output) << "<li>" << table << "</li>" << endl;
      }
      (*output) << "</ul>" << endl;
    }
  } else {
    (*output) << "Catalog" << endl << endl;
    (*output) << "List of databases:" << endl;
    (*output) << join(db_names, "\n") << endl << endl;

    BOOST_FOREACH(const string& db, db_names) {
      TGetTablesResult get_table_results;
      Status status = frontend_->GetTableNames(db, NULL, NULL, &get_table_results);
      if (!status.ok()) {
        (*output) << "Error: " << status.GetErrorMsg();
        continue;
      }
      vector<string>& table_names = get_table_results.tables;
      (*output) << db << " contains " << table_names.size()
                << " tables" << endl;
      BOOST_FOREACH(const string& table, table_names) {
        (*output) << "- " << table << endl;
      }
      (*output) << endl << endl;
    }
  }
}

void ImpalaServer::CatalogObjectsPathHandler(const Webserver::ArgumentMap& args,
    stringstream* output) {
  Webserver::ArgumentMap::const_iterator object_type_arg = args.find("object_type");
  Webserver::ArgumentMap::const_iterator object_name_arg = args.find("object_name");
  if (object_type_arg != args.end() && object_name_arg != args.end()) {
    TCatalogObjectType::type object_type =
        TCatalogObjectTypeFromName(object_type_arg->second);

    // Get the object type and name from the topic entry key
    TCatalogObject request;
    TCatalogObjectFromObjectName(object_type, object_name_arg->second, &request);

    // Get the object and dump its contents.
    TCatalogObject result;
    Status status = frontend_->GetCatalogObject(request, &result);
    if (status.ok()) {
      if (args.find("raw") == args.end()) {
        (*output) << "<pre>" << ThriftDebugString(result) << "</pre>";
      } else {
        (*output) << ThriftDebugString(result);
      }
    } else {
      (*output) << status.GetErrorMsg();
    }
  } else {
    (*output) << "Please specify values for the object_type and object_name parameters.";
  }
}

void ImpalaServer::LogFileFlushThread() {
  while (true) {
    sleep(5);
    profile_logger_->Flush();
  }
}

void ImpalaServer::AuditEventLoggerFlushThread() {
  while (true) {
    sleep(5);
    Status status = audit_event_logger_->Flush();
    if (!status.ok()) {
      LOG(ERROR) << "Error flushing audit event log: " << status.GetErrorMsg();
      if (FLAGS_abort_on_failed_audit_event) {
        LOG(ERROR) << "Shutting down Impala Server due to "
                   << "abort_on_failed_audit_event=true";
        exit(1);
      }
    }
  }
}

void ImpalaServer::ArchiveQuery(const QueryExecState& query) {
  string encoded_profile_str = query.profile().SerializeToArchiveString();

  if (VLOG_QUERY_IS_ON) {
    stringstream ss;
    ss << "Final profile for query_id=" << query.query_id() << endl
       << encoded_profile_str;
    VLOG_QUERY << ss.str();
  }

  // If there was an error initialising archival (e.g. directory is
  // not writeable), FLAGS_log_query_to_file will have been set to
  // false
  if (FLAGS_log_query_to_file) {
    int64_t timestamp = time_since_epoch().total_milliseconds();
    stringstream ss;
    ss << timestamp << " " << query.query_id() << " " << encoded_profile_str;
    Status status = profile_logger_->AppendEntry(ss.str());
    if (!status.ok()) {
      LOG_EVERY_N(WARNING, 1000) << "Could not write to profile log file file ("
                                 << google::COUNTER << " attempts failed): "
                                 << status.GetErrorMsg();
      LOG_EVERY_N(WARNING, 1000)
          << "Disable query logging with --log_query_to_file=false";
    }
  }

  if (FLAGS_query_log_size == 0) return;
  QueryStateRecord record(query, true, encoded_profile_str);
  {
    lock_guard<mutex> l(query_log_lock_);
    // Add record to the beginning of the log, and to the lookup index.
    query_log_index_[query.query_id()] = query_log_.insert(query_log_.begin(), record);

    if (FLAGS_query_log_size > -1 && FLAGS_query_log_size < query_log_.size()) {
      DCHECK_EQ(query_log_.size() - FLAGS_query_log_size, 1);
      query_log_index_.erase(query_log_.back().id);
      query_log_.pop_back();
    }
  }
}

ImpalaServer::~ImpalaServer() {}

Status ImpalaServer::Execute(const TClientRequest& request,
    shared_ptr<SessionState> session_state,
    const TSessionState& query_session_state,
    shared_ptr<QueryExecState>* exec_state) {
  bool registered_exec_state;
  ImpaladMetrics::IMPALA_SERVER_NUM_QUERIES->Increment(1L);
  Status status = ExecuteInternal(request, session_state, query_session_state,
      &registered_exec_state, exec_state);
  if (!status.ok() && registered_exec_state) {
    UnregisterQuery((*exec_state)->query_id(), &status);
  }
  return status;
}

Status ImpalaServer::ExecuteInternal(
    const TClientRequest& request,
    shared_ptr<SessionState> session_state,
    const TSessionState& query_session_state,
    bool* registered_exec_state,
    shared_ptr<QueryExecState>* exec_state) {
  DCHECK(session_state != NULL);

  *registered_exec_state = false;

  exec_state->reset(new QueryExecState(
      exec_env_, frontend_.get(), this, session_state, query_session_state, request.stmt));

  (*exec_state)->query_events()->MarkEvent("Start execution");

  TExecRequest result;
  {
    // Keep a lock on exec_state so that registration and setting
    // result_metadata are atomic.
    //
    // Note: this acquires the exec_state lock *before* the
    // query_exec_state_map_ lock. This is the opposite of
    // GetQueryExecState(..., true), and therefore looks like a
    // candidate for deadlock. The reason this works here is that
    // GetQueryExecState cannot find exec_state (under the exec state
    // map lock) and take it's lock until RegisterQuery has
    // finished. By that point, the exec state map lock will have been
    // given up, so the classic deadlock interleaving is not possible.
    lock_guard<mutex> l(*(*exec_state)->lock());

    // register exec state as early as possible so that queries that
    // take a long time to plan show up, and to handle incoming status
    // reports before execution starts.
    RETURN_IF_ERROR(RegisterQuery(session_state, *exec_state));
    *registered_exec_state = true;

    RETURN_IF_ERROR((*exec_state)->UpdateQueryStatus(
        frontend_->GetExecRequest(request, &result)));
    (*exec_state)->query_events()->MarkEvent("Planning finished");
    if (result.__isset.result_set_metadata) {
      (*exec_state)->set_result_metadata(result.result_set_metadata);
    }
  }
  if (IsAuditEventLoggingEnabled()) {
    LogAuditRecord(*(exec_state->get()), result);
  }

  // start execution of query; also starts fragment status reports
  RETURN_IF_ERROR((*exec_state)->Exec(&result));
  if (result.stmt_type == TStmtType::DDL) {
    Status status = UpdateCatalogMetrics();
    if (!status.ok()) {
      VLOG_QUERY << "Couldn't update catalog metrics: " << status.GetErrorMsg();
    }
  }

  if ((*exec_state)->coord() != NULL) {
    const unordered_set<TNetworkAddress>& unique_hosts =
        (*exec_state)->coord()->unique_hosts();
    if (!unique_hosts.empty()) {
      lock_guard<mutex> l(query_locations_lock_);
      BOOST_FOREACH(const TNetworkAddress& port, unique_hosts) {
        query_locations_[port].insert((*exec_state)->query_id());
      }
    }
  }
  return Status::OK;
}

Status ImpalaServer::RegisterQuery(shared_ptr<SessionState> session_state,
    const shared_ptr<QueryExecState>& exec_state) {
  lock_guard<mutex> l2(session_state->lock);
  if (session_state->closed) {
    return Status("Session has been closed, ignoring query.");
  }

  if (session_state->expired) {
    return Status("Session has expired, ignoring query.");
  }

  const TUniqueId& query_id = exec_state->query_id();
  {
    lock_guard<mutex> l(query_exec_state_map_lock_);
    QueryExecStateMap::iterator entry = query_exec_state_map_.find(query_id);
    if (entry != query_exec_state_map_.end()) {
      // There shouldn't be an active query with that same id.
      // (query_id is globally unique)
      stringstream ss;
      ss << "query id " << PrintId(query_id) << " already exists";
      return Status(TStatusCode::INTERNAL_ERROR, ss.str());
    }

    session_state->inflight_queries.insert(query_id);
    query_exec_state_map_.insert(make_pair(query_id, exec_state));
  }

  if (FLAGS_idle_query_timeout > 0) {
    lock_guard<mutex> l(query_expiration_lock_);
    queries_by_timestamp_.insert(
        make_pair(ms_since_epoch() + (1000 * FLAGS_idle_query_timeout), query_id));
  }
  return Status::OK;
}

bool ImpalaServer::UnregisterQuery(const TUniqueId& query_id, const Status* cause) {
  VLOG_QUERY << "UnregisterQuery(): query_id=" << query_id;

  // Cancel the query if it's still running
  CancelInternal(query_id, cause);

  shared_ptr<QueryExecState> exec_state;
  {
    lock_guard<mutex> l(query_exec_state_map_lock_);
    QueryExecStateMap::iterator entry = query_exec_state_map_.find(query_id);
    if (entry == query_exec_state_map_.end()) {
      VLOG_QUERY << "unknown query id: " << PrintId(query_id);
      return false;
    } else {
      exec_state = entry->second;
    }
    query_exec_state_map_.erase(entry);
  }

  // Ignore all audit events except for those due to an AuthorizationException.
  if (IsAuditEventLoggingEnabled() &&
      Frontend::IsAuthorizationError(exec_state->query_status())) {
    LogAuditRecord(*exec_state.get(), exec_state->exec_request());
  }
  exec_state->Done();

  {
    lock_guard<mutex> l(exec_state->parent_session()->lock);
    exec_state->parent_session()->inflight_queries.erase(query_id);
  }

  ArchiveQuery(*exec_state);

  if (exec_state->coord() != NULL) {
    const unordered_set<TNetworkAddress>& unique_hosts =
        exec_state->coord()->unique_hosts();
    if (!unique_hosts.empty()) {
      lock_guard<mutex> l(query_locations_lock_);
      BOOST_FOREACH(const TNetworkAddress& hostport, unique_hosts) {
        // Query may have been removed already by cancellation path. In
        // particular, if node to fail was last sender to an exchange, the
        // coordinator will realise and fail the query at the same time the
        // failure detection path does the same thing. They will harmlessly race
        // to remove the query from this map.
        QueryLocations::iterator it = query_locations_.find(hostport);
        if (it != query_locations_.end()) {
          it->second.erase(exec_state->query_id());
        }
      }
    }
  }

  return true;
}

void ImpalaServer::Wait(shared_ptr<QueryExecState> exec_state) {
  // block until results are ready
  Status status = exec_state->Wait();
  {
    lock_guard<mutex> l(*(exec_state->lock()));
    if (exec_state->returns_result_set()) {
      exec_state->query_events()->MarkEvent("Rows available");
    } else {
      exec_state->query_events()->MarkEvent("Request finished");
    }

    exec_state->UpdateQueryStatus(status);
  }
  if (status.ok()) {
    exec_state->UpdateQueryState(QueryState::FINISHED);
  }
}

Status ImpalaServer::UpdateCatalogMetrics() {
  TGetDbsResult db_names;
  RETURN_IF_ERROR(frontend_->GetDbNames(NULL, NULL, &db_names));
  ImpaladMetrics::CATALOG_NUM_DBS->Update(db_names.dbs.size());
  ImpaladMetrics::CATALOG_NUM_TABLES->Update(0L);
  BOOST_FOREACH(const string& db, db_names.dbs) {
    TGetTablesResult table_names;
    RETURN_IF_ERROR(frontend_->GetTableNames(db, NULL, NULL, &table_names));
    ImpaladMetrics::CATALOG_NUM_TABLES->Increment(table_names.tables.size());
  }

  return Status::OK;
}

Status ImpalaServer::CancelInternal(const TUniqueId& query_id, const Status* cause) {
  VLOG_QUERY << "Cancel(): query_id=" << PrintId(query_id);
  shared_ptr<QueryExecState> exec_state = GetQueryExecState(query_id, true);
  if (exec_state == NULL) return Status("Invalid or unknown query handle");

  lock_guard<mutex> l(*exec_state->lock(), adopt_lock_t());
  // TODO: can we call Coordinator::Cancel() here while holding lock?
  exec_state->Cancel(cause);
  return Status::OK;
}

void ImpalaServer::CancelSessionQueriesAsync(SessionState* session_state,
    const string& cause) {
  unordered_set<TUniqueId> inflight_queries;
  {
    lock_guard<mutex> l(session_state->lock);
    // Once closed is set to true, no more queries can be started. The inflight list
    // will not grow.
    inflight_queries.insert(session_state->inflight_queries.begin(),
        session_state->inflight_queries.end());
  }

  // Unregister all open queries from this session.
  Status status(cause);
  BOOST_FOREACH(const TUniqueId& query_id, inflight_queries) {
    cancellation_thread_pool_->Offer(CancellationWork(query_id, status, true));
  }
}

void ImpalaServer::CancelSessionQueries(SessionState* session_state) {
  unordered_set<TUniqueId> inflight_queries;
  {
    lock_guard<mutex> l(session_state->lock);
    // Once closed is set to true, no more queries can be started. The inflight list
    // will not grow.
    inflight_queries.insert(session_state->inflight_queries.begin(),
        session_state->inflight_queries.end());
  }

  // Unregister all open queries from this session.
  Status status("Session closed");
  BOOST_FOREACH(const TUniqueId& query_id, inflight_queries) {
    UnregisterQuery(query_id, &status);
  }
}

Status ImpalaServer::CloseSessionInternal(const TUniqueId& session_id,
    bool ignore_if_absent) {
  // Find the session_state and remove it from the map.
  shared_ptr<SessionState> session_state;
  {
    lock_guard<mutex> l(session_state_map_lock_);
    SessionStateMap::iterator entry = session_state_map_.find(session_id);
    if (entry == session_state_map_.end()) {
      if (ignore_if_absent) {
        return Status::OK;
      } else {
        return Status("Invalid session ID");
      }
    }
    session_state = entry->second;
    session_state_map_.erase(session_id);
  }
  DCHECK(session_state != NULL);
  if (session_state->session_type == TSessionType::BEESWAX) {
    ImpaladMetrics::IMPALA_SERVER_NUM_OPEN_BEESWAX_SESSIONS->Increment(-1L);
  } else {
    ImpaladMetrics::IMPALA_SERVER_NUM_OPEN_HS2_SESSIONS->Increment(-1L);
  }
  {
    lock_guard<mutex> l(session_state->lock);
    DCHECK(!session_state->closed);
    session_state->closed = true;
  }
  CancelSessionQueries(session_state.get());
  return Status::OK;
}

Status ImpalaServer::GetSessionState(const TUniqueId& session_id,
    shared_ptr<SessionState>* session_state, bool mark_active) {
  lock_guard<mutex> l(session_state_map_lock_);
  SessionStateMap::iterator i = session_state_map_.find(session_id);
  if (i == session_state_map_.end()) {
    *session_state = boost::shared_ptr<SessionState>();
    return Status("Invalid session id");
  } else {
    if (mark_active) {
      lock_guard<mutex> session_lock(i->second->lock);
      if (i->second->expired) {
        stringstream ss;
        ss << "Client session expired due to more than " << FLAGS_idle_session_timeout
           << "s of inactivity (last activity was at: "
           << i->second->last_accessed.DebugString() << ").";
        return Status(ss.str());
      }
      if (i->second->closed) return Status("Session is closed");
      ++i->second->ref_count;
    }
    *session_state = i->second;
    return Status::OK;
  }
}


Status ImpalaServer::ParseQueryOptions(const string& options,
    TQueryOptions* query_options) {
  if (options.length() == 0) return Status::OK;
  vector<string> kv_pairs;
  split(kv_pairs, options, is_any_of(","), token_compress_on);
  BOOST_FOREACH(string& kv_string, kv_pairs) {
    trim(kv_string);
    if (kv_string.length() == 0) continue;
    vector<string> key_value;
    split(key_value, kv_string, is_any_of("="), token_compress_on);
    if (key_value.size() != 2) {
      stringstream ss;
      ss << "Ignoring invalid configuration option " << kv_string
         << ": bad format (expected key=value)";
      return Status(ss.str());
    }
    RETURN_IF_ERROR(SetQueryOptions(key_value[0], key_value[1], query_options));
  }
  return Status::OK;
}

Status ImpalaServer::SetQueryOptions(const string& key, const string& value,
    TQueryOptions* query_options) {
  int option = GetQueryOption(key);
  if (option < 0) {
    stringstream ss;
    ss << "Ignoring invalid configuration option: " << key;
    return Status(ss.str());
  } else {
    switch (option) {
      case TImpalaQueryOptions::ABORT_ON_ERROR:
        query_options->__set_abort_on_error(
            iequals(value, "true") || iequals(value, "1"));
        break;
      case TImpalaQueryOptions::MAX_ERRORS:
        query_options->__set_max_errors(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::DISABLE_CODEGEN:
        query_options->__set_disable_codegen(
            iequals(value, "true") || iequals(value, "1"));
        break;
      case TImpalaQueryOptions::BATCH_SIZE:
        query_options->__set_batch_size(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::MEM_LIMIT: {
        // Parse the mem limit spec and validate it.
        bool is_percent;
        int64_t bytes_limit = ParseUtil::ParseMemSpec(value, &is_percent);
        if (bytes_limit < 0) {
          return Status("Failed to parse mem limit from '" + value + "'.");
        }
        if (is_percent) {
          return Status("Invalid query memory limit with percent '" + value + "'.");
        }
        query_options->__set_mem_limit(bytes_limit);
        break;
      }
      case TImpalaQueryOptions::NUM_NODES:
        query_options->__set_num_nodes(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::MAX_SCAN_RANGE_LENGTH:
        query_options->__set_max_scan_range_length(atol(value.c_str()));
        break;
      case TImpalaQueryOptions::MAX_IO_BUFFERS:
        query_options->__set_max_io_buffers(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::NUM_SCANNER_THREADS:
        query_options->__set_num_scanner_threads(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::ALLOW_UNSUPPORTED_FORMATS:
        query_options->__set_allow_unsupported_formats(
            iequals(value, "true") || iequals(value, "1"));
        break;
      case TImpalaQueryOptions::DEFAULT_ORDER_BY_LIMIT:
        query_options->__set_default_order_by_limit(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::DEBUG_ACTION:
        query_options->__set_debug_action(value.c_str());
        break;
      case TImpalaQueryOptions::PARQUET_COMPRESSION_CODEC: {
        if (value.empty()) break;
        if (iequals(value, "none")) {
          query_options->__set_parquet_compression_codec(THdfsCompression::NONE);
        } else if (iequals(value, "gzip")) {
          query_options->__set_parquet_compression_codec(THdfsCompression::GZIP);
        } else if (iequals(value, "snappy")) {
          query_options->__set_parquet_compression_codec(THdfsCompression::SNAPPY);
        } else {
          stringstream ss;
          ss << "Invalid parquet compression codec: " << value;
          return Status(ss.str());
        }
        break;
      }
      case TImpalaQueryOptions::ABORT_ON_DEFAULT_LIMIT_EXCEEDED:
        query_options->__set_abort_on_default_limit_exceeded(
            iequals(value, "true") || iequals(value, "1"));
        break;
      case TImpalaQueryOptions::HBASE_CACHING:
        query_options->__set_hbase_caching(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::HBASE_CACHE_BLOCKS:
        query_options->__set_hbase_cache_blocks(
            iequals(value, "true") || iequals(value, "1"));
        break;
      case TImpalaQueryOptions::PARQUET_FILE_SIZE:
        query_options->__set_parquet_file_size(atoi(value.c_str()));
        break;
      case TImpalaQueryOptions::EXPLAIN_LEVEL:
        if (iequals(value, "verbose") || iequals(value, "1")) {
          query_options->__set_explain_level(TExplainLevel::VERBOSE);
        } else if (iequals(value, "normal") || iequals(value, "0")) {
          query_options->__set_explain_level(TExplainLevel::NORMAL);
        } else {
          stringstream ss;
          ss << "Invalid explain level: " << value;
          return Status(ss.str());
        }
        break;
      case TImpalaQueryOptions::SYNC_DDL:
        query_options->__set_sync_ddl(iequals(value, "true") || iequals(value, "1"));
        break;
      default:
        // We hit this DCHECK(false) if we forgot to add the corresponding entry here
        // when we add a new query option.
        LOG(ERROR) << "Missing exec option implementation: " << key;
        DCHECK(false);
        break;
    }
  }
  return Status::OK;
}

inline shared_ptr<ImpalaServer::FragmentExecState> ImpalaServer::GetFragmentExecState(
    const TUniqueId& fragment_instance_id) {
  lock_guard<mutex> l(fragment_exec_state_map_lock_);
  FragmentExecStateMap::iterator i = fragment_exec_state_map_.find(fragment_instance_id);
  if (i == fragment_exec_state_map_.end()) {
    return shared_ptr<FragmentExecState>();
  } else {
    return i->second;
  }
}

void ImpalaServer::ExecPlanFragment(
    TExecPlanFragmentResult& return_val, const TExecPlanFragmentParams& params) {
  VLOG_QUERY << "ExecPlanFragment() instance_id=" << params.params.fragment_instance_id
             << " coord=" << params.coord << " backend#=" << params.backend_num;
  StartPlanFragmentExecution(params).SetTStatus(&return_val);
}

void ImpalaServer::ReportExecStatus(
    TReportExecStatusResult& return_val, const TReportExecStatusParams& params) {
  VLOG_FILE << "ReportExecStatus() query_id=" << params.query_id
            << " backend#=" << params.backend_num
            << " instance_id=" << params.fragment_instance_id
            << " done=" << (params.done ? "true" : "false");
  // TODO: implement something more efficient here, we're currently
  // acquiring/releasing the map lock and doing a map lookup for
  // every report (assign each query a local int32_t id and use that to index into a
  // vector of QueryExecStates, w/o lookup or locking?)
  shared_ptr<QueryExecState> exec_state = GetQueryExecState(params.query_id, false);
  if (exec_state.get() == NULL) {
    return_val.status.__set_status_code(TStatusCode::INTERNAL_ERROR);
    stringstream str;
    str << "unknown query id: " << params.query_id;
    return_val.status.error_msgs.push_back(str.str());
    LOG(ERROR) << str.str();
    return;
  }
  exec_state->coord()->UpdateFragmentExecStatus(params).SetTStatus(&return_val);
}

void ImpalaServer::CancelPlanFragment(
    TCancelPlanFragmentResult& return_val, const TCancelPlanFragmentParams& params) {
  VLOG_QUERY << "CancelPlanFragment(): instance_id=" << params.fragment_instance_id;
  shared_ptr<FragmentExecState> exec_state =
      GetFragmentExecState(params.fragment_instance_id);
  if (exec_state.get() == NULL) {
    stringstream str;
    str << "unknown fragment id: " << params.fragment_instance_id;
    Status status(TStatusCode::INTERNAL_ERROR, str.str());
    status.SetTStatus(&return_val);
    return;
  }
  // we only initiate cancellation here, the map entry as well as the exec state
  // are removed when fragment execution terminates (which is at present still
  // running in exec_state->exec_thread_)
  exec_state->Cancel().SetTStatus(&return_val);
}

void ImpalaServer::TransmitData(
    TTransmitDataResult& return_val, const TTransmitDataParams& params) {
  VLOG_ROW << "TransmitData(): instance_id=" << params.dest_fragment_instance_id
           << " node_id=" << params.dest_node_id
           << " #rows=" << params.row_batch.num_rows
           << " eos=" << (params.eos ? "true" : "false");
  // TODO: fix Thrift so we can simply take ownership of thrift_batch instead
  // of having to copy its data
  if (params.row_batch.num_rows > 0) {
    Status status = exec_env_->stream_mgr()->AddData(
        params.dest_fragment_instance_id, params.dest_node_id, params.row_batch);
    status.SetTStatus(&return_val);
    if (!status.ok()) {
      // should we close the channel here as well?
      return;
    }
  }

  if (params.eos) {
    exec_env_->stream_mgr()->CloseSender(
        params.dest_fragment_instance_id, params.dest_node_id).SetTStatus(&return_val);
  }
}

Status ImpalaServer::StartPlanFragmentExecution(
    const TExecPlanFragmentParams& exec_params) {
  if (!exec_params.fragment.__isset.output_sink) {
    return Status("missing sink in plan fragment");
  }

  const TPlanFragmentExecParams& params = exec_params.params;
  shared_ptr<FragmentExecState> exec_state(
      new FragmentExecState(
        params.query_id, exec_params.backend_num, params.fragment_instance_id,
        exec_env_, exec_params.coord));
  // Call Prepare() now, before registering the exec state, to avoid calling
  // exec_state->Cancel().
  // We might get an async cancellation, and the executor requires that Cancel() not
  // be called before Prepare() returns.
  RETURN_IF_ERROR(exec_state->Prepare(exec_params));

  {
    lock_guard<mutex> l(fragment_exec_state_map_lock_);
    // register exec_state before starting exec thread
    fragment_exec_state_map_.insert(make_pair(params.fragment_instance_id, exec_state));
  }

  // execute plan fragment in new thread
  // TODO: manage threads via global thread pool
  exec_state->set_exec_thread(new Thread("impala-server", "exec-plan-fragment",
      &ImpalaServer::RunExecPlanFragment, this, exec_state.get()));

  return Status::OK;
}

void ImpalaServer::RunExecPlanFragment(FragmentExecState* exec_state) {
  ImpaladMetrics::IMPALA_SERVER_NUM_FRAGMENTS->Increment(1L);
  exec_state->Exec();

  // we're done with this plan fragment
  {
    lock_guard<mutex> l(fragment_exec_state_map_lock_);
    FragmentExecStateMap::iterator i =
        fragment_exec_state_map_.find(exec_state->fragment_instance_id());
    if (i != fragment_exec_state_map_.end()) {
      // ends up calling the d'tor, if there are no async cancellations
      fragment_exec_state_map_.erase(i);
    } else {
      LOG(ERROR) << "missing entry in fragment exec state map: instance_id="
                 << exec_state->fragment_instance_id();
    }
  }
#ifndef ADDRESS_SANITIZER
  // tcmalloc and address sanitizer can not be used together
  if (FLAGS_log_mem_usage_interval > 0) {
    uint64_t num_complete = ImpaladMetrics::IMPALA_SERVER_NUM_FRAGMENTS->value();
    if (num_complete % FLAGS_log_mem_usage_interval == 0) {
      char buf[2048];
      // This outputs how much memory is currently being used by this impalad
      MallocExtension::instance()->GetStats(buf, 2048);
      LOG(INFO) << buf;
    }
  }
#endif
}

int ImpalaServer::GetQueryOption(const string& key) {
  map<int, const char*>::const_iterator itr =
      _TImpalaQueryOptions_VALUES_TO_NAMES.begin();
  for (; itr != _TImpalaQueryOptions_VALUES_TO_NAMES.end(); ++itr) {
    if (iequals(key, (*itr).second)) {
      return itr->first;
    }
  }
  return -1;
}

void ImpalaServer::InitializeConfigVariables() {
  Status status = ParseQueryOptions(FLAGS_default_query_options, &default_query_options_);
  if (!status.ok()) {
    // Log error and exit if the default query options are invalid.
    LOG(ERROR) << "Invalid default query options. Please check -default_query_options.\n"
               << status.GetErrorMsg();
    exit(1);
  }
  LOG(INFO) << "Default query options:" << ThriftDebugString(default_query_options_);

  map<string, string> string_map;
  TQueryOptionsToMap(default_query_options_, &string_map);
  map<string, string>::const_iterator itr = string_map.begin();
  for (; itr != string_map.end(); ++itr) {
    ConfigVariable option;
    option.__set_key(itr->first);
    option.__set_value(itr->second);
    default_configs_.push_back(option);
  }
  ConfigVariable support_start_over;
  support_start_over.__set_key("support_start_over");
  support_start_over.__set_value("false");
  default_configs_.push_back(support_start_over);
}

void ImpalaServer::TQueryOptionsToMap(const TQueryOptions& query_option,
    map<string, string>* configuration) {
  map<int, const char*>::const_iterator itr =
      _TImpalaQueryOptions_VALUES_TO_NAMES.begin();
  for (; itr != _TImpalaQueryOptions_VALUES_TO_NAMES.end(); ++itr) {
    stringstream val;
    switch (itr->first) {
      case TImpalaQueryOptions::ABORT_ON_ERROR:
        val << query_option.abort_on_error;
        break;
      case TImpalaQueryOptions::MAX_ERRORS:
        val << query_option.max_errors;
        break;
      case TImpalaQueryOptions::DISABLE_CODEGEN:
        val << query_option.disable_codegen;
        break;
      case TImpalaQueryOptions::BATCH_SIZE:
        val << query_option.batch_size;
        break;
      case TImpalaQueryOptions::MEM_LIMIT:
        val << query_option.mem_limit;
        break;
      case TImpalaQueryOptions::NUM_NODES:
        val << query_option.num_nodes;
        break;
      case TImpalaQueryOptions::MAX_SCAN_RANGE_LENGTH:
        val << query_option.max_scan_range_length;
        break;
      case TImpalaQueryOptions::MAX_IO_BUFFERS:
        val << query_option.max_io_buffers;
        break;
      case TImpalaQueryOptions::NUM_SCANNER_THREADS:
        val << query_option.num_scanner_threads;
        break;
      case TImpalaQueryOptions::ALLOW_UNSUPPORTED_FORMATS:
        val << query_option.allow_unsupported_formats;
        break;
      case TImpalaQueryOptions::DEFAULT_ORDER_BY_LIMIT:
        val << query_option.default_order_by_limit;
        break;
      case TImpalaQueryOptions::DEBUG_ACTION:
        val << query_option.debug_action;
        break;
      case TImpalaQueryOptions::ABORT_ON_DEFAULT_LIMIT_EXCEEDED:
        val << query_option.abort_on_default_limit_exceeded;
        break;
      case TImpalaQueryOptions::PARQUET_COMPRESSION_CODEC:
        val << query_option.parquet_compression_codec;
        break;
      case TImpalaQueryOptions::HBASE_CACHING:
        val << query_option.hbase_caching;
        break;
      case TImpalaQueryOptions::HBASE_CACHE_BLOCKS:
        val << query_option.hbase_cache_blocks;
        break;
      case TImpalaQueryOptions::PARQUET_FILE_SIZE:
        val << query_option.parquet_file_size;
        break;
      case TImpalaQueryOptions::EXPLAIN_LEVEL:
        val << query_option.explain_level;
        break;
      case TImpalaQueryOptions::SYNC_DDL:
        val << query_option.sync_ddl;
        break;
      default:
        // We hit this DCHECK(false) if we forgot to add the corresponding entry here
        // when we add a new query option.
        LOG(ERROR) << "Missing exec option implementation: " << itr->second;
        DCHECK(false);
    }
    (*configuration)[itr->second] = val.str();
  }
}

void ImpalaServer::SessionState::ToThrift(const TUniqueId& session_id,
    TSessionState* state) {
  lock_guard<mutex> l(lock);
  state->session_id = session_id;
  state->session_type = session_type;
  state->database = database;
  // The do_as_user will only be set if impersonation is enabled and the
  // proxy user is authorized to impersonate as this user.
  state->user = do_as_user.empty() ? user : do_as_user;
  state->network_address = network_address;
}

void ImpalaServer::CancelFromThreadPool(uint32_t thread_id,
    const CancellationWork& cancellation_work) {
  if (cancellation_work.unregister()) {
    if (!UnregisterQuery(cancellation_work.query_id(), &cancellation_work.cause())) {
      VLOG_QUERY << "Query de-registration (" << cancellation_work.query_id()
                 << ") failed";
    }
  } else {
    Status status =
        CancelInternal(cancellation_work.query_id(), &cancellation_work.cause());
    if (!status.ok()) {
      VLOG_QUERY << "Query cancellation (" << cancellation_work.query_id()
                 << ") did not succeed: " << status.GetErrorMsg();
    }
  }
}

Status ImpalaServer::AuthorizeProxyUser(const string& user, const string& do_as_user) {
  if (user.empty()) {
    return Status("Unable to impersonate using empty proxy username.");
  } else if (user.empty()) {
    return Status("Unable to impersonate using empty doAs username.");
  }

  stringstream error_msg;
  error_msg << "User '" << user << "' is not authorized to impersonate '"
            << do_as_user << "'.";
  if (authorized_proxy_user_config_.size() == 0) {
    error_msg << " User impersonation is disabled.";
    return Status(error_msg.str());
  }

  // Get the short version of the user name (the user name up to the first '/' or '@')
  // from the full principal name.
  size_t end_idx = min(user.find("/"), user.find("@"));
  // If neither are found (or are found at the beginning of the user name),
  // return the username. Otherwise, return the username up to the matching character.
  string short_user(
      end_idx == string::npos || end_idx == 0 ? user : user.substr(0, end_idx));

  // Check if the proxy user exists. If he/she does, then check if they are allowed
  // to impersonate the do_as_user.
  ProxyUserMap::const_iterator proxy_user =
      authorized_proxy_user_config_.find(short_user);
  if (proxy_user != authorized_proxy_user_config_.end()) {
    BOOST_FOREACH(const string& user, proxy_user->second) {
      if (user == "*" || user == do_as_user) return Status::OK;
    }
  }
  return Status(error_msg.str());
}

void ImpalaServer::CatalogUpdateCallback(
    const StateStoreSubscriber::TopicDeltaMap& incoming_topic_deltas,
    vector<TTopicDelta>* subscriber_topic_updates) {
  StateStoreSubscriber::TopicDeltaMap::const_iterator topic =
      incoming_topic_deltas.find(CatalogServer::IMPALA_CATALOG_TOPIC);
  if (topic == incoming_topic_deltas.end()) return;
  const TTopicDelta& delta = topic->second;


  // Process any updates
  if (delta.topic_entries.size() != 0 || delta.topic_deletions.size() != 0)  {
    TUpdateCatalogCacheRequest update_req;
    update_req.__set_is_delta(delta.is_delta);
    // Process all Catalog updates (new and modified objects) and determine what the
    // new catalog version will be.
    int64_t new_catalog_version = catalog_update_info_.catalog_version;
    BOOST_FOREACH(const TTopicItem& item, delta.topic_entries) {
      uint32_t len = item.value.size();
      TCatalogObject catalog_object;
      Status status = DeserializeThriftMsg(reinterpret_cast<const uint8_t*>(
          item.value.data()), &len, false, &catalog_object);
      if (!status.ok()) {
        LOG(ERROR) << "Error deserializing item: " << status.GetErrorMsg();
        continue;
      }
      if (catalog_object.type == TCatalogObjectType::CATALOG) {
        update_req.__set_catalog_service_id(catalog_object.catalog.catalog_service_id);
        new_catalog_version = catalog_object.catalog_version;
      }
      update_req.updated_objects.push_back(catalog_object);
    }

    // We need to look up the dropped functions and remove them from the library
    // cache. The data sent from the catalog service does not contain all the
    // function metadata so we'll ask our local frontend for it. We need to do
    // this before updating the catalog.
    vector<TCatalogObject> dropped_functions;

    // Process all Catalog deletions (dropped objects). We only know the keys (object
    // names) so must parse each key to determine the TCatalogObject.
    BOOST_FOREACH(const string& key, delta.topic_deletions) {
      LOG(INFO) << "Catalog topic entry deletion: " << key;
      TCatalogObject catalog_object;
      Status status = TCatalogObjectFromEntryKey(key, &catalog_object);
      if (!status.ok()) {
        LOG(ERROR) << "Error parsing catalog topic entry deletion key: " << key << " "
                   << "Error: " << status.GetErrorMsg();
        continue;
      }
      update_req.removed_objects.push_back(catalog_object);
      if (catalog_object.type == TCatalogObjectType::FUNCTION) {
        TCatalogObject dropped_function;
        if (frontend_->GetCatalogObject(catalog_object, &dropped_function).ok()) {
          dropped_functions.push_back(dropped_function);
        }
        // Nothing to do in error case.
      }
    }

    // Call the FE to apply the changes to the Impalad Catalog.
    TUpdateCatalogCacheResponse resp;
    Status s = frontend_->UpdateCatalogCache(update_req, &resp);
    if (!s.ok()) {
      LOG(ERROR) << "There was an error processing the impalad catalog update. Requesting"
                 << " a full topic update to recover: " << s.GetErrorMsg();
      subscriber_topic_updates->push_back(TTopicDelta());
      TTopicDelta& update = subscriber_topic_updates->back();
      update.topic_name = CatalogServer::IMPALA_CATALOG_TOPIC;
      update.__set_from_version(0L);
      ImpaladMetrics::CATALOG_READY->Update(false);
      // Dropped all cached lib files (this behaves as if all functions are dropped).
      exec_env_->lib_cache()->DropCache();
    } else {
      {
        unique_lock<mutex> unique_lock(catalog_version_lock_);
        catalog_update_info_.catalog_version = new_catalog_version;
        catalog_update_info_.catalog_topic_version = delta.to_version;
        catalog_update_info_.catalog_service_id = resp.catalog_service_id;
      }
      ImpaladMetrics::CATALOG_READY->Update(new_catalog_version > 0);
      UpdateCatalogMetrics();
      // Remove all dropped functions from the library cache.
      // TODO: is this expensive? We'd like to process heartbeats promptly.
      for (int i = 0; i < dropped_functions.size(); ++i) {
        exec_env_->lib_cache()->RemoveEntry(dropped_functions[i].fn.hdfs_location);
      }
    }
  }

  // Always update the minimum subscriber version for the catalog topic.
  {
    unique_lock<mutex> unique_lock(catalog_version_lock_);
    min_subscriber_catalog_topic_version_ = delta.min_subscriber_topic_version;
  }
  catalog_version_update_cv_.notify_all();
}

Status ImpalaServer::ProcessCatalogUpdateResult(
    const TCatalogUpdateResult& catalog_update_result, bool wait_for_all_subscribers) {
  // If wait_for_all_subscribers is false, or if this this update result contains a
  // catalog object to add or remove, assume it is "fast" update and directly apply the
  // update to the local impalad's catalog cache. Otherwise, wait for a statestore
  // heartbeat that contains this update version.
  if ((catalog_update_result.__isset.updated_catalog_object ||
      catalog_update_result.__isset.removed_catalog_object) &&
      !wait_for_all_subscribers) {
    TUpdateCatalogCacheRequest update_req;
    update_req.__set_is_delta(true);
    update_req.__set_catalog_service_id(catalog_update_result.catalog_service_id);

    if (catalog_update_result.__isset.updated_catalog_object) {
      update_req.updated_objects.push_back(catalog_update_result.updated_catalog_object);
    }
    if (catalog_update_result.__isset.removed_catalog_object) {
      update_req.removed_objects.push_back(catalog_update_result.removed_catalog_object);
    }
     // Apply the changes to the local catalog cache.
    TUpdateCatalogCacheResponse resp;
    Status status = frontend_->UpdateCatalogCache(update_req, &resp);
    if (!status.ok()) LOG(ERROR) << status.GetErrorMsg();
    return status;
  } else {
    unique_lock<mutex> unique_lock(catalog_version_lock_);
    int64_t min_req_catalog_version = catalog_update_result.version;
    const TUniqueId& catalog_service_id = catalog_update_result.catalog_service_id;

    // Wait for the update to be processed locally.
    // TODO: What about query cancellation?
    VLOG_QUERY << "Waiting for catalog version: " << min_req_catalog_version
               << " current version: " << catalog_update_info_.catalog_version;
    while (catalog_update_info_.catalog_version < min_req_catalog_version &&
           catalog_update_info_.catalog_service_id == catalog_service_id) {
      catalog_version_update_cv_.wait(unique_lock);
    }

    if (!wait_for_all_subscribers) return Status::OK;

    // Now wait for this update to be propagated to all catalog topic subscribers.
    // If we make it here it implies the first condition was met (the update was processed
    // locally or the catalog service id has changed).
    int64_t min_req_subscriber_topic_version = catalog_update_info_.catalog_topic_version;

    VLOG_QUERY << "Waiting for min subscriber topic version: "
               << min_req_subscriber_topic_version << " current version: "
               << min_subscriber_catalog_topic_version_;
    while (min_subscriber_catalog_topic_version_ < min_req_subscriber_topic_version &&
           catalog_update_info_.catalog_service_id == catalog_service_id) {
      catalog_version_update_cv_.wait(unique_lock);
    }
  }
  return Status::OK;
}

void ImpalaServer::MembershipCallback(
    const StateStoreSubscriber::TopicDeltaMap& incoming_topic_deltas,
    vector<TTopicDelta>* subscriber_topic_updates) {
  // TODO: Consider rate-limiting this. In the short term, best to have
  // state-store heartbeat less frequently.
  StateStoreSubscriber::TopicDeltaMap::const_iterator topic =
      incoming_topic_deltas.find(SimpleScheduler::IMPALA_MEMBERSHIP_TOPIC);

  if (topic != incoming_topic_deltas.end()) {
    const TTopicDelta& delta = topic->second;
    // If this is not a delta, the update should include all entries in the topic so
    // clear the saved mapping of known backends.
    if (!delta.is_delta) known_backends_.clear();

    // Process membership additions.
    BOOST_FOREACH(const TTopicItem& item, delta.topic_entries) {
      uint32_t len = item.value.size();
      TBackendDescriptor backend_descriptor;
      Status status = DeserializeThriftMsg(reinterpret_cast<const uint8_t*>(
          item.value.data()), &len, false, &backend_descriptor);
      if (!status.ok()) {
        VLOG(2) << "Error deserializing topic item with key: " << item.key;
        continue;
      }
      // This is a new item - add it to the map of known backends.
      known_backends_.insert(make_pair(item.key, backend_descriptor.address));
    }
    // Process membership deletions.
    BOOST_FOREACH(const string& backend_id, delta.topic_deletions) {
      known_backends_.erase(backend_id);
    }

    // Create a set of known backend network addresses. Used to test for cluster
    // membership by network address.
    set<TNetworkAddress> current_membership;
    BOOST_FOREACH(const BackendAddressMap::value_type& backend, known_backends_) {
      current_membership.insert(backend.second);
    }

    // Maps from query id (to be cancelled) to a list of failed Impalads that are
    // the cause of the cancellation.
    map<TUniqueId, vector<TNetworkAddress> > queries_to_cancel;
    {
      // Build a list of queries that are running on failed hosts (as evidenced by their
      // absence from the membership list).
      // TODO: crash-restart failures can give false negatives for failed Impala demons.
      lock_guard<mutex> l(query_locations_lock_);
      QueryLocations::const_iterator loc_entry = query_locations_.begin();
      while (loc_entry != query_locations_.end()) {
        if (current_membership.find(loc_entry->first) == current_membership.end()) {
          unordered_set<TUniqueId>::const_iterator query_id = loc_entry->second.begin();
          // Add failed backend locations to all queries that ran on that backend.
          for(; query_id != loc_entry->second.end(); ++query_id) {
            vector<TNetworkAddress>& failed_hosts = queries_to_cancel[*query_id];
            failed_hosts.push_back(loc_entry->first);
          }
          exec_env_->client_cache()->CloseConnections(loc_entry->first);
          // We can remove the location wholesale once we know backend's failed. To do so
          // safely during iteration, we have to be careful not in invalidate the current
          // iterator, so copy the iterator to do the erase(..) and advance the original.
          QueryLocations::const_iterator failed_backend = loc_entry;
          ++loc_entry;
          query_locations_.erase(failed_backend);
        } else {
          ++loc_entry;
        }
      }
    }

    if (cancellation_thread_pool_->GetQueueSize() + queries_to_cancel.size() >
        MAX_CANCELLATION_QUEUE_SIZE) {
      // Ignore the cancellations - we'll be able to process them on the next heartbeat
      // instead.
      LOG_EVERY_N(WARNING, 60) << "Cancellation queue is full";
    } else {
      // Since we are the only producer for this pool, we know that this cannot block
      // indefinitely since the queue is large enough to accept all new cancellation
      // requests.
      map<TUniqueId, vector<TNetworkAddress> >::iterator cancellation_entry;
      for (cancellation_entry = queries_to_cancel.begin();
          cancellation_entry != queries_to_cancel.end();
          ++cancellation_entry) {
        stringstream cause_msg;
        cause_msg << "Cancelled due to unreachable impalad(s): ";
        for (int i = 0; i < cancellation_entry->second.size(); ++i) {
          cause_msg << cancellation_entry->second[i];
          if (i + 1 != cancellation_entry->second.size()) cause_msg << ", ";
        }
        cancellation_thread_pool_->Offer(
            CancellationWork(cancellation_entry->first, Status(cause_msg.str()), false));
      }
    }
  }
}

ImpalaServer::QueryStateRecord::QueryStateRecord(const QueryExecState& exec_state,
    bool copy_profile, const string& encoded_profile) {
  id = exec_state.query_id();
  const TExecRequest& request = exec_state.exec_request();

  stmt = exec_state.sql_stmt();
  stmt_type = request.stmt_type;
  user = exec_state.user();
  default_db = exec_state.default_db();
  start_time = exec_state.start_time();
  end_time = exec_state.end_time();
  has_coord = false;

  Coordinator* coord = exec_state.coord();
  if (coord != NULL) {
    num_complete_fragments = coord->progress().num_complete();
    total_fragments = coord->progress().total();
    has_coord = true;
  }
  query_state = exec_state.query_state();
  num_rows_fetched = exec_state.num_rows_fetched();

  if (copy_profile) {
    stringstream ss;
    exec_state.profile().PrettyPrint(&ss);
    profile_str = ss.str();
    if (encoded_profile.empty()) {
      encoded_profile_str = exec_state.profile().SerializeToArchiveString();
    } else {
      encoded_profile_str = encoded_profile;
    }
  }
}

bool ImpalaServer::QueryStateRecord::operator() (
    const QueryStateRecord& lhs, const QueryStateRecord& rhs) const {
  if (lhs.start_time == rhs.start_time) return lhs.id < rhs.id;
  return lhs.start_time < rhs.start_time;
}

void ImpalaServer::ConnectionStart(
    const ThriftServer::ConnectionContext& connection_context) {
  if (connection_context.server_name == BEESWAX_SERVER_NAME) {
    // Beeswax only allows for one session per connection, so we can share the session ID
    // with the connection ID
    const TUniqueId& session_id = connection_context.connection_id;
    shared_ptr<SessionState> session_state;
    session_state.reset(new SessionState);
    session_state->closed = false;
    session_state->start_time = TimestampValue::local_time();
    session_state->last_accessed = session_state->start_time;
    session_state->database = "default";
    session_state->session_type = TSessionType::BEESWAX;
    session_state->network_address = connection_context.network_address;
    // If the username was set by a lower-level transport, use it.
    if (!connection_context.username.empty()) {
      session_state->user = connection_context.username;
    }

    {
      lock_guard<mutex> l(session_state_map_lock_);
      bool success =
          session_state_map_.insert(make_pair(session_id, session_state)).second;
      // The session should not have already existed.
      DCHECK(success);
    }
    {
      lock_guard<mutex> l(connection_to_sessions_map_lock_);
      connection_to_sessions_map_[connection_context.connection_id].push_back(session_id);
    }
    ImpaladMetrics::IMPALA_SERVER_NUM_OPEN_BEESWAX_SESSIONS->Increment(1L);
  }
}

void ImpalaServer::ConnectionEnd(
    const ThriftServer::ConnectionContext& connection_context) {
  unique_lock<mutex> l(connection_to_sessions_map_lock_);
  ConnectionToSessionMap::iterator it =
      connection_to_sessions_map_.find(connection_context.connection_id);

  // Not every connection must have an associated session
  if (it == connection_to_sessions_map_.end()) return;

  LOG(INFO) << "Connection from client " << connection_context.network_address
            << " closed, closing " << it->second.size() << " associated session(s)";

  BOOST_FOREACH(const TUniqueId& session_id, it->second) {
    Status status = CloseSessionInternal(session_id, true);
    if (!status.ok()) {
      LOG(WARNING) << "Error closing session " << session_id << ": "
                   << status.GetErrorMsg();
    }
  }
  connection_to_sessions_map_.erase(it);
}

void ImpalaServer::ExpireSessions() {
  while (true) {
    // Sleep for half the session timeout; the maximum delay between a session expiring
    // and this method picking it up is equal to the size of this sleep.
    SleepForMs(FLAGS_idle_session_timeout * 500);
    lock_guard<mutex> l(session_state_map_lock_);
    int64_t now = ms_since_epoch();
    VLOG(3) << "Session expiration thread waking up";
    // TODO: If holding session_state_map_lock_ for the duration of this loop is too
    // expensive, consider a priority queue.
    BOOST_FOREACH(SessionStateMap::value_type& session_state, session_state_map_) {
      {
        lock_guard<mutex> l(session_state.second->lock);
        if (session_state.second->ref_count > 0) continue;
        // A session closed by other means is in the process of being removed, and it's
        // best not to interfere.
        if (session_state.second->closed || session_state.second->expired) continue;
        int64_t last_accessed = session_state.second->last_accessed;
        if (now - last_accessed <= FLAGS_idle_session_timeout) continue;
        LOG(INFO) << "Expiring session: " << session_state.first << ", user:"
                  << session_state.second->user << ", last active: "
                  << session_state.second->last_accessed.DebugString();
        session_state.second->expired = true;
        ImpaladMetrics::NUM_SESSIONS_EXPIRED->Increment(1L);
      }
      CancelSessionQueriesAsync(session_state.second.get(),
          "Session expired due to inactivity");
    }
  }
}

void ImpalaServer::ExpireQueries() {
  while (true) {
    int64_t next_wakeup;
    // The following block accomplishes three things:
    //
    // 1. Update the ordered list of queries by checking the 'idle_time' parameter in
    // query_exec_state. We are able to avoid doing this for *every* query in flight
    // thanks to the observation that expiry times never move backwards, only
    // forwards. Therefore once we find a query that a) hasn't changed its idle time and
    // b) has not yet expired we can stop moving through the list. If the idle time has
    // changed, we need to re-insert the query in the right place in queries_by_timestamp_
    //
    // 2. Remove any queries that would have expired but have already been closed for any
    // reason.
    //
    // 3. Compute the next time a query *might* expire, so that the sleep at the end of
    // this loop has an accurate duration to wait. If the list of queries is empty, the
    // default sleep duration is half the idle query timeout.
    {
      lock_guard<mutex> l(query_expiration_lock_);
      ExpirationQueue::iterator expiration_event = queries_by_timestamp_.begin();
      int64_t now = ms_since_epoch();
      while (expiration_event != queries_by_timestamp_.end()) {
        // If the last-observed expiration time for this query is still in the future, we
        // know that the true expiration time will be at least that far off. So we can
        // break here and sleep.
        if (expiration_event->first > now) break;
        shared_ptr<QueryExecState> query_state =
            GetQueryExecState(expiration_event->second, false);
        if (query_state.get() == NULL) {
          // Query was deleted some other way.
          queries_by_timestamp_.erase(expiration_event++);
          continue;
        }
        // First, check the actual expiration time in case the query has updated it
        // since the last time we looked.
        int64_t expiration =
            query_state->last_active() + (FLAGS_idle_query_timeout * 1000);
        if (now < expiration) {
          // If the real expiration date is in the future we may need to re-insert the
          // query's expiration event at its correct location.
          if (expiration == expiration_event->first) {
            // The query hasn't been updated since it was inserted, so we know (by the
            // fact that queries are inserted in-expiration-order initially) that it is
            // still the next query to expire. No need to re-insert it.
            break;
          } else {
            // Erase and re-insert with an updated expiration time.
            TUniqueId query_id = expiration_event->second;
            queries_by_timestamp_.erase(expiration_event++);
            queries_by_timestamp_.insert(make_pair(expiration, query_id));
          }
        } else if (!query_state->is_active()) {
          // Otherwise time to expire this query
          VLOG_QUERY << "Expiring query due to client inactivity: "
                     << expiration_event->second << ", last activity was at: "
                     << TimestampValue(query_state->last_active(), 0).DebugString();
          stringstream ss;
          ss << "Query " << PrintId(expiration_event->second)
             << " expired due to client inactivity " << "(timeout is "
             << FLAGS_idle_query_timeout << "s)";

          cancellation_thread_pool_->Offer(
              CancellationWork(expiration_event->second, Status(ss.str()), false));
          queries_by_timestamp_.erase(expiration_event++);
          ImpaladMetrics::NUM_QUERIES_EXPIRED->Increment(1L);
        } else {
          // Iterator is moved on in every other branch.
          ++expiration_event;
        }
      }
      if (queries_by_timestamp_.size() > 0) {
        next_wakeup = queries_by_timestamp_.begin()->first;
      } else {
        // If no queries, wake up in half the idle query timeout
        next_wakeup = now + (FLAGS_idle_query_timeout * 500);
      }
    }

    // If the next expiration time is right now (because the query is active, and has
    // taken longer than FLAGS_idle_query_timeout to perform a method call), rate limit
    // the frequency with which we wake up to avoid busy waiting.
    next_wakeup = max(2000L, next_wakeup - ms_since_epoch());
    SleepForMs(next_wakeup);
  }
}

Status CreateImpalaServer(ExecEnv* exec_env, int beeswax_port, int hs2_port,
    int be_port, ThriftServer** beeswax_server, ThriftServer** hs2_server,
    ThriftServer** be_server, ImpalaServer** impala_server) {
  DCHECK((beeswax_port == 0) == (beeswax_server == NULL));
  DCHECK((hs2_port == 0) == (hs2_server == NULL));
  DCHECK((be_port == 0) == (be_server == NULL));

  shared_ptr<ImpalaServer> handler(new ImpalaServer(exec_env));

  if (beeswax_port != 0 && beeswax_server != NULL) {
    // Beeswax FE must be a TThreadPoolServer because ODBC and Hue only support
    // TThreadPoolServer.
    shared_ptr<TProcessor> beeswax_processor(new ImpalaServiceProcessor(handler));
    *beeswax_server = new ThriftServer(BEESWAX_SERVER_NAME, beeswax_processor,
        beeswax_port, AuthManager::GetInstance()->GetClientFacingAuthProvider(),
        exec_env->metrics(), FLAGS_fe_service_threads, ThriftServer::ThreadPool);

    (*beeswax_server)->SetConnectionHandler(handler.get());
    if (!FLAGS_ssl_server_certificate.empty()) {
      LOG(INFO) << "Enabling SSL for Beeswax";
      RETURN_IF_ERROR((*beeswax_server)->EnableSsl(
              FLAGS_ssl_server_certificate, FLAGS_ssl_private_key));
    }

    LOG(INFO) << "Impala Beeswax Service listening on " << beeswax_port;
  }

  if (hs2_port != 0 && hs2_server != NULL) {
    // HiveServer2 JDBC driver does not support non-blocking server.
    shared_ptr<TProcessor> hs2_fe_processor(
        new ImpalaHiveServer2ServiceProcessor(handler));
    *hs2_server = new ThriftServer(HS2_SERVER_NAME, hs2_fe_processor, hs2_port,
        AuthManager::GetInstance()->GetClientFacingAuthProvider(), exec_env->metrics(),
        FLAGS_fe_service_threads, ThriftServer::ThreadPool);

    (*hs2_server)->SetConnectionHandler(handler.get());
    if (!FLAGS_ssl_server_certificate.empty()) {
      LOG(INFO) << "Enabling SSL for HiveServer2";
      RETURN_IF_ERROR((*hs2_server)->EnableSsl(
              FLAGS_ssl_server_certificate, FLAGS_ssl_private_key));
    }

    LOG(INFO) << "Impala HiveServer2 Service listening on " << hs2_port;
  }

  if (be_port != 0 && be_server != NULL) {
    shared_ptr<TProcessor> be_processor(new ImpalaInternalServiceProcessor(handler));
    *be_server = new ThriftServer("backend", be_processor, be_port, NULL,
        exec_env->metrics(), FLAGS_be_service_threads);

    LOG(INFO) << "ImpalaInternalService listening on " << be_port;
  }
  if (impala_server != NULL) *impala_server = handler.get();

  return Status::OK;
}

bool ImpalaServer::GetSessionIdForQuery(const TUniqueId& query_id,
    TUniqueId* session_id) {
  DCHECK(session_id != NULL);
  lock_guard<mutex> l(query_exec_state_map_lock_);
  QueryExecStateMap::iterator i = query_exec_state_map_.find(query_id);
  if (i == query_exec_state_map_.end()) {
    return false;
  } else {
    *session_id = i->second->session_id();
    return true;
  }
}

shared_ptr<ImpalaServer::QueryExecState> ImpalaServer::GetQueryExecState(
    const TUniqueId& query_id, bool lock) {
  lock_guard<mutex> l(query_exec_state_map_lock_);
  QueryExecStateMap::iterator i = query_exec_state_map_.find(query_id);
  if (i == query_exec_state_map_.end()) {
    return shared_ptr<QueryExecState>();
  } else {
    if (lock) i->second->lock()->lock();
    return i->second;
  }
}

}
