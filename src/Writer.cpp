/* Base class for thread to write output
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

#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "global.h"
#include "ConfigurationException.h"
#include "NetworkException.h"
#include "OracleAnalyzer.h"
#include "OutputBuffer.h"
#include "RuntimeException.h"
#include "Writer.h"

using namespace std;
using namespace rapidjson;

namespace OpenLogReplicator {
    Writer::Writer(const char* alias, OracleAnalyzer* oracleAnalyzer, uint64_t maxMessageMb, uint64_t pollIntervalUS,
            uint64_t checkpointIntervalS, uint64_t queueSize, typeSCN startScn, typeSEQ startSequence, const char* startTime,
            int64_t startTimeRel) :
        Thread(alias),
        oracleAnalyzer(oracleAnalyzer),
        confirmedMessages(0),
        sentMessages(0),
        tmpQueueSize(0),
        maxQueueSize(0),
        queue(nullptr),
        maxMessageMb(maxMessageMb),
        pollIntervalUS(pollIntervalUS),
        previousCheckpoint(time(nullptr)),
        checkpointIntervalS(checkpointIntervalS),
        queueSize(queueSize),
        confirmedScn(ZERO_SCN),
        startScn(startScn),
        startSequence(startSequence),
        startTime(startTime),
        startTimeRel(startTimeRel),
        streaming(false),
        outputBuffer(oracleAnalyzer->outputBuffer) {

        queue = new OutputBufferMsg*[queueSize];
        if (queue == nullptr) {
            RUNTIME_FAIL("couldn't allocate " << queueSize * sizeof(struct OutputBufferMsg) << " bytes memory (for: message queue)");
        }
    }

    Writer::~Writer() {
        if (queue != nullptr) {
            delete[] queue;
            queue = nullptr;
        }
    }

    void Writer::createMessage(OutputBufferMsg* msg) {
        ++sentMessages;

        queue[tmpQueueSize++] = msg;
        if (tmpQueueSize > maxQueueSize)
            maxQueueSize = tmpQueueSize;
    }

    void Writer::sortQueue(void) {
        if (tmpQueueSize == 0)
            return;

        OutputBufferMsg** oldQueue = queue;
        queue = new OutputBufferMsg*[queueSize];
        if (queue == nullptr) {
            if (oldQueue != nullptr) {
                delete[] oldQueue;
                oldQueue = nullptr;
            }
            RUNTIME_FAIL("couldn't allocate " << queueSize * sizeof(struct OutputBufferMsg) << " bytes memory (for: message queue)");
        }
        uint64_t oldQueueSize = tmpQueueSize;

        for (uint64_t newId = 0 ; newId < tmpQueueSize; ++newId) {
            queue[newId] = oldQueue[0];
            uint64_t i = 0;
            --oldQueueSize;
            while (i < oldQueueSize) {
                if (i * 2 + 2 < oldQueueSize && oldQueue[i * 2 + 2]->id < oldQueue[oldQueueSize]->id) {
                    if (oldQueue[i * 2 + 1]->id < oldQueue[i * 2 + 2]->id) {
                        oldQueue[i] = oldQueue[i * 2 + 1];
                        i = i * 2 + 1;
                    } else {
                        oldQueue[i] = oldQueue[i * 2 + 2];
                        i = i * 2 + 2;
                    }
                } else if (i * 2 + 1 < oldQueueSize && oldQueue[i * 2 + 1]->id < oldQueue[oldQueueSize]->id) {
                    oldQueue[i] = oldQueue[i * 2 + 1];
                    i = i * 2 + 1;
                } else
                    break;
            }
            oldQueue[i] = oldQueue[oldQueueSize];
        }

        if (oldQueue != nullptr) {
            delete[] oldQueue;
            oldQueue = nullptr;
        }
    }

    void Writer::confirmMessage(OutputBufferMsg* msg) {
        if (msg == nullptr) {
            if (tmpQueueSize == 0) {
                RUNTIME_FAIL("trying to confirm empty message");
            }
            msg = queue[0];
        }

        msg->flags |= OUTPUT_BUFFER_CONFIRMED;
        if (msg->flags & OUTPUT_BUFFER_ALLOCATED) {
            delete[] msg->data;
            msg->flags &= ~OUTPUT_BUFFER_ALLOCATED;
        }
        ++confirmedMessages;

        uint64_t maxId = 0;
        {
            while (tmpQueueSize > 0 && (queue[0]->flags & OUTPUT_BUFFER_CONFIRMED) != 0) {
                maxId = queue[0]->queueId;
                confirmedScn = queue[0]->scn;

                if (--tmpQueueSize == 0)
                    break;

                uint64_t i = 0;
                while (i < tmpQueueSize) {
                    if (i * 2 + 2 < tmpQueueSize && queue[i * 2 + 2]->id < queue[tmpQueueSize]->id) {
                        if (queue[i * 2 + 1]->id < queue[i * 2 + 2]->id) {
                            queue[i] = queue[i * 2 + 1];
                            i = i * 2 + 1;
                        } else {
                            queue[i] = queue[i * 2 + 2];
                            i = i * 2 + 2;
                        }
                    } else if (i * 2 + 1 < tmpQueueSize && queue[i * 2 + 1]->id < queue[tmpQueueSize]->id) {
                        queue[i] = queue[i * 2 + 1];
                        i = i * 2 + 1;
                    } else
                        break;
                }
                queue[i] = queue[tmpQueueSize];
            }
        }

        OutputBufferQueue* tmpFirstBuffer = nullptr;
        {
            unique_lock<mutex> lck(outputBuffer->mtx);
            tmpFirstBuffer = outputBuffer->firstBuffer;
            while (outputBuffer->firstBuffer->id < maxId) {
                outputBuffer->firstBuffer = outputBuffer->firstBuffer->next;
                --outputBuffer->buffersAllocated;
            }
        }

        if (tmpFirstBuffer != nullptr) {
            while (tmpFirstBuffer->id < maxId) {
                OutputBufferQueue* nextBuffer = tmpFirstBuffer->next;
                oracleAnalyzer->freeMemoryChunk("KAFKA", (uint8_t*)tmpFirstBuffer, true);
                tmpFirstBuffer = nextBuffer;
            }
            {
                unique_lock<mutex> lck(oracleAnalyzer->mtx);
                oracleAnalyzer->memoryCond.notify_all();
            }
        }
    }

    void* Writer::run(void) {
        TRACE(TRACE2_THREADS, "THREADS: WRITER (" << hex << this_thread::get_id() << ") START");

        INFO("writer is starting: " << getName());

        try {
            //external loop for client disconnection
            while (!shutdown) {
                try {
                    //client connected
                    readCheckpoint();

                    OutputBufferMsg* msg = nullptr;
                    OutputBufferQueue* curBuffer = outputBuffer->firstBuffer;
                    uint64_t curLength = 0, tmpLength = 0;
                    tmpQueueSize = 0;

                    //start streaming
                    while (!shutdown) {

                        //get message to send
                        while (!shutdown) {
                            //check for client checkpoint
                            pollQueue();
                            writeCheckpoint(false);

                            {
                                unique_lock<mutex> lck(outputBuffer->mtx);

                                //next buffer
                                if (curBuffer->length == curLength && curBuffer->next != nullptr) {
                                    curBuffer = curBuffer->next;
                                    curLength = 0;
                                }

                                //found something
                                msg = (OutputBufferMsg*) (curBuffer->data + curLength);

                                if (curBuffer->length > curLength + sizeof(struct OutputBufferMsg) && msg->length > 0) {
                                    oracleAnalyzer->waitingForWriter = true;
                                    tmpLength = curBuffer->length;
                                    break;
                                }

                                oracleAnalyzer->waitingForWriter = false;
                                oracleAnalyzer->memoryCond.notify_all();

                                if (tmpQueueSize > 0) {
                                    outputBuffer->writersCond.wait_for(lck, chrono::nanoseconds(pollIntervalUS));
                                } else {
                                    if (stop) {
                                        INFO("writer flushed, shutting down");
                                        doShutdown();
                                    } else {
                                        outputBuffer->writersCond.wait_for(lck, chrono::seconds(5));
                                    }
                                }
                            }
                        }

                        if (shutdown)
                            break;

                        //send message
                        while (curLength + sizeof(struct OutputBufferMsg) < tmpLength && !shutdown) {
                            msg = (OutputBufferMsg*) (curBuffer->data + curLength);
                            if (msg->length == 0)
                                break;

                            //queue is full
                            pollQueue();
                            while (tmpQueueSize >= queueSize && !shutdown) {
                                DEBUG("output queue is full (" << dec << tmpQueueSize << " elements), sleeping " << dec << pollIntervalUS << "us");
                                usleep(pollIntervalUS);
                                pollQueue();
                            }
                            writeCheckpoint(false);
                            if (shutdown)
                                break;

                            //outputBuffer->firstBufferPos += OUTPUT_BUFFER_RECORD_HEADER_SIZE;
                            uint64_t length8 = (msg->length + 7) & 0xFFFFFFFFFFFFFFF8;
                            curLength += sizeof(struct OutputBufferMsg);

                            //message in one part - send directly from buffer
                            if (curLength + length8 <= OUTPUT_BUFFER_DATA_SIZE) {
                                createMessage(msg);
                                sendMessage(msg);
                                curLength += length8;
                                msg = (OutputBufferMsg*) (curBuffer->data + curLength);

                            //message in many parts - copy
                            } else {
                                msg->data = (uint8_t*)malloc(msg->length);
                                if (msg->data == nullptr) {
                                    RUNTIME_FAIL("couldn't allocate " << dec << msg->length << " bytes memory (for: temporary buffer for JSON message)");
                                }
                                msg->flags |= OUTPUT_BUFFER_ALLOCATED;

                                uint64_t copied = 0;
                                while (msg->length - copied > 0) {
                                    uint64_t toCopy = msg->length - copied;
                                    if (toCopy > tmpLength - curLength) {
                                        toCopy = tmpLength - curLength;
                                        memcpy(msg->data + copied, curBuffer->data + curLength, toCopy);
                                        curBuffer = curBuffer->next;
                                        tmpLength = OUTPUT_BUFFER_DATA_SIZE;
                                        curLength = 0;
                                    } else {
                                        memcpy(msg->data + copied, curBuffer->data + curLength, toCopy);
                                        curLength += (toCopy + 7) & 0xFFFFFFFFFFFFFFF8;
                                    }
                                    copied += toCopy;
                                }

                                createMessage(msg);
                                sendMessage(msg);
                                pollQueue();
                                writeCheckpoint(false);
                                break;
                            }
                        }
                    }

                    writeCheckpoint(true);

                } catch (NetworkException& ex) {
                    streaming = false;
                    //client got disconnected
                }
            }

        } catch (ConfigurationException& ex) {
            stopMain();
        } catch (RuntimeException& ex) {
            stopMain();
        }

        INFO("writer is stopping: " << getName() << ", max queue size: " << dec << maxQueueSize);

        TRACE(TRACE2_THREADS, "THREADS: WRITER (" << hex << this_thread::get_id() << ") STOP");
        return 0;
    }

    void Writer::writeCheckpoint(bool force) {
        if (oracleAnalyzer->checkpointScn == confirmedScn || confirmedScn == ZERO_SCN)
            return;
        if (oracleAnalyzer->schemaScn >= confirmedScn)
            force = true;

        time_t now = time(nullptr);
        uint64_t timeSinceCheckpoint = (now - previousCheckpoint);
        if (timeSinceCheckpoint < checkpointIntervalS && !force)
            return;

        TRACE(TRACE2_CHECKPOINT, "CHECKPOINT: writer checkpoint scn: " << dec << oracleAnalyzer->checkpointScn << " confirmed scn: " << confirmedScn);
        string fileName(oracleAnalyzer->checkpointPath + "/" + oracleAnalyzer->database + "-chkpt.json");
        ofstream outfile;
        outfile.open(fileName.c_str(), ios::out | ios::trunc);

        if (!outfile.is_open()) {
            RUNTIME_FAIL("writing checkpoint data to " << fileName);
        }

        stringstream ss;
        ss << "{\"database\":\"" << oracleAnalyzer->database
                << "\",\"scn\":" << dec << confirmedScn
                << ",\"resetlogs\":" << dec << oracleAnalyzer->resetlogs
                << ",\"activation\":" << dec << oracleAnalyzer->activation << "}";

        outfile << ss.rdbuf();
        outfile.close();

        oracleAnalyzer->checkpointScn = confirmedScn;
        previousCheckpoint = now;
    }

    void Writer::readCheckpoint(void) {
        ifstream infile;
        string fileName(oracleAnalyzer->checkpointPath + "/" + oracleAnalyzer->database + "-chkpt.json");

        struct stat fileStat;
        int ret = stat(fileName.c_str(), &fileStat);
        TRACE(TRACE2_FILE, "FILE: stat for file: " << fileName << " - " << strerror(errno));
        if (ret != 0) {
            startReader();
            return;
        }
        if (fileStat.st_size > CHECKPOINT_FILE_MAX_SIZE || fileStat.st_size == 0) {
            RUNTIME_FAIL("checkpoint file: " << fileName << " wrong size: " << dec << fileStat.st_size);
        }

        infile.open(fileName.c_str(), ios::in);
        if (!infile.is_open()) {
            RUNTIME_FAIL("checkpoint file: " << fileName << " read error");
        }

        string configJSON((istreambuf_iterator<char>(infile)), istreambuf_iterator<char>());
        Document document;

        if (configJSON.length() == 0 || document.Parse(configJSON.c_str()).HasParseError()) {
            RUNTIME_FAIL("parsing " << fileName << " at offset: " << document.GetErrorOffset() <<
                    ", message: " << GetParseError_En(document.GetParseError()));
        }

        const char* database = getJSONfieldS(fileName, JSON_PARAMETER_LENGTH, document, "database");
        if (oracleAnalyzer->database.compare(database) != 0) {
            RUNTIME_FAIL("parsing of " << fileName << " - invalid database name");
        }

        oracleAnalyzer->resetlogs = getJSONfieldU32(fileName, document, "resetlogs");
        oracleAnalyzer->activation = getJSONfieldU32(fileName, document, "activation");

        //started earlier - continue work & ignore default startup parameters
        startScn = getJSONfieldU64(fileName, document, "scn");
        startSequence = ZERO_SEQ;
        startTime.clear();
        startTimeRel = 0;
        INFO("checkpoint - reading scn: " << dec << startScn);

        infile.close();
        startReader();
    }

    void Writer::startReader(void) {
        if (startScn == ZERO_SCN && startSequence == ZERO_SEQ && startTime.length() == 0 && startTimeRel == 0)
            startScn = 0;

        oracleAnalyzer->startSequence = startSequence;
        oracleAnalyzer->startScn = startScn;
        oracleAnalyzer->startTime = startTime;
        oracleAnalyzer->startTimeRel = startTimeRel;

        DEBUG("attempt to start analyzer");
        if (oracleAnalyzer->firstScn == ZERO_SCN && !shutdown) {
            unique_lock<mutex> lck(oracleAnalyzer->mtx);
            oracleAnalyzer->writerCond.notify_all();
            outputBuffer->writersCond.wait(lck);
        }

        if (oracleAnalyzer->firstScn != ZERO_SCN && !shutdown) {
            DEBUG("analyzer started");
        }
    }
}
