/* Header for ReplicatorOnline class
   Copyright (C) 2018-2022 Adam Leszczynski (aleszczynski@bersler.com)

This file is part of OpenLogReplicator.

OpenLogReplicator is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License as published
by the Free Software Foundation; either version 3, or (at your option)
any later version.

OpenLogReplicator is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License
along with OpenLogReplicator; see the file LICENSE;  If not see
<http://www.gnu.org/licenses/>.  */

#include "Replicator.h"

#ifndef REPLICATORONLINE_H_
#define REPLICATORONLINE_H_

namespace OpenLogReplicator {
    class DatabaseConnection;
    class DatabaseEnvironment;
    class Schema;

    class ReplicatorOnline : public Replicator {
    protected:
        static const char* SQL_GET_ARCHIVE_LOG_LIST;
        static const char* SQL_GET_DATABASE_INFORMATION;
        static const char* SQL_GET_DATABASE_INCARNATION;
        static const char* SQL_GET_DATABASE_ROLE;
        static const char* SQL_GET_DATABASE_SCN;
        static const char* SQL_GET_CON_INFO;
        static const char* SQL_GET_SCN_FROM_TIME;
        static const char* SQL_GET_SCN_FROM_TIME_RELATIVE;
        static const char* SQL_GET_SEQUENCE_FROM_SCN;
        static const char* SQL_GET_SEQUENCE_FROM_SCN_STANDBY;
        static const char* SQL_GET_LOGFILE_LIST;
        static const char* SQL_GET_PARAMETER;
        static const char* SQL_GET_PROPERTY;
        static const char* SQL_GET_SYS_CCOL_USER;
        static const char* SQL_GET_SYS_CCOL_OBJ;
        static const char* SQL_GET_SYS_CDEF_USER;
        static const char* SQL_GET_SYS_CDEF_OBJ;
        static const char* SQL_GET_SYS_COL_USER;
        static const char* SQL_GET_SYS_COL_OBJ;
        static const char* SQL_GET_SYS_DEFERRED_STG_USER;
        static const char* SQL_GET_SYS_DEFERRED_STG_OBJ;
        static const char* SQL_GET_SYS_ECOL_USER;
        static const char* SQL_GET_SYS_ECOL_OBJ;
        static const char* SQL_GET_SYS_ECOL11_USER;
        static const char* SQL_GET_SYS_ECOL11_OBJ;
        static const char* SQL_GET_SYS_OBJ_USER;
        static const char* SQL_GET_SYS_OBJ_NAME;
        static const char* SQL_GET_SYS_TAB_USER;
        static const char* SQL_GET_SYS_TAB_OBJ;
        static const char* SQL_GET_SYS_TABCOMPART_USER;
        static const char* SQL_GET_SYS_TABCOMPART_OBJ;
        static const char* SQL_GET_SYS_TABPART_USER;
        static const char* SQL_GET_SYS_TABPART_OBJ;
        static const char* SQL_GET_SYS_TABSUBPART_USER;
        static const char* SQL_GET_SYS_TABSUBPART_OBJ;
        static const char* SQL_GET_SYS_USER;
        static const char* SQL_CHECK_CONNECTION;
        bool standby;

        void positionReader() override;
        void loadDatabaseMetadata() override;
        bool checkConnection() override;
        std::string getParameterValue(const char* parameter) const;
        std::string getPropertyValue(const char* property) const;
        void checkTableForGrants(const char* tableName);
        void checkTableForGrantsFlashback(const char* tableName, typeScn scn);
        const char* getModeName() const override;
        void verifySchema(typeScn currentScn) override;
        void createSchema() override;
        void readSystemDictionariesDetails(Schema* schema, typeScn targetScn, typeUser user, typeObj obj);
        void readSystemDictionaries(Schema* schema, typeScn targetScn, std::string& owner, std::string& table, typeOptions options);
        void createSchemaForTable(typeScn targetScn, std::string& owner, std::string& table, std::vector<std::string>& keys, std::string& keysStr,
                                  typeOptions options);
        void updateOnlineRedoLogData() override;

    public:
        DatabaseEnvironment* env;
        DatabaseConnection* conn;
        bool keepConnection;

        ReplicatorOnline(Ctx* ctx, void (*archGetLog)(Replicator* replicator), Builder* builder, Metadata* metadata, TransactionBuffer* transactionBuffer,
                         std::string alias, const char* database, const char* user, const char* password, const char* connectString, bool keepConnection);
        ~ReplicatorOnline() override;

        void goStandby() override;

        static void archGetLogOnline(Replicator* replicator);
    };
}

#endif