/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2016 - Daniel De Matteis
 *  Copyright (C)      2016 - Gregor Richards
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#if defined(_MSC_VER) && !defined(_XBOX)
#pragma comment(lib, "ws2_32")
#endif

#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <retro_assert.h>
#include <net/net_compat.h>
#include <net/net_socket.h>
#include <features/features_cpu.h>
#include <retro_endianness.h>

#include "netplay_private.h"
#include "netplay_discovery.h"

#include "../../autosave.h"
#include "../../configuration.h"
#include "../../command.h"
#include "../../movie.h"
#include "../../runloop.h"

#if defined(AF_INET6) && !defined(HAVE_SOCKET_LEGACY)
#define HAVE_INET6 1
#endif

static int init_tcp_connection(const struct addrinfo *res,
      bool server,
      struct sockaddr *other_addr, socklen_t addr_size)
{
   bool ret = true;
   int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

   if (fd < 0)
   {
      ret = false;
      goto end;
   }

#if defined(IPPROTO_TCP) && defined(TCP_NODELAY)
   {
      int flag = 1;
      if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
#ifdef _WIN32
         (const char*)
#else
         (const void*)
#endif
         &flag,
         sizeof(int)) < 0)
         RARCH_WARN("Could not set netplay TCP socket to nodelay. Expect jitter.\n");
   }
#endif

#if defined(F_SETFD) && defined(FD_CLOEXEC)
   /* Don't let any inherited processes keep open our port */
   if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
      RARCH_WARN("Cannot set Netplay port to close-on-exec. It may fail to reopen if the client disconnects.\n");
#endif

   if (server)
   {
      if (socket_connect(fd, (void*)res, false) < 0)
      {
         ret = false;
         goto end;
      }
   }
   else
   {
#if defined(HAVE_INET6) && defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY)
      /* Make sure we accept connections on both IPv6 and IPv4 */
      int on = 0;
      if (res->ai_family == AF_INET6)
      {
         if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&on, sizeof(on)) < 0)
            RARCH_WARN("Failed to listen on both IPv6 and IPv4\n");
      }
#endif
      if (  !socket_bind(fd, (void*)res) || 
            listen(fd, 1024) < 0)
      {
         ret = false;
         goto end;
      }
   }

end:
   if (!ret && fd >= 0)
   {
      socket_close(fd);
      fd = -1;
   }

   return fd;
}

static bool init_tcp_socket(netplay_t *netplay, void *direct_host,
      const char *server, uint16_t port)
{
   char port_buf[16];
   bool ret                        = false;
   const struct addrinfo *tmp_info = NULL;
   struct addrinfo *res            = NULL;
   struct addrinfo hints           = {0};

   port_buf[0] = '\0';

   if (!direct_host)
   {
#ifdef HAVE_INET6
      /* Default to hosting on IPv6 and IPv4 */
      if (!server)
         hints.ai_family = AF_INET6;
#endif
      hints.ai_socktype = SOCK_STREAM;
      if (!server)
         hints.ai_flags = AI_PASSIVE;

      snprintf(port_buf, sizeof(port_buf), "%hu", (unsigned short)port);
      if (getaddrinfo_retro(server, port_buf, &hints, &res) < 0)
      {
#ifdef HAVE_INET6
         if (!server)
         {
            /* Didn't work with IPv6, try wildcard */
            hints.ai_family = 0;
            if (getaddrinfo_retro(server, port_buf, &hints, &res) < 0)
               return false;
         }
         else
#endif
         return false;
      }

      if (!res)
         return false;

   }
   else
   {
      /* I'll build my own addrinfo! With blackjack and hookers! */
      struct netplay_host *host = (struct netplay_host *) direct_host;
      hints.ai_family = host->addr.sa_family;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_protocol = 0;
      hints.ai_addrlen = host->addrlen;
      hints.ai_addr = &host->addr;
      res = &hints;

   }

   /* If we're serving on IPv6, make sure we accept all connections, including
    * IPv4 */
#ifdef HAVE_INET6
   if (!direct_host && !server && res->ai_family == AF_INET6)
   {
      struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) res->ai_addr;
      sin6->sin6_addr = in6addr_any;
   }
#endif

   /* If "localhost" is used, it is important to check every possible 
    * address for IPv4/IPv6. */
   tmp_info = res;

   while (tmp_info)
   {
      struct sockaddr_storage sad;
      int fd = init_tcp_connection(
            tmp_info,
            direct_host || server,
            (struct sockaddr*)&sad,
            sizeof(sad));

      if (fd >= 0)
      {
         ret = true;
         if (direct_host || server)
         {
            netplay->connections[0].active = true;
            netplay->connections[0].fd = fd;
            netplay->connections[0].addr = sad;
         }
         else
         {
            netplay->listen_fd = fd;
         }
         break;
      }

      tmp_info = tmp_info->ai_next;
   }

   if (res && !direct_host)
      freeaddrinfo_retro(res);

   if (!ret)
      RARCH_ERR("Failed to set up netplay sockets.\n");

   return ret;
}

static bool init_socket(netplay_t *netplay, void *direct_host, const char *server, uint16_t port)
{
   if (!network_init())
      return false;

   if (!init_tcp_socket(netplay, direct_host, server, port))
      return false;

   if (netplay->is_server && netplay->nat_traversal)
      netplay_init_nat_traversal(netplay);

   return true;
}

/**
 * netplay_update_unread_ptr
 *
 * Update the global unread_ptr and unread_frame_count to correspond to the
 * earliest unread frame count of any connected player */
void netplay_update_unread_ptr(netplay_t *netplay)
{
   if (netplay->is_server && !netplay->connected_players)
   {
      /* Nothing at all to read! */
      netplay->unread_ptr = netplay->self_ptr;
      netplay->unread_frame_count = netplay->self_frame_count;

   }
   else
   {
      size_t new_unread_ptr = 0;
      uint32_t new_unread_frame_count = (uint32_t) -1;
      uint32_t player;

      for (player = 0; player < MAX_USERS; player++)
      {
         if (!(netplay->connected_players & (1<<player))) continue;
         if (netplay->read_frame_count[player] < new_unread_frame_count)
         {
            new_unread_ptr = netplay->read_ptr[player];
            new_unread_frame_count = netplay->read_frame_count[player];
         }
      }

      if (!netplay->is_server && netplay->server_frame_count < new_unread_frame_count)
      {
         new_unread_ptr = netplay->server_ptr;
         new_unread_frame_count = netplay->server_frame_count;
      }

      netplay->unread_ptr = new_unread_ptr;
      netplay->unread_frame_count = new_unread_frame_count;
   }
}

/**
 * netplay_simulate_input:
 * @netplay             : pointer to netplay object
 * @sim_ptr             : frame index for which to simulate input
 * @resim               : are we resimulating, or simulating this frame for the
 *                        first time?
 *
 * "Simulate" input by assuming it hasn't changed since the last read input.
 */
void netplay_simulate_input(netplay_t *netplay, size_t sim_ptr, bool resim)
{
   uint32_t player;
   size_t prev;
   struct delta_frame *simframe, *pframe;

   simframe = &netplay->buffer[sim_ptr];

   for (player = 0; player < MAX_USERS; player++)
   {
      if (!(netplay->connected_players & (1<<player))) continue;
      if (simframe->have_real[player]) continue;

      prev = PREV_PTR(netplay->read_ptr[player]);
      pframe = &netplay->buffer[prev];

      if (resim)
      {
         /* In resimulation mode, we only copy the buttons. The reason for this
          * is nonobvious:
          *
          * If we resimulated nothing, then the /duration/ with which any input
          * was pressed would be approximately correct, since the original
          * simulation came in as the input came in, but the /number of times/
          * the input was pressed would be wrong, as there would be an
          * advancing wavefront of real data overtaking the simulated data
          * (which is really just real data offset by some frames).
          *
          * That's acceptable for arrows in most situations, since the amount
          * you move is tied to the duration, but unacceptable for buttons,
          * which will seem to jerkily be pressed numerous times with those
          * wavefronts.
          */
         const uint32_t keep = (1U<<RETRO_DEVICE_ID_JOYPAD_UP) |
                               (1U<<RETRO_DEVICE_ID_JOYPAD_DOWN) |
                               (1U<<RETRO_DEVICE_ID_JOYPAD_LEFT) |
                               (1U<<RETRO_DEVICE_ID_JOYPAD_RIGHT);
         uint32_t sim_state = simframe->simulated_input_state[player][0] & keep;
         sim_state |= pframe->real_input_state[player][0] & ~keep;
         simframe->simulated_input_state[player][0] = sim_state;
      }
      else
      {
         memcpy(simframe->simulated_input_state[player],
                pframe->real_input_state[player],
                WORDS_PER_INPUT * sizeof(uint32_t));
      }
   }
}

#ifndef HAVE_SOCKET_LEGACY
/* Custom inet_ntop. Win32 doesn't seem to support this ... */
void netplay_log_connection(const struct sockaddr_storage *their_addr,
      unsigned slot, const char *nick)
{
   union
   {
      const struct sockaddr_storage *storage;
      const struct sockaddr_in *v4;
      const struct sockaddr_in6 *v6;
   } u;
   const char *str               = NULL;
   char buf_v4[INET_ADDRSTRLEN]  = {0};
   char buf_v6[INET6_ADDRSTRLEN] = {0};
   char msg[512];

   msg[0] = '\0';

   u.storage = their_addr;

   switch (their_addr->ss_family)
   {
      case AF_INET:
         {
            struct sockaddr_in in;

            memset(&in, 0, sizeof(in));

            str           = buf_v4;
            in.sin_family = AF_INET;
            memcpy(&in.sin_addr, &u.v4->sin_addr, sizeof(struct in_addr));

            getnameinfo((struct sockaddr*)&in, sizeof(struct sockaddr_in),
                  buf_v4, sizeof(buf_v4),
                  NULL, 0, NI_NUMERICHOST);
         }
         break;
      case AF_INET6:
         {
            struct sockaddr_in6 in;
            memset(&in, 0, sizeof(in));

            str            = buf_v6;
            in.sin6_family = AF_INET6;
            memcpy(&in.sin6_addr, &u.v6->sin6_addr, sizeof(struct in6_addr));

            getnameinfo((struct sockaddr*)&in, sizeof(struct sockaddr_in6),
                  buf_v6, sizeof(buf_v6), NULL, 0, NI_NUMERICHOST);
         }
         break;
      default:
         break;
   }

   if (str)
   {
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_GOT_CONNECTION_FROM_NAME),
            nick, str);
      runloop_msg_queue_push(msg, 1, 180, false);
      RARCH_LOG("%s\n", msg);
   }
   else
   {
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_GOT_CONNECTION_FROM),
            nick);
      runloop_msg_queue_push(msg, 1, 180, false);
      RARCH_LOG("%s\n", msg);
   }
   RARCH_LOG("%s %u\n", msg_hash_to_str(MSG_CONNECTION_SLOT),
         slot);
}

#else
void netplay_log_connection(const struct sockaddr_storage *their_addr,
      unsigned slot, const char *nick)
{
   char msg[512];

   msg[0] = '\0';

   snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_GOT_CONNECTION_FROM),
         nick);
   runloop_msg_queue_push(msg, 1, 180, false);
   RARCH_LOG("%s\n", msg);
   RARCH_LOG("%s %u\n",
         msg_hash_to_str(MSG_CONNECTION_SLOT), slot);
}

#endif

static bool netplay_init_socket_buffers(netplay_t *netplay)
{
   /* Make our packet buffer big enough for a save state and frames-many frames
    * of input data, plus the headers for each of them */
   size_t i;
   size_t packet_buffer_size = netplay->zbuffer_size +
      netplay->delay_frames * WORDS_PER_FRAME + (netplay->delay_frames+1)*3;
   netplay->packet_buffer_size = packet_buffer_size;

   for (i = 0; i < netplay->connections_size; i++)
   {
      struct netplay_connection *connection = &netplay->connections[i];
      if (connection->active)
      {
         if (connection->send_packet_buffer.data)
         {
            if (!netplay_resize_socket_buffer(&connection->send_packet_buffer,
                  packet_buffer_size) ||
                !netplay_resize_socket_buffer(&connection->recv_packet_buffer,
                  packet_buffer_size))
               return false;
         }
         else
         {
            if (!netplay_init_socket_buffer(&connection->send_packet_buffer,
                  packet_buffer_size) ||
                !netplay_init_socket_buffer(&connection->recv_packet_buffer,
                  packet_buffer_size))
               return false;
         }
      }
   }

   return true;
}

/**
 * netplay_try_init_serialization
 *
 * Try to initialize serialization. For quirky cores.
 *
 * Returns true if serialization is now ready, false otherwise.
 */
bool netplay_try_init_serialization(netplay_t *netplay)
{
   retro_ctx_serialize_info_t serial_info;
   size_t packet_buffer_size;

   if (netplay->state_size)
      return true;

   if (!netplay_init_serialization(netplay))
      return false;

   /* Check if we can actually save */
   serial_info.data_const = NULL;
   serial_info.data = netplay->buffer[netplay->self_ptr].state;
   serial_info.size = netplay->state_size;

   if (!core_serialize(&serial_info))
      return false;

   /* Once initialized, we no longer exhibit this quirk */
   netplay->quirks &= ~((uint64_t) NETPLAY_QUIRK_INITIALIZATION);

   return netplay_init_socket_buffers(netplay);
}

bool netplay_wait_and_init_serialization(netplay_t *netplay)
{
   int frame;

   if (netplay->state_size)
      return true;

   /* Wait a maximum of 60 frames */
   for (frame = 0; frame < 60; frame++) {
      if (netplay_try_init_serialization(netplay))
         return true;

#if defined(HAVE_THREADS)
      autosave_lock();
#endif
      core_run();
#if defined(HAVE_THREADS)
      autosave_unlock();
#endif
   }

   return false;
}

bool netplay_init_serialization(netplay_t *netplay)
{
   unsigned i;
   retro_ctx_size_info_t info;

   if (netplay->state_size)
      return true;

   core_serialize_size(&info);

   if (!info.size)
      return false;

   netplay->state_size = info.size;

   for (i = 0; i < netplay->buffer_size; i++)
   {
      netplay->buffer[i].state = calloc(netplay->state_size, 1);

      if (!netplay->buffer[i].state)
      {
         netplay->quirks |= NETPLAY_QUIRK_NO_SAVESTATES;
         return false;
      }
   }

   netplay->zbuffer_size = netplay->state_size * 2;
   netplay->zbuffer = (uint8_t *) calloc(netplay->zbuffer_size, 1);
   if (!netplay->zbuffer)
   {
      netplay->quirks |= NETPLAY_QUIRK_NO_TRANSMISSION;
      netplay->zbuffer_size = 0;
      return false;
   }

   return true;
}

static bool netplay_init_buffers(netplay_t *netplay, unsigned frames)
{
   size_t packet_buffer_size;

   if (!netplay)
      return false;

   /* * 2 + 1 because:
    * Self sits in the middle,
    * Other is allowed to drift as much as 'frames' frames behind
    * Read is allowed to drift as much as 'frames' frames ahead */
   netplay->buffer_size = frames * 2 + 1;

   netplay->buffer = (struct delta_frame*)calloc(netplay->buffer_size,
         sizeof(*netplay->buffer));

   if (!netplay->buffer)
      return false;

   if (!(netplay->quirks & (NETPLAY_QUIRK_NO_SAVESTATES|NETPLAY_QUIRK_INITIALIZATION)))
      netplay_init_serialization(netplay);

   return netplay_init_socket_buffers(netplay);
}

/**
 * netplay_new:
 * @direct_host          : Netplay host discovered from scanning.
 * @server               : IP address of server.
 * @port                 : Port of server.
 * @password             : Password required to connect.
 * @delay_frames         : Amount of delay frames.
 * @check_frames         : Frequency with which to check CRCs.
 * @cb                   : Libretro callbacks.
 * @nat_traversal        : If true, attempt NAT traversal.
 * @nick                 : Nickname of user.
 * @quirks               : Netplay quirks required for this session.
 *
 * Creates a new netplay handle. A NULL host means we're 
 * hosting (user 1).
 *
 * Returns: new netplay handle.
 **/
netplay_t *netplay_new(void *direct_host, const char *server, uint16_t port,
   const char *password, unsigned delay_frames, unsigned check_frames,
   const struct retro_callbacks *cb, bool nat_traversal, const char *nick,
   uint64_t quirks)
{
   netplay_t *netplay = (netplay_t*)calloc(1, sizeof(*netplay));
   if (!netplay)
      return NULL;

   netplay->listen_fd         = -1;
   netplay->tcp_port          = port;
   netplay->cbs               = *cb;
   netplay->connected_players = 0;
   netplay->is_server         = server == NULL;
   netplay->nat_traversal     = netplay->is_server ? nat_traversal : false;
   netplay->delay_frames      = delay_frames;
   netplay->check_frames      = check_frames;
   netplay->quirks            = quirks;
   netplay->self_mode         = netplay->is_server ?
                                NETPLAY_CONNECTION_PLAYING :
                                NETPLAY_CONNECTION_NONE;

   if (netplay->is_server)
   {
      netplay->connections = NULL;
      netplay->connections_size = 0;
   }
   else
   {
      netplay->connections = &netplay->one_connection;
      netplay->connections_size = 1;
      netplay->connections[0].fd = -1;
   }

   strlcpy(netplay->nick, nick[0] ? nick : RARCH_DEFAULT_NICK, sizeof(netplay->nick));
   strlcpy(netplay->password, password ? password : "", sizeof(netplay->password));

   if (!init_socket(netplay, direct_host, server, port))
   {
      free(netplay);
      return NULL;
   }

   if (!netplay_init_buffers(netplay, delay_frames))
   {
      free(netplay);
      return NULL;
   }

   if (!netplay->is_server)
   {
      netplay_handshake_init_send(netplay, &netplay->connections[0]);
      netplay->connections[0].mode = netplay->self_mode = NETPLAY_CONNECTION_INIT;
   }

   /* FIXME: Not really the right place to do this, socket initialization needs
    * to be fixed in general */
   if (netplay->is_server)
   {
      if (!socket_nonblock(netplay->listen_fd))
         goto error;
   }
   else
   {
      if (!socket_nonblock(netplay->connections[0].fd))
         goto error;
   }

   return netplay;

error:
   if (netplay->listen_fd >= 0)
      socket_close(netplay->listen_fd);

   if (netplay->connections && netplay->connections[0].fd >= 0)
      socket_close(netplay->connections[0].fd);

   free(netplay);
   return NULL;
}

/**
 * netplay_free:
 * @netplay              : pointer to netplay object
 *
 * Frees netplay handle.
 **/
void netplay_free(netplay_t *netplay)
{
   size_t i;

   if (netplay->listen_fd >= 0)
      socket_close(netplay->listen_fd);

   for (i = 0; i < netplay->connections_size; i++)
   {
      struct netplay_connection *connection = &netplay->connections[i];
      if (connection->active)
      {
         socket_close(connection->fd);
         netplay_deinit_socket_buffer(&connection->send_packet_buffer);
         netplay_deinit_socket_buffer(&connection->recv_packet_buffer);
      }
   }

   if (netplay->connections && netplay->connections != &netplay->one_connection)
      free(netplay->connections);

   if (netplay->nat_traversal)
      natt_free(&netplay->nat_traversal_state);

   if (netplay->buffer)
   {
      for (i = 0; i < netplay->buffer_size; i++)
         if (netplay->buffer[i].state)
            free(netplay->buffer[i].state);

      free(netplay->buffer);
   }

   if (netplay->zbuffer)
      free(netplay->zbuffer);

   if (netplay->compression_stream)
      netplay->compression_backend->stream_free(netplay->compression_stream);

   if (netplay->addr)
      freeaddrinfo_retro(netplay->addr);

   free(netplay);
}
