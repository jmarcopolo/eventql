/**
 * This file is part of the "libfnord" project
 *   Copyright (c) 2015 Paul Asmuth
 *
 * FnordMetric is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License v3.0. You should have received a
 * copy of the GNU General Public License along with this program. If not, see
 * <http://www.gnu.org/licenses/>.
 */
#include "stx/util/binarymessagewriter.h"
#include "tsdb/TSDBServlet.h"
#include "tsdb/RecordEnvelope.pb.h"
#include "stx/json/json.h"
#include <stx/wallclock.h>
#include <stx/thread/wakeup.h>
#include "stx/protobuf/MessageEncoder.h"
#include "stx/protobuf/MessagePrinter.h"
#include "stx/protobuf/msg.h"
#include <stx/util/Base64.h>
#include <stx/fnv.h>
#include <sstable/sstablereader.h>
#include <chartsql/runtime/ASCIITableFormat.h>
#include <chartsql/runtime/JSONSSEStreamFormat.h>

using namespace stx;

namespace tsdb {

TSDBServlet::TSDBServlet(TSDBNode* node) : node_(node) {}

void TSDBServlet::handleHTTPRequest(
    RefPtr<http::HTTPRequestStream> req_stream,
    RefPtr<http::HTTPResponseStream> res_stream) {
  req_stream->readBody();
  const auto& req = req_stream->request();
  URI uri(req.uri());

  http::HTTPResponse res;
  res.populateFromRequest(req);
  res.addHeader("Access-Control-Allow-Origin", "*");
  res.addHeader("Access-Control-Allow-Methods", "GET, POST");
  res.addHeader("Access-Control-Allow-Headers", "X-TSDB-Namespace");

  if (req.method() == http::HTTPMessage::M_OPTIONS) {
    res.setStatus(http::kStatusOK);
    res_stream->writeResponse(res);
    return;
  }

  try {
    if (uri.path() == "/tsdb/insert") {
      insertRecords(&req, &res, &uri);
      res_stream->writeResponse(res);
      return;
    }

    if (uri.path() == "/tsdb/stream") {
      streamPartition(&req, &res, res_stream, &uri);
      return;
    }

    if (uri.path() == "/tsdb/partition_info") {
      fetchPartitionInfo(&req, &res, &uri);
      res_stream->writeResponse(res);
      return;
    }

    if (uri.path() == "/tsdb/sql") {
      executeSQL(&req, &res, &uri);
      res_stream->writeResponse(res);
      return;
    }

    if (uri.path() == "/tsdb/sql_stream") {
      executeSQLStream(&req, &res, res_stream, &uri);
      return;
    }

    res.setStatus(stx::http::kStatusNotFound);
    res.addBody("not found");
    res_stream->writeResponse(res);
  } catch (const Exception& e) {
    stx::logError("tsdb", e, "error while processing HTTP request");

    res.setStatus(http::kStatusInternalServerError);
    res.addBody(StringUtil::format("error: $0: $1", e.getTypeName(), e.getMessage()));
    res_stream->writeResponse(res);
  }

  res_stream->finishResponse();
}

void TSDBServlet::insertRecords(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    URI* uri) {
  auto record_list = msg::decode<RecordEnvelopeList>(req->body());

  Vector<RefPtr<Partition>> partition_refs;
  HashMap<Partition*, Vector<RecordRef>> grouped;
  for (const auto& record : record_list.records()) {
    auto partition = node_->findOrCreatePartition(
        record.tsdb_namespace(),
        record.stream_key(),
        SHA1Hash::fromHexString(record.partition_key()));

    auto record_data = record.record_data().data();
    auto record_size = record.record_data().size();

    if (grouped.count(partition.get()) == 0) {
      partition_refs.emplace_back(partition);
    }

    grouped[partition.get()].emplace_back(
        SHA1Hash::fromHexString(record.record_id()),
        Buffer(record_data, record_size));
  }

  for (const auto& group : grouped) {
    group.first->getWriter()->insertRecords(group.second);
  }

  res->setStatus(http::kStatusCreated);
}

void TSDBServlet::streamPartition(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream,
    URI* uri) {
  const auto& params = uri->queryParams();

  String tsdb_namespace;
  if (!URI::getParam(params, "namespace", &tsdb_namespace)) {
    res->setStatus(stx::http::kStatusBadRequest);
    res->addBody("missing ?namespace=... parameter");
    res_stream->writeResponse(*res);
    return;
  }

  String stream_key;
  if (!URI::getParam(params, "stream", &stream_key)) {
    res->setStatus(stx::http::kStatusBadRequest);
    res->addBody("missing ?stream=... parameter");
    res_stream->writeResponse(*res);
    return;
  }

  String partition_key;
  if (!URI::getParam(params, "partition", &partition_key)) {
    res->setStatus(stx::http::kStatusBadRequest);
    res->addBody("missing ?partition=... parameter");
    res_stream->writeResponse(*res);
    return;
  }

  size_t sample_mod = 0;
  size_t sample_idx = 0;
  String sample_str;
  if (URI::getParam(params, "sample", &sample_str)) {
    auto parts = StringUtil::split(sample_str, ":");

    if (parts.size() != 2) {
      res->setStatus(stx::http::kStatusBadRequest);
      res->addBody("invalid ?sample=... parameter, format is <mod>:<idx>");
      res_stream->writeResponse(*res);
    }

    sample_mod = std::stoull(parts[0]);
    sample_idx = std::stoull(parts[1]);
  }


  res->setStatus(http::kStatusOK);
  res->addHeader("Content-Type", "application/octet-stream");
  res->addHeader("Connection", "close");
  res_stream->startResponse(*res);

  auto partition = node_->findPartition(
      tsdb_namespace,
      stream_key,
      SHA1Hash::fromHexString(partition_key));

  if (!partition.isEmpty()) {
    FNV<uint64_t> fnv;
    auto files = partition.get()->listFiles();

    for (const auto& f : files) {
      auto fpath = FileUtil::joinPaths(node_->dbPath(), f);
      sstable::SSTableReader reader(fpath);
      auto cursor = reader.getCursor();

      while (cursor->valid()) {
        uint64_t* key;
        size_t key_size;
        cursor->getKey((void**) &key, &key_size);
        if (key_size != SHA1Hash::kSize) {
          RAISE(kRuntimeError, "invalid row");
        }

        if (sample_mod == 0 ||
            (fnv.hash(key, key_size) % sample_mod == sample_idx)) {
          void* data;
          size_t data_size;
          cursor->getData(&data, &data_size);

          util::BinaryMessageWriter buf;
          if (data_size > 0) {
            buf.appendUInt64(data_size);
            buf.append(data, data_size);
            res_stream->writeBodyChunk(Buffer(buf.data(), buf.size()));
          }
          res_stream->waitForReader();
        }

        if (!cursor->next()) {
          break;
        }
      }
    }
  }

  util::BinaryMessageWriter buf;
  buf.appendUInt64(0);
  res_stream->writeBodyChunk(Buffer(buf.data(), buf.size()));

  res_stream->finishResponse();
}

void TSDBServlet::fetchPartitionInfo(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    URI* uri) {
  const auto& params = uri->queryParams();

  String tsdb_namespace;
  if (!URI::getParam(params, "namespace", &tsdb_namespace)) {
    res->setStatus(stx::http::kStatusBadRequest);
    res->addBody("missing ?namespace=... parameter");
    return;
  }

  String stream_key;
  if (!URI::getParam(params, "stream", &stream_key)) {
    res->setStatus(stx::http::kStatusBadRequest);
    res->addBody("missing ?stream=... parameter");
    return;
  }

  String partition_key;
  if (!URI::getParam(params, "partition", &partition_key)) {
    res->setStatus(stx::http::kStatusBadRequest);
    res->addBody("missing ?partition=... parameter");
    return;
  }

  auto partition = node_->findPartition(
      tsdb_namespace,
      stream_key,
      SHA1Hash::fromHexString(partition_key));

  PartitionInfo pinfo;
  pinfo.set_partition_key(partition_key);
  if (!partition.isEmpty()) {
    pinfo = partition.get()->partitionInfo();
  }

  res->setStatus(http::kStatusOK);
  res->addHeader("Content-Type", "application/x-protobuf");
  res->addBody(*msg::encode(pinfo));
}

void TSDBServlet::executeSQL(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    URI* uri) {
  auto tsdb_namespace = req->getHeader("X-TSDB-Namespace");
  auto query = req->body().toString();

  Buffer result;
  //node_->sqlEngine()->executeQuery(
  //    tsdb_namespace,
  //    query,
  //    new csql::ASCIITableFormat(BufferOutputStream::fromBuffer(&result)));

  res->setStatus(http::kStatusOK);
  res->addHeader("Content-Type", "text/plain");
  res->addHeader("Connection", "close");
  res->addBody(result);
}

void TSDBServlet::executeSQLStream(
    const http::HTTPRequest* req,
    http::HTTPResponse* res,
    RefPtr<http::HTTPResponseStream> res_stream,
    URI* uri) {
  http::HTTPSSEStream sse_stream(res, res_stream);
  sse_stream.start();

  try {
    const auto& params = uri->queryParams();

    String tsdb_namespace;
    if (!URI::getParam(params, "namespace", &tsdb_namespace)) {
      RAISE(kRuntimeError, "missing ?namespace=... parameter");
    }

    String query;
    if (!URI::getParam(params, "query", &query)) {
      RAISE(kRuntimeError, "missing ?query=... parameter");
    }

    //node_->sqlEngine()->executeQuery(
    //    tsdb_namespace,
    //    query,
    //    new csql::JSONSSEStreamFormat(&sse_stream));

  } catch (const StandardException& e) {
    stx::logError("sql", e, "SQL execution failed");

    Buffer buf;
    json::JSONOutputStream json(BufferOutputStream::fromBuffer(&buf));
    json.beginObject();
    json.addObjectEntry("error");
    json.addString(e.what());
    json.endObject();

    sse_stream.sendEvent(buf, Some(String("error")));
  }

  sse_stream.finish();
}


}

