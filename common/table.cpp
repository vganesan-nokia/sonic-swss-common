#include <hiredis/hiredis.h>
#include <system_error>

#include "common/table.h"
#include "common/logger.h"
#include "common/redisreply.h"
#include "common/rediscommand.h"
#include "common/redisapi.h"
#include "common/json.hpp"

using namespace std;
using namespace swss;
using json = nlohmann::json;

// NOTE: Vertical bar ('|') is the new standard for table name separator
// moving forward. We plan to eventually deprecate the colon separator
// and transition all databases to use the vertical bar.
const std::string TableBase::TABLE_NAME_SEPARATOR_COLON = ":";
const std::string TableBase::TABLE_NAME_SEPARATOR_VBAR = "|";

const TableNameSeparatorMap TableBase::tableNameSeparatorMap = {
   { APPL_DB,             TABLE_NAME_SEPARATOR_COLON },
   { ASIC_DB,             TABLE_NAME_SEPARATOR_COLON },
   { COUNTERS_DB,         TABLE_NAME_SEPARATOR_COLON },
   { LOGLEVEL_DB,         TABLE_NAME_SEPARATOR_COLON },
   { CONFIG_DB,           TABLE_NAME_SEPARATOR_VBAR  },
   { PFC_WD_DB,           TABLE_NAME_SEPARATOR_COLON },
   { FLEX_COUNTER_DB,     TABLE_NAME_SEPARATOR_COLON },
   { STATE_DB,            TABLE_NAME_SEPARATOR_VBAR  },
   { GB_ASIC_DB,          TABLE_NAME_SEPARATOR_VBAR  },
   { GB_COUNTERS_DB,      TABLE_NAME_SEPARATOR_VBAR  },
   { GB_FLEX_COUNTER_DB,  TABLE_NAME_SEPARATOR_VBAR  },
   { VOQ_DB,              TABLE_NAME_SEPARATOR_COLON }
};

Table::Table(const DBConnector *db, const string &tableName)
    : Table(new RedisPipeline(db, 1), tableName, false)
{
    m_pipeowned = true;
}

Table::Table(RedisPipeline *pipeline, const string &tableName, bool buffered)
    : TableBase(tableName, SonicDBConfig::getSeparator(pipeline->getDBConnector()))
    , m_buffered(buffered)
    , m_pipeowned(false)
    , m_pipe(pipeline)
{
    std::string luaScript = loadLuaScript("table_dump.lua");
    m_shaDump = pipeline->loadRedisScript(luaScript);
}

Table::~Table()
{
    if (m_pipeowned)
    {
        delete m_pipe;
    }
}

void Table::setBuffered(bool buffered)
{
    m_buffered = buffered;
}

void Table::flush()
{
    m_pipe->flush();
}

bool Table::get(const string &key, vector<FieldValueTuple> &values)
{
    RedisCommand hgetall_key;
    hgetall_key.format("HGETALL %s", getKeyName(key).c_str());
    RedisReply r = m_pipe->push(hgetall_key, REDIS_REPLY_ARRAY);
    redisReply *reply = r.getContext();
    values.clear();

    if (!reply->elements)
        return false;

    if (reply->elements & 1)
        throw system_error(make_error_code(errc::address_not_available),
                           "Unable to connect netlink socket");

    for (unsigned int i = 0; i < reply->elements; i += 2)
    {
        values.emplace_back(stripSpecialSym(reply->element[i]->str),
                                    reply->element[i + 1]->str);
    }

    return true;
}

bool Table::hget(const string &key, const std::string &field,  std::string &value)
{
    RedisCommand hget_entry;
    hget_entry.format("HGET %s %s", getKeyName(key).c_str(), field.c_str());
    RedisReply r = m_pipe->push(hget_entry);
    redisReply *reply = r.getContext();

    if (reply->type == REDIS_REPLY_NIL)
    {
        value.clear();
        return false;
    }

    if (reply->type != REDIS_REPLY_STRING)
        throw system_error(make_error_code(errc::io_error),
                "Got unexpected reply type");

    value = stripSpecialSym(reply->str);

    return true;
}

void Table::hset(const string &key, const std::string &field, const std::string &value,
                const string& /*op*/, const string& /*prefix*/)
{
    RedisCommand cmd;
    cmd.formatHSET(getKeyName(key), field, value);

    m_pipe->push(cmd, REDIS_REPLY_INTEGER);
    if (!m_buffered)
    {
        m_pipe->flush();
    }
}

void Table::set(const string &key, const vector<FieldValueTuple> &values,
                const string& /*op*/, const string& /*prefix*/)
{
    if (values.size() == 0)
        return;

    RedisCommand cmd;
    cmd.formatHMSET(getKeyName(key), values.begin(), values.end());

    m_pipe->push(cmd, REDIS_REPLY_STATUS);
    if (!m_buffered)
    {
        m_pipe->flush();
    }
}

void Table::del(const string &key, const string& /* op */, const string& /*prefix*/)
{
    RedisCommand del_key;
    del_key.format("DEL %s", getKeyName(key).c_str());
    m_pipe->push(del_key, REDIS_REPLY_INTEGER);
}

void Table::hdel(const string &key, const string &field, const string& /* op */, const string& /*prefix*/)
{
    RedisCommand cmd;
    cmd.formatHDEL(getKeyName(key), field);
    m_pipe->push(cmd, REDIS_REPLY_INTEGER);
}

void TableEntryEnumerable::getContent(vector<KeyOpFieldsValuesTuple> &tuples)
{
    vector<string> keys;
    getKeys(keys);

    tuples.clear();

    for (const auto &key: keys)
    {
        vector<FieldValueTuple> values;
        string op = "";

        get(key, values);
        tuples.emplace_back(key, op, values);
    }
}

void Table::getKeys(vector<string> &keys)
{
    RedisCommand keys_cmd;
    keys_cmd.format("KEYS %s%s*", getTableName().c_str(), getTableNameSeparator().c_str());
    RedisReply r = m_pipe->push(keys_cmd, REDIS_REPLY_ARRAY);
    redisReply *reply = r.getContext();
    keys.clear();

    for (unsigned int i = 0; i < reply->elements; i++)
    {
        string key = reply->element[i]->str;
        keys.push_back(key.substr(getTableName().length()+1));
    }
}

void Table::dump(TableDump& tableDump)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_TIMER("getting");

    RedisCommand command;
    command.format("EVALSHA %s 1 %s ''",
            m_shaDump.c_str(),
            getTableName().c_str());

    RedisReply r = m_pipe->push(command, REDIS_REPLY_STRING);

    auto ctx = r.getContext();

    std::string data = ctx->str;

    json j = json::parse(data);

    size_t tableNameLen = getTableName().length() + 1; // + ":"

    for (json::iterator it = j.begin(); it != j.end(); ++it)
    {
        TableMap map;

        json jj = it.value();

        for (json::iterator itt = jj.begin(); itt != jj.end(); ++itt)
        {
            if (itt.key() == "NULL")
            {
                continue;
            }

            map[itt.key()] = itt.value();
        }

        std::string key = it.key().substr(tableNameLen);

        tableDump[key] = map;
    }
}

string Table::stripSpecialSym(const string &key)
{
    size_t pos = key.find('@');
    if (pos != key.npos)
    {
        return key.substr(0, pos);
    }

    return key;
}
