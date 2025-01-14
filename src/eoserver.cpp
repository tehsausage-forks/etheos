
/* $Id$
 * EOSERV is released under the zlib license.
 * See LICENSE.txt for more info.
 */

#include "eoserver.hpp"

#include "config.hpp"
#include "eoclient.hpp"
#include "packet.hpp"
#include "sln.hpp"
#include "timer.hpp"
#include "world.hpp"
#include "handlers/handlers.hpp"

#include "console.hpp"
#include "socket.hpp"
#include "util.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

void server_ping_all(void *server_void)
{
	EOServer *server = static_cast<EOServer *>(server_void);

	UTIL_FOREACH(server->clients, rawclient)
	{
		EOClient *client = static_cast<EOClient *>(rawclient);

		if (client->needpong)
		{
			client->AsyncOpPending(false);
			client->Close();
		}
		else
		{
			client->PingNewSequence();
			auto seq_bytes = client->GetSeqUpdateBytes();

			PacketBuilder builder(PACKET_CONNECTION, PACKET_PLAYER, 3);
			builder.AddShort(seq_bytes.first);
			builder.AddChar(seq_bytes.second);

			client->needpong = true;
			client->Send(builder);
		}
	}
}

void server_pump_queue(void *server_void)
{
	EOServer *server = static_cast<EOServer *>(server_void);
	double now = Timer::GetTime();

	UTIL_FOREACH(server->clients, rawclient)
	{
		EOClient *client = static_cast<EOClient *>(rawclient);

		if (!client->Connected())
			continue;

		std::size_t size = client->queue.queue.size();

		if (size > std::size_t(int(server->world->config["PacketQueueMax"])))
		{
			Console::Wrn("Client was disconnected for filling up the action queue: %s", static_cast<std::string>(client->GetRemoteAddr()).c_str());
			client->AsyncOpPending(false);
			client->Close();
			continue;
		}

		if (size != 0 && client->queue.next <= now)
		{
			std::unique_ptr<ActionQueue_Action> action = std::move(client->queue.queue.front());
			client->queue.queue.pop();

#ifndef DEBUG_EXCEPTIONS
			try
			{
#endif // DEBUG_EXCEPTIONS
				Handlers::Handle(action->reader.Family(), action->reader.Action(), client, action->reader, !action->auto_queue);
#ifndef DEBUG_EXCEPTIONS
			}
			catch (Socket_Exception& e)
			{
				Console::Err("Client caused an exception and was closed: %s.", static_cast<std::string>(client->GetRemoteAddr()).c_str());
				Console::Err("%s: %s", e.what(), e.error());
				client->AsyncOpPending(false);
				client->Close();
			}
			catch (Database_Exception& e)
			{
				Console::Err("Client caused an exception and was closed: %s.", static_cast<std::string>(client->GetRemoteAddr()).c_str());
				Console::Err("%s: %s", e.what(), e.error());
				client->AsyncOpPending(false);
				client->Close();
			}
			catch (std::runtime_error& e)
			{
				Console::Err("Client caused an exception and was closed: %s.", static_cast<std::string>(client->GetRemoteAddr()).c_str());
				Console::Err("Runtime Error: %s", e.what());
				client->AsyncOpPending(false);
				client->Close();
			}
			catch (std::logic_error& e)
			{
				Console::Err("Client caused an exception and was closed: %s.", static_cast<std::string>(client->GetRemoteAddr()).c_str());
				Console::Err("Logic Error: %s", e.what());
				client->AsyncOpPending(false);
				client->Close();
			}
			catch (std::exception& e)
			{
				Console::Err("Client caused an exception and was closed: %s.", static_cast<std::string>(client->GetRemoteAddr()).c_str());
				Console::Err("Uncaught Exception: %s", e.what());
				client->AsyncOpPending(false);
				client->Close();
			}
			catch (...)
			{
				Console::Err("Client caused an exception and was closed: %s.", static_cast<std::string>(client->GetRemoteAddr()).c_str());
				client->AsyncOpPending(false);
				client->Close();
			}
#endif // DEBUG_EXCEPTIONS

			client->queue.next = now + action->time;
		}
	}
}

void EOServer::Initialize(std::shared_ptr<DatabaseFactory> databaseFactory, const Config &eoserv_config, const Config &admin_config)
{
	this->world = new World(databaseFactory, eoserv_config, admin_config);

	TimeEvent *event = new TimeEvent(server_ping_all, this, double(this->world->config["PingRate"]), Timer::FOREVER);
	this->world->timer.Register(event);

	event = new TimeEvent(server_pump_queue, this, 0.001, Timer::FOREVER);
	this->world->timer.Register(event);

	this->world->server = this;

	if (this->world->config["SLN"])
	{
		this->sln = new SLN(this);
	}
	else
	{
		this->sln = 0;
	}

	this->start = Timer::GetTime();
}

Client *EOServer::ClientFactory(const Socket &sock)
{
	 return new EOClient(sock, this);
}

void EOServer::Tick()
{
	std::vector<Client *> *active_clients = 0;
	EOClient *newclient = static_cast<EOClient *>(this->Poll());

	if (newclient)
	{
		int ip_connections = 0;
		bool throttle = false;
		IPAddress remote_addr = newclient->GetRemoteAddr();

		const int reconnect_limit = int(this->world->config["IPReconnectLimit"]);
		const int max_per_ip = int(this->world->config["MaxConnectionsPerIP"]);
		const int log_connection = static_cast<LogConnection>(int(this->world->config["LogConnection"]));

		UTIL_IFOREACH(connection_log, connection)
		{
			if (connection->second + reconnect_limit < Timer::GetTime())
			{
				connection = connection_log.erase(connection);

				if (connection == connection_log.end())
					break;

				continue;
			}

			if (connection->first == remote_addr)
			{
				throttle = true;
			}
		}

		UTIL_FOREACH(this->clients, client)
		{
			if (client->GetRemoteAddr() == newclient->GetRemoteAddr())
			{
				++ip_connections;
			}
		}

		if (throttle)
		{
			Console::Wrn("Connection from %s was rejected (reconnecting too fast)", std::string(newclient->GetRemoteAddr()).c_str());
			newclient->Close(true);
		}
		else if (max_per_ip != 0 && ip_connections > max_per_ip)
		{
			Console::Wrn("Connection from %s was rejected (too many connections from this address)", std::string(remote_addr).c_str());
			newclient->Close(true);
		}
		else if (this->clients.size() >= this->maxconn)
		{
			Console::Wrn("Connection from %s was rejected (too many connections to server)", std::string(remote_addr).c_str());
			newclient->Close(true);
		}
		else if (log_connection == LogConnection::LogAll || (log_connection == LogConnection::FilterPrivate && !remote_addr.IsPrivate()))
		{
			connection_log[remote_addr] = Timer::GetTime();
			Console::Out("New connection from %s (%i/%i connections)", std::string(remote_addr).c_str(), this->Connections(), this->MaxConnections());
		}
	}

	try
	{
		active_clients = this->Select(0.001);
	}
	catch (Socket_SelectFailed &e)
	{
		(void)e;
		if (errno != EINTR)
			throw;
	}

	if (active_clients)
	{
		UTIL_FOREACH(*active_clients, client)
		{
			EOClient *eoclient = static_cast<EOClient *>(client);
			eoclient->Tick();
		}

		active_clients->clear();
	}

	this->BuryTheDead();

	this->world->timer.Tick();
}

EOServer::~EOServer()
{
	// All clients must be fully closed before the world ends
	UTIL_FOREACH(this->clients, client)
	{
		client->AsyncOpPending(false);
		client->Close();
	}

	// Spend up to 2 seconds shutting down
	while (this->clients.size() > 0)
	{
		std::vector<Client *> *active_clients = this->Select(0.1);

		if (active_clients)
		{
			UTIL_FOREACH(*active_clients, client)
			{
				EOClient *eoclient = static_cast<EOClient *>(client);
				eoclient->Tick();
			}

			active_clients->clear();
		}

		this->BuryTheDead();
	}

	delete this->sln;
	delete this->world;

	Close();
}
