#include "kqp_rbo_expr_serializer.h"

#include <yql/essentials/public/udf/udf_data_type.h>

#include <util/generic/hash.h>
#include <util/generic/hash_set.h>
#include <util/generic/vector.h>
#include <util/string/builder.h>

namespace NKikimr::NKqp {

using namespace NYql;

namespace {

// Layout, all integers are varints:
//   magic "KQPS", version byte, flags
//   strings: count, (length, bytes)*          -- in first-use order
//   types:   count, type records               -- children before parents
//   nodes:   count, node records               -- post-order, the root is the last node
// A node record is its kind byte, when types are written its type index + 1 (0 for an untyped node), then
// the payload.
// Children are written as the distance back to an earlier node.
constexpr TStringBuf Magic = "KQPS";
constexpr ui8 Version = 1;
constexpr ui64 WithTypesFlag = 1;

enum class ENodeKind : ui8 {
    Atom = 0,
    Callable = 1,
    List = 2,
    Lambda = 3,
    Argument = 4,
    Arguments = 5,
    World = 6,
};

class TWriter {
public:
    void WriteVar(ui64 value) {
        while (value >= 0x80) {
            Out.push_back(static_cast<char>(value | 0x80));
            value >>= 7;
        }
        Out.push_back(static_cast<char>(value));
    }

    void WriteByte(ui8 value) {
        Out.push_back(static_cast<char>(value));
    }

    void WriteRaw(TStringBuf value) {
        Out.append(value.data(), value.size());
    }

    void WriteBytes(TStringBuf value) {
        WriteVar(value.size());
        WriteRaw(value);
    }

    TString Out;
};

class TReader {
public:
    explicit TReader(TStringBuf data)
        : Data(data)
    {}

    bool ReadVar(ui64& value) {
        value = 0;
        for (ui32 shift = 0; shift < 64; shift += 7) {
            if (Pos >= Data.size()) {
                return false;
            }
            const ui8 byte = Data[Pos++];
            value |= ui64(byte & 0x7F) << shift;
            if (!(byte & 0x80)) {
                return true;
            }
        }
        return false;
    }

    bool ReadVar32(ui32& value) {
        ui64 wide = 0;
        if (!ReadVar(wide) || wide > Max<ui32>()) {
            return false;
        }
        value = static_cast<ui32>(wide);
        return true;
    }

    // A count of items that take at least one byte each, so it can't exceed the rest of the data.
    bool ReadCount(ui32& value) {
        return ReadVar32(value) && value <= Remaining();
    }

    bool ReadByte(ui8& value) {
        if (Pos >= Data.size()) {
            return false;
        }
        value = Data[Pos++];
        return true;
    }

    bool ReadRaw(size_t size, TStringBuf& value) {
        if (size > Remaining()) {
            return false;
        }
        value = Data.SubStr(Pos, size);
        Pos += size;
        return true;
    }

    bool ReadBytes(TStringBuf& value) {
        ui64 size = 0;
        return ReadVar(size) && ReadRaw(size, value);
    }

    size_t Remaining() const {
        return Data.size() - Pos;
    }

private:
    TStringBuf Data;
    size_t Pos = 0;
};

class TGraphWriter {
public:
    explicit TGraphWriter(ESerializedTypes types)
        : Types(types)
        , WithTypes(types != ESerializedTypes::None)
    {}

    std::optional<TString> Write(const TExprNode& root) {
        if (!CollectNodes(root)) {
            return std::nullopt;
        }

        Writer.WriteRaw(Magic);
        Writer.WriteByte(Version);
        Writer.WriteVar(WithTypes ? WithTypesFlag : 0);

        Writer.WriteVar(Strings.size());
        for (const auto& string : Strings) {
            Writer.WriteBytes(string);
        }

        Writer.WriteVar(TypeList.size());
        for (const auto* type : TypeList) {
            WriteType(*type);
        }

        Writer.WriteVar(Nodes.size());
        for (size_t index = 0; index < Nodes.size(); ++index) {
            WriteNode(*Nodes[index], index);
        }
        return std::move(Writer.Out);
    }

private:
    // Post-order without recursion: each node once, children in order.
    bool CollectNodes(const TExprNode& root) {
        TVector<std::pair<const TExprNode*, ui32>> stack;
        THashSet<const TExprNode*> visited;
        stack.emplace_back(&root, 0);
        visited.insert(&root);
        while (!stack.empty()) {
            auto& [node, nextChild] = stack.back();
            if (nextChild < node->ChildrenSize()) {
                const auto* child = node->Child(nextChild++);
                if (visited.insert(child).second) {
                    stack.emplace_back(child, 0);
                }
                continue;
            }

            if (!AddNode(*node)) {
                return false;
            }
            stack.pop_back();
        }
        return true;
    }

    bool AddNode(const TExprNode& node) {
        if (node.IsAtom() || node.IsCallable()) {
            AddString(node.Content());
        }
        if (WithTypes) {
            const TTypeAnnotationNode* type = node.GetTypeAnn();
            if (type ? !AddType(*type) : Types == ESerializedTypes::Required) {
                return false;
            }
        }
        NodeIds.emplace(&node, Nodes.size());
        Nodes.push_back(&node);
        return true;
    }

    void AddString(TStringBuf string) {
        if (StringIds.emplace(string, Strings.size()).second) {
            Strings.push_back(string);
        }
    }

    bool AddType(const TTypeAnnotationNode& type) {
        if (TypeIds.contains(&type)) {
            return true;
        }

        bool supported = true;
        switch (type.GetKind()) {
            case ETypeAnnotationKind::Unit:
            case ETypeAnnotationKind::World:
            case ETypeAnnotationKind::Void:
            case ETypeAnnotationKind::Null:
            case ETypeAnnotationKind::Generic:
            case ETypeAnnotationKind::EmptyList:
            case ETypeAnnotationKind::EmptyDict:
            case ETypeAnnotationKind::Pg:
                break;
            case ETypeAnnotationKind::Data: {
                const auto& data = *type.Cast<TDataExprType>();
                AddString(data.GetName());
                if (const auto* params = dynamic_cast<const TDataExprParamsType*>(&data)) {
                    AddString(params->GetParamOne());
                    AddString(params->GetParamTwo());
                }
                break;
            }
            case ETypeAnnotationKind::Optional:
                supported = AddType(*type.Cast<TOptionalExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::List:
                supported = AddType(*type.Cast<TListExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Stream:
                supported = AddType(*type.Cast<TStreamExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Flow:
                supported = AddType(*type.Cast<TFlowExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Block:
                supported = AddType(*type.Cast<TBlockExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Scalar:
                supported = AddType(*type.Cast<TScalarExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Linear:
                supported = AddType(*type.Cast<TLinearExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Type:
                supported = AddType(*type.Cast<TTypeExprType>()->GetType());
                break;
            case ETypeAnnotationKind::Variant:
                supported = AddType(*type.Cast<TVariantExprType>()->GetUnderlyingType());
                break;
            case ETypeAnnotationKind::Tagged: {
                const auto& tagged = *type.Cast<TTaggedExprType>();
                AddString(tagged.GetTag());
                supported = AddType(*tagged.GetBaseType());
                break;
            }
            case ETypeAnnotationKind::Resource:
                AddString(type.Cast<TResourceExprType>()->GetTag());
                break;
            case ETypeAnnotationKind::Tuple:
                for (const auto* item : type.Cast<TTupleExprType>()->GetItems()) {
                    supported = supported && AddType(*item);
                }
                break;
            case ETypeAnnotationKind::Multi:
                for (const auto* item : type.Cast<TMultiExprType>()->GetItems()) {
                    supported = supported && AddType(*item);
                }
                break;
            case ETypeAnnotationKind::Struct:
                for (const auto* item : type.Cast<TStructExprType>()->GetItems()) {
                    AddString(item->GetName());
                    supported = supported && AddType(*item->GetItemType());
                }
                break;
            case ETypeAnnotationKind::Item: {
                const auto& item = *type.Cast<TItemExprType>();
                AddString(item.GetName());
                supported = AddType(*item.GetItemType());
                break;
            }
            case ETypeAnnotationKind::Dict: {
                const auto& dict = *type.Cast<TDictExprType>();
                supported = AddType(*dict.GetKeyType()) && AddType(*dict.GetPayloadType());
                break;
            }
            case ETypeAnnotationKind::Callable: {
                const auto& callable = *type.Cast<TCallableExprType>();
                AddString(callable.GetPayload());
                supported = AddType(*callable.GetReturnType());
                for (const auto& argument : callable.GetArguments()) {
                    AddString(argument.Name);
                    supported = supported && AddType(*argument.Type);
                }
                break;
            }
            default:
                // Error, Universal, UniversalStruct, DynamicLinear: never part of a cached program.
                supported = false;
                break;
        }

        if (!supported) {
            return false;
        }
        TypeIds.emplace(&type, TypeList.size());
        TypeList.push_back(&type);
        return true;
    }

    void WriteString(TStringBuf string) {
        Writer.WriteVar(StringIds.at(string));
    }

    void WriteTypeRef(const TTypeAnnotationNode& type) {
        Writer.WriteVar(TypeIds.at(&type));
    }

    void WriteType(const TTypeAnnotationNode& type) {
        Writer.WriteVar(static_cast<ui64>(type.GetKind()));
        switch (type.GetKind()) {
            case ETypeAnnotationKind::Data: {
                const auto& data = *type.Cast<TDataExprType>();
                WriteString(data.GetName());
                if (const auto* params = dynamic_cast<const TDataExprParamsType*>(&data)) {
                    Writer.WriteByte(1);
                    WriteString(params->GetParamOne());
                    WriteString(params->GetParamTwo());
                } else {
                    Writer.WriteByte(0);
                }
                break;
            }
            case ETypeAnnotationKind::Pg:
                Writer.WriteVar(type.Cast<TPgExprType>()->GetId());
                break;
            case ETypeAnnotationKind::Optional:
                WriteTypeRef(*type.Cast<TOptionalExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::List:
                WriteTypeRef(*type.Cast<TListExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Stream:
                WriteTypeRef(*type.Cast<TStreamExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Flow:
                WriteTypeRef(*type.Cast<TFlowExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Block:
                WriteTypeRef(*type.Cast<TBlockExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Scalar:
                WriteTypeRef(*type.Cast<TScalarExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Linear:
                WriteTypeRef(*type.Cast<TLinearExprType>()->GetItemType());
                break;
            case ETypeAnnotationKind::Type:
                WriteTypeRef(*type.Cast<TTypeExprType>()->GetType());
                break;
            case ETypeAnnotationKind::Variant:
                WriteTypeRef(*type.Cast<TVariantExprType>()->GetUnderlyingType());
                break;
            case ETypeAnnotationKind::Tagged: {
                const auto& tagged = *type.Cast<TTaggedExprType>();
                WriteString(tagged.GetTag());
                WriteTypeRef(*tagged.GetBaseType());
                break;
            }
            case ETypeAnnotationKind::Resource:
                WriteString(type.Cast<TResourceExprType>()->GetTag());
                break;
            case ETypeAnnotationKind::Tuple: {
                const auto& items = type.Cast<TTupleExprType>()->GetItems();
                Writer.WriteVar(items.size());
                for (const auto* item : items) {
                    WriteTypeRef(*item);
                }
                break;
            }
            case ETypeAnnotationKind::Multi: {
                const auto& items = type.Cast<TMultiExprType>()->GetItems();
                Writer.WriteVar(items.size());
                for (const auto* item : items) {
                    WriteTypeRef(*item);
                }
                break;
            }
            case ETypeAnnotationKind::Struct: {
                const auto& items = type.Cast<TStructExprType>()->GetItems();
                Writer.WriteVar(items.size());
                for (const auto* item : items) {
                    WriteString(item->GetName());
                    WriteTypeRef(*item->GetItemType());
                }
                break;
            }
            case ETypeAnnotationKind::Item: {
                const auto& item = *type.Cast<TItemExprType>();
                WriteString(item.GetName());
                WriteTypeRef(*item.GetItemType());
                break;
            }
            case ETypeAnnotationKind::Dict: {
                const auto& dict = *type.Cast<TDictExprType>();
                WriteTypeRef(*dict.GetKeyType());
                WriteTypeRef(*dict.GetPayloadType());
                break;
            }
            case ETypeAnnotationKind::Callable: {
                const auto& callable = *type.Cast<TCallableExprType>();
                WriteTypeRef(*callable.GetReturnType());
                Writer.WriteVar(callable.GetOptionalArgumentsCount());
                WriteString(callable.GetPayload());
                Writer.WriteVar(callable.GetArguments().size());
                for (const auto& argument : callable.GetArguments()) {
                    WriteTypeRef(*argument.Type);
                    WriteString(argument.Name);
                    Writer.WriteVar(argument.Flags);
                }
                break;
            }
            default:
                // Kinds without payload.
                break;
        }
    }

    void WriteChildren(const TExprNode& node, size_t index) {
        Writer.WriteVar(node.ChildrenSize());
        for (const auto& child : node.Children()) {
            Writer.WriteVar(index - NodeIds.at(child.Get()));
        }
    }

    void WriteNode(const TExprNode& node, size_t index) {
        ENodeKind kind = ENodeKind::World;
        switch (node.Type()) {
            case TExprNode::Atom: kind = ENodeKind::Atom; break;
            case TExprNode::Callable: kind = ENodeKind::Callable; break;
            case TExprNode::List: kind = ENodeKind::List; break;
            case TExprNode::Lambda: kind = ENodeKind::Lambda; break;
            case TExprNode::Argument: kind = ENodeKind::Argument; break;
            case TExprNode::Arguments: kind = ENodeKind::Arguments; break;
            case TExprNode::World: kind = ENodeKind::World; break;
            default: Y_ENSURE(false, "Unexpected expression node type " << static_cast<ui32>(node.Type()));
        }
        Writer.WriteByte(static_cast<ui8>(kind));
        if (WithTypes) {
            const TTypeAnnotationNode* type = node.GetTypeAnn();
            Writer.WriteVar(type ? TypeIds.at(type) + 1 : 0);
        }

        switch (kind) {
            case ENodeKind::Atom:
                Writer.WriteVar(node.Flags());
                WriteString(node.Content());
                break;
            case ENodeKind::Callable:
                WriteString(node.Content());
                WriteChildren(node, index);
                break;
            case ENodeKind::List:
            case ENodeKind::Lambda:
            case ENodeKind::Arguments:
                WriteChildren(node, index);
                break;
            case ENodeKind::Argument:
            case ENodeKind::World:
                break;
        }
    }

    const ESerializedTypes Types;
    const bool WithTypes;
    TWriter Writer;
    TVector<const TExprNode*> Nodes;
    THashMap<const TExprNode*, size_t> NodeIds;
    TVector<TStringBuf> Strings;
    THashMap<TStringBuf, size_t> StringIds;
    TVector<const TTypeAnnotationNode*> TypeList;
    THashMap<const TTypeAnnotationNode*, size_t> TypeIds;
};

class TGraphReader {
public:
    TGraphReader(TStringBuf data, TExprContext& ctx, TPositionHandle pos)
        : Reader(data)
        , Ctx(ctx)
        , Pos(pos)
    {}

    TExprNode::TPtr Read() {
        TStringBuf magic;
        ui8 version = 0;
        ui64 flags = 0;
        if (!Reader.ReadRaw(Magic.size(), magic) || magic != Magic || !Reader.ReadByte(version) || version != Version
            || !Reader.ReadVar(flags) || (flags & ~WithTypesFlag)) {
            return nullptr;
        }
        WithTypes = flags & WithTypesFlag;

        ui32 count = 0;
        if (!Reader.ReadCount(count)) {
            return nullptr;
        }
        Strings.reserve(count);
        for (ui32 i = 0; i < count; ++i) {
            TStringBuf string;
            if (!Reader.ReadBytes(string)) {
                return nullptr;
            }
            Strings.push_back(string);
        }

        if (!Reader.ReadCount(count)) {
            return nullptr;
        }
        Types.reserve(count);
        for (ui32 i = 0; i < count; ++i) {
            if (!ReadType()) {
                return nullptr;
            }
        }

        if (!Reader.ReadCount(count) || count == 0) {
            return nullptr;
        }
        Nodes.reserve(count);
        for (ui32 i = 0; i < count; ++i) {
            if (!ReadNode()) {
                return nullptr;
            }
        }
        if (Reader.Remaining() != 0) {
            return nullptr;
        }
        return Nodes.back();
    }

private:
    bool ReadString(TStringBuf& string) {
        ui32 index = 0;
        if (!Reader.ReadVar32(index) || index >= Strings.size()) {
            return false;
        }
        string = Strings[index];
        return true;
    }

    bool ReadTypeRef(const TTypeAnnotationNode*& type) {
        ui32 index = 0;
        if (!Reader.ReadVar32(index) || index >= Types.size()) {
            return false;
        }
        type = Types[index];
        return true;
    }

    bool ReadTypeList(TTypeAnnotationNode::TListType& items) {
        ui32 count = 0;
        if (!Reader.ReadCount(count)) {
            return false;
        }
        items.reserve(count);
        for (ui32 i = 0; i < count; ++i) {
            const TTypeAnnotationNode* item = nullptr;
            if (!ReadTypeRef(item)) {
                return false;
            }
            items.push_back(item);
        }
        return true;
    }

    template <typename TType>
    bool ReadWrapperType(const TTypeAnnotationNode*& result) {
        const TTypeAnnotationNode* item = nullptr;
        if (!ReadTypeRef(item)) {
            return false;
        }
        result = Ctx.MakeType<TType>(item);
        return true;
    }

    bool ReadType() {
        ui64 kind = 0;
        if (!Reader.ReadVar(kind)) {
            return false;
        }

        const TTypeAnnotationNode* result = nullptr;
        bool ok = true;
        switch (static_cast<ETypeAnnotationKind>(kind)) {
            case ETypeAnnotationKind::Unit:
                result = Ctx.MakeType<TUnitExprType>();
                break;
            case ETypeAnnotationKind::World:
                result = Ctx.MakeType<TWorldExprType>();
                break;
            case ETypeAnnotationKind::Void:
                result = Ctx.MakeType<TVoidExprType>();
                break;
            case ETypeAnnotationKind::Null:
                result = Ctx.MakeType<TNullExprType>();
                break;
            case ETypeAnnotationKind::Generic:
                result = Ctx.MakeType<TGenericExprType>();
                break;
            case ETypeAnnotationKind::EmptyList:
                result = Ctx.MakeType<TEmptyListExprType>();
                break;
            case ETypeAnnotationKind::EmptyDict:
                result = Ctx.MakeType<TEmptyDictExprType>();
                break;
            case ETypeAnnotationKind::Data: {
                TStringBuf name;
                ui8 hasParams = 0;
                if (!ReadString(name) || !Reader.ReadByte(hasParams)) {
                    return false;
                }
                const auto slot = NUdf::FindDataSlot(name);
                if (!slot) {
                    return false;
                }
                if (hasParams) {
                    TStringBuf one;
                    TStringBuf two;
                    ok = ReadString(one) && ReadString(two);
                    if (ok) {
                        result = Ctx.MakeType<TDataExprParamsType>(*slot, one, two);
                    }
                } else {
                    result = Ctx.MakeType<TDataExprType>(*slot);
                }
                break;
            }
            case ETypeAnnotationKind::Pg: {
                ui32 id = 0;
                ok = Reader.ReadVar32(id);
                if (ok) {
                    result = Ctx.MakeType<TPgExprType>(id);
                }
                break;
            }
            case ETypeAnnotationKind::Optional:
                ok = ReadWrapperType<TOptionalExprType>(result);
                break;
            case ETypeAnnotationKind::List:
                ok = ReadWrapperType<TListExprType>(result);
                break;
            case ETypeAnnotationKind::Stream:
                ok = ReadWrapperType<TStreamExprType>(result);
                break;
            case ETypeAnnotationKind::Flow:
                ok = ReadWrapperType<TFlowExprType>(result);
                break;
            case ETypeAnnotationKind::Block:
                ok = ReadWrapperType<TBlockExprType>(result);
                break;
            case ETypeAnnotationKind::Scalar:
                ok = ReadWrapperType<TScalarExprType>(result);
                break;
            case ETypeAnnotationKind::Linear:
                ok = ReadWrapperType<TLinearExprType>(result);
                break;
            case ETypeAnnotationKind::Type:
                ok = ReadWrapperType<TTypeExprType>(result);
                break;
            case ETypeAnnotationKind::Variant:
                ok = ReadWrapperType<TVariantExprType>(result);
                break;
            case ETypeAnnotationKind::Tagged: {
                TStringBuf tag;
                const TTypeAnnotationNode* base = nullptr;
                ok = ReadString(tag) && ReadTypeRef(base);
                if (ok) {
                    result = Ctx.MakeType<TTaggedExprType>(base, tag);
                }
                break;
            }
            case ETypeAnnotationKind::Resource: {
                TStringBuf tag;
                ok = ReadString(tag);
                if (ok) {
                    result = Ctx.MakeType<TResourceExprType>(tag);
                }
                break;
            }
            case ETypeAnnotationKind::Tuple: {
                TTypeAnnotationNode::TListType items;
                ok = ReadTypeList(items);
                if (ok) {
                    result = Ctx.MakeType<TTupleExprType>(items);
                }
                break;
            }
            case ETypeAnnotationKind::Multi: {
                TTypeAnnotationNode::TListType items;
                ok = ReadTypeList(items);
                if (ok) {
                    result = Ctx.MakeType<TMultiExprType>(items);
                }
                break;
            }
            case ETypeAnnotationKind::Struct: {
                ui32 count = 0;
                ok = Reader.ReadCount(count);
                TVector<const TItemExprType*> items;
                items.reserve(ok ? count : 0);
                for (ui32 i = 0; ok && i < count; ++i) {
                    TStringBuf name;
                    const TTypeAnnotationNode* itemType = nullptr;
                    ok = ReadString(name) && ReadTypeRef(itemType);
                    if (ok) {
                        items.push_back(Ctx.MakeType<TItemExprType>(name, itemType));
                    }
                }
                if (ok) {
                    result = Ctx.MakeType<TStructExprType>(items);
                }
                break;
            }
            case ETypeAnnotationKind::Item: {
                TStringBuf name;
                const TTypeAnnotationNode* itemType = nullptr;
                ok = ReadString(name) && ReadTypeRef(itemType);
                if (ok) {
                    result = Ctx.MakeType<TItemExprType>(name, itemType);
                }
                break;
            }
            case ETypeAnnotationKind::Dict: {
                const TTypeAnnotationNode* key = nullptr;
                const TTypeAnnotationNode* payload = nullptr;
                ok = ReadTypeRef(key) && ReadTypeRef(payload);
                if (ok) {
                    result = Ctx.MakeType<TDictExprType>(key, payload);
                }
                break;
            }
            case ETypeAnnotationKind::Callable: {
                const TTypeAnnotationNode* returnType = nullptr;
                ui32 optionalCount = 0;
                TStringBuf payload;
                ui32 count = 0;
                ok = ReadTypeRef(returnType) && Reader.ReadVar32(optionalCount) && ReadString(payload) && Reader.ReadCount(count);
                TVector<TCallableExprType::TArgumentInfo> arguments;
                arguments.reserve(ok ? count : 0);
                for (ui32 i = 0; ok && i < count; ++i) {
                    TCallableExprType::TArgumentInfo argument;
                    ok = ReadTypeRef(argument.Type) && ReadString(argument.Name) && Reader.ReadVar(argument.Flags);
                    arguments.push_back(argument);
                }
                if (ok && optionalCount <= arguments.size()) {
                    result = Ctx.MakeType<TCallableExprType>(returnType, arguments, optionalCount, payload);
                }
                break;
            }
            default:
                return false;
        }

        if (!ok || !result) {
            return false;
        }
        Types.push_back(result);
        return true;
    }

    bool ReadChildren(TExprNode::TListType& children) {
        ui32 count = 0;
        if (!Reader.ReadCount(count)) {
            return false;
        }
        children.reserve(count);
        for (ui32 i = 0; i < count; ++i) {
            ui32 distance = 0;
            if (!Reader.ReadVar32(distance) || distance == 0 || distance > Nodes.size()) {
                return false;
            }
            children.push_back(Nodes[Nodes.size() - distance]);
        }
        return true;
    }

    bool ReadNode() {
        ui8 kind = 0;
        ui32 typeRef = 0;
        if (!Reader.ReadByte(kind) || (WithTypes && (!Reader.ReadVar32(typeRef) || typeRef > Types.size()))) {
            return false;
        }

        TExprNode::TPtr node;
        TExprNode::TListType children;
        switch (static_cast<ENodeKind>(kind)) {
            case ENodeKind::Atom: {
                ui32 flags = 0;
                TStringBuf content;
                if (!Reader.ReadVar32(flags) || (flags & ~TNodeFlags::FlagsMask) || !ReadString(content)) {
                    return false;
                }
                node = Ctx.NewAtom(Pos, content, flags);
                break;
            }
            case ENodeKind::Callable: {
                TStringBuf name;
                if (!ReadString(name) || !ReadChildren(children)) {
                    return false;
                }
                node = Ctx.NewCallable(Pos, name, std::move(children));
                break;
            }
            case ENodeKind::List:
                if (!ReadChildren(children)) {
                    return false;
                }
                node = Ctx.NewList(Pos, std::move(children));
                break;
            case ENodeKind::Argument:
                node = Ctx.NewArgument(Pos, TStringBuilder() << "arg" << ArgumentCount++);
                break;
            case ENodeKind::Arguments:
                if (!ReadChildren(children)) {
                    return false;
                }
                for (const auto& child : children) {
                    if (!child->IsArgument() || !BoundArguments.insert(child.Get()).second) {
                        return false;
                    }
                }
                node = Ctx.NewArguments(Pos, std::move(children));
                break;
            case ENodeKind::Lambda:
                if (!ReadChildren(children) || children.empty() || children.front()->Type() != TExprNode::Arguments) {
                    return false;
                }
                node = Ctx.NewLambda(Pos, std::move(children));
                break;
            case ENodeKind::World:
                node = Ctx.NewWorld(Pos);
                break;
            default:
                return false;
        }

        if (typeRef) {
            node->SetTypeAnn(Types[typeRef - 1]);
        }
        Nodes.push_back(std::move(node));
        return true;
    }

    TReader Reader;
    TExprContext& Ctx;
    const TPositionHandle Pos;
    bool WithTypes = false;
    TVector<TStringBuf> Strings;
    TVector<const TTypeAnnotationNode*> Types;
    TVector<TExprNode::TPtr> Nodes;
    THashSet<const TExprNode*> BoundArguments;
    ui32 ArgumentCount = 0;
};

} // anonymous namespace

std::optional<TString> SerializeExprGraph(const TExprNode& root, ESerializedTypes types) {
    return TGraphWriter(types).Write(root);
}

TExprNode::TPtr DeserializeExprGraph(TStringBuf data, TExprContext& ctx, TPositionHandle pos) {
    return TGraphReader(data, ctx, pos).Read();
}

} // namespace NKikimr::NKqp
