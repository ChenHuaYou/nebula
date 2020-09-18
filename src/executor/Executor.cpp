/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License,
 * attached with Common Clause Condition 1.0, found in the LICENSES directory.
 */

#include "executor/Executor.h"

#include <folly/String.h>
#include <folly/executors/InlineExecutor.h>

#include "common/interface/gen-cpp2/graph_types.h"
#include "context/ExecutionContext.h"
#include "context/QueryContext.h"
#include "executor/ExecutionError.h"
#include "executor/admin/BalanceExecutor.h"
#include "executor/admin/BalanceLeadersExecutor.h"
#include "executor/admin/ChangePasswordExecutor.h"
#include "executor/admin/CharsetExecutor.h"
#include "executor/admin/ConfigExecutor.h"
#include "executor/admin/CreateUserExecutor.h"
#include "executor/admin/DropUserExecutor.h"
#include "executor/admin/GrantRoleExecutor.h"
#include "executor/admin/ListRolesExecutor.h"
#include "executor/admin/ListUserRolesExecutor.h"
#include "executor/admin/ListUsersExecutor.h"
#include "executor/admin/PartExecutor.h"
#include "executor/admin/RevokeRoleExecutor.h"
#include "executor/admin/ShowBalanceExecutor.h"
#include "executor/admin/ShowHostsExecutor.h"
#include "executor/admin/SnapshotExecutor.h"
#include "executor/admin/SpaceExecutor.h"
#include "executor/admin/StopBalanceExecutor.h"
#include "executor/admin/SubmitJobExecutor.h"
#include "executor/admin/SwitchSpaceExecutor.h"
#include "executor/admin/UpdateUserExecutor.h"
#include "executor/logic/LoopExecutor.h"
#include "executor/logic/PassThroughExecutor.h"
#include "executor/logic/SelectExecutor.h"
#include "executor/logic/StartExecutor.h"
#include "executor/maintain/EdgeExecutor.h"
#include "executor/maintain/TagExecutor.h"
#include "executor/mutate/DeleteExecutor.h"
#include "executor/mutate/InsertExecutor.h"
#include "executor/mutate/UpdateExecutor.h"
#include "executor/query/AggregateExecutor.h"
#include "executor/query/DataCollectExecutor.h"
#include "executor/query/DataJoinExecutor.h"
#include "executor/query/DedupExecutor.h"
#include "executor/query/FilterExecutor.h"
#include "executor/query/GetEdgesExecutor.h"
#include "executor/query/GetNeighborsExecutor.h"
#include "executor/query/GetVerticesExecutor.h"
#include "executor/query/IndexScanExecutor.h"
#include "executor/query/IntersectExecutor.h"
#include "executor/query/LimitExecutor.h"
#include "executor/query/MinusExecutor.h"
#include "executor/query/ProjectExecutor.h"
#include "executor/query/SortExecutor.h"
#include "executor/query/UnionExecutor.h"
#include "planner/Admin.h"
#include "planner/Logic.h"
#include "planner/Maintain.h"
#include "planner/Mutate.h"
#include "planner/PlanNode.h"
#include "planner/Query.h"
#include "util/ObjectPool.h"
#include "util/ScopedTimer.h"

using folly::stringPrintf;

namespace nebula {
namespace graph {

// static
Executor *Executor::create(const PlanNode *node, QueryContext *qctx) {
    std::unordered_map<int64_t, Executor *> visited;
    return makeExecutor(node, qctx, &visited);
}

// static
Executor *Executor::makeExecutor(const PlanNode *node,
                                 QueryContext *qctx,
                                 std::unordered_map<int64_t, Executor *> *visited) {
    DCHECK(qctx != nullptr);
    DCHECK(node != nullptr);
    auto iter = visited->find(node->id());
    if (iter != visited->end()) {
        return iter->second;
    }

    Executor *exec = makeExecutor(qctx, node);
    switch (node->dependencies().size()) {
        case 0: {
            // Do nothing
            break;
        }
        case 1: {
            if (node->kind() == PlanNode::Kind::kSelect) {
                auto select = asNode<Select>(node);
                auto thenBody = makeExecutor(select->then(), qctx, visited);
                auto elseBody = makeExecutor(select->otherwise(), qctx, visited);
                auto selectExecutor = static_cast<SelectExecutor *>(exec);
                selectExecutor->setThenBody(thenBody);
                selectExecutor->setElseBody(elseBody);
            } else if (node->kind() == PlanNode::Kind::kLoop) {
                auto loop = asNode<Loop>(node);
                auto body = makeExecutor(loop->body(), qctx, visited);
                auto loopExecutor = static_cast<LoopExecutor *>(exec);
                loopExecutor->setLoopBody(body);
            }
            auto dep = makeExecutor(node->dep(0), qctx, visited);
            exec->dependsOn(dep);
            break;
        }
        case 2: {
            auto left = makeExecutor(node->dep(0), qctx, visited);
            auto right = makeExecutor(node->dep(1), qctx, visited);
            exec->dependsOn(left)->dependsOn(right);
            break;
        }
        default: {
            LOG(FATAL) << "Unsupported plan node type which has dependencies: "
                       << node->dependencies().size();
            break;
        }
    }

    visited->insert({node->id(), exec});
    return exec;
}

// static
Executor *Executor::makeExecutor(QueryContext *qctx, const PlanNode *node) {
    auto pool = qctx->objPool();
    switch (node->kind()) {
        case PlanNode::Kind::kPassThrough: {
            return pool->add(new PassThroughExecutor(node, qctx));
        }
        case PlanNode::Kind::kAggregate: {
            return pool->add(new AggregateExecutor(node, qctx));
        }
        case PlanNode::Kind::kSort: {
            return pool->add(new SortExecutor(node, qctx));
        }
        case PlanNode::Kind::kFilter: {
            return pool->add(new FilterExecutor(node, qctx));
        }
        case PlanNode::Kind::kGetEdges: {
            return pool->add(new GetEdgesExecutor(node, qctx));
        }
        case PlanNode::Kind::kGetVertices: {
            return pool->add(new GetVerticesExecutor(node, qctx));
        }
        case PlanNode::Kind::kGetNeighbors: {
            return pool->add(new GetNeighborsExecutor(node, qctx));
        }
        case PlanNode::Kind::kLimit: {
            return pool->add(new LimitExecutor(node, qctx));
        }
        case PlanNode::Kind::kProject: {
            return pool->add(new ProjectExecutor(node, qctx));
        }
        case PlanNode::Kind::kIndexScan: {
            return pool->add(new IndexScanExecutor(node, qctx));
        }
        case PlanNode::Kind::kStart: {
            return pool->add(new StartExecutor(node, qctx));
        }
        case PlanNode::Kind::kUnion: {
            return pool->add(new UnionExecutor(node, qctx));
        }
        case PlanNode::Kind::kIntersect: {
            return pool->add(new IntersectExecutor(node, qctx));
        }
        case PlanNode::Kind::kMinus: {
            return pool->add(new MinusExecutor(node, qctx));
        }
        case PlanNode::Kind::kLoop: {
            return pool->add(new LoopExecutor(node, qctx));
        }
        case PlanNode::Kind::kSelect: {
            return pool->add(new SelectExecutor(node, qctx));
        }
        case PlanNode::Kind::kDedup: {
            return pool->add(new DedupExecutor(node, qctx));
        }
        case PlanNode::Kind::kSwitchSpace: {
            return pool->add(new SwitchSpaceExecutor(node, qctx));
        }
        case PlanNode::Kind::kCreateSpace: {
            return pool->add(new CreateSpaceExecutor(node, qctx));
        }
        case PlanNode::Kind::kDescSpace: {
            return pool->add(new DescSpaceExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowSpaces: {
            return pool->add(new ShowSpacesExecutor(node, qctx));
        }
        case PlanNode::Kind::kDropSpace: {
            return pool->add(new DropSpaceExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowCreateSpace: {
            return pool->add(new ShowCreateSpaceExecutor(node, qctx));
        }
        case PlanNode::Kind::kCreateTag: {
            return pool->add(new CreateTagExecutor(node, qctx));
        }
        case PlanNode::Kind::kDescTag: {
            return pool->add(new DescTagExecutor(node, qctx));
        }
        case PlanNode::Kind::kAlterTag: {
            return pool->add(new AlterTagExecutor(node, qctx));
        }
        case PlanNode::Kind::kCreateEdge: {
            return pool->add(new CreateEdgeExecutor(node, qctx));
        }
        case PlanNode::Kind::kDescEdge: {
            return pool->add(new DescEdgeExecutor(node, qctx));
        }
        case PlanNode::Kind::kAlterEdge: {
            return pool->add(new AlterEdgeExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowTags: {
            return pool->add(new ShowTagsExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowEdges: {
            return pool->add(new ShowEdgesExecutor(node, qctx));
        }
        case PlanNode::Kind::kDropTag: {
            return pool->add(new DropTagExecutor(node, qctx));
        }
        case PlanNode::Kind::kDropEdge: {
            return pool->add(new DropEdgeExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowCreateTag: {
            return pool->add(new ShowCreateTagExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowCreateEdge: {
            return pool->add(new ShowCreateEdgeExecutor(node, qctx));
        }
        case PlanNode::Kind::kInsertVertices: {
            return pool->add(new InsertVerticesExecutor(node, qctx));
        }
        case PlanNode::Kind::kInsertEdges: {
            return pool->add(new InsertEdgesExecutor(node, qctx));
        }
        case PlanNode::Kind::kDataCollect: {
            return pool->add(new DataCollectExecutor(node, qctx));
        }
        case PlanNode::Kind::kCreateSnapshot: {
            return pool->add(new CreateSnapshotExecutor(node, qctx));
        }
        case PlanNode::Kind::kDropSnapshot: {
            return pool->add(new DropSnapshotExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowSnapshots: {
            return pool->add(new ShowSnapshotsExecutor(node, qctx));
        }
        case PlanNode::Kind::kDataJoin: {
            return pool->add(new DataJoinExecutor(node, qctx));
        }
        case PlanNode::Kind::kDeleteVertices: {
            return pool->add(new DeleteVerticesExecutor(node, qctx));
        }
        case PlanNode::Kind::kDeleteEdges: {
            return pool->add(new DeleteEdgesExecutor(node, qctx));
        }
        case PlanNode::Kind::kUpdateVertex: {
            return pool->add(new UpdateVertexExecutor(node, qctx));
        }
        case PlanNode::Kind::kUpdateEdge: {
            return pool->add(new UpdateEdgeExecutor(node, qctx));
        }
        case PlanNode::Kind::kCreateUser: {
            return pool->add(new CreateUserExecutor(node, qctx));
        }
        case PlanNode::Kind::kDropUser: {
            return pool->add(new DropUserExecutor(node, qctx));
        }
        case PlanNode::Kind::kUpdateUser: {
            return pool->add(new UpdateUserExecutor(node, qctx));
        }
        case PlanNode::Kind::kGrantRole: {
            return pool->add(new GrantRoleExecutor(node, qctx));
        }
        case PlanNode::Kind::kRevokeRole: {
            return pool->add(new RevokeRoleExecutor(node, qctx));
        }
        case PlanNode::Kind::kChangePassword: {
            return pool->add(new ChangePasswordExecutor(node, qctx));
        }
        case PlanNode::Kind::kListUserRoles: {
            return pool->add(new ListUserRolesExecutor(node, qctx));
        }
        case PlanNode::Kind::kListUsers: {
            return pool->add(new ListUsersExecutor(node, qctx));
        }
        case PlanNode::Kind::kListRoles: {
            return pool->add(new ListRolesExecutor(node, qctx));
        }
        case PlanNode::Kind::kBalanceLeaders: {
            return pool->add(new BalanceLeadersExecutor(node, qctx));
        }
        case PlanNode::Kind::kBalance: {
            return pool->add(new BalanceExecutor(node, qctx));
        }
        case PlanNode::Kind::kStopBalance: {
            return pool->add(new StopBalanceExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowBalance: {
            return pool->add(new ShowBalanceExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowConfigs: {
            return pool->add(new ShowConfigsExecutor(node, qctx));
        }
        case PlanNode::Kind::kSetConfig: {
            return pool->add(new SetConfigExecutor(node, qctx));
        }
        case PlanNode::Kind::kGetConfig: {
            return pool->add(new GetConfigExecutor(node, qctx));
        }
        case PlanNode::Kind::kSubmitJob: {
            return pool->add(new SubmitJobExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowHosts: {
            return pool->add(new ShowHostsExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowParts: {
            return pool->add(new ShowPartsExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowCharset: {
            return pool->add(new ShowCharsetExecutor(node, qctx));
        }
        case PlanNode::Kind::kShowCollation: {
            return pool->add(new ShowCollationExecutor(node, qctx));
        }
        case PlanNode::Kind::kUnknown: {
            break;
        }
    }
    LOG(FATAL) << "Unknown plan node kind " << static_cast<int32_t>(node->kind());
    return nullptr;
}

Executor::Executor(const std::string &name, const PlanNode *node, QueryContext *qctx)
    : id_(DCHECK_NOTNULL(node)->id()),
      name_(name),
      node_(DCHECK_NOTNULL(node)),
      qctx_(DCHECK_NOTNULL(qctx)),
      ectx_(DCHECK_NOTNULL(qctx->ectx())) {
    // Initialize the position in ExecutionContext for each executor before execution plan
    // starting to run. This will avoid lock something for thread safety in real execution
    if (!ectx_->exist(node->outputVar())) {
        ectx_->initVar(node->outputVar());
    }
}

Executor::~Executor() {}

Status Executor::open() {
    numRows_ = 0;
    execTime_ = 0;
    totalDuration_.reset();
    return Status::OK();
}

Status Executor::close() {
    cpp2::ProfilingStats stats;
    stats.set_total_duration_in_us(totalDuration_.elapsedInUSec());
    stats.set_rows(numRows_);
    stats.set_exec_duration_in_us(execTime_);
    qctx()->addProfilingData(node_->id(), std::move(stats));
    return Status::OK();
}

folly::Future<Status> Executor::start(Status status) const {
    return folly::makeFuture(std::move(status)).via(runner());
}

folly::Future<Status> Executor::error(Status status) const {
    return folly::makeFuture<Status>(ExecutionError(std::move(status))).via(runner());
}

Status Executor::finish(Result &&result) {
    numRows_ = result.size();
    ectx_->setResult(node()->outputVar(), std::move(result));
    return Status::OK();
}

Status Executor::finish(Value &&value) {
    return finish(ResultBuilder().value(std::move(value)).iter(Iterator::Kind::kDefault).finish());
}

folly::Executor *Executor::runner() const {
    if (!qctx() || !qctx()->rctx() || !qctx()->rctx()->runner()) {
        // This is just for test
        return &folly::InlineExecutor::instance();
    }
    return qctx()->rctx()->runner();
}

}   // namespace graph
}   // namespace nebula