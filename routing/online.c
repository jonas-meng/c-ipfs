#include <stdlib.h>

#include "ipfs/routing/routing.h"
#include "libp2p/record/message.h"
#include "libp2p/net/stream.h"
#include "libp2p/conn/session.h"
#include "libp2p/routing/dht_protocol.h"
#include "libp2p/utils/logger.h"

/**
 * Implements the routing interface for communicating with network clients
 */

/**
 * Helper method to send and receive a kademlia message
 * @param session_context the session context
 * @param message what to send
 * @returns what was received
 */
struct Libp2pMessage* ipfs_routing_online_send_receive_message(struct Stream* stream, struct Libp2pMessage* message) {
	size_t protobuf_size = 0, results_size = 0;
	unsigned char* protobuf = NULL, *results = NULL;
	struct Libp2pMessage* return_message = NULL;
	struct SessionContext session_context;

	protobuf_size = libp2p_message_protobuf_encode_size(message);
	protobuf = (unsigned char*)malloc(protobuf_size);
	libp2p_message_protobuf_encode(message, &protobuf[0], protobuf_size, &protobuf_size);

	session_context.default_stream = stream;
	session_context.insecure_stream = stream;

	// upgrade to kademlia protocol
	if (!libp2p_routing_dht_upgrade_stream(&session_context)) {
		goto exit;
	}

	// send the message, and expect the same back
	session_context.default_stream->write(&session_context, protobuf, protobuf_size);
	session_context.default_stream->read(&session_context, &results, &results_size, 5);

	// see if we can unprotobuf
	if (!libp2p_message_protobuf_decode(results, results_size, &return_message))
		goto exit;

	exit:

	if (protobuf != NULL)
		free(protobuf);
	if (results != NULL)
		free(results);

	return return_message;
}

/***
 * Ask the network for anyone that can provide a hash
 * @param routing the context
 * @param key the hash to look for
 * @param key_size the size of the hash
 * @param peers an array of Peer structs that can provide the hash
 * @returns true(1) on success, otherwise false(0)
 */
int ipfs_routing_online_find_remote_providers(struct IpfsRouting* routing, unsigned char* key, size_t key_size, struct Libp2pVector** peers) {
	int found = 0;
	// build the message to be transmitted
	struct Libp2pMessage* message = libp2p_message_new();
	message->message_type = MESSAGE_TYPE_GET_PROVIDERS;
	message->key_size = key_size;
	message->key = malloc(message->key_size);
	memcpy(message->key, key, message->key_size);
	// loop through the connected peers, asking for the hash
	struct Libp2pLinkedList* current_entry = routing->local_node->peerstore->head_entry;
	while (current_entry != NULL) {
		struct Libp2pPeer* peer = ((struct PeerEntry*)current_entry->item)->peer;
		if (peer->connection_type == CONNECTION_TYPE_CONNECTED) {
			// Ask for hash, if it has it, break out of the loop and stop looking
			struct Libp2pMessage* return_message = ipfs_routing_online_send_receive_message(peer->connection, message);
			if (return_message != NULL && return_message->provider_peer_head != NULL) {
				found = 1;
				*peers = libp2p_utils_vector_new(1);
				struct Libp2pLinkedList * current_provider_peer_list_item = return_message->provider_peer_head;
				while (current_provider_peer_list_item != NULL) {
					struct Libp2pPeer *current_peer = current_provider_peer_list_item->item;
					libp2p_utils_vector_add(*peers, libp2p_peer_copy(current_peer));
					current_provider_peer_list_item = current_provider_peer_list_item->next;
				}
				libp2p_message_free(return_message);
				break;
			}
			// TODO: Make this multithreaded
		}
		if (found)
			current_entry = NULL;
		else
			current_entry = current_entry->next;
	}
	// clean up
	libp2p_message_free(message);
	return found;
}

/**
 * Looking for a provider of a hash. This first looks locally, then asks the network
 * @param routing the context
 * @param key the hash to look for
 * @param key_size the size of the hash
 * @param multiaddresses the results
 * @returns true(1) on success, otherwise false(0)
 */
int ipfs_routing_online_find_providers(struct IpfsRouting* routing, unsigned char* key, size_t key_size, struct Libp2pVector** peers) {
	unsigned char* peer_id;
	int peer_id_size;
	struct Libp2pPeer *peer;

	// see if we can find the key, and retrieve the peer who has it
	if (!libp2p_providerstore_get(routing->local_node->providerstore, key, key_size, &peer_id, &peer_id_size)) {
		// we need to look remotely
		return ipfs_routing_online_find_remote_providers(routing, key, key_size, peers);
	}

	// now translate the peer id into a peer to get the multiaddresses
	peer = libp2p_peerstore_get_peer(routing->local_node->peerstore, peer_id, peer_id_size);
	if (peer == NULL)
		return 0;

	*peers = libp2p_utils_vector_new(1);
	libp2p_utils_vector_add(*peers, peer);
	return 1;
}

/***
 * helper method. Connect to a peer and ask it for information
 * about another peer
 */
int ipfs_routing_online_ask_peer_for_peer(struct Libp2pPeer* whoToAsk, const char* peer_id, size_t peer_id_size, struct Libp2pPeer **result) {
	if (whoToAsk->connection_type == CONNECTION_TYPE_CONNECTED) {
		struct Libp2pMessage *message = libp2p_message_new();
		if (message == NULL)
			return 0;
		message->message_type = MESSAGE_TYPE_FIND_NODE;
		message->key_size = peer_id_size;
		message->key = malloc(peer_id_size);
		if (message->key == NULL)
			return 0;
		memcpy(message->key, peer_id, peer_id_size);
		struct Libp2pMessage *return_message = ipfs_routing_online_send_receive_message(whoToAsk->connection, message);
		if (return_message != NULL)
			*result = return_message->provider_peer_head->item;
			return 1;
	}
	return 0;
}

/**
 * Find a peer
 * @param routing the context
 * @param peer_id the id to look for
 * @param peer_id_size the size of the id
 * @param peer the result of the search
 * @returns true(1) on success, otherwise false(0)
 */
int ipfs_routing_online_find_peer(struct IpfsRouting* routing, const char* peer_id, size_t peer_id_size, struct Libp2pPeer **result) {
	// first look to see if we have it in the peerstore
	struct Peerstore* peerstore = routing->local_node->peerstore;
	*result = libp2p_peerstore_get_peer(peerstore, (unsigned char*)peer_id, peer_id_size);
	if (*result != NULL) {
		return 1;
	}
	//ask the swarm to find the peer
	// TODO: Multithread
	struct Libp2pLinkedList *current = peerstore->head_entry;
	while(current != NULL) {
		struct Libp2pPeer *current_peer = ((struct PeerEntry*)current->item)->peer;
		ipfs_routing_online_ask_peer_for_peer(current_peer, peer_id, peer_id_size, result);
		if (*result != NULL)
			return 1;
		current = current->next;
	}
	return 0;
}

/**
 * Notify the network that this host can provide this key
 * @param routing information about this host
 * @param key the key (hash) of the data
 * @param key_size the length of the key
 * @returns true(1) on success, otherwise false
 */
int ipfs_routing_online_provide(struct IpfsRouting* routing, char* key, size_t key_size) {
	struct Libp2pPeer* local_peer = libp2p_peer_new();
	local_peer->id_size = strlen(routing->local_node->identity->peer_id);
	local_peer->id = malloc(local_peer->id_size);
	memcpy(local_peer->id, routing->local_node->identity->peer_id, local_peer->id_size);
	local_peer->connection_type = CONNECTION_TYPE_CONNECTED;
	local_peer->addr_head = libp2p_utils_linked_list_new();
	struct MultiAddress* temp_ma = multiaddress_new_from_string((char*)routing->local_node->repo->config->addresses->swarm_head->item);
	int port = multiaddress_get_ip_port(temp_ma);
	multiaddress_free(temp_ma);
	char str[255];
	sprintf(str, "/ip4/127.0.0.1/tcp/%d/ipfs/%s/", port, routing->local_node->identity->peer_id);
	struct MultiAddress* ma = multiaddress_new_from_string(str);
	libp2p_logger_debug("online", "Adding local MultiAddress %s to peer.\n", ma->string);
	local_peer->addr_head->item = ma;

	struct Libp2pMessage* msg = libp2p_message_new();
	msg->key_size = key_size;
	msg->key = malloc(msg->key_size);
	memcpy(msg->key, key, msg->key_size);
	msg->message_type = MESSAGE_TYPE_ADD_PROVIDER;
	msg->provider_peer_head = libp2p_utils_linked_list_new();
	msg->provider_peer_head->item = local_peer;

	struct Libp2pLinkedList *current = routing->local_node->peerstore->head_entry;
	while (current != NULL) {
		struct PeerEntry* current_peer_entry = (struct PeerEntry*)current->item;
		struct Libp2pPeer* current_peer = current_peer_entry->peer;
		if (current_peer->connection_type == CONNECTION_TYPE_CONNECTED) {
			// ignoring results is okay this time
			struct Libp2pMessage* rslt = ipfs_routing_online_send_receive_message(current_peer->connection, msg);
			if (rslt != NULL)
				libp2p_message_free(rslt);
		}
		current = current->next;
	}

	// this will take care of local_peer too
	libp2p_message_free(msg);

	return 1;
}

/**
 * Ping a remote
 * @param routing the context
 * @param message the message that we want to send
 * @returns true(1) on success, otherwise false(0)
 */
int ipfs_routing_online_ping(struct IpfsRouting* routing, struct Libp2pPeer* peer) {
	int retVal = 0;
	struct Libp2pMessage* msg = NULL, *msg_returned = NULL;

	if (peer->connection_type != CONNECTION_TYPE_CONNECTED) {
		if (!libp2p_peer_connect(peer))
			return 0;
	}
	if (peer->connection_type == CONNECTION_TYPE_CONNECTED) {

		// build the message
		msg = libp2p_message_new();
		msg->message_type = MESSAGE_TYPE_PING;

		msg_returned = ipfs_routing_online_send_receive_message(peer->connection, msg);

		if (msg_returned == NULL)
			goto exit;
		if (msg_returned->message_type != msg->message_type)
			goto exit;
	}

	retVal = 1;
	exit:
	return retVal;
}

/**
 * Connects to swarm
 * @param routing the routing struct
 * @returns 0 on success, otherwise error code
 */
int ipfs_routing_online_bootstrap(struct IpfsRouting* routing) {
	char* peer_id = NULL;
	int peer_id_size = 0;
	struct MultiAddress* address = NULL;
	struct Libp2pPeer *peer = NULL;

	// for each address in our bootstrap list, add info into the peerstore
	struct Libp2pVector* bootstrap_peers = routing->local_node->repo->config->bootstrap_peers;
	for(int i = 0; i < bootstrap_peers->total; i++) {
		address = (struct MultiAddress*)libp2p_utils_vector_get(bootstrap_peers, i);
		// attempt to get the peer ID
		peer_id = multiaddress_get_peer_id(address);
		if (peer_id != NULL) {
			peer_id_size = strlen(peer_id);
			peer = libp2p_peer_new();
			peer->id_size = peer_id_size;
			peer->id = malloc(peer->id_size);
			if (peer->id == NULL) { // out of memory?
				libp2p_peer_free(peer);
				free(peer_id);
				return -1;
			}
			memcpy(peer->id, peer_id, peer->id_size);
			peer->addr_head = libp2p_utils_linked_list_new();
			if (peer->addr_head == NULL) { // out of memory?
				libp2p_peer_free(peer);
				free(peer_id);
				return -1;
			}
			peer->addr_head->item = multiaddress_copy(address);
			libp2p_peerstore_add_peer(routing->local_node->peerstore, peer);
			libp2p_peer_free(peer);
			// now find it and attempt to connect
			peer = libp2p_peerstore_get_peer(routing->local_node->peerstore, (const unsigned char*)peer_id, peer_id_size);
			free(peer_id);
			if (peer == NULL) {
				return -1; // this should never happen
			}
			if (peer->connection == NULL) { // should always be true unless we added it twice (TODO: we should prevent that earlier)
				libp2p_peer_connect(peer);
			}
		}
	}

	return 0;
}

/**
 * Create a new ipfs_routing struct for online clients
 * @param fs_repo the repo
 * @param private_key the local private key
 * @param stream the stream to put in the struct
 * @reurns the ipfs_routing struct that handles messages
 */
ipfs_routing* ipfs_routing_new_online (struct IpfsNode* local_node, struct RsaPrivateKey *private_key, struct Stream* stream) {
    ipfs_routing *onlineRouting = malloc (sizeof(ipfs_routing));

    if (onlineRouting) {
        onlineRouting->local_node     = local_node;
        onlineRouting->sk            = private_key;
        onlineRouting->stream = stream;

        onlineRouting->PutValue      = ipfs_routing_generic_put_value;
        onlineRouting->GetValue      = ipfs_routing_generic_get_value;
        onlineRouting->FindProviders = ipfs_routing_online_find_providers;
        onlineRouting->FindPeer      = ipfs_routing_online_find_peer;
        onlineRouting->Provide       = ipfs_routing_online_provide;
        onlineRouting->Ping          = ipfs_routing_online_ping;
        onlineRouting->Bootstrap     = ipfs_routing_online_bootstrap;
    }

    return onlineRouting;
}

int ipfs_routing_online_free(ipfs_routing* incoming) {
	free(incoming);
	return 1;
}
