/*
    Example source code for "Client/Server Connection"

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

#define PROTOCOL2_IMPLEMENTATION
#define NETWORK2_IMPLEMENTATION

#include "network2.h"
#include "protocol2.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

//const uint32_t ProtocolId = 0x12341651;

const int MaxClients = 32;
const int ServerPort = 50000;
const int ClientPort = 60000;
const int ChallengeHashSize = 1031;                 // keep this prime
const float ChallengeSendRate = 0.1f;
const float ChallengeTimeOut = 10.0f;
/*
const float ConnectionTimeOut = 5.0f;
const float KeepAliveRate = 1.0f;
*/

uint64_t GenerateSalt()
{
    return ( ( uint64_t( rand() ) <<  0 ) & 0x000000000000FFFFull ) | 
           ( ( uint64_t( rand() ) << 16 ) & 0x00000000FFFF0000ull ) | 
           ( ( uint64_t( rand() ) << 32 ) & 0x0000FFFF00000000ull ) |
           ( ( uint64_t( rand() ) << 48 ) & 0xFFFF000000000000ull );
}

enum PacketTypes
{
    PACKET_CONNECTION_REQUEST,                      // client requests a connection.
    PACKET_CONNECTION_DENIED,                       // server denies client connection request.
    PACKET_CONNECTION_CHALLENGE,                    // server response to client connection request.
    PACKET_CONNECTION_RESPONSE,                     // client response to server connection challenge.
    PACKET_CONNECTION_KEEP_ALIVE,                   // keep alive packet sent at some low rate (once per-second) to keep the connection alive
    PACKET_CONNECTION_DISCONNECTED,                 // courtesy packet to indicate that the client has been disconnected. better than a timeout
    NUM_CLIENT_SERVER_NUM_PACKETS
};

struct ConnectionRequestPacket : public protocol2::Packet
{
    uint64_t client_salt;
    uint8_t data[256];

    ConnectionRequestPacket() : Packet( PACKET_CONNECTION_REQUEST )
    {
        client_salt = 0;
        memset( data, 0, sizeof( data ) );
    }

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_uint64( stream, client_salt );
        if ( Stream::IsReading && stream.GetBitsRemaining() < 256 * 8 )
            return false;
        serialize_bytes( stream, data, 256 );
        return true;
    }

    PROTOCOL2_DECLARE_VIRTUAL_SERIALIZE_FUNCTIONS();
};

enum ConnectionDeniedReason
{
    CONNECTION_DENIED_SERVER_FULL,
    CONNECTION_DENIED_ALREADY_CONNECTED,
    CONNECTION_DENIED_NUM_VALUES
};

struct ConnectionDeniedPacket : public protocol2::Packet
{
    uint64_t client_salt;
    ConnectionDeniedReason reason;

    ConnectionDeniedPacket() : Packet( PACKET_CONNECTION_DENIED )
    {
        client_salt = 0;
        reason = CONNECTION_DENIED_NUM_VALUES;
    }

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_uint64( stream, client_salt );
        serialize_enum( stream, reason, ConnectionDeniedReason, CONNECTION_DENIED_NUM_VALUES );
        return true;
    }

    PROTOCOL2_DECLARE_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct ServerChallengeEntry
{
    uint64_t client_salt;                           // random number generated by client and sent to server in connection request
    uint64_t server_salt;                           // random number generated by server and sent back to client in challenge packet
    double create_time;                             // time this challenge entry was created. used for challenge timeout
    double last_packet_send_time;                   // the last time we sent a challenge packet to this client
    network2::Address address;                      // address the connection request came from
};

struct ServerChallengeHash
{
    int num_entries;
    uint8_t exists[ChallengeHashSize];
    ServerChallengeEntry entries[ChallengeHashSize];

    ServerChallengeHash() { memset( this, 0, sizeof( ServerChallengeHash ) ); }
};

uint64_t CalculateChallengeHashKey( const network2::Address & address, uint64_t clientSalt )
{
    char buffer[256];
    const char * addressString = address.ToString( buffer, sizeof( buffer ) );
    const int addressLength = strlen( addressString );
    return protocol2::murmur_hash_64( &clientSalt, 8, protocol2::murmur_hash_64( addressString, addressLength, 0 ) );
}

struct Server
{
    int m_numConnectedClients;                                          // number of connected clients
    
    bool m_clientConnected[MaxClients];                                 // true if client n is connected
    
    uint64_t m_clientSalt[MaxClients];                                  // array of client salt values per-client
    
    uint64_t m_serverSalt[MaxClients];                                  // array of server salt values per-client
    
    network2::Address m_clientAddress[MaxClients];                      // array of client address values per-client
    
    double m_clientLastPacketReceiveTime[MaxClients];                   // last time a packet was received from a client (used for timeouts)

    ServerChallengeHash m_challengeHash;                                // challenge hash entries. stores client challenge/response data

protected:

    void ResetClientState( int clientIndex )
    {
        assert( clientIndex >= 0 );
        assert( clientIndex < MaxClients );
        m_clientConnected[clientIndex] = false;
        m_clientSalt[clientIndex] = 0;
        m_serverSalt[clientIndex] = 0;
        m_clientAddress[clientIndex] = network2::Address();
        m_clientLastPacketReceiveTime[clientIndex] = -1000.0;            // IMPORTANT: avoid bad behavior near t=0.0
    }

    void AddClient( int clientIndex, const network2::Address & address, uint64_t clientSalt, uint64_t serverSalt )
    {
        assert( m_numConnectedClients >= 0 );
        assert( m_numConnectedClients < MaxClients - 1 );
        assert( !m_clientConnected[clientIndex] );
        m_numConnectedClients++;
        m_clientConnected[clientIndex] = true;
        m_clientSalt[clientIndex] = clientSalt;
        m_serverSalt[clientIndex] = serverSalt;
    }

    bool IsConnected( const network2::Address & address ) const
    {
        for ( int i = 0; i < MaxClients; ++i )
        {
            if ( !m_clientConnected[i] )
                continue;
            if ( m_clientAddress[i] == address )
                return true;
        }
        return false;
    }

    ServerChallengeEntry * FindOrInsertChallenge( const network2::Address & address, uint64_t clientSalt, double time, int & index )
    {
        if ( m_challengeHash.num_entries >= ChallengeHashSize / 4 )         // be really conservative. we don't want any clustering
            return NULL;

        const uint64_t key = CalculateChallengeHashKey( address, clientSalt );

        index = key % ChallengeHashSize;
        
        printf( "client salt = %llx\n", clientSalt );
        printf( "challenge hash key = %llx\n", key );
        printf( "challenge hash index = %d\n", index );

        if ( !m_challengeHash.exists[index] || ( m_challengeHash.exists[index] && m_challengeHash.entries[index].create_time + ChallengeTimeOut < time ) )
        {
            printf( "found empty entry in challenge hash at index %d\n", index );

            ServerChallengeEntry * entry = &m_challengeHash.entries[index];

            entry->client_salt = clientSalt;
            entry->server_salt = GenerateSalt();
            entry->last_packet_send_time = time - ChallengeSendRate * 2;
            entry->create_time = time;
            entry->address = address;

            m_challengeHash.exists[index] = 1;

            return entry;
        }

        if ( m_challengeHash.exists[index] && 
             m_challengeHash.entries[index].client_salt == clientSalt && 
             m_challengeHash.entries[index].address == address )
        {
            printf( "found existing challenge hash entry at index %d\n", index );

            return &m_challengeHash.entries[index];
        }

        return NULL;
    }

public:

    Server()
    {
        m_numConnectedClients = 0;
        for ( int i = 0; i < MaxClients; ++i )
            ResetClientState( i );
    }

    void ProcessConnectionRequest( const ConnectionRequestPacket & packet, const network2::Address & address, double time )
    {
        char buffer[256];
        const char *addressString = address.ToString( buffer, sizeof( buffer ) );        
        printf( "processing connection request packet from: %s\n", addressString );

        if ( m_numConnectedClients == MaxClients )
        {
            printf( "denied: server is full\n" );
            // todo: respond with connection denied packet (reason: full)
            return;
        }

        if ( IsConnected( address ) )
        {
            printf( "denied: already connected\n" );
            // todo: respond with connection denied packet (reason: already connected)
            return;
        }

        int index;
        ServerChallengeEntry * entry = FindOrInsertChallenge( address, packet.client_salt, time, index );
        if ( !entry )
            return;

        assert( entry );
        assert( entry->address == address );
        assert( entry->client_salt == packet.client_salt );

        if ( entry->last_packet_send_time + ChallengeSendRate < time )
        {
            printf( "sending connection challenge to %s (server salt = %llx)\n", addressString, entry->server_salt );
            // todo: send connection challenge packet
            entry->last_packet_send_time = time;
        }
    }
};

enum ClientState
{
    CLIENT_STATE_DISCONNECTED,
    CLIENT_STATE_SENDING_CONNECTION_REQUEST,
    CLIENT_STATE_SENDING_CHALLENGE_RESPONSE,
    CLIENT_STATE_CONNECTED,
};

struct Client
{
    network2::Address server_address;
    uint64_t server_guid;
    uint64_t client_guid;
};

int main()
{
    srand( (unsigned int) time( NULL ) );

    printf( "client/server connection\n" );

    network2::InitializeNetwork();

    network2::Address clientAddress( "::1", ClientPort );
    network2::Address serverAddress( "::1", ServerPort );

    network2::Socket clientSocket( ClientPort );
    network2::Socket serverSocket( ServerPort );

    if ( clientSocket.GetError() != network2::SOCKET_ERROR_NONE || serverSocket.GetError() != network2::SOCKET_ERROR_NONE )
        return 1;
    
    const int NumIterations = 30;

    double time = 0.0;

    const uint64_t client_salt = GenerateSalt();

    Server server;

    printf( "----------------------------------------------------------\n" );

    for ( int i = 0; i < NumIterations; ++i )
    {
        printf( "t = %f\n", time );

        if ( i <= 2 )
        {
            ConnectionRequestPacket packet;
            packet.client_salt = client_salt;

            server.ProcessConnectionRequest( packet, clientAddress, time );
        }

        char send_data[256];
        memset( send_data, 0, sizeof( send_data ) );
        clientSocket.SendPacket( serverAddress, send_data, sizeof( send_data ) );

        network2::Address from;
        char recv_data[256];
        while ( int read_bytes = serverSocket.ReceivePacket( from, recv_data, sizeof( recv_data ) ) )
        {
            printf( "received packet: %d bytes\n", read_bytes );
        }

        time += 0.1f;

        printf( "----------------------------------------------------------\n" );
    }

    network2::ShutdownNetwork();

    return 0;
}
