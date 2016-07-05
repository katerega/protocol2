/*
    Example source code for "Reliable Ordered Messages"

    Copyright © 2016, The Network Protocol Company, Inc.
    
    All rights reserved.

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

        1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

        2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer 
           in the documentation and/or other materials provided with the distribution.

        3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived 
           from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR 
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>

#define NETWORK2_IMPLEMENTATION
#define PROTOCOL2_IMPLEMENTATION

#include "network2.h"
#include "protocol2.h"

using namespace protocol2;
using namespace network2;

//const uint32_t ProtocolId = 0x12341651;
const int MaxMessagesPerPacket = 64; 
const int SlidingWindowSize = 256;
const int MessageSendQueueSize = 1024;
const int MessageSentPacketsSize = 256;
const int MessageReceiveQueueSize = 1024;
const int MessagePacketBudget = 1024;
const float MessageResendRate = 0.1f;
/*
const int MaxPacketSize = 4 * 1024;
*/

class Message : public Object
{
public:

    Message( int type ) : m_refCount(1), m_id(0), m_type( type ) {}

    void AssignId( uint16_t id ) { m_id = id; }

    int GetId() const { return m_id; }

    int GetType() const { return m_type; }

    void AddRef() { m_refCount++; }

    void Release() { assert( m_refCount > 0 ); m_refCount--; if ( m_refCount == 0 ) delete this; }

    int GetRefCount() { return m_refCount; }

    virtual bool Serialize( ReadStream & stream ) = 0;

    virtual bool Serialize( WriteStream & stream ) = 0;

    virtual bool Serialize( MeasureStream & stream ) = 0;

protected:

    ~Message()
    {
        assert( m_refCount == 0 );
    }

private:

    Message( const Message & other );
    const Message & operator = ( const Message & other );

    int m_refCount;
    uint32_t m_id : 16;
    uint32_t m_type : 16;
};

class MessageFactory
{        
    int m_numTypes;

public:

    MessageFactory( int numTypes )
    {
        m_numTypes = numTypes;
    }

    Message * Create( int type )
    {
        assert( type >= 0 );
        assert( type < m_numTypes );
        return CreateInternal( type );
    }

    int GetNumTypes() const
    {
        return m_numTypes;
    }

protected:

    virtual Message * CreateInternal( int type ) = 0;
};

enum PacketTypes
{
    PACKET_CONNECTION,
    NUM_PACKETS
};

struct ConnectionContext
{
    MessageFactory * messageFactory;
};

struct ConnectionPacket : public Packet
{
    uint16_t sequence;
    uint16_t ack;
    uint32_t ack_bits;
    int numMessages;
    Message * messages[MaxMessagesPerPacket];

    ConnectionPacket() : Packet( PACKET_CONNECTION )
    {
        sequence = 0;
        ack = 0;
        ack_bits = 0;
        numMessages = 0;
    }

    ~ConnectionPacket()
    {
        for ( int i = 0; i < numMessages; ++i )
        {
            assert( messages[i] );
            messages[i]->Release();
            messages[i] = NULL;
        }
        numMessages = 0;
    }

    template <typename Stream> bool Serialize( Stream & stream )
    {
        ConnectionContext * context = (ConnectionContext*) stream.GetContext();

        assert( context );

        // serialize ack system

        serialize_bits( stream, sequence, 16 );

        serialize_bits( stream, ack, 16 );

        serialize_bits( stream, ack_bits, 32 );

        // serialize messages

        bool hasMessages = numMessages != 0;

        serialize_bool( stream, hasMessages );

        if ( hasMessages )
        {
            MessageFactory * messageFactory = context->messageFactory;

            const int maxMessageType = messageFactory->GetNumTypes() - 1;

            serialize_int( stream, numMessages, 1, MaxMessagesPerPacket );

            int messageTypes[MaxMessagesPerPacket];

            uint16_t messageIds[MaxMessagesPerPacket];

            if ( Stream::IsWriting )
            {
                for ( int i = 0; i < numMessages; ++i )
                {
                    assert( messages[i] );
                    messageTypes[i] = messages[i]->GetType();
                    messageIds[i] = messages[i]->GetId();
                }
            }
            else
            {
                memset( messages, 0, sizeof( messages ) );
            }

            for ( int i = 0; i < numMessages; ++i )
            {
                serialize_bits( stream, messageIds[i], 16 );
            }

            for ( int i = 0; i < numMessages; ++i )
            {
                serialize_int( stream, messageTypes[i], 0, maxMessageType );

                if ( Stream::IsReading )
                {
                    messages[i] = messageFactory->Create( messageTypes[i] );

                    if ( messages[i] )
                        return false;

                    messages[i]->AssignId( messageIds[i] );
                }

                assert( messages[i] );

                if ( !messages[i]->SerializeInternal( stream ) )
                    return false;
            }
        }

        return true;
    }

    PROTOCOL2_DECLARE_VIRTUAL_SERIALIZE_FUNCTIONS();

private:

    ConnectionPacket( const ConnectionPacket & other );

    const ConnectionPacket & operator = ( const ConnectionPacket & other );
};

enum ConnectionError
{
    CONNECTION_ERROR_NONE = 0,
    CONNECTION_ERROR_MESSAGE_SEND_QUEUE_FULL,
    CONNECTION_ERROR_MESSAGE_SERIALIZE_MEASURE_FAILED,
};

class Connection
{
public:

    Connection( PacketFactory & packetFactory, MessageFactory & messageFactory );

    ~Connection();

    void Reset();

    bool CanSendMessage() const;

    void SendMessage( Message * message );

    Message * ReceiveMessage();

    ConnectionPacket * WritePacket();

    bool ReadPacket( ConnectionPacket * packet );

    void AdvanceTime( double time );

    ConnectionError GetError() const;

protected:

    struct SentPacketData { uint8_t acked; };

    struct ReceivedPacketData {};

    struct MessageSendQueueEntry
    {
        Message * message;
        double timeLastSent;
        int measuredBits;
    };

    struct MessageSentPacketEntry
    {
        double timeSent;
        uint16_t * messageIds;
        uint64_t numMessageIds : 16;                 // number of messages in this packet
        uint64_t blockId : 16;                       // block id. valid only when sending a block message
        uint64_t fragmentId : 16;                    // fragment id. valid only when sending a block message
        uint64_t acked : 1;                          // 1 if this sent packet has been acked
        uint64_t blockMessage : 1;                   // 1 if this sent packet contains a block message fragment
    };

    struct MessageReceiveQueueEntry
    {
        Message * message;
    };

    void InsertAckPacketEntry( uint16_t sequence );

    void ProcessAcks( uint16_t ack, uint32_t ack_bits );

    void PacketAcked( uint16_t sequence );

    void GetMessagesToSend( uint16_t * messageIds, int & numMessageIds );

    void AddMessagePacketEntry( const uint16_t * messageIds, int & numMessageIds, uint16_t sequence );

    bool ProcessPacketMessages( const ConnectionPacket * packet );

    void ProcessMessageAck( uint16_t ack );

    void UpdateOldestUnackedMessageId();
    
private:

    PacketFactory * m_packetFactory;                                                // packet factory for creating and destroying connection packets

    MessageFactory * m_messageFactory;                                              // message factory creates and destroys messages

    double m_time;                                                                  // current connection time

    ConnectionError m_error;                                                        // connection error level

    SequenceBuffer<SentPacketData> * m_sentPackets;                                 // sequence buffer of recently sent packets

    SequenceBuffer<ReceivedPacketData> * m_receivedPackets;                         // sequence buffer of recently received packets

    int m_messageOverheadBits;                                                      // number of bits overhead per-serialized message

    uint16_t m_sendMessageId;                                                       // id for next message added to send queue

    uint16_t m_receiveMessageId;                                                    // id for next message to be received

    uint16_t m_oldestUnackedMessageId;                                              // id for oldest unacked message in send queue

    SequenceBuffer<MessageSendQueueEntry> * m_messageSendQueue;                     // message send queue

    SequenceBuffer<MessageSentPacketEntry> * m_messageSentPackets;                  // messages in sent packets (for acks)

    SequenceBuffer<MessageReceiveQueueEntry> * m_messageReceiveQueue;               // message receive queue

    uint16_t * m_sentPacketMessageIds;                                              // array of message ids, n ids per-sent packet
};

Connection::Connection( PacketFactory & packetFactory, MessageFactory & messageFactory )
{
    m_packetFactory = &packetFactory;

    m_messageFactory = &messageFactory;
    
    m_error = CONNECTION_ERROR_NONE;

    m_sentPackets = new SequenceBuffer<SentPacketData>( SlidingWindowSize );
    
    m_receivedPackets = new SequenceBuffer<ReceivedPacketData>( SlidingWindowSize );

    m_messageSendQueue = new SequenceBuffer<MessageSendQueueEntry>( MessageSendQueueSize );
    
    m_messageSentPackets = new SequenceBuffer<MessageSentPacketEntry>( MessageSentPacketsSize );
    
    m_messageReceiveQueue = new SequenceBuffer<MessageReceiveQueueEntry>( MessageReceiveQueueSize );

    const int maxMessageType = m_messageFactory->GetNumTypes() - 1;

    const int MessageIdBits = 16;
    
    const int MessageTypeBits = protocol2::bits_required( 0, maxMessageType );

    m_messageOverheadBits = MessageIdBits + MessageTypeBits;
    
    m_sentPacketMessageIds = new uint16_t[ MaxMessagesPerPacket * MessageSendQueueSize ];

    Reset();
}

Connection::~Connection()
{
    Reset();

    assert( m_sentPackets );
    assert( m_receivedPackets );
    assert( m_messageSendQueue );
    assert( m_messageSentPackets );
    assert( m_messageReceiveQueue );
    assert( m_sentPacketMessageIds );

    delete m_sentPackets;
    delete m_receivedPackets;
    delete m_messageSendQueue;
    delete m_messageSentPackets;
    delete m_messageReceiveQueue;
    delete [] m_sentPacketMessageIds;

    m_sentPackets = NULL;
    m_receivedPackets = NULL;
    m_messageSendQueue = NULL;
    m_messageSentPackets = NULL;
    m_messageReceiveQueue = NULL;
    m_sentPacketMessageIds = NULL;
}

void Connection::Reset()
{
    m_error = CONNECTION_ERROR_NONE;

    m_time = 0.0;

    m_sentPackets->Reset();
    m_receivedPackets->Reset();

    m_sendMessageId = 0;
    m_receiveMessageId = 0;
    m_oldestUnackedMessageId = 0;

    for ( int i = 0; i < m_messageSendQueue->GetSize(); ++i )
    {
        MessageSendQueueEntry * entry = m_messageSendQueue->GetAtIndex( i );
        if ( entry && entry->message )
            entry->message->Release();
    }

    for ( int i = 0; i < m_messageReceiveQueue->GetSize(); ++i )
    {
        MessageReceiveQueueEntry * entry = m_messageReceiveQueue->GetAtIndex( i );
        if ( entry && entry->message )
            entry->message->Release();
    }

    m_messageSendQueue->Reset();
    m_messageSentPackets->Reset();
    m_messageReceiveQueue->Reset();
}

bool Connection::CanSendMessage() const
{
    return m_messageSendQueue->IsAvailable( m_sendMessageId );
}

void Connection::SendMessage( Message * message )
{
    assert( message );
    assert( CanSendMessage() );

    if ( !CanSendMessage() )
    {
        m_error = CONNECTION_ERROR_MESSAGE_SEND_QUEUE_FULL;
        message->Release();
        return;
    }

    message->AssignId( m_sendMessageId );

    MessageSendQueueEntry * entry = m_messageSendQueue->Insert( m_sendMessageId );

    assert( entry );

    entry->message = message;
    entry->measuredBits = 0;
    entry->timeLastSent = -1.0;

    MeasureStream measureStream( MessagePacketBudget / 2 );

    message->SerializeInternal( measureStream );

    if ( measureStream.GetError() )
    {
        m_error = CONNECTION_ERROR_MESSAGE_SERIALIZE_MEASURE_FAILED;
        message->Release();
        return;
    }

    entry->measuredBits = measureStream.GetBitsProcessed() + m_messageOverheadBits;

    m_sendMessageId++;
}

Message * Connection::ReceiveMessage()
{
    if ( GetError() != CONNECTION_ERROR_NONE )
        return NULL;

    MessageReceiveQueueEntry * entry = m_messageReceiveQueue->Find( m_receiveMessageId );
    if ( !entry )
        return NULL;

    Message * message = entry->message;

    assert( message );
    assert( message->GetId() == m_receiveMessageId );

    m_messageReceiveQueue->Remove( m_receiveMessageId );

    m_receiveMessageId++;

    return message;
}

ConnectionPacket * Connection::WritePacket()
{
    if ( m_error != CONNECTION_ERROR_NONE )
        return NULL;

    ConnectionPacket * packet = (ConnectionPacket*) m_packetFactory->CreatePacket( PACKET_CONNECTION );

    if ( !packet )
        return NULL;

    packet->sequence = m_sentPackets->GetSequence();

    GenerateAckBits( *m_receivedPackets, packet->ack, packet->ack_bits );

    InsertAckPacketEntry( packet->sequence );

    int numMessageIds;
    
    uint16_t messageIds[MaxMessagesPerPacket];

    GetMessagesToSend( messageIds, numMessageIds );

    AddMessagePacketEntry( messageIds, numMessageIds, packet->sequence );

    packet->numMessages = numMessageIds;

    for ( int i = 0; i < numMessageIds; ++i )
    {
        MessageSendQueueEntry * entry = m_messageSendQueue->Find( messageIds[i] );
        assert( entry && entry->message );
        packet->messages[i] = entry->message;
        entry->message->AddRef();
    }

    return packet;
}

bool Connection::ReadPacket( ConnectionPacket * packet )
{
    if ( m_error != CONNECTION_ERROR_NONE )
        return false;

    assert( packet );
    assert( packet->GetType() == PACKET_CONNECTION );

    if ( !ProcessPacketMessages( packet ) )
        goto discard;

    if ( !m_receivedPackets->Insert( packet->sequence ) )
        goto discard;

    ProcessAcks( packet->ack, packet->ack_bits );

    return true;

discard:

    return false;            
}

void Connection::AdvanceTime( double time )
{
    m_time = time;
}

ConnectionError Connection::GetError() const
{
    return m_error;
}

void Connection::InsertAckPacketEntry( uint16_t sequence )
{
    SentPacketData * entry = m_sentPackets->Insert( sequence );
    
    assert( entry );

    if ( entry )
    {
        entry->acked = 0;
    }
}

void Connection::ProcessAcks( uint16_t ack, uint32_t ack_bits )
{
    for ( int i = 0; i < 32; ++i )
    {
        if ( ack_bits & 1 )
        {                    
            const uint16_t sequence = ack - i;
            SentPacketData * packetData = m_sentPackets->Find( sequence );
            if ( packetData && !packetData->acked )
            {
                PacketAcked( sequence );
                packetData->acked = 1;
            }
        }
        ack_bits >>= 1;
    }
}

void Connection::PacketAcked( uint16_t sequence )
{
    ProcessMessageAck( sequence );
}

void Connection::GetMessagesToSend( uint16_t * messageIds, int & numMessageIds )
{
    numMessageIds = 0;

    MessageSendQueueEntry * firstEntry = m_messageSendQueue->Find( m_oldestUnackedMessageId );

    if ( !firstEntry )
        return;
    

    const int GiveUpBits = 8 * 8;

    int availableBits = MessagePacketBudget * 8;

    for ( int i = 0; i < MessageSendQueueSize; ++i )
    {
        if ( availableBits <= GiveUpBits )
            break;
        
        const uint16_t messageId = m_oldestUnackedMessageId + i;

        MessageSendQueueEntry * entry = m_messageSendQueue->Find( messageId );
        
        if ( !entry )
            break;

        if ( entry->timeLastSent + MessageResendRate <= m_time && availableBits - entry->measuredBits >= 0 )
        {
            messageIds[numMessageIds++] = messageId;
            entry->timeLastSent = m_time;
            availableBits -= entry->measuredBits;
        }

        if ( numMessageIds == MaxMessagesPerPacket )
            break;
    }
}

void Connection::AddMessagePacketEntry( const uint16_t * messageIds, int & numMessageIds, uint16_t sequence )
{
    MessageSentPacketEntry * sentPacket = m_messageSentPackets->Insert( sequence );
    
    assert( sentPacket );

    sentPacket->acked = 0;
    sentPacket->blockMessage = 0;
    sentPacket->timeSent = m_time;
    const int sentPacketIndex = m_sentPackets->GetIndex( sequence );
    sentPacket->messageIds = &m_sentPacketMessageIds[sentPacketIndex*MaxMessagesPerPacket];
    sentPacket->numMessageIds = numMessageIds;
    for ( int i = 0; i < numMessageIds; ++i )
        sentPacket->messageIds[i] = messageIds[i];
}

bool Connection::ProcessPacketMessages( const ConnectionPacket * packet )
{
    bool earlyMessage = false;

    const uint16_t minMessageId = m_receiveMessageId;
    const uint16_t maxMessageId = m_receiveMessageId + MessageReceiveQueueSize - 1;

    for ( int i = 0; i < (int) packet->numMessages; ++i )
    {
        Message * message = packet->messages[i];

        assert( message );

        const uint16_t messageId = message->GetId();

        if ( sequence_less_than( messageId, minMessageId ) )
            continue;

        if ( sequence_greater_than( messageId, maxMessageId ) )
        {
            earlyMessage = true;
            continue;
        }

        if ( m_messageReceiveQueue->Find( messageId ) )
            continue;

        MessageReceiveQueueEntry * entry = m_messageReceiveQueue->Insert( messageId );

        entry->message = message;

        entry->message->AddRef();
    }

    return !earlyMessage;
}

void Connection::ProcessMessageAck( uint16_t ack )
{
    MessageSentPacketEntry * sentPacketEntry = m_messageSentPackets->Find( ack );

    if ( !sentPacketEntry || sentPacketEntry->acked )
        return;

    for ( int i = 0; i < sentPacketEntry->numMessageIds; ++i )
    {
        const uint16_t messageId = sentPacketEntry->messageIds[i];

        MessageSendQueueEntry * sendQueueEntry = m_messageSendQueue->Find( messageId );
        
        if ( sendQueueEntry )
        {
            assert( sendQueueEntry->message );
            assert( sendQueueEntry->message->GetId() == messageId );

            sendQueueEntry->message->Release();

            m_messageSendQueue->Remove( messageId );
        }
    }

    UpdateOldestUnackedMessageId();
}

void Connection::UpdateOldestUnackedMessageId()
{
    const uint16_t stopMessageId = m_messageSendQueue->GetSequence();

    while ( true )
    {
        if ( m_oldestUnackedMessageId == stopMessageId )
            break;

        MessageSendQueueEntry * entry = m_messageSendQueue->Find( m_oldestUnackedMessageId );
        if ( entry )
            break;
       
        ++m_oldestUnackedMessageId;
    }

    assert( !sequence_greater_than( m_oldestUnackedMessageId, stopMessageId ) );
}

int main()
{
    printf( "\nreliable ordered messages\n\n" );

    // ...

    return 0;
}
