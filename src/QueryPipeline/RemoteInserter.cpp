#include <QueryPipeline/RemoteInserter.h>

#include <Client/Connection.h>
#include <base/logger_useful.h>

#include <Common/NetException.h>
#include <Common/CurrentThread.h>
#include <Interpreters/InternalTextLogsQueue.h>
#include <IO/ConnectionTimeouts.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int UNEXPECTED_PACKET_FROM_SERVER;
}


RemoteInserter::RemoteInserter(
    Connection & connection_,
    const ConnectionTimeouts & timeouts,
    const String & query_,
    const Settings & settings_,
    const ClientInfo & client_info_)
    : connection(connection_)
    , query(query_)
    , server_revision(connection.getServerRevision(timeouts))
{
    ClientInfo modified_client_info = client_info_;
    modified_client_info.query_kind = ClientInfo::QueryKind::SECONDARY_QUERY;
    if (CurrentThread::isInitialized())
    {
        modified_client_info.client_trace_context
            = CurrentThread::get().thread_trace_context;
    }

    /** Send query and receive "header", that describes table structure.
      * Header is needed to know, what structure is required for blocks to be passed to 'write' method.
      */
    connection.sendQuery(timeouts, query, "", QueryProcessingStage::Complete, &settings_, &modified_client_info, false);

    while (true)
    {
        Packet packet = connection.receivePacket();

        if (Protocol::Server::Data == packet.type)
        {
            header = packet.block;
            break;
        }
        else if (Protocol::Server::Exception == packet.type)
        {
            packet.exception->rethrow();
            break;
        }
        else if (Protocol::Server::Log == packet.type)
        {
            /// Pass logs from remote server to client
            if (auto log_queue = CurrentThread::getInternalTextLogsQueue())
                log_queue->pushBlock(std::move(packet.block));
        }
        else if (Protocol::Server::TableColumns == packet.type)
        {
            /// Server could attach ColumnsDescription in front of stream for column defaults. There's no need to pass it through cause
            /// client's already got this information for remote table. Ignore.
        }
        else
            throw NetException("Unexpected packet from server (expected Data or Exception, got "
                + String(Protocol::Server::toString(packet.type)) + ")", ErrorCodes::UNEXPECTED_PACKET_FROM_SERVER);
    }
}


void RemoteInserter::write(Block block)
{
    try
    {
        connection.sendData(block, /* name */"", /* scalar */false);
    }
    catch (const NetException &)
    {
        /// Try to get more detailed exception from server
        auto packet_type = connection.checkPacket(/* timeout_microseconds */0);
        if (packet_type && *packet_type == Protocol::Server::Exception)
        {
            Packet packet = connection.receivePacket();
            packet.exception->rethrow();
        }

        throw;
    }
}


void RemoteInserter::writePrepared(ReadBuffer & buf, size_t size)
{
    /// We cannot use 'header'. Input must contain block with proper structure.
    connection.sendPreparedData(buf, size);
}


void RemoteInserter::onFinish()
{
    /// Empty block means end of data.
    connection.sendData(Block(), /* name */"", /* scalar */false);

    /// Wait for EndOfStream or Exception packet, skip Log packets.
    while (true)
    {
        Packet packet = connection.receivePacket();

        if (Protocol::Server::EndOfStream == packet.type)
            break;
        else if (Protocol::Server::Exception == packet.type)
            packet.exception->rethrow();
        else if (Protocol::Server::Log == packet.type)
        {
            // Do nothing
        }
        else
            throw NetException("Unexpected packet from server (expected EndOfStream or Exception, got "
            + String(Protocol::Server::toString(packet.type)) + ")", ErrorCodes::UNEXPECTED_PACKET_FROM_SERVER);
    }

    finished = true;
}

RemoteInserter::~RemoteInserter()
{
    /// If interrupted in the middle of the loop of communication with the server, then interrupt the connection,
    ///  to not leave the connection in unsynchronized state.
    if (!finished)
    {
        try
        {
            connection.disconnect();
        }
        catch (...)
        {
            tryLogCurrentException(__PRETTY_FUNCTION__);
        }
    }
}

}
