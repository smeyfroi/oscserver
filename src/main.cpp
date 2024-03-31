#include <mqueue.h>
#include <fcntl.h>              /* For definition of O_NONBLOCK */
#include <iostream>
#include <fstream>
#include <string>
#include <cstring>
#include <chrono>
using namespace std::chrono_literals;
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#include <unistd.h>

#define _BSD_SOURCE   /* To get definitions of NI_MAXHOST and NI_MAXSERV from <netdb.h> */
#define ADDRSTRLEN (NI_MAXHOST + NI_MAXSERV + 10)

#define BACKLOG 50
int lfd;
const char* OSC_PORT = "8000";
void startOscServer() {
  /* Call getaddrinfo() to obtain a list of addresses that we can try binding to */
  struct addrinfo hints;
  struct addrinfo *result;
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_canonname = NULL;
  hints.ai_addr = NULL;
  hints.ai_next = NULL;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC; /* Allows IPv4 or IPv6 */
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV; /* Wildcard IP address; service name is numeric */
  if (getaddrinfo(NULL, OSC_PORT, &hints, &result) != 0) {
    std::cerr << "ERR: getaddrinfo" << std::endl;
    exit(1);
  }
  /* Walk through returned list until we find an address structure that can be used to successfully create and bind a socket */
  int optval = 1;
  struct addrinfo *rp;
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    lfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (lfd == -1)
      continue;                   /* On error, try next address */
    if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
      std::cerr << "ERR: setsockopt" << std::endl;
      exit(-1);
    }
    if (bind(lfd, rp->ai_addr, rp->ai_addrlen) == 0)
      break;                      /* Success */
    /* bind() failed: close this socket and try next address */
    close(lfd);
  }
  if (rp == NULL) {
    std::cerr << "ERR: Could not bind socket to any address" << std::endl;
    exit(-1);
  }
  if (listen(lfd, BACKLOG) == -1) {
    std::cerr << "ERR: listen" << std::endl;
    exit(-1);
  }
  freeaddrinfo(result);
  std::cout << "Opened OSC listener" << std::endl;
}

constexpr size_t MAX_MQ_MESSAGE_SIZE = 2048; // must be at least mq_msgsize AND (max osc buffer + OSC_TERMINATOR_LENGTH)

// The same code in analyser so whoever gets there first will create the queue
mqd_t read_mqd;
const char* OSC_QUEUE_NAME = "/osc";
void openMessageQueueForRead() {
  mode_t perms = S_IRUSR | S_IWUSR;
  struct mq_attr attr;
  attr.mq_maxmsg = 10;
  attr.mq_msgsize = MAX_MQ_MESSAGE_SIZE;
  read_mqd = mq_open(OSC_QUEUE_NAME, O_RDONLY | O_CREAT, perms, &attr);
  if (read_mqd == (mqd_t)-1) {
    std::cerr << "Can't open mq '" << OSC_QUEUE_NAME << "' for read" << std::endl;
    exit(1);
  }
  std::cout << "Opened mq for read" << std::endl;
}

char messageBuffer[MAX_MQ_MESSAGE_SIZE];
const size_t OSC_TERMINATOR_LENGTH = 6;
const char* OSC_TERMINATOR = "[/TCP]";

constexpr size_t IS_ADDR_STR_LEN = 4096;
void readMessages() {
  openMessageQueueForRead();

  // handle client connections, serially, forever
  unsigned int prio;
  while(true) {

    // Accept a client connection, obtaining client's address
    struct sockaddr_storage claddr;
    socklen_t addrlen = sizeof(struct sockaddr_storage);
    int cfd = accept(lfd, (struct sockaddr *) &claddr, &addrlen);
    if (cfd == -1) {
      std::this_thread::sleep_for(500ms);
      continue;
    }

    // Log the new connection
    {
      char host[NI_MAXHOST];
      char service[NI_MAXSERV];
      char addrStr[ADDRSTRLEN];
      if (getnameinfo((struct sockaddr *) &claddr, addrlen, host, NI_MAXHOST, service, NI_MAXSERV, 0) == 0)
        snprintf(addrStr, ADDRSTRLEN, "(%s, %s)", host, service);
      else
        snprintf(addrStr, ADDRSTRLEN, "(?UNKNOWN?)");
      std::cout << "OSC client connection from " << addrStr << std::endl;
    }

    // Prepare to read from mq
    struct mq_attr attr;
    if (mq_getattr(read_mqd, &attr) == -1) {
      std::cerr << "Can't fetch attributes for mq '" << OSC_QUEUE_NAME << "'" << std::endl;
      break;
    }
    if (MAX_MQ_MESSAGE_SIZE > attr.mq_msgsize) {
      std::cerr << "MAX_MQ_MESSAGE_SIZE " << MAX_MQ_MESSAGE_SIZE << " > mq_msgsize " << attr.mq_msgsize << std::endl;
      break;
    }

    // Flush stale messages
    attr.mq_flags = O_NONBLOCK;
    mq_setattr(read_mqd, &attr, NULL);
    ssize_t flushRead = 1;
    while(flushRead > -1) {
      flushRead = mq_receive(read_mqd, messageBuffer, attr.mq_msgsize, &prio);
    }
    attr.mq_flags = 0;
    mq_setattr(read_mqd, &attr, NULL);

    // send OSC messages to the connected client until it disconnects
    while(true) {

      // read an OSC message
      ssize_t sizeRead = mq_receive(read_mqd, messageBuffer, attr.mq_msgsize, &prio);

      // append a terminator for the TCP stream
      if (sizeRead > MAX_MQ_MESSAGE_SIZE - OSC_TERMINATOR_LENGTH) {
        std::cerr << "Message size " << sizeRead << " > max of " << (MAX_MQ_MESSAGE_SIZE - OSC_TERMINATOR_LENGTH) << std::endl;
      }
      memcpy(static_cast<void*>(messageBuffer+sizeRead), OSC_TERMINATOR, OSC_TERMINATOR_LENGTH);

      // Write OSC packet to client TCP
      size_t bufferSize = sizeRead + OSC_TERMINATOR_LENGTH;
      if (write(cfd, messageBuffer, bufferSize) != bufferSize) {
        // And finish with this client if it went away
        close(cfd);
        std::cerr << "Disconnect and wait for new OSC connection" << std::endl;
        break;
      }
    }
  }
}

int main(int argc, char* argv[]) {
  /* Ignore the SIGPIPE signal, so that we find out about broken connection
     errors via a failure from write(). */
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    std::cerr << "ERR: signal" << std::endl;
    exit(1);
  }

  std::cout << "Start OSC server\n";
  startOscServer();
  std::cout << "Start reading from mq\n";
  readMessages();
}
