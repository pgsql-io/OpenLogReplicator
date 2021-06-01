/* Thread reading Oracle Redo Logs using online mode
   Copyright (C) 2018-2021 Adam Leszczynski (aleszczynski@bersler.com)

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

#include <algorithm>
#include <regex>
#include <unistd.h>

#include "DatabaseConnection.h"
#include "DatabaseEnvironment.h"
#include "DatabaseStatement.h"
#include "OracleAnalyzerOnline.h"
#include "OracleColumn.h"
#include "OracleObject.h"
#include "OutputBuffer.h"
#include "Reader.h"
#include "RedoLog.h"
#include "RuntimeException.h"
#include "Schema.h"
#include "SchemaElement.h"

using namespace std;

namespace OpenLogReplicator {
    const char* OracleAnalyzerOnline::SQL_GET_ARCHIVE_LOG_LIST(
            "SELECT"
            "   NAME"
            ",  SEQUENCE#"
            ",  FIRST_CHANGE#"
            ",  NEXT_CHANGE#"
            " FROM"
            "   SYS.V_$ARCHIVED_LOG"
            " WHERE"
            "   SEQUENCE# >= :i"
            "   AND RESETLOGS_ID = :j"
            "   AND ACTIVATION# = :k"
            "   AND NAME IS NOT NULL"
            " ORDER BY"
            "   SEQUENCE#"
            ",  DEST_ID");

    const char* OracleAnalyzerOnline::SQL_GET_DATABASE_INFORMATION(
            "SELECT"
            "   DECODE(D.LOG_MODE, 'ARCHIVELOG', 1, 0)"
            ",  DECODE(D.SUPPLEMENTAL_LOG_DATA_MIN, 'NO', 0, 1)"
            ",  DECODE(D.SUPPLEMENTAL_LOG_DATA_PK, 'YES', 1, 0)"
            ",  DECODE(D.SUPPLEMENTAL_LOG_DATA_ALL, 'YES', 1, 0)"
            ",  DECODE(TP.ENDIAN_FORMAT, 'Big', 1, 0)"
            ",  DI.RESETLOGS_ID"
            ",  D.ACTIVATION#"
            ",  VER.BANNER"
            ",  SYS_CONTEXT('USERENV','DB_NAME')"
            ",  CURRENT_SCN"
            " FROM"
            "   SYS.V_$DATABASE D"
            " JOIN"
            "   SYS.V_$TRANSPORTABLE_PLATFORM TP ON"
            "     TP.PLATFORM_NAME = D.PLATFORM_NAME"
            " JOIN"
            "   SYS.V_$VERSION VER ON"
            "     VER.BANNER LIKE '%Oracle Database%'"
            " JOIN"
            "   SYS.V_$DATABASE_INCARNATION DI ON"
            "     DI.STATUS = 'CURRENT'");

    const char* OracleAnalyzerOnline::SQL_GET_DATABASE_SCN(
            "SELECT"
            "   D.CURRENT_SCN"
            " FROM"
            "   SYS.V_$DATABASE D");

    const char* OracleAnalyzerOnline::SQL_GET_CON_INFO(
            "SELECT"
            "   SYS_CONTEXT('USERENV','CON_ID')"
            ",  SYS_CONTEXT('USERENV','CON_NAME')"
            " FROM"
            "   DUAL");

    const char* OracleAnalyzerOnline::SQL_GET_SCN_FROM_SEQUENCE(
            "SELECT"
            "   FIRST_CHANGE# - 1 AS FIRST_CHANGE#"
            " FROM"
            "   SYS.V_$ARCHIVED_LOG"
            " WHERE"
            "   SEQUENCE# = :i"
            "   AND RESETLOGS_ID = :j"
            "   AND ACTIVATION# = :k"
            " UNION ALL "
            "SELECT"
            "   FIRST_CHANGE# - 1 AS FIRST_CHANGE#"
            " FROM"
            "   SYS.V_$LOG"
            " WHERE"
            "   SEQUENCE# = :i");

    const char* OracleAnalyzerOnline::SQL_GET_SCN_FROM_SEQUENCE_STANDBY(
            "SELECT"
            "   FIRST_CHANGE# -1 AS FIRST_CHANGE#"
            " FROM"
            "   SYS.V_$ARCHIVED_LOG"
            " WHERE"
            "   SEQUENCE# = :i"
            "   AND RESETLOGS_ID = :j"
            "   AND ACTIVATION# = :k"
            " UNION ALL "
            "SELECT"
            "   FIRST_CHANGE# - 1 AS FIRST_CHANGE#"
            " FROM"
            "   SYS.V_$STANDBY_LOG"
            " WHERE"
            "   SEQUENCE# = :i");

    const char* OracleAnalyzerOnline::SQL_GET_SCN_FROM_TIME(
            "SELECT TIMESTAMP_TO_SCN(TO_DATE('YYYY-MM-DD HH24:MI:SS', :i) FROM DUAL");

    const char* OracleAnalyzerOnline::SQL_GET_SCN_FROM_TIME_RELATIVE(
            "SELECT TIMESTAMP_TO_SCN(SYSDATE - (:i/24/3600)) FROM DUAL");

    const char* OracleAnalyzerOnline::SQL_GET_SEQUENCE_FROM_SCN(
            "SELECT MAX(SEQUENCE#) FROM ("
            "  SELECT"
            "     SEQUENCE#"
            "   FROM"
            "     SYS.V_$LOG"
            "   WHERE"
            "     FIRST_CHANGE# - 1 <= :i"
            "   UNION "
            "  SELECT"
            "     SEQUENCE#"
            "   FROM"
            "     SYS.V_$ARCHIVED_LOG"
            "   WHERE"
            "     FIRST_CHANGE# - 1 <= :i)");

    const char* OracleAnalyzerOnline::SQL_GET_SEQUENCE_FROM_SCN_STANDBY(
            "SELECT"
            "   MAX(SEQUENCE#)"
            " FROM"
            "   SYS.V_$STANDBY_LOG"
            " WHERE"
            "   FIRST_CHANGE# <= :i");

    const char* OracleAnalyzerOnline::SQL_GET_LOGFILE_LIST(
            "SELECT"
            "   LF.GROUP#"
            ",  LF.MEMBER"
            " FROM"
            "   SYS.V_$LOGFILE LF"
            " WHERE"
            "   TYPE = :i"
            " ORDER BY"
            "   LF.GROUP# ASC"
            ",  LF.IS_RECOVERY_DEST_FILE DESC"
            ",  LF.MEMBER ASC");

    const char* OracleAnalyzerOnline::SQL_GET_PARAMETER(
            "SELECT"
            "   VALUE"
            " FROM"
            "   SYS.V_$PARAMETER"
            " WHERE"
            "   NAME = :i");

    const char* OracleAnalyzerOnline::SQL_GET_PROPERTY(
            "SELECT"
            "   PROPERTY_VALUE"
            " FROM"
            "   DATABASE_PROPERTIES"
            " WHERE"
            "   PROPERTY_NAME = :i");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_CCOL_USER(
            "SELECT"
            "   L.ROWID, L.CON#, L.INTCOL#, L.OBJ#, MOD(L.SPARE1, 18446744073709551616),"
            "   MOD(L.SPARE1 / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " JOIN"
            "   SYS.CCOL$ AS OF SCN :j L ON"
            "     O.OBJ# = L.OBJ#"
            " WHERE"
            "   O.OWNER# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_CCOL_OBJ(
            "SELECT"
            "   L.ROWID, L.CON#, L.INTCOL#, L.OBJ#, MOD(L.SPARE1, 18446744073709551616),"
            "   MOD(L.SPARE1 / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.CCOL$ AS OF SCN :j L"
            " WHERE"
            "   L.OBJ# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_CDEF_USER(
            "SELECT"
            "   D.ROWID, D.CON#, D.OBJ#, D.TYPE#"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " JOIN"
            "   SYS.CDEF$ AS OF SCN :j D ON"
            "     O.OBJ# = D.OBJ#"
            " WHERE"
            "   O.OWNER# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_CDEF_OBJ(
            "SELECT"
            "   D.ROWID, D.CON#, D.OBJ#, D.TYPE#"
            " FROM"
            "   SYS.CDEF$ AS OF SCN :j D"
            " WHERE"
            "   D.OBJ# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_COL_USER(
            "SELECT"
            "   C.ROWID, C.OBJ#, C.COL#, C.SEGCOL#, C.INTCOL#, C.NAME, C.TYPE#, C.LENGTH, C.PRECISION#, C.SCALE, C.CHARSETFORM, C.CHARSETID, C.NULL$,"
            "   MOD(C.PROPERTY, 18446744073709551616), MOD(C.PROPERTY / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " JOIN"
            "   SYS.COL$ AS OF SCN :j C ON"
            "     O.OBJ# = C.OBJ#"
            " WHERE"
            "   O.OWNER# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_COL_OBJ(
            "SELECT"
            "   C.ROWID, C.OBJ#, C.COL#, C.SEGCOL#, C.INTCOL#, C.NAME, C.TYPE#, C.LENGTH, C.PRECISION#, C.SCALE, C.CHARSETFORM, C.CHARSETID, C.NULL$,"
            "   MOD(C.PROPERTY, 18446744073709551616), MOD(C.PROPERTY / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.COL$ AS OF SCN :j C"
            " WHERE"
            "   C.OBJ# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_DEFERRED_STG_USER(
            "SELECT"
            "   DS.ROWID, DS.OBJ#, MOD(DS.FLAGS_STG, 18446744073709551616), MOD(DS.FLAGS_STG / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " JOIN"
            "   SYS.DEFERRED_STG$ AS OF SCN :j DS ON"
            "     O.OBJ# = DS.OBJ#"
            " WHERE"
            "   O.OWNER# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_DEFERRED_STG_OBJ(
            "SELECT"
            "   DS.ROWID, DS.OBJ#, MOD(DS.FLAGS_STG, 18446744073709551616), MOD(DS.FLAGS_STG / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.DEFERRED_STG$ AS OF SCN :j DS"
            " WHERE"
            "   DS.OBJ# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_ECOL_USER(
            "SELECT"
            "   E.ROWID, E.TABOBJ#, E.COLNUM, E.GUARD_ID"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " JOIN"
            "   SYS.ECOL$ AS OF SCN :j E ON"
            "     O.OBJ# = E.TABOBJ#"
            " WHERE"
            "   O.OWNER# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_ECOL_OBJ(
            "SELECT"
            "   E.ROWID, E.TABOBJ#, E.COLNUM, E.GUARD_ID"
            " FROM"
            "   SYS.ECOL$ AS OF SCN :j E"
            " WHERE"
            "   E.TABOBJ# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_ECOL11_USER(
            "SELECT"
            "   E.ROWID, E.TABOBJ#, E.COLNUM, -1 AS GUARD_ID"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " JOIN"
            "   SYS.ECOL$ AS OF SCN :j E ON"
            "     O.OBJ# = E.TABOBJ#"
            " WHERE"
            "   O.OWNER# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_ECOL11_OBJ(
            "SELECT"
            "   E.ROWID, E.TABOBJ#, E.COLNUM, -1 AS GUARD_ID"
            " FROM"
            "   SYS.ECOL$ AS OF SCN :j E"
            " WHERE"
            "   E.TABOBJ# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_OBJ_USER(
            "SELECT"
            "   O.ROWID, O.OWNER#, O.OBJ#, O.DATAOBJ#, O.NAME, O.TYPE#,"
            "   MOD(O.FLAGS, 18446744073709551616), MOD(O.FLAGS / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " WHERE"
            "   O.OWNER# = :j");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_OBJ_NAME(
            "SELECT"
            "   O.ROWID, O.OWNER#, O.OBJ#, O.DATAOBJ#, O.NAME, O.TYPE#,"
            "   MOD(O.FLAGS, 18446744073709551616), MOD(O.FLAGS / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " WHERE"
            "   O.OWNER# = :j AND O.NAME = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_SEG_USER(
            "SELECT"
            "   S.ROWID, S.FILE#, S.BLOCK#, S.TS#, MOD(S.SPARE1, 18446744073709551616),"
            "   MOD(S.SPARE1 / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " JOIN"
            "   SYS.TAB$ AS OF SCN :i T ON"
            "     T.OBJ# = O.OBJ#"
            " JOIN"
            "   SYS.SEG$ AS OF SCN :j S ON "
            "     T.FILE# = S.FILE# AND T.BLOCK# = S.BLOCK# AND T.TS# = S.TS#"
            " WHERE"
            "   O.OWNER# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_SEG_OBJ(
            "SELECT"
            "   S.ROWID, S.FILE#, S.BLOCK#, S.TS#, MOD(S.SPARE1, 18446744073709551616),"
            "   MOD(S.SPARE1 / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.TAB$ AS OF SCN :i T"
            " JOIN"
            "   SYS.SEG$ AS OF SCN :j S ON "
            "     T.FILE# = S.FILE# AND T.BLOCK# = S.BLOCK# AND T.TS# = S.TS#"
            " WHERE"
            "   T.OBJ# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_TAB_USER(
            "SELECT"
            "   T.ROWID, T.OBJ#, T.DATAOBJ#, T.TS#, T.FILE#, T.BLOCK#, T.CLUCOLS,"
            "   MOD(T.FLAGS, 18446744073709551616), MOD(T.FLAGS / 18446744073709551616, 18446744073709551616),"
            "   MOD(T.PROPERTY, 18446744073709551616), MOD(T.PROPERTY / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " JOIN"
            "   SYS.TAB$ AS OF SCN :j T ON"
            "     O.OBJ# = T.OBJ#"
            " WHERE"
            "   O.OWNER# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_TAB_OBJ(
            "SELECT"
            "   T.ROWID, T.OBJ#, T.DATAOBJ#, T.TS#, T.FILE#, T.BLOCK#, T.CLUCOLS,"
            "   MOD(T.FLAGS, 18446744073709551616), MOD(T.FLAGS / 18446744073709551616, 18446744073709551616),"
            "   MOD(T.PROPERTY, 18446744073709551616), MOD(T.PROPERTY / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.TAB$ AS OF SCN :j T"
            " WHERE"
            "   T.OBJ# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_TABCOMPART_USER(
            "SELECT"
            "   TCP.ROWID, TCP.OBJ#, TCP.DATAOBJ#, TCP.BO#"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " JOIN"
            "   SYS.TABCOMPART$ AS OF SCN :j TCP ON"
            "     O.OBJ# = TCP.OBJ#"
            " WHERE"
            "   O.OWNER# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_TABCOMPART_OBJ(
            "SELECT"
            "   TCP.ROWID, TCP.OBJ#, TCP.DATAOBJ#, TCP.BO#"
            " FROM"
            "   SYS.TABCOMPART$ AS OF SCN :j TCP"
            " WHERE"
            "   TCP.OBJ# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_TABPART_USER(
            "SELECT"
            "   TP.ROWID, TP.OBJ#, TP.DATAOBJ#, TP.BO#"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " JOIN"
            "   SYS.TABPART$ AS OF SCN :j TP ON"
            "     O.OBJ# = TP.OBJ#"
            " WHERE"
            "   O.OWNER# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_TABPART_OBJ(
            "SELECT"
            "   TP.ROWID, TP.OBJ#, TP.DATAOBJ#, TP.BO#"
            " FROM"
            "   SYS.TABPART$ AS OF SCN :j TP"
            " WHERE"
            "   TP.OBJ# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_TABSUBPART_USER(
            "SELECT"
            "   TSP.ROWID, TSP.OBJ#, TSP.DATAOBJ#, TSP.POBJ#"
            " FROM"
            "   SYS.OBJ$ AS OF SCN :i O"
            " JOIN"
            "   SYS.TABSUBPART$ AS OF SCN :j TSP ON"
            "     O.OBJ# = TSP.OBJ#"
            " WHERE"
            "   O.OWNER# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_TABSUBPART_OBJ(
            "SELECT"
            "   TSP.ROWID, TSP.OBJ#, TSP.DATAOBJ#, TSP.POBJ#"
            " FROM"
            "   SYS.TABSUBPART$ AS OF SCN :j TSP"
            " WHERE"
            "   TSP.OBJ# = :k");

    const char* OracleAnalyzerOnline::SQL_GET_SYS_USER(
            "SELECT"
            "   U.ROWID, U.USER#, U.NAME, MOD(U.SPARE1, 18446744073709551616),"
            "   MOD(U.SPARE1 / 18446744073709551616, 18446744073709551616)"
            " FROM"
            "   SYS.USER$ AS OF SCN :i U"
            " WHERE"
            "   REGEXP_LIKE(U.NAME, :j)");

    OracleAnalyzerOnline::OracleAnalyzerOnline(OutputBuffer *outputBuffer, uint64_t dumpRedoLog, uint64_t dumpRawData,
            const char *alias, const char *database, uint64_t memoryMinMb, uint64_t memoryMaxMb, uint64_t readBufferMax,
            uint64_t disableChecks, const char *user, const char *password, const char *connectString, bool standby) :
        OracleAnalyzer(outputBuffer, dumpRedoLog, dumpRawData, alias, database, memoryMinMb, memoryMaxMb, readBufferMax, disableChecks),
        standby(standby),
        user(user),
        password(password),
        connectString(connectString),
        env(nullptr),
        conn(nullptr),
        keepConnection(false) {

        env = new DatabaseEnvironment();
        if (env == nullptr) {
            RUNTIME_FAIL("couldn't allocate " << dec << sizeof(class DatabaseEnvironment) << " bytes memory (for: database environment)");
        }
    }

    OracleAnalyzerOnline::~OracleAnalyzerOnline() {
        closeConnection();

        if (env != nullptr) {
            delete env;
            env = nullptr;
        }
    }

    void OracleAnalyzerOnline::initialize(void) {
        checkConnection();
        if (shutdown)
            return;

        typeresetlogs currentResetlogs;
        typeactivation currentActivation;
        typeSCN currentScn;

        if ((disableChecks & DISABLE_CHECK_GRANTS) == 0) {
            checkTableForGrants("SYS.V_$ARCHIVED_LOG");
            checkTableForGrants("SYS.V_$DATABASE");
            checkTableForGrants("SYS.V_$DATABASE_INCARNATION");
            checkTableForGrants("SYS.V_$LOG");
            checkTableForGrants("SYS.V_$LOGFILE");
            checkTableForGrants("SYS.V_$PARAMETER");
            checkTableForGrants("SYS.V_$STANDBY_LOG");
            checkTableForGrants("SYS.V_$TRANSPORTABLE_PLATFORM");
        }

        {
            DatabaseStatement stmt(conn);
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_DATABASE_INFORMATION);
            stmt.createStatement(SQL_GET_DATABASE_INFORMATION);
            uint64_t logMode; stmt.defineUInt64(1, logMode);
            uint64_t supplementalLogMin; stmt.defineUInt64(2, supplementalLogMin);
            stmt.defineUInt64(3, suppLogDbPrimary);
            stmt.defineUInt64(4, suppLogDbAll);
            stmt.defineUInt64(5, bigEndian);
            stmt.defineUInt32(6, currentResetlogs);
            stmt.defineUInt32(7, currentActivation);
            char bannerStr[81]; stmt.defineString(8, bannerStr, sizeof(bannerStr));
            char contextStr[81]; stmt.defineString(9, contextStr, sizeof(contextStr));
            stmt.defineUInt64(10, currentScn);

            if (stmt.executeQuery()) {
                if (logMode == 0) {
                    WARNING("HINT run: SHUTDOWN IMMEDIATE;");
                    WARNING("HINT run: STARTUP MOUNT;");
                    WARNING("HINT run: ALTER DATABASE ARCHIVELOG;");
                    WARNING("HINT run: ALTER DATABASE OPEN;");
                    RUNTIME_FAIL("database not in ARCHIVELOG mode");
                }

                if (supplementalLogMin == 0) {
                    WARNING("HINT run: ALTER DATABASE ADD SUPPLEMENTAL LOG DATA;");
                    WARNING("HINT run: ALTER SYSTEM ARCHIVE LOG CURRENT;");
                    RUNTIME_FAIL("SUPPLEMENTAL_LOG_DATA_MIN missing");
                }

                if (bigEndian)
                    setBigEndian();

                if (resetlogs != 0 && currentResetlogs != resetlogs) {
                    RUNTIME_FAIL("database resetlogs:" << dec << currentResetlogs << ", expected: " << resetlogs);
                } else {
                    resetlogs = currentResetlogs;
                }

                if (activation != 0 && currentActivation != activation) {
                    RUNTIME_FAIL("database activation: " << dec << currentActivation << ", expected: " << activation);
                } else {
                    activation = currentActivation;
                }

                //12+
                conId = 0;
                if (memcmp(bannerStr, "Oracle Database 11g", 19) != 0) {
                    version12 = true;
                    DatabaseStatement stmt2(conn);
                    TRACE(TRACE2_SQL, "SQL: " << SQL_GET_CON_INFO);
                    stmt2.createStatement(SQL_GET_CON_INFO);
                    stmt2.defineInt16(1, conId);
                    char conNameChar[81];
                    stmt2.defineString(2, conNameChar, sizeof(conNameChar));
                    if (stmt2.executeQuery())
                        conName = conNameChar;
                }
                context = contextStr;

                INFO("version: " << dec << bannerStr << ", context: " << context << ", resetlogs: " << dec << resetlogs <<
                        ", activation: " << activation << ", con_id: " << conId << ", con_name: " << conName);
            } else {
                RUNTIME_FAIL("trying to read SYS.V_$DATABASE");
            }
        }

        if ((disableChecks & DISABLE_CHECK_GRANTS) == 0) {
            checkTableForGrantsFlashback("SYS.CCOL$", currentScn);
            checkTableForGrantsFlashback("SYS.CDEF$", currentScn);
            checkTableForGrantsFlashback("SYS.COL$", currentScn);
            checkTableForGrantsFlashback("SYS.DEFERRED_STG$", currentScn);
            checkTableForGrantsFlashback("SYS.ECOL$", currentScn);
            checkTableForGrantsFlashback("SYS.OBJ$", currentScn);
            checkTableForGrantsFlashback("SYS.SEG$", currentScn);
            checkTableForGrantsFlashback("SYS.TAB$", currentScn);
            checkTableForGrantsFlashback("SYS.TABCOMPART$", currentScn);
            checkTableForGrantsFlashback("SYS.TABPART$", currentScn);
            checkTableForGrantsFlashback("SYS.TABSUBPART$", currentScn);
            checkTableForGrantsFlashback("SYS.USER$", currentScn);
        }

        dbRecoveryFileDest = getParameterValue("db_recovery_file_dest");
        logArchiveDest = getParameterValue("log_archive_dest");
        dbBlockChecksum = getParameterValue("db_block_checksum");
        std::transform(dbBlockChecksum.begin(), dbBlockChecksum.end(), dbBlockChecksum.begin(), ::toupper);
        if (logArchiveFormat.length() == 0 && dbRecoveryFileDest.length() == 0)
            logArchiveFormat = getParameterValue("log_archive_format");
        nlsCharacterSet = getPropertyValue("NLS_CHARACTERSET");
        nlsNcharCharacterSet = getPropertyValue("NLS_NCHAR_CHARACTERSET");
        outputBuffer->setNlsCharset(nlsCharacterSet, nlsNcharCharacterSet);

        {
            DatabaseStatement stmt(conn);
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_LOGFILE_LIST);
            TRACE(TRACE2_SQL, "PARAM1: " << standby);
            stmt.createStatement(SQL_GET_LOGFILE_LIST);
            if (standby)
                stmt.bindString(1, "STANDBY");
            else
                stmt.bindString(1, "ONLINE");

            int64_t group = -1; stmt.defineInt64(1, group);
            char pathStr[514]; stmt.defineString(2, pathStr, sizeof(pathStr));
            int64_t ret = stmt.executeQuery();

            Reader *onlineReader = nullptr;
            int64_t lastGroup = -1;
            string path;

            while (ret) {
                if (group != lastGroup) {
                    onlineReader = readerCreate(group);
                    lastGroup = group;
                }
                path = pathStr;
                onlineReader->paths.push_back(path);
                ret = stmt.next();
            }

            if (readers.size() == 0) {
                if (standby) {
                    RUNTIME_FAIL("failed to find standby redo log files");
                } else {
                    RUNTIME_FAIL("failed to find online redo log files");
                }
            }
        }

        checkOnlineRedoLogs();
        archReader = readerCreate(0);
    }

    void OracleAnalyzerOnline::positionReader(void) {
        //position by sequence
        if (startSequence > 0) {
            DatabaseStatement stmt(conn);
            if (standby) {
                TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SCN_FROM_SEQUENCE_STANDBY);
                TRACE(TRACE2_SQL, "PARAM1: " << startSequence);
                TRACE(TRACE2_SQL, "PARAM2: " << resetlogs);
                TRACE(TRACE2_SQL, "PARAM3: " << activation);
                TRACE(TRACE2_SQL, "PARAM4: " << startSequence);
                stmt.createStatement(SQL_GET_SCN_FROM_SEQUENCE_STANDBY);
            } else {
                TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SCN_FROM_SEQUENCE);
                TRACE(TRACE2_SQL, "PARAM1: " << startSequence);
                TRACE(TRACE2_SQL, "PARAM2: " << resetlogs);
                TRACE(TRACE2_SQL, "PARAM3: " << activation);
                TRACE(TRACE2_SQL, "PARAM4: " << startSequence);
                stmt.createStatement(SQL_GET_SCN_FROM_SEQUENCE);
            }

            stmt.bindUInt32(1, startSequence);
            stmt.bindUInt32(2, resetlogs);
            stmt.bindUInt32(3, activation);
            stmt.bindUInt32(4, startSequence);
            stmt.defineUInt64(1, firstScn);

            if (!stmt.executeQuery()) {
                RUNTIME_FAIL("can't find redo sequence " << dec << startSequence);
            }
            sequence = startSequence;

        //position by time
        } else if (startTime.length() > 0) {
            DatabaseStatement stmt(conn);
            if (standby) {
                RUNTIME_FAIL("can't position by time for standby database");
            } else {
                TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SCN_FROM_TIME);
                TRACE(TRACE2_SQL, "PARAM1: " << startTime);
                stmt.createStatement(SQL_GET_SCN_FROM_TIME);
            }
            stringstream ss;
            stmt.bindString(1, startTime);
            stmt.defineUInt64(1, firstScn);

            if (!stmt.executeQuery()) {
                RUNTIME_FAIL("can't find scn for: " << startTime);
            }

        } else if (startTimeRel > 0) {
            DatabaseStatement stmt(conn);
            if (standby) {
                RUNTIME_FAIL("can't position by relative time for standby database");
            } else {
                TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SCN_FROM_TIME_RELATIVE);
                TRACE(TRACE2_SQL, "PARAM1: " << startTimeRel);
                stmt.createStatement(SQL_GET_SCN_FROM_TIME_RELATIVE);
            }

            stmt.bindInt64(1, startTimeRel);
            stmt.defineUInt64(1, firstScn);

            if (!stmt.executeQuery()) {
                RUNTIME_FAIL("can't find scn for " << dec << startTime);
            }

        } else if (startScn > 0) {
            firstScn = startScn;

        //NOW
        } else {
            DatabaseStatement stmt(conn);
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_DATABASE_SCN);
            stmt.createStatement(SQL_GET_DATABASE_SCN);
            stmt.defineUInt64(1, firstScn);

            if (!stmt.executeQuery()) {
                RUNTIME_FAIL("can't find database current scn");
            }
        }

        if (firstScn == ZERO_SCN) {
            RUNTIME_FAIL("getting database scn");
        }

        if (sequence == ZERO_SEQ) {
            DEBUG("starting sequence not found - starting with new batch");

            DatabaseStatement stmt(conn);
            if (standby) {
                TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SEQUENCE_FROM_SCN_STANDBY);
                TRACE(TRACE2_SQL, "PARAM1: " << firstScn);
                stmt.createStatement(SQL_GET_SEQUENCE_FROM_SCN_STANDBY);
            } else {
                TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SEQUENCE_FROM_SCN);
                TRACE(TRACE2_SQL, "PARAM1: " << firstScn);
                stmt.createStatement(SQL_GET_SEQUENCE_FROM_SCN);
            }
            stmt.bindUInt64(1, firstScn);
            stmt.defineUInt32(1, sequence);

            if (!stmt.executeQuery()) {
                RUNTIME_FAIL("getting database sequence for scn: " << dec << firstScn);
            }
        }
    }

    void OracleAnalyzerOnline::checkConnection(void) {
        INFO("connecting to Oracle instance of " << database << " to " << connectString);
        while (!shutdown) {
            if (conn == nullptr) {
                try {
                    conn = new DatabaseConnection(env, user, password, connectString, false);
                } catch (RuntimeException &ex) {
                    //
                }
            }

            if (conn != nullptr)
                break;

            WARNING("cannot connect to database, retry in 5 sec.");
            sleep(5);
        }
    }

    void OracleAnalyzerOnline::goStandby(void) {
        if (keepConnection)
            closeConnection();
    }


    void OracleAnalyzerOnline::closeConnection(void) {
        if (conn != nullptr) {
            delete conn;
            conn = nullptr;
        }
    }

    string OracleAnalyzerOnline::getParameterValue(const char *parameter) {
        char value[4001];
        DatabaseStatement stmt(conn);
        TRACE(TRACE2_SQL, "SQL: " << SQL_GET_PARAMETER);
        TRACE(TRACE2_SQL, "PARAM1: " << parameter);
        stmt.createStatement(SQL_GET_PARAMETER);
        stmt.bindString(1, parameter);
        stmt.defineString(1, value, sizeof(value));

        if (stmt.executeQuery())
            return value;

        //no value found
        RUNTIME_FAIL("can't get parameter value for " << parameter);
    }

    string OracleAnalyzerOnline::getPropertyValue(const char *property) {
        char value[4001];
        DatabaseStatement stmt(conn);
        TRACE(TRACE2_SQL, "SQL: " << SQL_GET_PROPERTY);
        TRACE(TRACE2_SQL, "PARAM1: " << property);
        stmt.createStatement(SQL_GET_PROPERTY);
        stmt.bindString(1, property);
        stmt.defineString(1, value, sizeof(value));

        if (stmt.executeQuery())
            return value;

        //no value found
        RUNTIME_FAIL("can't get proprty value for " << property);
    }

    void OracleAnalyzerOnline::checkTableForGrants(string tableName) {
        try {
            string query("SELECT 1 FROM " + tableName + " WHERE 0 = 1");

            DatabaseStatement stmt(conn);
            TRACE(TRACE2_SQL, "SQL: " << query);
            stmt.createStatement(query.c_str());
            uint64_t dummy; stmt.defineUInt64(1, dummy);
            stmt.executeQuery();
        } catch (RuntimeException &ex) {
            if (conId > 0) {
                WARNING("HINT run: ALTER SESSION SET CONTAINER = " << conName << ";");
                WARNING("HINT run: GRANT SELECT ON " << tableName << " TO " << user << ";");
                RUNTIME_FAIL("grants missing");
            } else {
                WARNING("HINT run: GRANT SELECT ON " << tableName << " TO " << user << ";");
                RUNTIME_FAIL("grants missing");
            }
            throw RuntimeException (ex.msg);
        }
    }

    void OracleAnalyzerOnline::checkTableForGrantsFlashback(string tableName, typeSCN scn) {
        try {
            string query("SELECT 1 FROM " + tableName + " AS OF SCN " + to_string(scn) + " WHERE 0 = 1");

            DatabaseStatement stmt(conn);
            TRACE(TRACE2_SQL, "SQL: " << query);
            stmt.createStatement(query.c_str());
            uint64_t dummy; stmt.defineUInt64(1, dummy);
            stmt.executeQuery();
        } catch (RuntimeException &ex) {
            if (conId > 0) {
                WARNING("HINT run: ALTER SESSION SET CONTAINER = " << conName << ";");
                WARNING("HINT run: GRANT SELECT, FLASHBACK ON " << tableName << " TO " << user << ";");
                RUNTIME_FAIL("grants missing");
            } else {
                WARNING("HINT run: GRANT SELECT, FLASHBACK ON " << tableName << " TO " << user << ";");
                RUNTIME_FAIL("grants missing");
            }
            throw RuntimeException (ex.msg);
        }
    }

    void OracleAnalyzerOnline::createSchema(void) {
        checkConnection();
        INFO("reading dictionaries for scn: " << dec << firstScn);
        schemaScn = firstScn;

        for (SchemaElement *element : schema->elements)
            createSchemaForTable(element->owner, element->table, element->keys, element->keysStr, element->options);
    }

    void OracleAnalyzerOnline::readSystemDictionariesDetails(typeUSER user, typeOBJ obj) {
        DEBUG("read dictionaries for user: " << dec << user << ", object: " << obj);

        //reading SYS.COL$
        DatabaseStatement stmtCol(conn);
        if (obj != 0) {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_COL_OBJ);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << obj);
            stmtCol.createStatement(SQL_GET_SYS_COL_OBJ);
            stmtCol.bindUInt64(1, schemaScn);
            stmtCol.bindUInt32(2, obj);
        } else {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_COL_USER);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM3: " << user);
            stmtCol.createStatement(SQL_GET_SYS_COL_USER);
            stmtCol.bindUInt64(1, schemaScn);
            stmtCol.bindUInt64(2, schemaScn);
            stmtCol.bindUInt32(3, user);
        }

        char colRowid[19]; stmtCol.defineString(1, colRowid, sizeof(colRowid));
        typeOBJ colObj; stmtCol.defineUInt32(2, colObj);
        typeCOL colCol; stmtCol.defineInt16(3, colCol);
        typeCOL colSegCol; stmtCol.defineInt16(4, colSegCol);
        typeCOL colIntCol; stmtCol.defineInt16(5, colIntCol);
        char colName[129]; stmtCol.defineString(6, colName, sizeof(colName));
        uint64_t colType; stmtCol.defineUInt64(7, colType);
        uint64_t colLength; stmtCol.defineUInt64(8, colLength);
        int64_t colPrecision = -1; stmtCol.defineInt64(9, colPrecision);
        int64_t colScale = -1; stmtCol.defineInt64(10, colScale);
        uint64_t colCharsetForm = 0; stmtCol.defineUInt64(11, colCharsetForm);
        uint64_t colCharsetId = 0; stmtCol.defineUInt64(12, colCharsetId);
        int64_t colNull; stmtCol.defineInt64(13, colNull);
        uint64_t colProperty1; stmtCol.defineUInt64(14, colProperty1);
        uint64_t colProperty2; stmtCol.defineUInt64(15, colProperty2);

        int64_t colRet = stmtCol.executeQuery();
        while (colRet) {
            schema->dictSysColAdd(colRowid, colObj, colCol, colSegCol, colIntCol, colName, colType, colLength, colPrecision, colScale, colCharsetForm,
                    colCharsetId, colNull, colProperty1, colProperty2);
            colPrecision = -1;
            colScale = -1;
            colCharsetForm = 0;
            colCharsetId = 0;
            colRet = stmtCol.next();
        }

        //reading SYS.CCOL$
        DatabaseStatement stmtCCol(conn);
        if (obj != 0) {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_CCOL_OBJ);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << obj);
            stmtCCol.createStatement(SQL_GET_SYS_CCOL_OBJ);
            stmtCCol.bindUInt64(1, schemaScn);
            stmtCCol.bindUInt32(2, obj);
        } else {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_CCOL_USER);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM3: " << user);
            stmtCCol.createStatement(SQL_GET_SYS_CCOL_USER);
            stmtCCol.bindUInt64(1, schemaScn);
            stmtCCol.bindUInt64(2, schemaScn);
            stmtCCol.bindUInt32(3, user);
        }

        char ccolRowid[19]; stmtCCol.defineString(1, ccolRowid, sizeof(ccolRowid));
        typeCON ccolCon; stmtCCol.defineUInt32(2, ccolCon);
        typeCOL ccolIntCol; stmtCCol.defineInt16(3, ccolIntCol);
        typeOBJ ccolObj; stmtCCol.defineUInt32(4, ccolObj);
        uint64_t ccolSpare11 = 0; stmtCCol.defineUInt64(5, ccolSpare11);
        uint64_t ccolSpare12 = 0; stmtCCol.defineUInt64(6, ccolSpare12);

        int64_t ccolRet = stmtCCol.executeQuery();
        while (ccolRet) {
            schema->dictSysCColAdd(ccolRowid, ccolCon, ccolIntCol, ccolObj, ccolSpare11, ccolSpare12);
            ccolSpare11 = 0;
            ccolSpare12 = 0;
            ccolRet = stmtCCol.next();
        }

        //reading SYS.CDEF$
        DatabaseStatement stmtCDef(conn);
        if (obj != 0) {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_CDEF_OBJ);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << obj);
            stmtCDef.createStatement(SQL_GET_SYS_CDEF_OBJ);
            stmtCDef.bindUInt64(1, schemaScn);
            stmtCDef.bindUInt32(2, obj);
        } else {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_CDEF_USER);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM3: " << user);
            stmtCDef.createStatement(SQL_GET_SYS_CDEF_USER);
            stmtCDef.bindUInt64(1, schemaScn);
            stmtCDef.bindUInt64(2, schemaScn);
            stmtCDef.bindUInt32(3, user);
        }

        char cdefRowid[19]; stmtCDef.defineString(1, cdefRowid, sizeof(cdefRowid));
        typeCON cdefCon; stmtCDef.defineUInt32(2, cdefCon);
        typeOBJ cdefObj; stmtCDef.defineUInt32(3, cdefObj);
        uint64_t cdefType; stmtCDef.defineUInt64(4, cdefType);

        int64_t cdefRet = stmtCDef.executeQuery();
        while (cdefRet) {
            schema->dictSysCDefAdd(cdefRowid, cdefCon, cdefObj, cdefType);
            cdefRet = stmtCDef.next();
        }

        //reading SYS.TAB$
        DatabaseStatement stmtDeferredStg(conn);
        if (obj != 0) {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_DEFERRED_STG_OBJ);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << obj);
            stmtDeferredStg.createStatement(SQL_GET_SYS_DEFERRED_STG_OBJ);
            stmtDeferredStg.bindUInt64(1, schemaScn);
            stmtDeferredStg.bindUInt32(2, obj);
        } else {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_DEFERRED_STG_USER);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM3: " << user);
            stmtDeferredStg.createStatement(SQL_GET_SYS_DEFERRED_STG_USER);
            stmtDeferredStg.bindUInt64(1, schemaScn);
            stmtDeferredStg.bindUInt64(2, schemaScn);
            stmtDeferredStg.bindUInt32(3, user);
        }

        char deferredStgRowid[19]; stmtDeferredStg.defineString(1, deferredStgRowid, sizeof(deferredStgRowid));
        typeOBJ deferredStgObj; stmtDeferredStg.defineUInt32(2, deferredStgObj);
        uint64_t deferredStgFlagsStg1 = 0; stmtDeferredStg.defineUInt64(3, deferredStgFlagsStg1);
        uint64_t deferredStgFlagsStg2 = 0; stmtDeferredStg.defineUInt64(4, deferredStgFlagsStg2);

        int64_t deferredStgRet = stmtDeferredStg.executeQuery();
        while (deferredStgRet) {
            schema->dictSysDeferredStgAdd(deferredStgRowid, deferredStgObj, deferredStgFlagsStg1, deferredStgFlagsStg2);
            deferredStgFlagsStg1 = 0;
            deferredStgFlagsStg2 = 0;
            deferredStgRet = stmtDeferredStg.next();
        }

        //reading SYS.ECOL$
        DatabaseStatement stmtECol(conn);
        if (version12) {
            if (obj != 0) {
                TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_ECOL_OBJ);
                TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
                TRACE(TRACE2_SQL, "PARAM2: " << obj);
                stmtECol.createStatement(SQL_GET_SYS_ECOL_OBJ);
                stmtECol.bindUInt64(1, schemaScn);
                stmtECol.bindUInt32(2, obj);
            } else {
                TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_ECOL_USER);
                TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
                TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
                TRACE(TRACE2_SQL, "PARAM3: " << user);
                stmtECol.createStatement(SQL_GET_SYS_ECOL_USER);
                stmtECol.bindUInt64(1, schemaScn);
                stmtECol.bindUInt64(2, schemaScn);
                stmtECol.bindUInt32(3, user);
            }
        } else {
            if (obj != 0) {
                TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_ECOL11_OBJ);
                TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
                TRACE(TRACE2_SQL, "PARAM2: " << obj);
                stmtECol.createStatement(SQL_GET_SYS_ECOL11_OBJ);
                stmtECol.bindUInt64(1, schemaScn);
                stmtECol.bindUInt32(2, obj);
            } else {
                TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_ECOL11_USER);
                TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
                TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
                TRACE(TRACE2_SQL, "PARAM3: " << user);
                stmtECol.createStatement(SQL_GET_SYS_ECOL11_USER);
                stmtECol.bindUInt64(1, schemaScn);
                stmtECol.bindUInt64(2, schemaScn);
                stmtECol.bindUInt32(3, user);
            }
        }

        char ecolRowid[19]; stmtECol.defineString(1, ecolRowid, sizeof(ecolRowid));
        typeOBJ ecolTabObj; stmtECol.defineUInt32(2, ecolTabObj);
        typeCOL ecolColNum = 0; stmtECol.defineInt16(3, ecolColNum);
        typeCOL ecolGuardId = -1; stmtECol.defineInt16(4, ecolGuardId);

        int64_t ecolRet = stmtECol.executeQuery();
        while (ecolRet) {
            schema->dictSysEColAdd(ecolRowid, ecolTabObj, ecolColNum, ecolGuardId);
            ecolColNum = 0;
            ecolGuardId = -1;
            ecolRet = stmtECol.next();
        }

        //reading SYS.TAB$
        DatabaseStatement stmtTab(conn);
        if (obj != 0) {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_TAB_OBJ);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << obj);
            stmtTab.createStatement(SQL_GET_SYS_TAB_OBJ);
            stmtTab.bindUInt64(1, schemaScn);
            stmtTab.bindUInt32(2, obj);
        } else {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_TAB_USER);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM3: " << user);
            stmtTab.createStatement(SQL_GET_SYS_TAB_USER);
            stmtTab.bindUInt64(1, schemaScn);
            stmtTab.bindUInt64(2, schemaScn);
            stmtTab.bindUInt32(3, user);
        }

        char tabRowid[19]; stmtTab.defineString(1, tabRowid, sizeof(tabRowid));
        typeOBJ tabObj; stmtTab.defineUInt32(2, tabObj);
        typeDATAOBJ tabDataObj = 0; stmtTab.defineUInt32(3, tabDataObj);
        uint32_t tabTs; stmtTab.defineUInt32(4, tabTs);
        uint32_t tabFile; stmtTab.defineUInt32(5, tabFile);
        uint32_t tabBlock; stmtTab.defineUInt32(6, tabBlock);
        typeCOL tabCluCols = 0; stmtTab.defineInt16(7, tabCluCols);
        uint64_t tabFlags1; stmtTab.defineUInt64(8, tabFlags1);
        uint64_t tabFlags2; stmtTab.defineUInt64(9, tabFlags2);
        uint64_t tabProperty1; stmtTab.defineUInt64(10, tabProperty1);
        uint64_t tabProperty2; stmtTab.defineUInt64(11, tabProperty2);

        int64_t tabRet = stmtTab.executeQuery();
        while (tabRet) {
            schema->dictSysTabAdd(tabRowid, tabObj, tabDataObj, tabTs, tabFile, tabBlock, tabCluCols, tabFlags1, tabFlags2,
                    tabProperty1, tabProperty2);
            tabDataObj = 0;
            tabCluCols = 0;
            tabRet = stmtTab.next();
        }

        //reading SYS.TABCOMPART$
        DatabaseStatement stmtTabComPart(conn);
        if (obj != 0) {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_TABCOMPART_OBJ);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << obj);
            stmtTabComPart.createStatement(SQL_GET_SYS_TABCOMPART_OBJ);
            stmtTabComPart.bindUInt64(1, schemaScn);
            stmtTabComPart.bindUInt32(2, obj);
        } else {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_TABCOMPART_USER);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM3: " << user);
            stmtTabComPart.createStatement(SQL_GET_SYS_TABCOMPART_USER);
            stmtTabComPart.bindUInt64(1, schemaScn);
            stmtTabComPart.bindUInt64(2, schemaScn);
            stmtTabComPart.bindUInt32(3, user);
        }

        char tabComPartRowid[19]; stmtTabComPart.defineString(1, tabComPartRowid, sizeof(tabComPartRowid));
        typeOBJ tabComPartObj; stmtTabComPart.defineUInt32(2, tabComPartObj);
        typeDATAOBJ tabComPartDataObj = 0; stmtTabComPart.defineUInt32(3, tabComPartDataObj);
        typeOBJ tabComPartBo; stmtTabComPart.defineUInt32(4, tabComPartBo);

        int64_t tabComPartRet = stmtTabComPart.executeQuery();
        while (tabComPartRet) {
            schema->dictSysTabComPartAdd(tabComPartRowid, tabComPartObj, tabComPartDataObj, tabComPartBo);
            tabComPartDataObj = 0;
            tabComPartRet = stmtTabComPart.next();
        }

        //reading SYS.SEG$
        DatabaseStatement stmtSeg(conn);
        if (obj != 0) {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_SEG_OBJ);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM3: " << obj);
            stmtSeg.createStatement(SQL_GET_SYS_SEG_OBJ);
            stmtSeg.bindUInt64(1, schemaScn);
            stmtSeg.bindUInt64(2, schemaScn);
            stmtSeg.bindUInt32(3, obj);
        } else {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_SEG_USER);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM3: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM4: " << user);
            stmtSeg.createStatement(SQL_GET_SYS_SEG_USER);
            stmtSeg.bindUInt64(1, schemaScn);
            stmtSeg.bindUInt64(2, schemaScn);
            stmtSeg.bindUInt64(3, schemaScn);
            stmtSeg.bindUInt32(4, user);
        }

        char segRowid[19]; stmtSeg.defineString(1, segRowid, sizeof(segRowid));
        uint32_t segFile; stmtSeg.defineUInt32(2, segFile);
        uint32_t segBlock; stmtSeg.defineUInt32(3, segBlock);
        uint32_t segTs; stmtSeg.defineUInt32(4, segTs);
        uint64_t segSpare11 = 0; stmtSeg.defineUInt64(5, segSpare11);
        uint64_t segSpare12 = 0; stmtSeg.defineUInt64(6, segSpare12);

        int64_t segRet = stmtSeg.executeQuery();
        while (segRet) {
            schema->dictSysSegAdd(segRowid, segFile, segBlock, segTs, segSpare11, segSpare12);
            segSpare11 = 0;
            segSpare12 = 0;
            segRet = stmtSeg.next();
        }

        //reading SYS.TABPART$
        DatabaseStatement stmtTabPart(conn);
        if (obj != 0) {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_TABPART_OBJ);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << obj);
            stmtTabPart.createStatement(SQL_GET_SYS_TABPART_OBJ);
            stmtTabPart.bindUInt64(1, schemaScn);
            stmtTabPart.bindUInt32(2, obj);
        } else {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_TABPART_USER);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM3: " << user);
            stmtTabPart.createStatement(SQL_GET_SYS_TABPART_USER);
            stmtTabPart.bindUInt64(1, schemaScn);
            stmtTabPart.bindUInt64(2, schemaScn);
            stmtTabPart.bindUInt32(3, user);
        }

        char tabPartRowid[19]; stmtTabPart.defineString(1, tabPartRowid, sizeof(tabPartRowid));
        typeOBJ tabPartObj; stmtTabPart.defineUInt32(2, tabPartObj);
        typeDATAOBJ tabPartDataObj = 0; stmtTabPart.defineUInt32(3, tabPartDataObj);
        typeOBJ tabPartBo; stmtTabPart.defineUInt32(4, tabPartBo);

        int64_t tabPartRet = stmtTabPart.executeQuery();
        while (tabPartRet) {
            schema->dictSysTabPartAdd(tabPartRowid, tabPartObj, tabPartDataObj, tabPartBo);
            tabPartDataObj = 0;
            tabPartRet = stmtTabPart.next();
        }

        //reading SYS.TABSUBPART$
        DatabaseStatement stmtTabSubPart(conn);
        if (obj != 0) {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_TABSUBPART_OBJ);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << obj);
            stmtTabSubPart.createStatement(SQL_GET_SYS_TABSUBPART_OBJ);
            stmtTabSubPart.bindUInt64(1, schemaScn);
            stmtTabSubPart.bindUInt32(2, obj);
        } else {
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_TABSUBPART_USER);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM3: " << user);
            stmtTabSubPart.createStatement(SQL_GET_SYS_TABSUBPART_USER);
            stmtTabSubPart.bindUInt64(1, schemaScn);
            stmtTabSubPart.bindUInt64(2, schemaScn);
            stmtTabSubPart.bindUInt32(3, user);
        }

        char tabSubPartRowid[19]; stmtTabSubPart.defineString(1, tabSubPartRowid, sizeof(tabSubPartRowid));
        typeOBJ tabSubPartObj; stmtTabSubPart.defineUInt32(2, tabSubPartObj);
        typeDATAOBJ tabSubPartDataObj = 0; stmtTabSubPart.defineUInt32(3, tabSubPartDataObj);
        typeOBJ tabSubPartPobj; stmtTabSubPart.defineUInt32(4, tabSubPartPobj);

        int64_t tabSubPartRet = stmtTabSubPart.executeQuery();
        while (tabSubPartRet) {
            schema->dictSysTabSubPartAdd(tabSubPartRowid, tabSubPartObj, tabSubPartDataObj, tabSubPartPobj);
            tabSubPartDataObj = 0;
            tabSubPartRet = stmtTabSubPart.next();
        }
    }

    void OracleAnalyzerOnline::readSystemDictionaries(string owner, string table, typeOPTIONS options) {
        string ownerRegexp = "^" + owner + "$";
        string tableRegexp = "^" + table + "$";
        bool single = (options != 0);

        DEBUG("read dictionaries for owner: " << owner << ", table: " << table << ", options: " << dec << (uint64_t)options);

        try {
            DatabaseStatement stmtUser(conn);

            //reading SYS.USER$
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_USER);
            TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
            TRACE(TRACE2_SQL, "PARAM2: " << ownerRegexp);
            stmtUser.createStatement(SQL_GET_SYS_USER);
            stmtUser.bindUInt64(1, schemaScn);
            stmtUser.bindString(2, ownerRegexp);
            char userRowid[19]; stmtUser.defineString(1, userRowid, sizeof(userRowid));
            typeUSER userUser; stmtUser.defineUInt32(2, userUser);
            char userName[129]; stmtUser.defineString(3, userName, sizeof(userName));
            uint64_t userSpare11 = 0; stmtUser.defineUInt64(4, userSpare11);
            uint64_t userSpare12 = 0; stmtUser.defineUInt64(5, userSpare12);

            int64_t retUser = stmtUser.executeQuery();
            while (retUser) {
                if (!schema->dictSysUserAdd(userRowid, userUser, userName, userSpare11, userSpare12, options)) {
                    userSpare11 = 0;
                    userSpare12 = 0;
                    retUser = stmtUser.next();
                    continue;
                }

                DatabaseStatement stmtObj(conn);
                //reading SYS.OBJ$
                if (options == 0) {
                    TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_OBJ_USER);
                    TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
                    TRACE(TRACE2_SQL, "PARAM2: " << userUser);
                    stmtObj.createStatement(SQL_GET_SYS_OBJ_USER);
                    stmtObj.bindUInt64(1, schemaScn);
                    stmtObj.bindUInt32(2, userUser);
                } else {
                    TRACE(TRACE2_SQL, "SQL: " << SQL_GET_SYS_OBJ_NAME);
                    TRACE(TRACE2_SQL, "PARAM1: " << schemaScn);
                    TRACE(TRACE2_SQL, "PARAM2: " << userUser);
                    TRACE(TRACE2_SQL, "PARAM3: " << table);
                    stmtObj.createStatement(SQL_GET_SYS_OBJ_NAME);
                    stmtObj.bindUInt64(1, schemaScn);
                    stmtObj.bindUInt32(2, userUser);
                    stmtObj.bindString(3, table);
                }

                char objRowid[19]; stmtObj.defineString(1, objRowid, sizeof(objRowid));
                typeUSER objOwner; stmtObj.defineUInt32(2, objOwner);
                typeOBJ objObj; stmtObj.defineUInt32(3, objObj);
                typeDATAOBJ objDataObj = 0; stmtObj.defineUInt32(4, objDataObj);
                char objName[129]; stmtObj.defineString(5, objName, sizeof(objName));
                uint64_t objType = 0; stmtObj.defineUInt64(6, objType);
                uint64_t objFlags1; stmtObj.defineUInt64(7, objFlags1);
                uint64_t objFlags2; stmtObj.defineUInt64(8, objFlags2);

                int64_t objRet = stmtObj.executeQuery();
                while (objRet) {
                    if (schema->dictSysObjAdd(objRowid, objOwner, objObj, objDataObj, objType, objName, objFlags1, objFlags2, single)) {
                        if (single)
                            readSystemDictionariesDetails(userUser, objObj);
                    }
                    objDataObj = 0;
                    objFlags1 = 0;
                    objFlags2 = 0;
                    objRet = stmtObj.next();
                }

                if (!single)
                    readSystemDictionariesDetails(userUser, 0);

                userSpare11 = 0;
                userSpare12 = 0;
                retUser = stmtUser.next();
            }
        } catch (RuntimeException &ex) {
            RUNTIME_FAIL("Error reading schema from flashback, try some later scn for start");
        }
    }

    void OracleAnalyzerOnline::createSchemaForTable(string &owner, string &table, vector<string> &keys, string &keysStr, typeOPTIONS options) {
        DEBUG("- creating table schema for owner: " << owner << " table: " << table << " options: " << (uint64_t) options);

        readSystemDictionaries(owner, table, options);
        schema->buildMaps(owner, table, keys, keysStr, options, true);
        if ((options & OPTIONS_SCHEMA_TABLE) == 0 && schema->users.find(owner) == schema->users.end())
            schema->users.insert(owner);
    }

    void OracleAnalyzerOnline::archGetLogOnline(OracleAnalyzer *oracleAnalyzer) {
        ((OracleAnalyzerOnline*)oracleAnalyzer)->checkConnection();

        {
            DatabaseStatement stmt(((OracleAnalyzerOnline*)oracleAnalyzer)->conn);
            TRACE(TRACE2_SQL, "SQL: " << SQL_GET_ARCHIVE_LOG_LIST);
            TRACE(TRACE2_SQL, "PARAM1: " << ((OracleAnalyzerOnline*)oracleAnalyzer)->sequence);
            TRACE(TRACE2_SQL, "PARAM2: " << oracleAnalyzer->resetlogs);
            TRACE(TRACE2_SQL, "PARAM3: " << oracleAnalyzer->activation);

            stmt.createStatement(SQL_GET_ARCHIVE_LOG_LIST);
            stmt.bindUInt32(1, ((OracleAnalyzerOnline*)oracleAnalyzer)->sequence);
            stmt.bindUInt32(2, oracleAnalyzer->resetlogs);
            stmt.bindUInt32(3, oracleAnalyzer->activation);

            char path[513]; stmt.defineString(1, path, sizeof(path));
            typeSEQ sequence; stmt.defineUInt32(2, sequence);
            typeSCN firstScn; stmt.defineUInt64(3, firstScn);
            typeSCN nextScn; stmt.defineUInt64(4, nextScn);
            int64_t ret = stmt.executeQuery();

            while (ret) {
                string mappedPath = oracleAnalyzer->applyMapping(path);

                RedoLog* redo = new RedoLog(oracleAnalyzer, 0, mappedPath.c_str());
                if (redo == nullptr) {
                    RUNTIME_FAIL("couldn't allocate " << dec << sizeof(RedoLog) << " bytes memory (arch log list#1)");
                }

                redo->firstScn = firstScn;
                redo->nextScn = nextScn;
                redo->sequence = sequence;
                ((OracleAnalyzerOnline*)oracleAnalyzer)->archiveRedoQueue.push(redo);
                ret = stmt.next();
            }
        }
        oracleAnalyzer->goStandby();
    }

    const char* OracleAnalyzerOnline::getModeName(void) const {
        return "online";
    }
}
