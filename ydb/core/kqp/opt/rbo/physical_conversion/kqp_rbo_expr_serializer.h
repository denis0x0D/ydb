#pragma once

#include <yql/essentials/ast/yql_expr.h>

#include <optional>

namespace NKikimr::NKqp {

enum class ESerializedTypes {
    None,       // Types are not written.
    IfPresent,  // Types of typed nodes are written; untyped nodes are allowed.
    Required,   // Every node must be typed.
};

/**
 * Compact binary form of an expression graph that does not depend on a TExprContext.
 *
 * Nodes are written once each, in post-order, so children always refer to earlier nodes and shared
 * nodes stay shared. Argument names, positions and node ids are not written, so equal graphs give
 * equal bytes. Written types are restored on load without running type annotation.
 *
 * Returns std::nullopt if the graph has something that can't be written, e.g. an untyped node when
 * types are required, or an unsupported type kind.
 */
std::optional<TString> SerializeExprGraph(const NYql::TExprNode& root, ESerializedTypes types);

// Returns nullptr if the data was not written by SerializeExprGraph of this version or is truncated.
// Every node gets `pos`.
NYql::TExprNode::TPtr DeserializeExprGraph(TStringBuf data, NYql::TExprContext& ctx, NYql::TPositionHandle pos);

} // namespace NKikimr::NKqp
