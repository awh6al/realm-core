////////////////////////////////////////////////////////////////////////////
//
// Copyright 2015 Realm Inc.
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
//
////////////////////////////////////////////////////////////////////////////

#include <realm/object-store/object_store.hpp>

#include <realm/object-store/feature_checks.hpp>
#include <realm/object-store/object_schema.hpp>
#include <realm/object-store/schema.hpp>
#include <realm/object-store/shared_realm.hpp>

#include <realm/group.hpp>
#include <realm/table.hpp>
#include <realm/table_view.hpp>
#include <realm/util/assert.hpp>
#include <realm/util/scope_exit.hpp>

#if REALM_ENABLE_SYNC
#include <realm/sync/instruction_replication.hpp>
#endif // REALM_ENABLE_SYNC

#include <string.h>
#include <unordered_set>

using namespace realm;

constexpr uint64_t ObjectStore::NotVersioned;

namespace {
const char* const c_metadataTableName = "metadata";
const char* const c_versionColumnName = "version";

const char c_object_table_prefix[] = "class_";

const char* const c_development_mode_msg =
    "If your app is running in development mode, you can delete the realm and restart the app to update your schema.";

void create_metadata_tables(Group& group)
{
    // The 'metadata' table is simply ignored by Sync
    TableRef metadata_table = group.get_or_add_table(c_metadataTableName);

    if (metadata_table->get_column_count() == 0) {
        metadata_table->add_column(type_Int, c_versionColumnName);
        metadata_table->create_object().set(c_versionColumnName, int64_t(ObjectStore::NotVersioned));
    }
}

void set_schema_version(Group& group, uint64_t version)
{
    group.get_table(c_metadataTableName)->get_object(0).set<int64_t>(c_versionColumnName, version);
}

template <typename Group>
auto table_for_object_schema(Group& group, ObjectSchema const& object_schema)
{
    return ObjectStore::table_for_object_type(group, object_schema.name);
}

DataType to_core_type(PropertyType type)
{
    REALM_ASSERT(type != PropertyType::Object); // Link columns have to be handled differently
    switch (type & ~PropertyType::Flags) {
        case PropertyType::Int:
            return type_Int;
        case PropertyType::Bool:
            return type_Bool;
        case PropertyType::Float:
            return type_Float;
        case PropertyType::Double:
            return type_Double;
        case PropertyType::String:
            return type_String;
        case PropertyType::Date:
            return type_Timestamp;
        case PropertyType::Data:
            return type_Binary;
        case PropertyType::ObjectId:
            return type_ObjectId;
        case PropertyType::Decimal:
            return type_Decimal;
        case PropertyType::UUID:
            return type_UUID;
        case PropertyType::Mixed:
            return type_Mixed;
        default:
            REALM_COMPILER_HINT_UNREACHABLE();
    }
}

std::optional<CollectionType> process_collection(const Property& property)
{
    // check if the final type is itself a collection.
    if (is_array(property.type)) {
        return CollectionType::List;
    }
    else if (is_set(property.type)) {
        return CollectionType::Set;
    }
    else if (is_dictionary(property.type)) {
        return CollectionType::Dictionary;
    }
    return {};
}

ColKey add_column(Group& group, Table& table, Property const& property)
{
    // Cannot directly insert a LinkingObjects column (a computed property).
    // LinkingObjects must be an artifact of an existing link column.
    REALM_ASSERT(property.type != PropertyType::LinkingObjects);

    if (property.is_primary) {
        // Primary key columns should have been created when the table was created
        if (auto col = table.get_column_key(property.name)) {
            return col;
        }
    }
    auto collection_type = process_collection(property);
    if (property.type == PropertyType::Object) {
        auto target_name = ObjectStore::table_name_for_object_type(property.object_type);
        TableRef link_table = group.get_table(target_name);
        REALM_ASSERT(link_table);
        return table.add_column(*link_table, property.name, collection_type);
    }
    else {
        auto key =
            table.add_column(to_core_type(property.type), property.name, is_nullable(property.type), collection_type);
        if (property.requires_index())
            table.add_search_index(key);
        if (property.requires_fulltext_index())
            table.add_fulltext_index(key);
        return key;
    }
}

void replace_column(Group& group, Table& table, Property const& old_property, Property const& new_property)
{
    table.remove_column(old_property.column_key);
    add_column(group, table, new_property);
}

TableRef create_table(Group& group, ObjectSchema const& object_schema)
{
    auto name = ObjectStore::table_name_for_object_type(object_schema.name);

    TableRef table = group.get_table(name);
    if (table)
        return table;

    if (auto* pk_property = object_schema.primary_key_property()) {
        auto table_type = object_schema.table_type == ObjectSchema::ObjectType::TopLevelAsymmetric
                              ? Table::Type::TopLevelAsymmetric
                              : Table::Type::TopLevel;
        table = group.add_table_with_primary_key(name, to_core_type(pk_property->type), pk_property->name,
                                                 is_nullable(pk_property->type), table_type);
    }
    else {
        if (object_schema.table_type == ObjectSchema::ObjectType::Embedded) {
            table = group.add_table(name, Table::Type::Embedded);
        }
        else {
            auto table_type = object_schema.table_type == ObjectSchema::ObjectType::TopLevelAsymmetric
                                  ? Table::Type::TopLevelAsymmetric
                                  : Table::Type::TopLevel;
            table = group.get_or_add_table(name, table_type);
        }
    }

    return table;
}

void add_initial_columns(Group& group, ObjectSchema const& object_schema)
{
    auto name = ObjectStore::table_name_for_object_type(object_schema.name);
    TableRef table = group.get_table(name);

    for (auto const& prop : object_schema.persisted_properties) {
#if REALM_ENABLE_SYNC
        // The sync::create_table* functions create the PK column for us.
        if (prop.is_primary)
            continue;
#endif // REALM_ENABLE_SYNC
        add_column(group, *table, prop);
    }
}

void make_property_optional(Table& table, Property property)
{
    property.type |= PropertyType::Nullable;
    const bool throw_on_null = false;
    property.column_key = table.set_nullability(property.column_key, true, throw_on_null);
}

void make_property_required(Group& group, Table& table, Property property)
{
    property.type &= ~PropertyType::Nullable;
    table.remove_column(property.column_key);
    property.column_key = add_column(group, table, property);
}

void add_search_index(Table& table, Property property, IndexType type)
{
    table.add_search_index(table.get_column_key(property.name), type);
}

void remove_search_index(Table& table, Property property)
{
    table.remove_search_index(table.get_column_key(property.name));
}

} // anonymous namespace

void ObjectStore::set_schema_version(Group& group, uint64_t version)
{
    ::create_metadata_tables(group);
    ::set_schema_version(group, version);
}

uint64_t ObjectStore::get_schema_version(Group const& group)
{
    ConstTableRef table = group.get_table(c_metadataTableName);
    if (!table || table->get_column_count() == 0) {
        return ObjectStore::NotVersioned;
    }
    return table->get_object(0).get<int64_t>(c_versionColumnName);
}

StringData ObjectStore::object_type_for_table_name(StringData table_name)
{
    if (table_name.begins_with(c_object_table_prefix)) {
        return table_name.substr(sizeof(c_object_table_prefix) - 1);
    }
    return StringData();
}

std::string ObjectStore::table_name_for_object_type(StringData object_type)
{
    return std::string(c_object_table_prefix) + std::string(object_type);
}

TableRef ObjectStore::table_for_object_type(Group& group, StringData object_type)
{
    auto name = table_name_for_object_type(object_type);
    return group.get_table(name);
}

ConstTableRef ObjectStore::table_for_object_type(Group const& group, StringData object_type)
{
    auto name = table_name_for_object_type(object_type);
    return group.get_table(name);
}

namespace {
struct SchemaDifferenceExplainer {
    std::vector<ObjectSchemaValidationException> errors;

    void operator()(schema_change::AddTable op)
    {
        errors.emplace_back("Class '%1' has been added.", op.object->name);
    }

    void operator()(schema_change::RemoveTable)
    {
        // We never do anything for RemoveTable
    }

    void operator()(schema_change::ChangeTableType op)
    {
        errors.emplace_back("Class '%1' has been changed from %2 to %3.", op.object->name, *op.old_table_type,
                            *op.new_table_type);
    }

    void operator()(schema_change::AddInitialProperties)
    {
        // Nothing. Always preceded by AddTable.
    }

    void operator()(schema_change::AddProperty op)
    {
        errors.emplace_back("Property '%1.%2' has been added.", op.object->name, op.property->name);
    }

    void operator()(schema_change::RemoveProperty op)
    {
        errors.emplace_back("Property '%1.%2' has been removed.", op.object->name, op.property->name);
    }

    void operator()(schema_change::ChangePropertyType op)
    {
        errors.emplace_back("Property '%1.%2' has been changed from '%3' to '%4'.", op.object->name,
                            op.new_property->name, op.old_property->type_string(), op.new_property->type_string());
    }

    void operator()(schema_change::MakePropertyNullable op)
    {
        errors.emplace_back("Property '%1.%2' has been made optional.", op.object->name, op.property->name);
    }

    void operator()(schema_change::MakePropertyRequired op)
    {
        errors.emplace_back("Property '%1.%2' has been made required.", op.object->name, op.property->name);
    }

    void operator()(schema_change::ChangePrimaryKey op)
    {
        if (op.property && !op.object->primary_key.empty()) {
            errors.emplace_back("Primary Key for class '%1' has changed from '%2' to '%3'.", op.object->name,
                                op.object->primary_key, op.property->name);
        }
        else if (op.property) {
            errors.emplace_back("Primary Key for class '%1' has been added.", op.object->name);
        }
        else {
            errors.emplace_back("Primary Key for class '%1' has been removed.", op.object->name);
        }
    }

    void operator()(schema_change::AddIndex op)
    {
        errors.emplace_back("Property '%1.%2' has been made indexed.", op.object->name, op.property->name);
    }

    void operator()(schema_change::RemoveIndex op)
    {
        errors.emplace_back("Property '%1.%2' has been made unindexed.", op.object->name, op.property->name);
    }
};

class TableHelper {
public:
    TableHelper(Group& g)
        : m_group(g)
    {
    }

    Table& operator()(const ObjectSchema* object_schema)
    {
        if (object_schema != m_current_object_schema) {
            m_current_table = table_for_object_schema(m_group, *object_schema);
            m_current_object_schema = object_schema;
        }
        REALM_ASSERT(m_current_table);
        return *m_current_table;
    }

private:
    Group& m_group;
    const ObjectSchema* m_current_object_schema = nullptr;
    TableRef m_current_table;
};

template <typename ErrorType, typename Verifier>
void verify_no_errors(Verifier&& verifier, std::vector<SchemaChange> const& changes)
{
    for (auto& change : changes) {
        change.visit(verifier);
    }

    if (!verifier.errors.empty()) {
        throw ErrorType(verifier.errors);
    }
}
} // anonymous namespace

bool ObjectStore::needs_migration(std::vector<SchemaChange> const& changes)
{
    using namespace schema_change;
    struct Visitor {
        bool operator()(AddIndex)
        {
            return false;
        }
        bool operator()(AddInitialProperties)
        {
            return false;
        }
        bool operator()(AddProperty)
        {
            return true;
        }
        bool operator()(AddTable)
        {
            return false;
        }
        bool operator()(RemoveTable)
        {
            return false;
        }
        bool operator()(ChangeTableType)
        {
            return true;
        }
        bool operator()(ChangePrimaryKey)
        {
            return true;
        }
        bool operator()(ChangePropertyType)
        {
            return true;
        }
        bool operator()(MakePropertyNullable)
        {
            return true;
        }
        bool operator()(MakePropertyRequired)
        {
            return true;
        }
        bool operator()(RemoveIndex)
        {
            return false;
        }
        bool operator()(RemoveProperty)
        {
            return true;
        }
    };

    return std::any_of(begin(changes), end(changes), [](auto&& change) {
        return change.visit(Visitor());
    });
}

void ObjectStore::verify_no_changes_required(std::vector<SchemaChange> const& changes)
{
    verify_no_errors<SchemaMismatchException>(SchemaDifferenceExplainer(), changes);
}

void ObjectStore::verify_no_migration_required(std::vector<SchemaChange> const& changes)
{
    using namespace schema_change;
    struct Verifier : SchemaDifferenceExplainer {
        using SchemaDifferenceExplainer::operator();

        // Adding a table or adding/removing indexes can be done automatically.
        // All other changes require migrations.
        void operator()(AddTable) {}
        void operator()(AddInitialProperties) {}
        void operator()(AddIndex) {}
        void operator()(RemoveIndex) {}
    } verifier;
    verify_no_errors<SchemaMismatchException>(verifier, changes);
}

bool ObjectStore::verify_valid_additive_changes(std::vector<SchemaChange> const& changes, bool update_indexes)
{
    using namespace schema_change;
    struct Verifier : SchemaDifferenceExplainer {
        using SchemaDifferenceExplainer::operator();

        bool index_changes = false;
        bool other_changes = false;

        // Additive mode allows adding things, extra columns, and adding/removing indexes
        void operator()(AddTable)
        {
            other_changes = true;
        }
        void operator()(AddInitialProperties)
        {
            other_changes = true;
        }
        void operator()(AddProperty)
        {
            other_changes = true;
        }
        void operator()(RemoveProperty) {}
        void operator()(AddIndex)
        {
            index_changes = true;
        }
        void operator()(RemoveIndex)
        {
            index_changes = true;
        }
    } verifier;
    verify_no_errors<InvalidAdditiveSchemaChangeException>(verifier, changes);
    return verifier.other_changes || (verifier.index_changes && update_indexes);
}

void ObjectStore::verify_valid_external_changes(std::vector<SchemaChange> const& changes)
{
    using namespace schema_change;
    struct Verifier : SchemaDifferenceExplainer {
        using SchemaDifferenceExplainer::operator();

        // Adding new things is fine
        void operator()(AddTable) {}
        void operator()(AddInitialProperties) {}
        void operator()(AddProperty) {}
        void operator()(AddIndex) {}
        void operator()(RemoveIndex) {}
        // Deleting tables is not okay
        void operator()(RemoveTable op)
        {
            errors.emplace_back("Class '%1' has been removed.", op.object->name);
        }
    } verifier;
    verify_no_errors<InvalidExternalSchemaChangeException>(verifier, changes);
}

void ObjectStore::verify_compatible_for_immutable_and_readonly(std::vector<SchemaChange> const& changes)
{
    using namespace schema_change;
    struct Verifier : SchemaDifferenceExplainer {
        using SchemaDifferenceExplainer::operator();

        void operator()(AddTable) {}
        void operator()(AddInitialProperties) {}
        void operator()(ChangeTableType) {}
        void operator()(RemoveProperty) {}
        void operator()(AddIndex) {}
        void operator()(RemoveIndex) {}
    } verifier;
    verify_no_errors<InvalidReadOnlySchemaChangeException>(verifier, changes);
}

static void apply_non_migration_changes(Group& group, std::vector<SchemaChange> const& changes)
{
    using namespace schema_change;
    struct Applier : SchemaDifferenceExplainer {
        Applier(Group& group)
            : group{group}
            , table{group}
        {
        }
        Group& group;
        TableHelper table;

        // Produce an exception listing the unsupported schema changes for
        // everything but the explicitly supported ones
        using SchemaDifferenceExplainer::operator();

        void operator()(AddTable op)
        {
            create_table(group, *op.object);
        }
        void operator()(AddInitialProperties op)
        {
            add_initial_columns(group, *op.object);
        }
        void operator()(AddIndex op)
        {
            table(op.object).add_search_index(op.property->column_key, op.type);
        }
        void operator()(RemoveIndex op)
        {
            table(op.object).remove_search_index(op.property->column_key);
        }
    } applier{group};
    verify_no_errors<SchemaMismatchException>(applier, changes);
}

static void set_primary_key(Table& table, const Property* property)
{
    ColKey col;
    if (property) {
        col = table.get_column_key(property->name);
        REALM_ASSERT(col);
    }
    table.set_primary_key_column(col);
}

static void create_initial_tables(Group& group, std::vector<SchemaChange> const& changes)
{
    using namespace schema_change;
    struct Applier {
        Applier(Group& group)
            : group{group}
            , table{group}
        {
        }
        Group& group;
        TableHelper table;

        void operator()(AddTable op)
        {
            create_table(group, *op.object);
        }
        void operator()(RemoveTable) {}
        void operator()(AddInitialProperties op)
        {
            add_initial_columns(group, *op.object);
        }

        // Note that in normal operation none of these will be hit, as if we're
        // creating the initial tables there shouldn't be anything to update.
        // Implementing these makes us better able to handle weird
        // not-quite-correct files produced by other things and has no obvious
        // downside.
        void operator()(ChangeTableType op)
        {
            table(op.object).set_table_type(static_cast<Table::Type>(*op.new_table_type), false);
        }
        void operator()(AddProperty op)
        {
            add_column(group, table(op.object), *op.property);
        }
        void operator()(RemoveProperty op)
        {
            table(op.object).remove_column(op.property->column_key);
        }
        void operator()(MakePropertyNullable op)
        {
            make_property_optional(table(op.object), *op.property);
        }
        void operator()(MakePropertyRequired op)
        {
            make_property_required(group, table(op.object), *op.property);
        }
        void operator()(ChangePrimaryKey op)
        {
            set_primary_key(table(op.object), op.property);
        }
        void operator()(AddIndex op)
        {
            add_search_index(table(op.object), *op.property, op.type);
        }
        void operator()(RemoveIndex op)
        {
            remove_search_index(table(op.object), *op.property);
        }

        void operator()(ChangePropertyType op)
        {
            replace_column(group, table(op.object), *op.old_property, *op.new_property);
        }
    } applier{group};

    for (auto& change : changes) {
        change.visit(applier);
    }
}

void ObjectStore::apply_additive_changes(Group& group, std::vector<SchemaChange> const& changes, bool update_indexes)
{
    using namespace schema_change;
    struct Applier {
        Applier(Group& group, bool update_indexes)
            : group{group}
            , table{group}
            , update_indexes{update_indexes}
        {
        }
        Group& group;
        TableHelper table;
        bool update_indexes;

        void operator()(AddTable op)
        {
            create_table(group, *op.object);
        }
        void operator()(RemoveTable) {}
        void operator()(AddInitialProperties op)
        {
            add_initial_columns(group, *op.object);
        }
        void operator()(AddProperty op)
        {
            add_column(group, table(op.object), *op.property);
        }
        void operator()(AddIndex op)
        {
            if (update_indexes) {
                add_search_index(table(op.object), *op.property, op.type);
            }
        }
        void operator()(RemoveIndex op)
        {
            if (update_indexes)
                table(op.object).remove_search_index(op.property->column_key);
        }
        void operator()(RemoveProperty) {}

        // No need for errors for these, as we've already verified that they aren't present
        void operator()(ChangeTableType) {}
        void operator()(ChangePrimaryKey) {}
        void operator()(ChangePropertyType) {}
        void operator()(MakePropertyNullable) {}
        void operator()(MakePropertyRequired) {}
    } applier{group, update_indexes};

    for (auto& change : changes) {
        change.visit(applier);
    }
}

static void apply_pre_migration_changes(Group& group, std::vector<SchemaChange> const& changes)
{
    using namespace schema_change;
    struct Applier {
        Applier(Group& group)
            : group{group}
            , table{group}
        {
        }
        Group& group;
        TableHelper table;

        void operator()(AddTable op)
        {
            create_table(group, *op.object);
        }
        void operator()(RemoveTable) {}
        void operator()(ChangeTableType)
        { /* delayed until after the migration */
        }
        void operator()(AddInitialProperties op)
        {
            add_initial_columns(group, *op.object);
        }
        void operator()(AddProperty op)
        {
            add_column(group, table(op.object), *op.property);
        }
        void operator()(RemoveProperty)
        { /* delayed until after the migration */
        }
        void operator()(ChangePropertyType op)
        {
            replace_column(group, table(op.object), *op.old_property, *op.new_property);
        }
        void operator()(MakePropertyNullable op)
        {
            make_property_optional(table(op.object), *op.property);
        }
        void operator()(MakePropertyRequired op)
        {
            make_property_required(group, table(op.object), *op.property);
        }
        void operator()(ChangePrimaryKey op)
        {
            table(op.object).set_primary_key_column(ColKey{});
        }
        void operator()(AddIndex op)
        {
            add_search_index(table(op.object), *op.property, op.type);
        }
        void operator()(RemoveIndex op)
        {
            remove_search_index(table(op.object), *op.property);
        }
    } applier{group};

    for (auto& change : changes) {
        change.visit(applier);
    }
}

enum class DidRereadSchema { Yes, No };
enum class HandleBackLinksAutomatically { Yes, No };

static void apply_post_migration_changes(Group& group, std::vector<SchemaChange> const& changes,
                                         Schema const& initial_schema, DidRereadSchema did_reread_schema,
                                         HandleBackLinksAutomatically handle_backlinks_automatically)
{
    using namespace schema_change;
    struct Applier {
        Applier(Group& group, Schema const& initial_schema, DidRereadSchema did_reread_schema,
                HandleBackLinksAutomatically handle_backlinks_automatically)
            : group{group}
            , initial_schema(initial_schema)
            , table(group)
            , did_reread_schema(did_reread_schema == DidRereadSchema::Yes)
            , handle_backlinks_automatically(handle_backlinks_automatically == HandleBackLinksAutomatically::Yes)
        {
        }
        Group& group;
        Schema const& initial_schema;
        TableHelper table;
        bool did_reread_schema;
        bool handle_backlinks_automatically;

        void operator()(RemoveProperty op)
        {
            if (!initial_schema.empty() &&
                !initial_schema.find(op.object->name)->property_for_name(op.property->name))
                throw LogicError(ErrorCodes::InvalidProperty, util::format("Renamed property '%1.%2' does not exist.",
                                                                           op.object->name, op.property->name));
            auto table = table_for_object_schema(group, *op.object);
            table->remove_column(op.property->column_key);
        }

        void operator()(ChangePrimaryKey op)
        {
            set_primary_key(table(op.object), op.property);
        }

        void operator()(AddTable op)
        {
            create_table(group, *op.object);
        }

        void operator()(AddInitialProperties op)
        {
            if (did_reread_schema)
                add_initial_columns(group, *op.object);
            else {
                // If we didn't re-read the schema then AddInitialProperties was already taken care of
                // during apply_pre_migration_changes.
            }
        }

        void operator()(AddIndex op)
        {
            table(op.object).add_search_index(op.property->column_key);
        }
        void operator()(RemoveIndex op)
        {
            table(op.object).remove_search_index(op.property->column_key);
        }

        void operator()(ChangeTableType op)
        {
            table(op.object).set_table_type(static_cast<Table::Type>(*op.new_table_type),
                                            handle_backlinks_automatically);
        }
        void operator()(RemoveTable) {}
        void operator()(ChangePropertyType) {}
        void operator()(MakePropertyNullable) {}
        void operator()(MakePropertyRequired) {}
        void operator()(AddProperty) {}
    } applier{group, initial_schema, did_reread_schema, handle_backlinks_automatically};

    for (auto& change : changes) {
        change.visit(applier);
    }
}

static const char* schema_mode_to_string(SchemaMode mode)
{
    switch (mode) {
        case SchemaMode::Automatic:
            return "Automatic";
        case SchemaMode::Immutable:
            return "Immutable";
        case SchemaMode::ReadOnly:
            return "ReadOnly";
        case SchemaMode::SoftResetFile:
            return "SoftResetFile";
        case SchemaMode::HardResetFile:
            return "HardResetFile";
        case SchemaMode::AdditiveDiscovered:
            return "AdditiveDiscovered";
        case SchemaMode::AdditiveExplicit:
            return "AdditiveExplicit";
        case SchemaMode::Manual:
            return "Manual";
    }
    return "";
}

void ObjectStore::apply_schema_changes(Transaction& transaction, uint64_t schema_version, Schema& target_schema,
                                       uint64_t target_schema_version, SchemaMode mode,
                                       std::vector<SchemaChange> const& changes, bool handle_automatically_backlinks,
                                       std::function<void()> migration_function,
                                       bool set_schema_version_on_version_decrease)
{
    using namespace std::chrono;
    auto t1 = steady_clock::now();
    auto logger = transaction.get_logger();
    if (schema_version == ObjectStore::NotVersioned) {
        logger->debug("Creating schema version %1 in mode '%2'", target_schema_version, schema_mode_to_string(mode));
    }
    else {
        logger->debug("Migrating from schema version %1 to %2 in mode '%3'", schema_version, target_schema_version,
                      schema_mode_to_string(mode));
    }
    util::ScopeExit cleanup([&]() noexcept {
        auto t2 = steady_clock::now();
        logger->debug("Migration did run in %1 us (%2 changes)", duration_cast<microseconds>(t2 - t1).count(),
                      changes.size());
    });

    create_metadata_tables(transaction);

    if (mode == SchemaMode::AdditiveDiscovered || mode == SchemaMode::AdditiveExplicit) {
        bool set_schema = (schema_version < target_schema_version || schema_version == ObjectStore::NotVersioned ||
                           set_schema_version_on_version_decrease);

        // With sync v2.x, indexes are no longer synced, so there's no reason to avoid creating them.
        bool update_indexes = true;
        apply_additive_changes(transaction, changes, update_indexes);

        if (set_schema)
            set_schema_version(transaction, target_schema_version);

        set_schema_keys(transaction, target_schema);
        return;
    }

    if (schema_version == ObjectStore::NotVersioned) {
        if (mode != SchemaMode::ReadOnly) {
            create_initial_tables(transaction, changes);
        }
        set_schema_version(transaction, target_schema_version);
        set_schema_keys(transaction, target_schema);
        return;
    }

    auto call_migration = [&] {
        logger->debug("Calling migration function");
        auto t3 = steady_clock::now();
        migration_function();
        auto t4 = steady_clock::now();
        logger->debug("Migration function did run in %1 us", duration_cast<microseconds>(t4 - t3).count());
    };

    if (mode == SchemaMode::Manual) {
        if (migration_function) {
            call_migration();
        }

        verify_no_changes_required(schema_from_group(transaction).compare(target_schema));
        transaction.validate_primary_columns();
        set_schema_keys(transaction, target_schema);
        set_schema_version(transaction, target_schema_version);
        return;
    }

    if (schema_version == target_schema_version) {
        apply_non_migration_changes(transaction, changes);
        set_schema_keys(transaction, target_schema);
        return;
    }

    auto old_schema = schema_from_group(transaction);
    apply_pre_migration_changes(transaction, changes);
    HandleBackLinksAutomatically handle_backlinks =
        handle_automatically_backlinks ? HandleBackLinksAutomatically::Yes : HandleBackLinksAutomatically::No;
    if (migration_function) {
        set_schema_keys(transaction, target_schema);
        call_migration();

        // Migration function may have changed the schema, so we need to re-read it
        auto schema = schema_from_group(transaction);
        apply_post_migration_changes(transaction, schema.compare(target_schema, mode), old_schema,
                                     DidRereadSchema::Yes, handle_backlinks);
        transaction.validate_primary_columns();
    }
    else {
        apply_post_migration_changes(transaction, changes, {}, DidRereadSchema::No, handle_backlinks);
    }

    set_schema_version(transaction, target_schema_version);
    set_schema_keys(transaction, target_schema);
}

Schema ObjectStore::schema_from_group(Group const& group)
{
    std::vector<ObjectSchema> schema;
    schema.reserve(group.size());
    for (auto key : group.get_table_keys()) {
        auto object_type = object_type_for_table_name(group.get_table_name(key));
        if (object_type.size()) {
            schema.emplace_back(group, object_type, key);
        }
    }
    return schema;
}

void ObjectStore::set_schema_keys(Group const& group, Schema& schema)
{
    for (auto& object_schema : schema) {
        auto table = table_for_object_schema(group, object_schema);
        if (!table) {
            continue;
        }
        object_schema.table_key = table->get_key();
        for (auto& property : object_schema.persisted_properties) {
            property.column_key = table->get_column_key(property.name);
        }
    }
}

void ObjectStore::delete_data_for_object(Group& group, StringData object_type)
{
    if (TableRef table = table_for_object_type(group, object_type)) {
        group.remove_table(table->get_key());
    }
}

bool ObjectStore::is_empty(Group const& group)
{
    for (auto key : group.get_table_keys()) {
        ConstTableRef table = group.get_table(key);
        auto object_type = object_type_for_table_name(table->get_name());
        if (object_type.size() == 0 || object_type.begins_with("__")) {
            continue;
        }
        if (!table->is_empty()) {
            return false;
        }
    }
    return true;
}

void ObjectStore::rename_property(Group& group, Schema& target_schema, StringData object_type, StringData old_name,
                                  StringData new_name)
{
    TableRef table = table_for_object_type(group, object_type);
    if (!table) {
        throw LogicError(
            ErrorCodes::NoSuchTable,
            util::format("Cannot rename properties for type '%1' because it does not exist.", object_type));
    }

    auto target_object_schema = target_schema.find(object_type);
    if (target_object_schema == target_schema.end()) {
        throw LogicError(
            ErrorCodes::NoSuchTable,
            util::format("Cannot rename properties for type '%1' because it has been removed from the Realm.",
                         object_type));
    }

    if (target_object_schema->property_for_name(old_name)) {
        throw LogicError(
            ErrorCodes::IllegalOperation,
            util::format("Cannot rename property '%1.%2' to '%3' because the source property still exists.",
                         object_type, old_name, new_name));
    }

    ObjectSchema table_object_schema(group, object_type, table->get_key());
    Property* old_property = table_object_schema.property_for_name(old_name);
    if (!old_property) {
        throw LogicError(
            ErrorCodes::InvalidProperty,
            util::format("Cannot rename property '%1.%2' because it does not exist.", object_type, old_name));
    }

    Property* new_property = table_object_schema.property_for_name(new_name);
    if (!new_property) {
        // New property doesn't exist in the table, which means we're probably
        // renaming to an intermediate property in a multi-version migration.
        // This is safe because the migration will fail schema validation unless
        // this property is renamed again to a valid name before the end.
        table->rename_column(old_property->column_key, new_name);
        return;
    }

    if (old_property->type != new_property->type || old_property->object_type != new_property->object_type) {
        throw LogicError(
            ErrorCodes::IllegalOperation,
            util::format("Cannot rename property '%1.%2' to '%3' because it would change from type '%4' to '%5'.",
                         object_type, old_name, new_name, old_property->type_string(), new_property->type_string()));
    }

    if (is_nullable(old_property->type) && !is_nullable(new_property->type)) {
        throw LogicError(
            ErrorCodes::IllegalOperation,
            util::format("Cannot rename property '%1.%2' to '%3' because it would change from optional to required.",
                         object_type, old_name, new_name));
    }

    table->remove_column(new_property->column_key);
    table->rename_column(old_property->column_key, new_name);

    if (auto prop = target_object_schema->property_for_name(new_name)) {
        prop->column_key = old_property->column_key;
    }

    // update nullability for column
    if (is_nullable(new_property->type) && !is_nullable(old_property->type)) {
        auto prop = *new_property;
        prop.column_key = old_property->column_key;
        make_property_optional(*table, prop);
    }
}

InvalidSchemaVersionException::InvalidSchemaVersionException(uint64_t old_version, uint64_t new_version,
                                                             bool must_exactly_equal)
    : LogicError(ErrorCodes::InvalidSchemaVersion,
                 util::format(must_exactly_equal ? "Provided schema version %1 does not equal last set version %2."
                                                 : "Provided schema version %1 is less than last set version %2.",
                              new_version, old_version))
    , m_old_version(old_version)
    , m_new_version(new_version)
{
}

static void append_errors(std::string& message, std::vector<ObjectSchemaValidationException> const& errors)
{
    for (auto const& error : errors) {
        message += "\n- ";
        message += error.m_message;
    }
}

static void append_line(std::string& message, std::string_view line)
{
    message += "\n";
    message += line;
}

SchemaValidationException::SchemaValidationException(std::vector<ObjectSchemaValidationException> const& errors)
    : LogicError(ErrorCodes::SchemaValidationFailed, [&] {
        std::string message = "Schema validation failed due to the following errors:";
        append_errors(message, errors);
        return message;
    }())
{
}

SchemaMismatchException::SchemaMismatchException(std::vector<ObjectSchemaValidationException> const& errors)
    : LogicError(ErrorCodes::SchemaMismatch, [&] {
        std::string message = "Migration is required due to the following errors:";
        append_errors(message, errors);
        return message;
    }())
{
}

InvalidReadOnlySchemaChangeException::InvalidReadOnlySchemaChangeException(
    std::vector<ObjectSchemaValidationException> const& errors)
    : LogicError(ErrorCodes::InvalidSchemaChange, [&] {
        std::string message = "The following changes cannot be made in read-only schema mode:";
        append_errors(message, errors);
        return message;
    }())
{
}

InvalidAdditiveSchemaChangeException::InvalidAdditiveSchemaChangeException(
    std::vector<ObjectSchemaValidationException> const& errors)
    : LogicError(ErrorCodes::InvalidSchemaChange, [&] {
        std::string message = "The following changes cannot be made in additive-only schema mode:";
        append_errors(message, errors);
        append_line(message, c_development_mode_msg);
        return message;
    }())
{
}

InvalidExternalSchemaChangeException::InvalidExternalSchemaChangeException(
    std::vector<ObjectSchemaValidationException> const& errors)
    : LogicError(ErrorCodes::InvalidSchemaChange, [&] {
        std::string message = "Unsupported schema changes were made by another client or process:";
        append_errors(message, errors);
        append_line(message, c_development_mode_msg);
        return message;
    }())
{
}
