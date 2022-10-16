/* Oracle Redo OpCode: 10.8
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

#include "../common/RedoLogRecord.h"
#include "OpCode0A08.h"

namespace OpenLogReplicator {
    void OpCode0A08::process(Ctx* ctx, RedoLogRecord* redoLogRecord) {
        OpCode::process(ctx, redoLogRecord);
        uint64_t fieldPos = 0;
        typeField fieldNum = 0;
        uint16_t fieldLength = 0;

        RedoLogRecord::nextField(ctx, redoLogRecord, fieldNum, fieldPos, fieldLength, 0x0A0801);
        // Field: 1
        if (fieldLength > 0) {
            if (ctx->dumpRedoLog >= 1) {
                ctx->dumpStream << "index redo (kdxlne): (count=" << std::dec << redoLogRecord->fieldCnt << ") init header of newly allocated leaf block" <<
                        std::endl;
            }

            ktbRedo(ctx, redoLogRecord, fieldPos, fieldLength);

            RedoLogRecord::nextField(ctx, redoLogRecord, fieldNum, fieldPos, fieldLength, 0x0A0802);
            // Field: 2
            kdxln(ctx, redoLogRecord, fieldPos, fieldLength);
        } else {
            if (ctx->dumpRedoLog >= 1) {
                ctx->dumpStream << "index redo (kdxlne): (count=" << std::dec << redoLogRecord->fieldCnt << ") init leaf block being split" << std::endl;
            }

            RedoLogRecord::nextField(ctx, redoLogRecord, fieldNum, fieldPos, fieldLength, 0x0A0803);
            // Field: 2

            if (fieldLength < 4) {
                WARNING("too short field split kdxlne: " << std::dec << fieldLength << " offset: " << redoLogRecord->dataOffset)
                return;
            }

            if (ctx->dumpRedoLog >= 1) {
                uint32_t kdxlenxt = ctx->read32(redoLogRecord->data + fieldPos + 0);
                ctx->dumpStream << "zeroed lock count and free space, kdxlenxt = 0x" << std::hex << kdxlenxt << std::endl;
            }
        }

        RedoLogRecord::nextField(ctx, redoLogRecord, fieldNum, fieldPos, fieldLength, 0x0A0804);
        // Field: 3
        uint64_t rows = fieldLength / 2 - 1;
        if (ctx->dumpRedoLog >= 1) {
            ctx->dumpStream << "new block has " << std::dec << rows << " rows" << std::endl;
            ctx->dumpStream << "dumping row index" << std::endl;
        }
        dumpMemory(ctx, redoLogRecord, fieldPos, fieldLength);

        RedoLogRecord::nextField(ctx, redoLogRecord, fieldNum, fieldPos, fieldLength, 0x0A0805);
        // Field: 4

        if (rows == 1) {
            redoLogRecord->indKey = fieldPos;
            redoLogRecord->indKeyLength = fieldLength;
        }

        if (ctx->dumpRedoLog >= 1) {
            ctx->dumpStream << "dumping rows" << std::endl;
        }
        dumpMemory(ctx, redoLogRecord, fieldPos, fieldLength);
    }

    void OpCode0A08::kdxln(Ctx* ctx, RedoLogRecord* redoLogRecord, uint64_t& fieldPos, uint16_t& fieldLength) {
        if (fieldLength < 16) {
            WARNING("too short field KDXLN: " << std::dec << fieldLength << " offset: " << redoLogRecord->dataOffset)
            return;
        }

        if (ctx->dumpRedoLog >= 1) {
            auto itl = (uint8_t) redoLogRecord->data[fieldPos];
            auto nco = (uint8_t) redoLogRecord->data[fieldPos + 1];
            auto dsz = (uint8_t) redoLogRecord->data[fieldPos + 2];
            auto col = (uint8_t) redoLogRecord->data[fieldPos + 3];
            auto flg = (uint8_t) redoLogRecord->data[fieldPos + 4];
            typeDba nxt = ctx->read32(redoLogRecord->data + fieldPos + 8);
            typeDba prv = ctx->read32(redoLogRecord->data + fieldPos + 12);

            ctx->dumpStream << "kdxlnitl = " << std::dec << (uint64_t)itl << std::endl;
            ctx->dumpStream << "kdxlnnco = " << std::dec << (uint64_t)nco << std::endl;
            ctx->dumpStream << "kdxlndsz = " << std::dec << (uint64_t)dsz << std::endl;
            ctx->dumpStream << "kdxlncol = " << std::dec << (uint64_t)col << std::endl;
            ctx->dumpStream << "kdxlnflg = " << std::dec << (uint64_t)flg << std::endl;
            ctx->dumpStream << "kdxlnnxt = 0x" << std::hex << nxt << std::endl;
            ctx->dumpStream << "kdxlnprv = 0x" << std::hex << prv << std::endl;
        }
    }
}
