/*
 Ten program używa poll(), aby równocześnie obsługiwać wielu klientów
 bez tworzenia procesów ani wątków.
*/

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "err.h"

#define BUF_SIZE 1024
#define QUEUE_LENGTH 5
#define TIMEOUT 5000

// 0
//   -> main socket
// 1 .. CONNECTIONS - 1
//   -> client sockets
// CONNECTIONS
//   -> main control socket
// CONNECTIONS + 1 .. ALL_CONNECTIONS - 1
//   -> control sockets
#define CONNECTIONS 3
#define ALL_CONNECTIONS 6

static bool finish = false;

/* Obsługa sygnału kończenia */
static void catch_int(int sig) {
  finish = true;
  fprintf(stderr, "Signal %d catched. No new connections will be accepted.\n",
          sig);
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fatal("Usage: %s client_port control_port \n", argv[0]);
  }

  uint16_t port = atoi(argv[1]);
  uint16_t control_port = atoi(argv[2]);

  char buf[BUF_SIZE];

  install_signal_handler(SIGINT, catch_int, SA_RESTART);

  struct pollfd poll_descriptors[ALL_CONNECTIONS];
  /* Inicjujemy tablicę z gniazdkami klientów, poll_descriptors[0] to gniazdko
   * centrali */
  for (int i = 0; i < ALL_CONNECTIONS; ++i) {
    poll_descriptors[i].fd = -1;
    poll_descriptors[i].events = POLLIN;
    poll_descriptors[i].revents = 0;
  }
  size_t active_clients = 0;
  size_t active_ctrl_clients = 0;

  /* Tworzymy gniazdko centrali */
  poll_descriptors[0].fd = open_socket();
  poll_descriptors[CONNECTIONS].fd = open_socket();

  bind_socket(poll_descriptors[0].fd, port);
  printf("Listening on port %u\n", port);
  bind_socket(poll_descriptors[CONNECTIONS].fd, control_port);
  printf("Listening for commands on port %u\n", control_port);

  start_listening(poll_descriptors[0].fd, QUEUE_LENGTH);
  start_listening(poll_descriptors[CONNECTIONS].fd, QUEUE_LENGTH);

  do {
    for (int i = 0; i < ALL_CONNECTIONS; ++i) {
      poll_descriptors[i].revents = 0;
    }

    /* Po Ctrl-C zamykamy gniazdko centrali */
    if (finish && poll_descriptors[0].fd >= 0) {
      CHECK_ERRNO(close(poll_descriptors[0].fd));
      poll_descriptors[0].fd = -1;
      CHECK_ERRNO(close(poll_descriptors[CONNECTIONS].fd));
      poll_descriptors[CONNECTIONS].fd = -1;
    }

    int poll_status = poll(poll_descriptors, ALL_CONNECTIONS, TIMEOUT);
    if (poll_status == -1) {
      if (errno == EINTR)
        fprintf(stderr, "Interrupted system call\n");
      else
        PRINT_ERRNO();
    } else if (poll_status > 0) {
      for (size_t accepting = 0, i = 0; i < 2;
           (accepting += CONNECTIONS), (++i)) {
        if (!finish && (poll_descriptors[accepting].revents & POLLIN)) {
          /* Przyjmuję nowe połączenie */
          int client_fd =
              accept_connection(poll_descriptors[accepting].fd, NULL);
          bool accepted = false;
          for (int i = accepting + 1;
               i < (accepting == 0 ? CONNECTIONS : ALL_CONNECTIONS); ++i) {
            if (poll_descriptors[i].fd == -1) {
              fprintf(stderr, "Received new connection (%d)\n", i);
              if (i > CONNECTIONS) {
                fprintf(stderr, "  (This is a control connection.)\n");
              }

              poll_descriptors[i].fd = client_fd;
              poll_descriptors[i].events = POLLIN;
              if (accepting == 0)
                active_clients++;
              else
                active_ctrl_clients++;
              accepted = true;
              break;
            }
          }
          if (!accepted) {
            CHECK_ERRNO(close(client_fd));
            fprintf(stderr, "Too many clients\n");
          }
        }
      }
      for (int i = 1; i < ALL_CONNECTIONS; ++i) {
        if (i == 0 || i == CONNECTIONS) {
          continue;
        }
        if (poll_descriptors[i].fd != -1 &&
            (poll_descriptors[i].revents & (POLLIN | POLLERR))) {
          ssize_t received_bytes = read(poll_descriptors[i].fd, buf, BUF_SIZE);
          if (received_bytes < 0) {
            fprintf(stderr,
                    "Error when reading message from connection %d (errno %d, "
                    "%s)\n",
                    i, errno, strerror(errno));
            CHECK_ERRNO(close(poll_descriptors[i].fd));
            poll_descriptors[i].fd = -1;
            if (i < CONNECTIONS) active_clients -= 1;
          } else if (received_bytes == 0) {
            fprintf(stderr, "Ending connection (%d)\n", i);
            CHECK_ERRNO(close(poll_descriptors[i].fd));
            poll_descriptors[i].fd = -1;
            if (i < CONNECTIONS)
              active_clients -= 1;
            else
              active_ctrl_clients -= 1;
          } else {
            if (i > CONNECTIONS) {
              if (buf[0] == 'c') {
                memset(buf, 0, BUF_SIZE);
                sprintf(buf,
                        "Number of active clients: %ld\nTotal number of "
                        "clients: %ld\n",
                        active_clients, active_clients + active_ctrl_clients);
                write(poll_descriptors[i].fd, buf, BUF_SIZE);
              } else {
                fprintf(stderr, "Terminating connection (%d)\n", i);
                CHECK_ERRNO(close(poll_descriptors[i].fd));
                poll_descriptors[i].fd = -1;
              }
            } else {
              printf("(%d) -->%.*s\n", i, (int)received_bytes, buf);
            }
          }
        }
      }
    } else {
      printf("%d milliseconds passed without any events\n", TIMEOUT);
    }
  } while (!finish || active_clients > 0);

  if (poll_descriptors[0].fd >= 0) CHECK_ERRNO(close(poll_descriptors[0].fd));
  exit(EXIT_SUCCESS);
}
